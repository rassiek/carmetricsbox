#include <Arduino_JSON.h>
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"

const char* apSSID = "CAN";
const char* apPassword = "cani1234";
const char* ssid = "ssid";
const char* password = "password";

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

/*
// Function to get the value for a specific key from lines - NO LONGER USED
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
*/

String getSensorReadings() {
  JSONVar newReadings; // Use a local JSONVar to build the new set of readings
  String lines[32];    // Increased size to handle more potential sensor lines
  
  if (lastBlock.length() == 0) {
    // Return empty JSON object string if lastBlock is empty
    return JSON.stringify(newReadings); 
  }

  int numLines = splitMessage(lastBlock, lines);

  for (int i = 0; i < numLines; i++) {
    String currentLine = lines[i];
    currentLine.trim();
    int separatorIndex = currentLine.indexOf(':');
    if (separatorIndex != -1) {
      String key = currentLine.substring(0, separatorIndex);
      key.trim();
      String value = currentLine.substring(separatorIndex + 1);
      value.trim();
      
      if (key.length() > 0 && value.length() > 0) {
        newReadings[key] = value;
      }
    }
  }
  
  // The global 'readings' variable is updated here for any other potential uses,
  // though it's better practice to pass data explicitly.
  // For the purpose of events.send, this ensures the global 'readings' is up-to-date if it's used elsewhere.
  // However, the function directly returns the stringified newReadings, which is what /readings endpoint uses.
  readings = newReadings; 

  String jsonString = JSON.stringify(newReadings);
  return jsonString;
}

// Helper function to send a command to Arduino via Serial2 and get the response
String sendArduinoCommand(const String& command, unsigned long timeout = 2000) {
  Serial2.print(command); // Send the command
  String response = "";
  unsigned long startTime = millis();
  bool foundEndMarker = false; // For commands that have a specific end marker

  if (command.indexOf("GET_ALL_CONF") != -1) { // Special handling for multi-line response
    while (millis() - startTime < timeout) {
      if (Serial2.available()) {
        String line = Serial2.readStringUntil('\n');
        line.trim();
        response += line + "\n"; // Append each line
        if (line.equals("END_CONF_LIST")) {
          foundEndMarker = true;
          break; 
        }
      }
    }
    if (!foundEndMarker) {
      Serial.println("WARN: GET_ALL_CONF response did not see END_CONF_LIST or timed out.");
    }
  } else { // For single-line ACK or simple responses
    while (millis() - startTime < timeout) {
      if (Serial2.available()) {
        response = Serial2.readStringUntil('\n');
        response.trim();
        break; // Got a response line
      }
    }
  }
  Serial.print("Arduino command '"); Serial.print(command.substring(0, command.length()-1)); 
  Serial.print("' response: '"); Serial.print(response); Serial.println("'");
  return response;
}

