/*
  Sentinel-ESP8266
  Home monitoring software running on ESP8266 ESP-12F.
*/

#include <FS.h> //this needs to be first, or it all crashes and burns...
#ifdef ESP32
#include <SPIFFS.h>
#endif
#include <ArduinoJson.h> //https://github.com/bblanchon/ArduinoJson
#include <ArduinoMqttClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <SFE_BMP180.h>
#include <WiFiClient.h>
#include <WiFiManager.h> //https://github.com/tzapu/WiFiManager
#include <Wire.h>

ESP8266WebServer server(80);
SFE_BMP180 pressure;

// ---------------------------------------------------------------------------------------------------
// VARIABLES
// ---------------------------------------------------------------------------------------------------

#define ALTITUDE 140.0 // Altitude in meters

int pair_delay = 5000;

// Interval for routines
const long interval = 1000;
unsigned long previousMillis = 0;
int count = 0;

// Default configuration, overwritten when configuring the device
char mqtt_server[] = "http://192.168.1.17";
char mqtt_port[] = "8080";
const char topic[] = "sensors/temperature";
char api_token[] = "12345";
char api_url[] = "http://192.168.17:3333";

// Device default
char device_id[] = "0";
char device_name[] = "Sentinel";
char device_paired[] = "false";
char device_zone[] = "office";
const String device_capabilities[] = {
    "sensor/pressure",
    "sensor/temperature"};

char env[] = "dev";

// ---------------------------------------------------------------------------------------------------
// CONFIG
// ---------------------------------------------------------------------------------------------------

// flag for saving data
bool shouldSaveConfig = false;

// callback notifying us of the need to save config
void saveConfigCallback()
{
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// ---------------------------------------------------------------------------------------------------
// SETUP
//
// - Configuration
//  - Check for a configuration file, 'config.json'
//  - Read and parse the content
//  - Assign value to variables
// - Wifi Manager
// - Sensor
// - Web Server
//
// NOTES: NO SPIFFS.format() and wifiManager.resetSettings();
// ---------------------------------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- Sentinel startup");
  chipInfo();

  // --- CONFIGURATION - READ
  Serial.println("[-] Read configuration file");

  // --->
  // SPIFFS.format();
  // ---

  if (SPIFFS.begin())
  {
    if (SPIFFS.exists("/config.json"))
    {
      // Opening the configuration file
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile)
      {
        Serial.println("Opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);

#ifdef ARDUINOJSON_VERSION_MAJOR >= 6
        DynamicJsonDocument json(1024);
        auto deserializeError = deserializeJson(json, buf.get());
        serializeJson(json, Serial);
        if (!deserializeError)
        {
#else
        DynamicJsonBuffer jsonBuffer;
        JsonObject &json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success())
        {
#endif
          Serial.println("[-] Update local configuration");
          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_port, json["mqtt_port"]);
          strcpy(api_token, json["api_token"]);
          strcpy(api_url, json["api_url"]);
          strcpy(device_paired, json["device_paired"]);
          strcpy(device_id, json["device_id"]);
        }
        else
        {
          Serial.println("> failed to open config.json");
        }
        configFile.close();
      }
    }
    else
    {
      Serial.println("> file config.json does not exist");
    }
  }
  else
  {
    Serial.println("> failed to mount file system");
  }
  // end configuration

  // --- WIFI MANAGER ---
  Serial.println("[-] Wifi Manager");

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);
  WiFiManagerParameter custom_api_token("apikey", "API token", api_token, 32);
  WiFiManagerParameter custom_api_url("apiurl", "API url", api_url, 32);

  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // set static ip
  wifiManager.setSTAStaticIPConfig(IPAddress(192, 168, 1, 99), IPAddress(192, 168, 1, 80), IPAddress(255, 255, 255, 0));

  // add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_api_token);
  wifiManager.addParameter(&custom_api_url);

  // reset settings - for testing
  // wifiManager.resetSettings();

  // sets timeout until configuration portal gets turned off
  // useful to make it all retry or go to sleep
  // in seconds
  // wifiManager.setTimeout(120);

  // fetches ssid and pass and tries to connect
  // if it does not connect it starts an access point with the specified name
  // here  "AutoConnectAP"
  // and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect("SentinelAP", "password"))
  {
    Serial.println("> failed to connect and hit timeout");
    delay(3000);
    // reset and try again, or maybe put it to deep sleep
    ESP.restart();
    delay(5000);
  }

  Serial.println("> Sentinel Connected...\n");

  // --- UPDATE CONFIGURATION
  Serial.println("[-] Update custom configuration (if changed by WifiManager)...");
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(api_token, custom_api_token.getValue());
  strcpy(api_url, custom_api_url.getValue());

  Serial.println("[-] Show configuration:");
  Serial.println("\tmqtt_server : " + String(mqtt_server));
  Serial.println("\tmqtt_port : " + String(mqtt_port));
  Serial.println("\tapi_token : " + String(api_token));
  Serial.println("\tapi_url : " + String(api_url));
  Serial.println("\tdevice_paired : " + String(device_paired));

  // --- SAVE CONFIGURATION

  if (shouldSaveConfig)
  {
    saveConfig();
  }

  // --- SENSOR
  Serial.println("[-] Sensor...");
  bmp180Info();

  // --- WEB SERVER
  Serial.println("[-] Web server...");
  server.on("/led", handleLed);
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/rpc", handleRequest);
  server.on("/reset", handleReset);
  server.onNotFound(handleNotFound);
  // FIXME: Handle error
  server.begin();
  Serial.println("> HTTP server started");

  // --- PAIRING
  Serial.println("[-] Device pairing...");
  Serial.println(device_paired);
  if (strcmp(device_paired, "false") == 0)
  {
    Serial.println("> Device not paired!");
    // TODO: Add a callback on success to change the "device_paired"
    pair();
    delay(3000);
  }
  while (strcmp(device_paired, "false") == 0)
  {
    Serial.println("> Device not paired...");
    pair();
    delay(3000);
  }
  Serial.println("> Device paired!");

  // --- Show board informations
  showInfo();
}

