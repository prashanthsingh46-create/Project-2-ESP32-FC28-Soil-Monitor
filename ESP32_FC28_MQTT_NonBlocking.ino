/*
  ECOPROSUS - ESP32 #1
  FC-28 Soil Moisture + OLED + WiFi + NTP + MQTT + JSON
  Non-blocking WiFi and MQTT connection
  OLED displays WiFi and MQTT connection status
  Double-blink TX LED during transmission
  ACK LED blinks 3 times when Raspberry Pi acknowledges
  Data transmission every 30 seconds
  ----------------------------------------------------------
  FC-28:
    AO -> GPIO 34
    DO -> GPIO 25

  OLED:
    SDA -> GPIO 21
    SCL -> GPIO 22
    Address -> 0x3C

  LEDs:
    TX LED  -> GPIO 18
    ACK LED -> GPIO 23

  MQTT Broker:
    Raspberry Pi IP -> 172.16.0.132
    Port            -> 1883

  MQTT Topics:
    Sensor -> ecoprosus/esp32-01/soil/moisture
    ACK    -> ecoprosus/esp32-01/ack        (Pi must publish payload: ACK)
    Status -> ecoprosus/esp32-01/status

  FC-28 Calibration:
    Wet = 1967
    Dry = 4095

  ACK handshake:
    1. ESP32 publishes JSON to the sensor topic (TX LED double-blinks)
    2. Raspberry Pi / Node-RED publishes "ACK" to the ACK topic
    3. ESP32 receives it and blinks the ACK LED 3 times on GPIO 23

  Time handling:
    The system clock is kept as TRUE UTC (configTime offset = 0).
    IST (+5:30) is added manually only for Serial display, so the JSON
    "timestamp" (which ends in Z) is genuinely UTC.

  Library required: "ArduinoJson" by Benoit Blanchon (v6.x or v7.x)
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "time.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ======================================================
// OLED CONFIGURATION
// ======================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

// ======================================================
// WIFI CONFIGURATION
// ======================================================

const char* ssid = "Ecopro";
const char* password = "ecopro$123";

// ======================================================
// NTP CONFIGURATION
// ======================================================

const char* ntpServer = "pool.ntp.org";

// Used ONLY to shift the displayed time - never passed to configTime()
const long gmtOffset_sec_local = 19800;   // +5:30 (IST)

// ======================================================
// FC-28 CONFIGURATION
// ======================================================

#define AO_PIN 34
#define DO_PIN 25

#define WET_VALUE 1967
#define DRY_VALUE 4095

// ======================================================
// LED CONFIGURATION
// ======================================================

#define TX_LED_PIN 18
#define ACK_LED_PIN 23

// ======================================================
// DEVICE CONFIGURATION
// ======================================================

const char* DEVICE_ID = "ESP32_Node_01";

// ======================================================
// MQTT CONFIGURATION
// ======================================================

const char* mqtt_server = "172.16.0.132";
const int mqtt_port = 1883;

const char* mqtt_topic =
  "ecoprosus/esp32-01/soil/moisture";

const char* mqtt_status_topic =
  "ecoprosus/esp32-01/status";

const char* mqtt_ack_topic =
  "ecoprosus/esp32-01/ack";

WiFiClient espClient;
PubSubClient client(espClient);

// ======================================================
// TIMERS
// ======================================================

unsigned long lastMqttPublish = 0;
const unsigned long mqttInterval = 30000;

unsigned long lastMqttAttempt = 0;
const unsigned long mqttRetryInterval = 5000;

unsigned long lastWiFiAttempt = 0;
const unsigned long wifiRetryInterval = 5000;

unsigned long lastOLEDUpdate = 0;
const unsigned long oledUpdateInterval = 200;

// ======================================================
// STATUS VARIABLES
// ======================================================

String mqttDisplayStatus = "STARTING";
String wifiDisplayStatus = "STARTING";

bool ackReceived = false;

int latestAnalogValue = 0;
int latestDigitalValue = LOW;
float latestMoisture = 0.0;
String latestSoilStatus = "unknown";

// ======================================================
// OLED SCROLL
// ======================================================

int scrollX = -100;

// Lower this number to slow the scrolling down (e.g. 6 or 4)
const int scrollSpeed = 12;

// ======================================================
// TRANSMISSION LED (double blink)
// ======================================================

void blinkTXLED()
{
  digitalWrite(TX_LED_PIN, HIGH);
  delay(300);

  digitalWrite(TX_LED_PIN, LOW);
  delay(200);

  digitalWrite(TX_LED_PIN, HIGH);
  delay(300);

  digitalWrite(TX_LED_PIN, LOW);
}

// ======================================================
// ACK LED (3-times blink on GPIO 23)
// ======================================================

void blinkACKLED()
{
  for (int i = 0; i < 3; i++)
  {
    digitalWrite(ACK_LED_PIN, HIGH);
    delay(200);
    digitalWrite(ACK_LED_PIN, LOW);
    delay(200);
  }
}

// ======================================================
// MQTT CALLBACK - runs when a message arrives on a subscribed topic
// ======================================================

void mqttCallback(
  char* topic,
  byte* payload,
  unsigned int length
)
{
  String message;

  for (unsigned int i = 0; i < length; i++)
  {
    message += (char)payload[i];
  }

  Serial.println();
  Serial.println("--------------------------------------");
  Serial.print("MQTT message received on: ");
  Serial.println(topic);
  Serial.print("Message: ");
  Serial.println(message);

  // Tolerate stray whitespace / lower-case from the Pi side
  message.trim();
  message.toUpperCase();

  if (String(topic) == mqtt_ack_topic)
  {
    if (message == "ACK")
    {
      ackReceived = true;

      Serial.println("Raspberry Pi ACK received!");

      blinkACKLED();
    }
  }
}

// ======================================================
// CONNECT WIFI - NON-BLOCKING
// ======================================================

void connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiDisplayStatus = "CONNECTED";
    return;
  }

  unsigned long now = millis();

  if (now - lastWiFiAttempt < wifiRetryInterval)
  {
    wifiDisplayStatus = "OFFLINE";
    return;
  }

  lastWiFiAttempt = now;

  Serial.println("Trying to connect to Wi-Fi...");

  wifiDisplayStatus = "CONNECTING";

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
}

// ======================================================
// CONNECT MQTT - NON-BLOCKING
// ======================================================

void connectMQTT()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    mqttDisplayStatus = "NO WIFI";
    return;
  }

  if (client.connected())
  {
    mqttDisplayStatus = "CONNECTED";
    return;
  }

  unsigned long now = millis();

  if (now - lastMqttAttempt < mqttRetryInterval)
  {
    mqttDisplayStatus = "BROKER OFF";
    return;
  }

  lastMqttAttempt = now;

  Serial.print("Connecting to MQTT broker...");

  mqttDisplayStatus = "CONNECTING";

  String clientID =
    String("ecoprosus-esp32-01-") +
    String((uint32_t)ESP.getEfuseMac(), HEX);

  if (client.connect(
        clientID.c_str(),
        mqtt_status_topic,
        0,
        true,
        "offline"))
  {
    Serial.println("connected!");

    mqttDisplayStatus = "CONNECTED";

    client.publish(
      mqtt_status_topic,
      "online",
      true
    );

    // Listen for the Raspberry Pi acknowledgement
    client.subscribe(mqtt_ack_topic);

    Serial.print("Subscribed to: ");
    Serial.println(mqtt_ack_topic);
  }
  else
  {
    Serial.print("failed, state=");
    Serial.println(client.state());

    mqttDisplayStatus = "BROKER OFF";
  }
}

// ======================================================
// CALCULATE SOIL MOISTURE
// ======================================================

float calculateMoisture(int analogValue)
{
  float moisture =
    ((float)(DRY_VALUE - analogValue) /
     (DRY_VALUE - WET_VALUE)) * 100.0;

  moisture = constrain(
    moisture,
    0.0,
    100.0
  );

  return moisture;
}

// ======================================================
// TRUE UTC ISO TIMESTAMP (for JSON "timestamp" field)
// Non-blocking: uses time(nullptr), not getLocalTime()
// ======================================================

String getISOTimestamp()
{
  time_t now = time(nullptr);   // genuinely UTC, since the clock is never offset

  if (now < 100000)             // NTP not synced yet
  {
    return "NTP_SYNC_ERROR";
  }

  struct tm utcTime;
  gmtime_r(&now, &utcTime);

  char timestamp[30];

  strftime(
    timestamp,
    sizeof(timestamp),
    "%Y-%m-%dT%H:%M:%SZ",
    &utcTime
  );

  return String(timestamp);
}

// ======================================================
// LOCAL (IST) TIME FOR DISPLAY - manual +5:30 offset, non-blocking
// ======================================================

bool getIstTime(struct tm* localTimeinfo)
{
  time_t now = time(nullptr);

  if (now < 100000)
  {
    return false;
  }

  time_t localEpoch = now + gmtOffset_sec_local;
  gmtime_r(&localEpoch, localTimeinfo);

  return true;
}

// ======================================================
// UPDATE OLED
// ======================================================

void updateOLED()
{
  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);

  // Scrolling title
  display.setTextSize(1);
  display.setCursor(scrollX, 0);
  display.println("Ecoprosus");

  display.setTextSize(1);

  display.setCursor(0, 14);
  display.print("WiFi: ");
  display.println(wifiDisplayStatus);

  display.setCursor(0, 26);
  display.print("MQTT: ");
  display.println(mqttDisplayStatus);

  display.setCursor(0, 38);
  display.print("Moisture: ");
  display.print(latestMoisture, 1);
  display.println("%");

  display.setCursor(0, 50);
  display.print(latestSoilStatus);
  display.print("  ADC:");
  display.print(latestAnalogValue);

  display.display();
}

// ======================================================
// SETUP
// ======================================================

void setup()
{
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("        ECOPROSUS ESP32 #1");
  Serial.println(" FC-28 + OLED + WiFi + MQTT + JSON");
  Serial.println("======================================");

  // ----------------------------------------------------
  // LEDs
  // ----------------------------------------------------

  pinMode(TX_LED_PIN, OUTPUT);
  digitalWrite(TX_LED_PIN, LOW);

  pinMode(ACK_LED_PIN, OUTPUT);
  digitalWrite(ACK_LED_PIN, LOW);

  // ----------------------------------------------------
  // FC-28
  // ----------------------------------------------------

  pinMode(DO_PIN, INPUT);

  // ----------------------------------------------------
  // OLED
  // ----------------------------------------------------

  Wire.begin(21, 22);

  if (!display.begin(
        SSD1306_SWITCHCAPVCC,
        OLED_ADDRESS))
  {
    Serial.println("SSD1306 allocation failed");

    while (true)
    {
      delay(1000);
    }
  }

  Serial.println("OLED initialized!");

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 20);
  display.println("Ecoprosus");
  display.display();

  delay(1000);

  // ----------------------------------------------------
  // Wi-Fi
  // ----------------------------------------------------

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  lastWiFiAttempt = millis();

  Serial.println("Wi-Fi connection started.");

  // ----------------------------------------------------
  // NTP - system clock kept as TRUE UTC (offset 0)
  // ----------------------------------------------------

  configTime(
    0,
    0,
    ntpServer
  );

  Serial.println("NTP synchronization started.");

  // ----------------------------------------------------
  // MQTT
  // ----------------------------------------------------

  client.setServer(
    mqtt_server,
    mqtt_port
  );

  client.setCallback(mqttCallback);

  // Larger buffer so longer JSON payloads never fail silently
  client.setBufferSize(512);

  // Allow immediate first MQTT attempt
  lastMqttAttempt = millis() - mqttRetryInterval;

  Serial.println("MQTT configured.");

  Serial.println();
  Serial.println("System ready!");
  Serial.println("--------------------------------------");
}

// ======================================================
// LOOP
// ======================================================

void loop()
{
  // ====================================================
  // CHECK WIFI
  // ====================================================

  if (WiFi.status() == WL_CONNECTED)
  {
    if (wifiDisplayStatus != "CONNECTED")
    {
      Serial.println("Wi-Fi connected!");

      Serial.print("ESP32 IP: ");
      Serial.println(WiFi.localIP());

      Serial.print("Wi-Fi RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
    }

    wifiDisplayStatus = "CONNECTED";
  }
  else
  {
    wifiDisplayStatus = "OFFLINE";
    connectWiFi();
  }

  // ====================================================
  // CHECK MQTT (client.loop() also delivers incoming ACKs)
  // ====================================================

  if (WiFi.status() == WL_CONNECTED)
  {
    if (!client.connected())
    {
      connectMQTT();
    }
    else
    {
      mqttDisplayStatus = "CONNECTED";
    }

    client.loop();
  }
  else
  {
    mqttDisplayStatus = "NO WIFI";
  }

  // ====================================================
  // READ FC-28
  // ====================================================

  latestAnalogValue = analogRead(AO_PIN);
  latestDigitalValue = digitalRead(DO_PIN);

  float voltage =
    (latestAnalogValue / 4095.0) * 3.3;

  latestMoisture =
    calculateMoisture(latestAnalogValue);

  if (latestDigitalValue == HIGH)
  {
    latestSoilStatus = "dry";
  }
  else
  {
    latestSoilStatus = "wet";
  }

  // ====================================================
  // SERIAL MONITOR (every 1 second, shows IST)
  // ====================================================

  static unsigned long lastSerialPrint = 0;

  if (millis() - lastSerialPrint >= 1000)
  {
    lastSerialPrint = millis();

    struct tm timeinfo;
    char dateTimeBuffer[30];

    if (getIstTime(&timeinfo))
    {
      strftime(
        dateTimeBuffer,
        sizeof(dateTimeBuffer),
        "%d-%m-%Y %H:%M:%S",
        &timeinfo
      );
    }
    else
    {
      strcpy(
        dateTimeBuffer,
        "NTP Sync..."
      );
    }

    Serial.println();
    Serial.println("--------------------------------------");
    Serial.print("Ecoprosus | ");
    Serial.println(dateTimeBuffer);

    Serial.print("Wi-Fi      : ");
    Serial.println(wifiDisplayStatus);

    Serial.print("MQTT       : ");
    Serial.println(mqttDisplayStatus);

    Serial.print("Analog     : ");
    Serial.println(latestAnalogValue);

    Serial.print("Voltage    : ");
    Serial.print(voltage, 2);
    Serial.println(" V");

    Serial.print("Moisture   : ");
    Serial.print(latestMoisture, 1);
    Serial.println(" %");

    Serial.print("Soil       : ");
    Serial.println(latestSoilStatus);
  }

  // ====================================================
  // MQTT PUBLISH EVERY 30 SECONDS
  // ====================================================

  if (millis() - lastMqttPublish >= mqttInterval)
  {
    lastMqttPublish = millis();

    if (client.connected())
    {
      Serial.println();
      Serial.println(">>> TRANSMISSION START");
      Serial.println(
        "TX LED: ON 0.3s -> OFF 0.2s -> ON 0.3s -> OFF"
      );

      blinkTXLED();

      // ------------------------------------------------
      // CREATE JSON
      // ------------------------------------------------

      JsonDocument doc;

      doc["device_id"] = DEVICE_ID;
      doc["timestamp"] = getISOTimestamp();

      JsonObject readings =
        doc["readings"].to<JsonObject>();

      readings["soil_moisture_pct"] =
        round(latestMoisture * 10) / 10.0;

      readings["soil_moisture_raw"] =
        latestAnalogValue;

      JsonObject status =
        doc["status"].to<JsonObject>();

      status["sensor"] =
        latestSoilStatus;

      status["reading_valid"] =
        true;

      // ------------------------------------------------
      // SERIALIZE JSON
      // ------------------------------------------------

      char payload[300];

      size_t len =
        serializeJson(
          doc,
          payload,
          sizeof(payload)
        );

      // ------------------------------------------------
      // PUBLISH JSON
      // ------------------------------------------------

      bool published =
        client.publish(
          mqtt_topic,
          payload,
          len
        );

      if (published)
      {
        Serial.println("MQTT published successfully:");
        Serial.println(payload);

        Serial.print("Topic: ");
        Serial.println(mqtt_topic);

        Serial.println(
          "Waiting for Raspberry Pi ACK..."
        );

        ackReceived = false;

        // Actively check the network packet queue for 2 seconds to capture Node-RED ACK instantly
        unsigned long startWait = millis();
        while (millis() - startWait < 2000)
        {
          client.loop();
          if (ackReceived)
          {
            break;
          }
          delay(10);
        }
      }
      else
      {
        Serial.println("MQTT publish failed!");
        mqttDisplayStatus = "PUBLISH FAIL";
      }
    }
    else
    {
      Serial.println(
        "MQTT unavailable. Reading not published."
      );
    }
  }

  // ====================================================
  // OLED UPDATE
  // ====================================================

  if (millis() - lastOLEDUpdate >= oledUpdateInterval)
  {
    lastOLEDUpdate = millis();

    updateOLED();

    scrollX += scrollSpeed;

    if (scrollX > SCREEN_WIDTH)
    {
      scrollX = -110;
    }
  }

  delay(20);
}