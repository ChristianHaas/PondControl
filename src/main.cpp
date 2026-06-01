#include <Arduino.h>
#include <basecomp.h>
#include <event.h>
#include <onewire.h>
#include <DallasTemperature.h>
#include <DS18B20.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WiFiUDP.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "commonstruct.h"
#include "udp_config.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <ArduinoOTA.h>
#include "ota_config.h"
#include <Logger.h>
#include "PondController.h"

// ── Sensor IDs (used in DS18B20 events to identify which sensor fired) ────────
#define SENSOR_ID_WATER 1
#define SENSOR_ID_AIR   2

// ── OLED ──────────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── DS18B20 – both sensors share one OneWire bus (pin 4) ─────────────────────
// Replace these addresses with the actual addresses found by a bus scan.
OneWire oneWire(4);
DallasTemperature sensors(&oneWire);
DeviceAddress addrWater = {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
DeviceAddress addrAir   = {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};

// ── Network ───────────────────────────────────────────────────────────────────
IPAddress webServerIP(192, 168, 68, ESP32_WebServer_IP);
WiFiUDP   udp;

// ── Objects ───────────────────────────────────────────────────────────────────
RemoteLogger   *logger;
PondController *pond;
EventBus       *eb;
DS18B20        *tempWater;
DS18B20        *tempAir;
bool            displayOk = false;

AsyncWebServer server(OTA_PORT);

static uint32_t messageCounter = 0;
struct_pond_message    pondMsg;
struct_pond_settings   incomingSettings;

// ── NTP ───────────────────────────────────────────────────────────────────────
const char* ntpServer      = "pool.ntp.org";
const long  gmtOffset_sec  = 3600;   // UTC+1 (CET)
const int   daylightOffset = 3600;   // +1 h DST

// ── UDP send ──────────────────────────────────────────────────────────────────
void sendPondStatus()
{
    pondMsg.msgType        = POND_STATUS;
    pondMsg.boardId        = ESP32_PondControl;
    pondMsg.waterTemp      = pond->getWaterTemp();
    pondMsg.airTemp        = pond->getAirTemp();
    pondMsg.feedAmount1    = pond->getFeedAmount1();
    pondMsg.feedAmount2    = pond->getFeedAmount2();
    strlcpy(pondMsg.feedTime1, pond->getFeedTime1(), sizeof(pondMsg.feedTime1));
    strlcpy(pondMsg.feedTime2, pond->getFeedTime2(), sizeof(pondMsg.feedTime2));
    pondMsg.timestamp      = millis();
    pondMsg.messageCounter = ++messageCounter;

    udp.beginPacket(webServerIP, ESP_UDP_PORT);
    udp.write((uint8_t *)&pondMsg, sizeof(pondMsg));
    if (!udp.endPacket())
        Serial.println("UDP send failed");
}

// ── WiFi watchdog ─────────────────────────────────────────────────────────────
void checkWiFiConnection()
{
    static unsigned long lastCheck = 0;
    static bool wasDisconnected = false;

    if (millis() - lastCheck < 10000) return;
    lastCheck = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
        if (!wasDisconnected)
        {
            wasDisconnected = true;
            Serial.println("WiFi disconnected — reconnecting...");
        }
        WiFi.reconnect();
    }
    else if (wasDisconnected)
    {
        wasDisconnected = false;
        udp.stop();
        udp.begin(ESP_UDP_PORT);
        configTime(gmtOffset_sec, daylightOffset, ntpServer);
        Serial.println("WiFi reconnected: " + WiFi.localIP().toString());
        if (logger) logger->log80("WiFi reconnected: " + WiFi.localIP().toString());
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);

    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);

    WiFi.mode(WIFI_STA);
    IPAddress local_IP(192, 168, 68, ESP32_PondControl_IP);
    IPAddress gateway(192, 168, 68, 1);
    IPAddress subnet(255, 255, 252, 0);

    if (!WiFi.config(local_IP, gateway, subnet))
        Serial.println("Static IP config failed, falling back to DHCP");

    WiFi.setHostname("PondController");
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.println("Connected! IP: " + WiFi.localIP().toString());
    WiFi.setSleep(false);

    configTime(gmtOffset_sec, daylightOffset, ntpServer);
    Serial.println("NTP sync requested from " + String(ntpServer));

    udp.begin(ESP_UDP_PORT);
    Serial.printf("UDP listening on port %d\n", ESP_UDP_PORT);

    // OTA
    if (OTA_ENABLED)
    {
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                  { request->send(200, "text/plain", "Hi! I am Pond Controller."); });
        server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request)
                  { request->send(200, "text/plain", "Resetting now..."); ESP.restart(); });
        ElegantOTA.setAuth(OTA_USERNAME, OTA_PASSWORD);
        ElegantOTA.begin(&server);
        ElegantOTA.onEnd([](bool success) {
            if (success) { delay(500); ESP.restart(); }
        });
        server.begin();
        Serial.println("HTTP server started");

        ArduinoOTA.setHostname(OTA_HOSTNAME);
        ArduinoOTA.setPassword(OTA_PASSWORD);
        ArduinoOTA.onStart([]() {
            Serial.println("OTA update starting...");
            esp_task_wdt_reset();   // prevent watchdog from firing at start
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            esp_task_wdt_reset();   // keep watchdog alive during flash write
            Serial.printf("OTA progress: %u%%\r", progress * 100 / total);
        });
        ArduinoOTA.onEnd([]()   { Serial.println("\nOTA update done."); });
        ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA error[%u]\n", error); });
        ArduinoOTA.begin();
        Serial.println("ArduinoOTA ready");
    }

    // OLED — non-fatal: device boots and OTA works even without display
    Wire.setTimeOut(3000);
    displayOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    if (!displayOk)
        Serial.println(F("SSD1306 not found — continuing without display"));

    // EventBus + components
    sensors.begin();
    eb = new EventBus();

    tempWater = new DS18B20("TempWater", &sensors, addrWater);
    tempWater->setId(SENSOR_ID_WATER);

    tempAir = new DS18B20("TempAir", &sensors, addrAir);
    tempAir->setId(SENSOR_ID_AIR);

    logger = new RemoteLogger(ESP32_PondControl, webServerIP, ESP_UDP_PORT);
    pond   = new PondController("PondCtrl", &display, displayOk);
    pond->attachLogger(logger);

    // All components on the EventBus — DS18B20 emits, PondController listens
    eb->attach(tempWater);
    eb->attach(tempAir);
    eb->attachListener(pond);

    logger->log80("Pond Controller started: " + WiFi.localIP().toString());
    Serial.println("Setup done");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop()
{
    esp_task_wdt_reset();

    if (OTA_ENABLED) ArduinoOTA.handle();

    checkWiFiConnection();

    // EventBus drives everything: DS18B20 samples async, emits events,
    // PondController receives them and calls action() at its own interval.
    eb->onLoop();

    // Incoming UDP packets (settings from web server)
    int packetSize = udp.parsePacket();
    if (packetSize > 0)
    {
        uint8_t buf[256];
        int len = udp.read(buf, sizeof(buf));
        if (len >= 1)
        {
            MessageType msgType = (MessageType)buf[0];
            if (msgType == POND_SETTINGS && len == (int)sizeof(struct_pond_settings))
            {
                memcpy(&incomingSettings, buf, sizeof(struct_pond_settings));
                pond->applySettings(incomingSettings.feedAmount1,
                                    incomingSettings.feedAmount2,
                                    incomingSettings.feedTime1,
                                    incomingSettings.feedTime2);
                logger->log80("Settings received via UDP");
                sendPondStatus();
            }
            else if (msgType == EVENT_MESSAGE && len == (int)sizeof(struct_event))
            {
                struct_event ev;
                memcpy(&ev, buf, sizeof(struct_event));
                eventstruct e = getEvent();
                e.eventCode = ev.eventCode;
                e.eventType = ev.eventType;
                e.val_int   = ev.val_int;
                e.val_float = ev.val_float;
                pond->handleEvent(e);
            }
        }
    }

    // Periodic status broadcast
    static unsigned long lastSendTime = 0;
    if (millis() - lastSendTime >= UDP_SEND_INTERVAL)
    {
        sendPondStatus();
        lastSendTime = millis();
    }
}