void showInfo()
{
  Serial.println("--- Device informations...");
  Serial.println(WiFi.macAddress());
  Serial.println(WiFi.localIP());
  Serial.println(device_id);
  Serial.println("---");
  Serial.println("");
}

// ---------------------------------------------------------------------------------------------------
// LOOP
// ---------------------------------------------------------------------------------------------------

void loop()
{
  // Domaine Name Server
  // MDNS.update();

  // MQTT
  // mqttClient.poll();

  // HTTP Server
  server.handleClient();

  // LoopWithoutDelay startegy

  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval)
  {
    // save the last time a message was sent
    previousMillis = currentMillis;
    // Call routine here...
    getData();
    // end routine
    count++;
  }
}

// ---------------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------------

void bmp180Info()
{
  Serial.println("--- BMP180 setup");
  if (pressure.begin())
    Serial.println("> BMP180 init success");
  else
  {
    Serial.println("> BMP180 init fail\n\n");
    while (1)
      ; // Pause forever.
  }
}

// ---------------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------------

void chipInfo()
{
  Serial.println("--- BOARD INFO");
  Serial.println("NodeMCU Amica V2 ESP8266 ES12F");
  unsigned int chip_id = ESP.getChipId();
  Serial.print("Chip id: ");
  Serial.println(chip_id);
  Serial.print("Core version: ");
  Serial.println(ESP.getCoreVersion());
  Serial.print("Sdk version: ");
  Serial.println(ESP.getSdkVersion());
  Serial.print("CPU frequency: ");
  Serial.println(ESP.getCpuFreqMHz());
  Serial.print("Flash chip id: ");
  Serial.println(ESP.getFlashChipId());
  Serial.print("Flash chip size: ");
  Serial.println(ESP.getFlashChipSize());
  Serial.print("Flash chip frenquency: ");
  Serial.println(ESP.getFlashChipSpeed());
  Serial.print("Supply voltage: ");
  Serial.println(ESP.getVcc());
}

// ---------------------------------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------------------------------

