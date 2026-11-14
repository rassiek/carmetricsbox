// CarMetrics Display - touch screen for the CarMetrics main board.
//
// Boards: JC3248W535 (ESP32-S3, 3.5" 320x480) and the Cheap Yellow Display
// ESP32-2432S028R (ESP32, 2.8" 320x240). Build: tools/cm.sh display
// (copies the shared board-to-board files from ../CarMetrics first).
//
// Pairs with the main board over ESP-NOW like a sensor / relay board (role
// "display": Accept it on the main board's Boards page), with the main board's
// Wi-Fi access point as the fallback. Its own web page (layout editor,
// settings, firmware) is on its access point CarMetrics-Display-xxxx at
// http://192.168.5.1/.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include "DispConfig.h"
#include "Hw.h"
#include "Model.h"
#include "Net.h"
#include "Ui.h"
#include "WebUi.h"

DispSettings settings;
Disp::Model model;
Disp::Ui* ui = nullptr;
volatile bool layoutDirty = false, settingsDirty = false, identifyReq = false;
String noticeText;
volatile bool noticeNew = false;
portMUX_TYPE noticeMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t lastTouchAt = 0, bootAt = 0;
bool dimmed = false, confirmed = false;

uint8_t rotation() { return settings.rotation > 3 ? Hw::defaultRotation() : settings.rotation; }
bool portrait() { return Hw::surface().height() >= Hw::surface().width(); }

void loadLayout() {
  String j = DispConfig::loadLayout();
  Net::Lock lk;
  if (j.length()) {
    JsonDocument d; std::string err;
    if (!deserializeJson(d, j) && model.setLayout(d.as<JsonVariantConst>(), err)) return;
    Serial.printf("[disp] saved layout not usable (%s): automatic layout\n", err.c_str());
  }
  model.useDefaultLayout(portrait());
}

void notice(const String& m) {
  portENTER_CRITICAL(&noticeMux); noticeText = m; noticeNew = true; portEXIT_CRITICAL(&noticeMux);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[disp] CarMetrics Display %s\n", DISP_VERSION);
  bootAt = millis();
  DispConfig::load(settings);
  WiFi.mode(WIFI_AP_STA);   // MAC available for the default name
  if (!Hw::begin(rotation(), settings.panel.c_str(), settings.touch)) Serial.println("[disp] no screen: running headless");
  Hw::setBrightness(settings.brightness);

  Disp::UiActions a;
  a.relay = [](uint8_t b, uint8_t i, bool on) { Net::relay(b, i, on); };
  a.mute = [](uint16_t m) { Net::mute(m); };
  a.setLimit = [](int s, uint8_t w, float v) { Net::setLimit(s, w, v); };
  a.brightness = [](int d) { settings.brightness = constrain(settings.brightness + d, 5, 100); Hw::setBrightness(settings.brightness); DispConfig::save(settings); };
  a.rotate = [] {
    settings.rotation = Hw::nextRotation(rotation()); DispConfig::save(settings);
    Hw::setRotation(settings.rotation);
    if (model.layoutIsDefault()) { Net::Lock lk; model.useDefaultLayout(portrait()); }
    ui->invalidate();
  };
  a.restart = [] { notice("Restarting…"); delay(300); ESP.restart(); };
  ui = new Disp::Ui(model, a);

  Net::Hooks nh;
  nh.catalogChanged = [] { layoutDirty = true; };
  nh.notice = notice;
  nh.identify = [] { identifyReq = true; };
  Net::begin(settings, model, nh);
  loadLayout();

  WebUi::Hooks wh;
  wh.layoutChanged = [] { layoutDirty = true; };
  wh.settingsChanged = [] { settingsDirty = true; };
  wh.identify = [] { identifyReq = true; };
  wh.screenSize = [](int& w, int& h) { w = Hw::surface().width(); h = Hw::surface().height(); };
  WebUi::begin(settings, model, wh);
  Serial.printf("[disp] own access point %s, http://%s/\n", Net::apName().c_str(), Net::apIp().c_str());
}

void loop() {
  uint32_t now = millis();
  Net::update(now);
  if (WebUi::rebootRequested()) { delay(500); ESP.restart(); }
  // 60 s running = this firmware is good (no rollback to the previous one)
  if (!confirmed && now - bootAt > 60000) { esp_ota_mark_app_valid_cancel_rollback(); confirmed = true; }
  if (settingsDirty) {
    settingsDirty = false;
    Hw::setBrightness(settings.brightness);
    if (rotation() != 255) Hw::setRotation(rotation());
    Net::applyWifiSettings();
    layoutDirty = true;
  }
  if (layoutDirty) {
    layoutDirty = false;
    { Net::Lock lk; if (model.layoutIsDefault()) model.useDefaultLayout(portrait()); }
    ui->layoutChanged();
  }
  if (identifyReq) { identifyReq = false; ui->identify(now); }
  if (noticeNew) {
    String m;
    portENTER_CRITICAL(&noticeMux); m = noticeText; noticeNew = false; portEXIT_CRITICAL(&noticeMux);
    ui->toast(m.c_str(), now);
  }
  // touch
  int x, y;
  bool t = Hw::readTouch(x, y);
  if (t) {
    lastTouchAt = now;
    if (dimmed) { dimmed = false; Hw::setBrightness(settings.brightness); }
  }
  {
    Net::Lock lk;
    if (!dimmed || !t) ui->touch(t, x, y, now);
  }
  if (settings.dimAfterMin && !dimmed && now - lastTouchAt > settings.dimAfterMin * 60000UL && model.worstLevel() != Disp::LVL_CRIT) {
    dimmed = true; Hw::setBrightness(10);
  }
  if (dimmed && model.worstLevel() == Disp::LVL_CRIT) { dimmed = false; Hw::setBrightness(settings.brightness); }
  // screen (about 20 frames a second at most; only changed parts are drawn)
  static uint32_t lastDraw = 0;
  if (now - lastDraw >= 50) {
    lastDraw = now;
    Disp::UiInfo i;
    i.name = settings.name.c_str(); i.firmware = DISP_VERSION; i.profile = Hw::profile();
    i.link = Net::linkKind(); i.paired = Net::paired(); i.boardId = Net::boardId(); i.channel = Net::channel();
    i.wifiSsid = Net::wifiSsid().c_str(); i.wifiIp = Net::wifiIp().c_str(); i.apName = Net::apName().c_str(); i.apIp = Net::apIp().c_str();
    i.brightness = settings.brightness; i.canRelays = Net::canCommand();
    ui->setInfo(i);
    Net::Lock lk;
    ui->draw(Hw::surface(), now);
  }
  delay(5);
}
