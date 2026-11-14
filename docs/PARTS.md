# CarMetrics — parts list

Status: **draft for review** (5.3.0). Grouped by board. "Search" is what to type on
AliExpress; check the photos and the listing's pinout before buying, since sellers
use the same words for different boards. Quantities are for one installation.
Wiring for every part: `docs/WIRING.md`.

---

## 1. Main / cabin board

| Qty | Part | Search | For | Notes / alternatives |
|---|---|---|---|---|
| 1 | ESP32-S3 dev board, 16 MB flash + 8 MB PSRAM | `ESP32-S3 N16R8 development board` | the main board | N8R8 (8 MB) works too. Prefer a board with the **UART** USB port and a u.FL antenna option |
| 1 | Screw-terminal breakout for that board | `ESP32-S3 breakout board terminal` | plug-in carrier, swappable | match the pin count and row spacing of your S3 board |
| 1 | 12 V → 5 V car buck converter, 3 A | `DC-DC 12V 24V to 5V 3A step down waterproof` | power | wide input (8–36 V). A good car USB charger also works |
| 1 | Inline blade fuse holder + 2 A fuse | `inline blade fuse holder waterproof` | 12 V feed (ignition-switched) | |
| 1 | USB-C female breakout (power only) | `USB Type-C female breakout board` | optional phone-charger input | jumper selects buck or USB |
| 1 | AMS1117-3.3 module | `AMS1117 3.3V power module` | 3.3 V rail for sensors | |
| 1 | 1000 µF 10 V capacitor | `1000uF 10V electrolytic` | holds 5 V during cranking | |
| 1 | MPU-6050 | `GY-521 MPU6050` | tilt (roll / pitch), acceleration | mount flat on the box |
| 1 | BME280 | `GY-BME280 3.3V` | cabin temperature, humidity, altitude | listings often send a BMP280 (no humidity): check the chip marking |
| 1 | OLED 1.5" 128×128 SSD1327 (I2C) | `1.5 inch OLED SSD1327 128x128 I2C` | dash display | 0.96" SSD1306 128×64 also supported |
| 1 | Active buzzer module, 3 pin | `active buzzer module 3 pin` | alarm sound | most sound on a **low** input: tick "buzzer active low" |
| 1 | 10 mm red LED + 100 Ω | `10mm LED red diffused` | flasher | |
| 1–2 | RGB LED, common cathode, 4 pin + 3 resistors (150 Ω R, 100 Ω G, 100 Ω B) | `5mm RGB LED common cathode` | status light (besides the S3's built-in one) | or a `KY-016 RGB LED module` (resistors on board). Common anode also supported |
| 1 | RJ45 screw-terminal breakout | `RJ45 breakout board screw terminal` | data cable to the engine bay | |
| 1 | 50 × 70 mm perfboard | `50x70mm PCB prototype board double sided` | cabin board | |
| — | Female header strips, screw terminals 5.08 mm | `female header 2.54mm`, `KF301 5.08mm screw terminal` | sockets for modules | |

## 2. Engine-bay worker board

| Qty | Part | Search | For | Notes / alternatives |
|---|---|---|---|---|
| 1 | ESP32 DevKit 38-pin + screw-terminal carrier, or ESP32-S3 | `ESP32 38 pin development board`, `ESP32 terminal adapter 38 pin` | worker board | your classic ESP32 works here |
| 1 | ADS1115 | `ADS1115 16 bit ADC module` | 4 analog inputs (powered from 5 V) | a second one at address 0x49 gives 8 |
| 1–3 | B25 voltage sensor | `voltage sensor module 0-25V` | 12 V analog inputs (battery, 12 V senders) → 0–5 V | divides by 5 |
| 1 | Level shifter, BSS138 type, 4 or 8 channel | `4 channel I2C logic level converter bi-directional` | I2C, 1-Wire, 5 V logic in | **not** the TXS0108E kind for I2C / 1-Wire |
| 1 | Opto-coupler board PC817, 4 channel, 12 V input | `PC817 4 channel optocoupler isolation board 12V` | alternator W (RPM), ignition, switches, warning lights | 5 V-input version + 1 kΩ in series if RPM starts too late |
| 1 | MAX6675 + K-type probe | `MAX6675 module`, `EGT probe K type 1/8 NPT` | exhaust gas temperature | use thermocouple extension wire |
| 4–6 | DS18B20 waterproof probes | `DS18B20 waterproof probe stainless` | coolant, gearbox, transfer case, intake | + one 4.7 kΩ pull-up per bus |
| 1 | IP65 enclosure + cable glands | `IP65 junction box 100x68x50`, `PG7 cable gland` | protection | firewall side, away from the exhaust |
| — | Conformal coating | `conformal coating PCB` | moisture | after testing |

## 3. Senders

| Qty | Part | Search | For | Notes |
|---|---|---|---|---|
| 1 | Pressure transducer 0–100 psi, 5 V, 0.5–4.5 V | `pressure transducer 5V 0.5-4.5V 100psi 1/8 NPT` | oil pressure | stainless; check thread for your engine |
| 1 | Pressure transducer 0–30 psi (or 0–2.5 bar) | `pressure transducer 5V 0.5-4.5V 30psi` | boost | gauge type; or a MAP sensor for absolute |
| — | Alternator W terminal | — | RPM | through the PC817 board |

## 4. Actuators

| Qty | Part | Search | For | Notes |
|---|---|---|---|---|
| 1 | **ESP32 8-relay board, 7–30 V** (LC Technology "ESP32 Relay X8") | `ESP32 8 channel relay module 7-30V` | **relay worker board**: lights, pumps, compressor, fans | runs the `relay-x8` firmware; needs a 3.3 V USB-to-serial adapter once (below). 10 A relays: switch bigger loads through automotive relays |
| 1 | USB-to-serial adapter, 3.3 V | `CP2102 USB to TTL 3.3V` | first flash of the relay board | set to 3.3 V, never 5 V |
| — | Relay modules (if not using the relay board) | `relay module 5V optocoupler` | single loads | |
| 1+ | Logic-level MOSFET module (D4184 / AOD4184) | `D4184 MOSFET module` | PWM: 12 V fans, pumps, lamps (≈15 A) | avoid IRF520 modules (barely switch at 3.3 V) |
| — | H-bridge BTS7960 43 A | `BTS7960 motor driver` | motors both ways (winch, actuator, flap) | |
| — | PWM fan controller (for radiator fans 15–25 A) | `PWM fan controller 12V 40A` | big fans | driven by a CarMetrics PWM output |
| — | Automotive relays 30/40 A + holders | `automotive relay 40A 12V with socket` | heavy loads behind the relay board | |

## 5. Cabling between boards

| Qty | Part | Search | For | Notes |
|---|---|---|---|---|
| 1 | Cat6 FTP patch cable, **stranded**, length to suit | `cat6 FTP patch cable shielded` | data cable through the firewall | label both ends **NOT ETHERNET** |
| 1 | IP67 RJ45 panel connector (optional) | `IP67 RJ45 waterproof panel mount` | sealed entry on the engine-bay box | |
| 1 | 3-core cable 0.5–0.75 mm² | `3 core cable 0.75mm` | 5 V, 3.3 V, GND to the engine bay | |
| — | Fuses for each battery sense wire (1 A) | `mini blade fuse 1A` | battery sense | fuse at the battery end |

## 6. Touch displays

| Qty | Part | Search | Firmware | Notes |
|---|---|---|---|---|
| 1 | Guition **JC3248W535** 3.5" 320×480 (ESP32-S3, 16 MB + 8 MB) | `JC3248W535 ESP32-S3 3.5 inch` | `display-jc3248w535` | buy the version **with the case** |
| 1 | **Cheap Yellow Display** ESP32-2432S028R 2.8" 320×240 | `ESP32-2432S028R` | `display-cyd` | 1-USB version = ILI9341; 2-USB version = ST7789 (Settings → Panel) |

## 7. Tools and consumables

| Part | Search |
|---|---|
| Multimeter | `digital multimeter auto range` |
| Soldering iron with temperature control | `soldering station T12` |
| Dupont leads, heat-shrink, cable ties | `dupont jumper wire`, `heat shrink tube kit` |
| Resistor and capacitor kits | `1/4W resistor kit`, `ceramic capacitor kit 100nF` |
| Zener diodes 4.7 V, 1N4148, 1N5822 | `BZX55C4V7`, `1N4148`, `1N5822` |