void post(double T)
{
  Serial.println("--- Post Measurements...");
  Serial.println(T);
  if (WiFi.status() == WL_CONNECTED)
  {
    WiFiClient client;
    HTTPClient http;
    String url = "http://192.168.1.17:3333/measurements";
    Serial.println(url);
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(200);
    doc["device_id"] = device_id;
    doc["type"] = "temperature";
    doc["value"] = T;

    String requestBody;
    serializeJson(doc, requestBody);
    int httpResponseCode = http.POST(requestBody);

    if (httpResponseCode > 0)
    {
      String response = http.getString();
      Serial.println(httpResponseCode);
      Serial.println(response);
    }
    else
    {
      Serial.printf("> Error occurred while sending HTTP POST: %s\n",
                    http.errorToString(httpResponseCode).c_str());
    }
  }
  else
  {
    Serial.println("> WiFi Disconnected");
  }
}

void pair()
{
  Serial.println("--- Pairing device, sending http request...");
  if (WiFi.status() == WL_CONNECTED)
  {
    WiFiClient client;
    HTTPClient http;

    http.begin(client, "http://192.168.1.17:3333/devices/pair");
    http.addHeader("Content-Type", "application/json");

    // Create the Pair Device Request
    DynamicJsonDocument device(200);
    device["name"] = device_name;
    device["mac"] = WiFi.macAddress();
    device["ip"] = WiFi.localIP();
    device["zone"] = device_zone;
    device["capabilities"] = device_zone;

    String requestBody;
    serializeJson(device, requestBody);
    int httpResponseCode = http.POST(requestBody);

    // Handle response
    if (httpResponseCode > 0)
    {
      String response = http.getString();
      Serial.println(httpResponseCode);
      Serial.println(response);
      // Update variable and save configuration
      DynamicJsonDocument res(1024);
      deserializeJson(res, response);
      strcpy(device_paired, "true");
      strcpy(device_id, res["data"]["id"]);

      saveConfig();
    }
    else
    {
      Serial.printf("> Error occurred while sending HTTP POST: %s\n",
                    http.errorToString(httpResponseCode).c_str());
    }
  }
  else
  {
    Serial.println("> WiFi Disconnected");
  }
}

// ---------------------------------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------------------------------
/*
  void publish(char* sensor_id, double temperature, double pressure) {

  // Create a measurement document
  DynamicJsonDocument doc(200);
  doc["sensor_id"] = sensor_id;
  doc["temperature"] = temperature;
  doc["pressure"] = pressure;

  String requestBody;
  serializeJson(doc, requestBody);

  // Publish message
  mqttClient.beginMessage(topic);
  mqttClient.print(requestBody);
  mqttClient.endMessage();
  }
*/
// ---------------------------------------------------------------------------------------------------
// WEB SERVER
// ---------------------------------------------------------------------------------------------------

// Reset a device by deleting is configuration file and restarting it
void handleReset()
{
  server.send(200, "text/plain", "Device reseted!\r\n");
}

void handleLed()
{
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  server.send(200, "text/plain", "ok\r\n");
}

void handleRoot()
{
  server.send(200, "text/plain", "Humidity & Temperature Sensor\r\n");
}

void handleData()
{
  server.send(200, "application/json", "{}\r\n");
}

void handleRequest()
{
  if (server.hasArg("plain") == false)
  { // Check if body received
    server.send(200, "text/plain", "Body not received");
    return;
  }
  String message = "Body received:\n";
  message += server.arg("plain");
  message += "\n";

  server.send(200, "text/plain", message);
  Serial.println(message);
}

void handleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

// ---------------------------------------------------------------------------------------------------
// MEASUREMENTS
// ---------------------------------------------------------------------------------------------------

