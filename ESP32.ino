#include <Arduino_JSON.h>
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"

const char* apSSID = "CAN";
const char* apPassword = "cani1234";
const char* ssid = "HouseLANnister";
const char* password = "Ella42019";

AsyncWebServer server(80);
AsyncEventSource events("/events");
JSONVar readings;
String lastBlock;  // Declare lastBlock as a global variable

String rawSerialData;

// Function to split a message into lines
int splitMessage(String message, String lines[]) {
  int count = 0;
  int start = 0;
  int end = message.indexOf('\n');
  while (end != -1) {
    lines[count++] = message.substring(start, end);
    start = end + 1;
    end = message.indexOf('\n', start);
  }
  return count;
}

// Function to get the value for a specific key from lines
String getValue(const String lines[], const String& key) {
  for (int i = 0; i < 16; i++) {
    int separatorIndex = lines[i].indexOf(':');
    if (separatorIndex != -1) {
      String currentKey = lines[i].substring(0, separatorIndex);
      currentKey.trim();
      if (currentKey == key) {
        String value = lines[i].substring(separatorIndex + 1);
        value.trim();
        return value;
      }
    }
  }
  return "";
}

String getSensorReadings() {
  String lines[16];
  int numLines = splitMessage(lastBlock, lines);
 // readings["dateTime"] = getValue(lines, "DATETIME");
  readings["cabinTemperature"] = getValue(lines, "CTMP");
  readings["coolantTemperature"] = getValue(lines, "CT");
  readings["intakeTemperature"] = getValue(lines, "IMT");
  readings["egt"] = getValue(lines, "EGT");
  readings["gearBoxTemperature"] = getValue(lines, "GBT");
  readings["transferCaseTemperature"] = getValue(lines, "TCT");
  readings["boost"] = getValue(lines, "BST");
  readings["oilPressure"] = getValue(lines, "Oil");
  readings["batteryVoltage"] = getValue(lines, "BAT1");

  String jsonString = JSON.stringify(readings);
  return jsonString;
}

void initSPIFFS() {
  if (!SPIFFS.begin()) {
    Serial.println("An error has occurred while mounting SPIFFS");
  }
  Serial.println("SPIFFS mounted successfully");
}

void setup() {

// Set a longer watchdog timeout (for example, 10 seconds)
  // Set a longer watchdog timeout (for example, 10 seconds)

  Serial.begin(115200);
  Serial2.begin(4800);



 // Set up ESP32 as an access point
  WiFi.softAP(apSSID, apPassword);

  IPAddress apIP = WiFi.softAPIP();
  Serial.println("Access Point IP address: " + apIP.toString());
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
/*
 
  //Set ESP32 as Wifi Client
 
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(2000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
 
  
  */
  
  
  initSPIFFS();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });


   // Route for serving CSS file
  server.on("/all.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/all.css", "text/css");
  });

     // Route for serving CSS file
  server.on("/charts", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/charts.html", "text/html");
  });

  // Route for serving JavaScript file
  server.on("/gauge.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/gauge.js", "application/javascript");
  });

    // Route for serving JavaScript file
  server.on("/highcharts.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/highcharts.js", "application/javascript");
  });
    

  server.serveStatic("/", SPIFFS, "/");

  server.on("/readings", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = getSensorReadings();
    delay(1000);
    request->send(200, "application/json", json);
    json = String();
  });

  events.onConnect([](AsyncEventSourceClient *client){
    if(client->lastId()){
      Serial.printf("Client reconnected! Last message ID that it got is: %u\n", client->lastId());
    }
    client->send("hello!", NULL, millis(), 1000);
  });

  server.addHandler(&events);
  server.begin();
}

void loop() {
  while (Serial2.available() > 0) {
    char c = Serial2.read();
    rawSerialData += c;
  }

  //if (rawSerialData.length() > 300) {
  //  rawSerialData = rawSerialData.substring(rawSerialData.length() - 100);
 // }

   if (rawSerialData.length() > 500) {
    rawSerialData = rawSerialData.substring(rawSerialData.length() - 500);
  }

  //Serial.println(lastBlock);

  int start = rawSerialData.lastIndexOf('<');
  int end = rawSerialData.lastIndexOf('>');

  if (start != -1 && end != -1 && end > start) {
    lastBlock = rawSerialData.substring(start + 1, end);
  }

 //Serial.println(lastBlock);

  String jsonReadings = getSensorReadings();
  events.send(jsonReadings.c_str(), "new_readings", millis());

  // Print the entire sensor readings
  Serial.println("Sensor Readings:");
  Serial.println(jsonReadings);

//  delay(500);
}