// Helper function to convert sensor type string to int (matching Arduino enum)
int convertSensorTypeStringToInt(String typeString) {
  if (typeString.equalsIgnoreCase("PRES_A")) return 1;
  if (typeString.equalsIgnoreCase("DS18B20")) return 2;
  if (typeString.equalsIgnoreCase("MAX6675")) return 3;
  if (typeString.equalsIgnoreCase("BMP085")) return 4;
  if (typeString.equalsIgnoreCase("VOLT_A")) return 5;
  if (typeString.equalsIgnoreCase("UNDEF")) return 0;
  return 0; // Default to UNDEFINED if no match
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
    // delay(1000); // Delay might not be necessary here, can cause sluggishness
    request->send(200, "application/json", json);
    // json = String(); // Not needed, json is local
  });

  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest *request){
    String rawConf = sendArduinoCommand("GET_ALL_CONF\n");
    JSONVar sensorsArray;
    
    String lines[64]; // Max 64 sensors for config display for now
    int numLines = splitMessage(rawConf, lines);
    bool listStarted = false;

    for (int i = 0; i < numLines; i++) {
      String currentLine = lines[i];
      currentLine.trim();
      if (currentLine.equals("START_CONF_LIST")) {
        listStarted = true;
        continue;
      }
      if (currentLine.equals("END_CONF_LIST")) {
        listStarted = false;
        break;
      }
      if (listStarted && currentLine.length() > 0) {
        JSONVar sensorConf;
        // Parse CSV: id,name,type,p1,p2,p3,addr_hex,enabled,wT,cT,lwT,lcT
        String parts[12];
        int partIndex = 0;
        int currentPos = 0;
        for(int p=0; p<12; p++){
            int nextComma = currentLine.indexOf(',', currentPos);
            if(nextComma == -1){
                parts[partIndex++] = currentLine.substring(currentPos);
                break;
            }
            parts[partIndex++] = currentLine.substring(currentPos, nextComma);
            currentPos = nextComma + 1;
        }

        if (partIndex == 12) {
          sensorConf["id"] = parts[0];
          sensorConf["name"] = parts[1];
          sensorConf["sensorType"] = parts[2];
          sensorConf["pin1"] = parts[3]; // Will be String, client can parse if needed
          sensorConf["pin2"] = parts[4];
          sensorConf["pin3"] = parts[5];
          sensorConf["oneWireAddress"] = parts[6];
          sensorConf["enabled"] = parts[7];
          sensorConf["warningThreshold"] = parts[8];
          sensorConf["criticalThreshold"] = parts[9];
          sensorConf["lowerWarningThreshold"] = parts[10];
          sensorConf["lowerCriticalThreshold"] = parts[11];
          sensorsArray[sensorsArray.size()] = sensorConf; // Add to array
        }
      }
    }
    request->send(200, "application/json", JSON.stringify(sensorsArray));
  });

  server.on("/api/saveconfig", HTTP_POST, [](AsyncWebServerRequest *request){
    String response = sendArduinoCommand("SAVE_CONF\n");
    if (response.indexOf("ACK_SAVE_CONF") != -1) {
      request->send(200, "application/json", "{\"status\": \"success\", \"message\": \"Configuration saved on Arduino\"}");
    } else {
      request->send(500, "application/json", "{\"status\": \"error\", \"message\": \"Failed to save configuration on Arduino\", \"arduino_response\": \"" + response + "\"}");
    }
  });

  server.on("/api/loaddefaults", HTTP_POST, [](AsyncWebServerRequest *request){
    String response = sendArduinoCommand("LOAD_DEFAULTS\n");
    if (response.indexOf("ACK_LOAD_DEFAULTS") != -1) {
      request->send(200, "application/json", "{\"status\": \"success\", \"message\": \"Default configurations loaded on Arduino\"}");
    } else {
      request->send(500, "application/json", "{\"status\": \"error\", \"message\": \"Failed to load defaults on Arduino\", \"arduino_response\": \"" + response + "\"}");
    }
  });
  
  // Handler for POST /api/setconfig
  server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    if (request->url().equals("/api/setconfig") && request->method() == HTTP_POST) {
      if (index == 0) { // First chunk
        request->_tempObject = new String();
      }
      String* bodyStr = (String*)request->_tempObject;
      bodyStr->concat((char*)data, len);
      
      if (index + len == total) { // All data received
        Serial.println("Received POST to /api/setconfig with body: " + *bodyStr);
        JSONVar jsonData = JSON.parse(*bodyStr);
        delete bodyStr; // Clean up allocated string
        request->_tempObject = nullptr;

        if (JSON.typeof(jsonData) == "undefined") {
          request->send(400, "application/json", "{\"status\": \"error\", \"message\": \"Invalid JSON payload\"}");
          return;
        }

        if (!jsonData.hasOwnProperty("id")) {
          request->send(400, "application/json", "{\"status\": \"error\", \"message\": \"Missing 'id' in JSON payload\"}");
          return;
        }

        String id = (const char*) jsonData["id"];
        String name = jsonData.hasOwnProperty("name") ? (const char*) jsonData["name"] : "Unknown";
        String typeStr = jsonData.hasOwnProperty("sensorType") ? (const char*) jsonData["sensorType"] : "UNDEF";
        int typeInt = convertSensorTypeStringToInt(typeStr);
        
        String p1 = jsonData.hasOwnProperty("pin1") ? String((int)jsonData["pin1"]) : "-1";
        String p2 = jsonData.hasOwnProperty("pin2") ? String((int)jsonData["pin2"]) : "-1";
        String p3 = jsonData.hasOwnProperty("pin3") ? String((int)jsonData["pin3"]) : "-1";
        
        String addr_hex = jsonData.hasOwnProperty("oneWireAddress") ? (const char*)jsonData["oneWireAddress"] : "0000000000000000";
        String enabled = jsonData.hasOwnProperty("enabled") ? ( (bool)jsonData["enabled"] ? "1" : "0" ) : "0";
        
        String wT = jsonData.hasOwnProperty("warningThreshold") ? String((double)jsonData["warningThreshold"], 2) : "0.00";
        String cT = jsonData.hasOwnProperty("criticalThreshold") ? String((double)jsonData["criticalThreshold"], 2) : "0.00";
        String lwT = jsonData.hasOwnProperty("lowerWarningThreshold") ? String((double)jsonData["lowerWarningThreshold"], 2) : "0.00";
        String lcT = jsonData.hasOwnProperty("lowerCriticalThreshold") ? String((double)jsonData["lowerCriticalThreshold"], 2) : "0.00";

        // Replace spaces in name with underscores for the command, Arduino will handle it or we adapt Arduino parser.
        // For now, let's assume Arduino's parser can handle spaces in name if it's not the first/last part of a field.
        // The current Arduino parser splits by space, so name should not have spaces or needs quotes.
        // Let's replace spaces in name with underscores for safety for now.
        name.replace(' ', '_');

        String command = "SET_SENSOR_CONF " + id + " " + name + " " + String(typeInt) + " " +
                         p1 + " " + p2 + " " + p3 + " " + addr_hex + " " + enabled + " " +
                         wT + " " + cT + " " + lwT + " " + lcT + "\n";
        
        Serial.print("Sending to Arduino: "); Serial.println(command);
        String response = sendArduinoCommand(command);

        if (response.startsWith("ACK_SET_CONF")) {
          request->send(200, "application/json", "{\"status\": \"success\", \"message\": \"Sensor " + id + " configured\", \"arduino_response\": \"" + response + "\"}");
        } else {
          request->send(500, "application/json", "{\"status\": \"error\", \"message\": \"Failed to set sensor " + id + " configuration on Arduino\", \"arduino_response\": \"" + response + "\"}");
        }
      }
    }
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