void getData()
{
  Serial.println("--- Start Measurements...");
  char status;
  double T, P, p0, a;

  // Loop here getting pressure readings every 10 seconds.

  // If you want sea-level-compensated pressure, as used in weather reports,
  // you will need to know the altitude at which your measurements are taken.
  // We're using a constant called ALTITUDE in this sketch:

  // Serial.println();
  // Serial.print("provided altitude: ");
  // Serial.print(ALTITUDE, 0);
  // Serial.print(" meters, ");
  // Serial.print(ALTITUDE * 3.28084, 0);
  // Serial.println(" feet");

  // If you want to measure altitude, and not pressure, you will instead need
  // to provide a known baseline pressure. This is shown at the end of the sketch.

  // You must first get a temperature measurement to perform a pressure reading.

  // Start a temperature measurement:
  // If request is successful, the number of ms to wait is returned.
  // If request is unsuccessful, 0 is returned.

  status = pressure.startTemperature();
  if (status != 0)
  {
    // Wait for the measurement to complete:
    delay(status);

    // Retrieve the completed temperature measurement:
    // Note that the measurement is stored in the variable T.
    // Function returns 1 if successful, 0 if failure.

    status = pressure.getTemperature(T);
    if (status != 0)
    {
      // Print out the measurement:
      // Serial.print("temperature: ");
      // Serial.print(T, 2);
      // Serial.print(" deg C, ");
      // Serial.print((9.0 / 5.0) * T + 32.0, 2);
      // Serial.println(" deg F");

      // Start a pressure measurement:
      // The parameter is the oversampling setting, from 0 to 3 (highest res, longest wait).
      // If request is successful, the number of ms to wait is returned.
      // If request is unsuccessful, 0 is returned.

      status = pressure.startPressure(3);
      if (status != 0)
      {
        // Wait for the measurement to complete:
        delay(status);

        // Retrieve the completed pressure measurement:
        // Note that the measurement is stored in the variable P.
        // Note also that the function requires the previous temperature measurement (T).
        // (If temperature is stable, you can do one temperature measurement for a number of pressure measurements.)
        // Function returns 1 if successful, 0 if failure.

        status = pressure.getPressure(P, T);
        if (status != 0)
        {
          // Print out the measurement:
          // Serial.print("absolute pressure: ");
          // Serial.print(P, 2);
          // Serial.print(" mb, ");
          // Serial.print(P * 0.0295333727, 2);
          // Serial.println(" inHg");

          // The pressure sensor returns abolute pressure, which varies with altitude.
          // To remove the effects of altitude, use the sealevel function and your current altitude.
          // This number is commonly used in weather reports.
          // Parameters: P = absolute pressure in mb, ALTITUDE = current altitude in m.
          // Result: p0 = sea-level compensated pressure in mb

          p0 = pressure.sealevel(P, ALTITUDE); // we're at 1655 meters (Boulder, CO)
          // Serial.print("relative (sea-level) pressure: ");
          // Serial.print(p0, 2);
          // Serial.print(" mb, ");
          // Serial.print(p0 * 0.0295333727, 2);
          // Serial.println(" inHg");

          // On the other hand, if you want to determine your altitude from the pressure reading,
          // use the altitude function along with a baseline pressure (sea-level or other).
          // Parameters: P = absolute pressure in mb, p0 = baseline pressure in mb.
          // Result: a = altitude in m.

          a = pressure.altitude(P, p0);
          // Serial.print("computed altitude: ");
          // Serial.print(a, 0);
          // Serial.print(" meters, ");
          // Serial.print(a * 3.28084, 0);
          // Serial.println(" feet");
        }
        else
          Serial.println("> error retrieving pressure measurement\n");
      }
      else
        Serial.println("> error starting pressure measurement\n");
    }
    else
      Serial.println("> error retrieving temperature measurement\n");
  }
  else
    Serial.println("> error starting temperature measurement\n");
  post(T);
}

// ---------------------------------------------------------------------------------------------------
// SAVE CONFIG
// ---------------------------------------------------------------------------------------------------

void saveConfig()
{
  Serial.println("> Save Configuration");
  // Start save
  Serial.println("[-] Save configuration...");
#ifdef ARDUINOJSON_VERSION_MAJOR >= 6
  DynamicJsonDocument json(1024);
#else
  DynamicJsonBuffer jsonBuffer;
  JsonObject &json = jsonBuffer.createObject();
#endif

  // Create the configuration object
  json["mqtt_server"] = mqtt_server;
  json["mqtt_port"] = mqtt_port;
  json["api_token"] = api_token;
  json["api_url"] = api_url;
  json["device_paired"] = device_paired;
  json["device_id"] = device_id;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile)
  {
    Serial.println("failed to open config file for writing");
  }

#ifdef ARDUINOJSON_VERSION_MAJOR >= 6
  serializeJson(json, Serial);
  serializeJson(json, configFile);
#else
  json.printTo(Serial);
  json.printTo(configFile);
#endif
  configFile.close();
  // end save
}
