#include <Arduino.h>
#include <basecomp.h>
#include <event.h>
#include <onewire.h>
#include <DallasTemperature.h>
#include <DS18B20.h>
#include <SPI.h>
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

AsyncWebServer server(OTA_PORT);

static uint32_t messageCounter = 0;
struct_pond_message pondMsg;
struct_pond_settings incomingSettings;

// ── NTP ───────────────────────────────────────────────────────────────────────
const char* ntpServer       = "pool.ntp.org";
const long  gmtOffset_sec   = 3600;   // UTC+1 (CET); adjust for your timezone
const int   daylightOffset  = 3600;   // +1 h DST

// ── UDP send ──────────────────────────────────────────────────────────────────
void sendPondStatus()
{
    pondMsg.msgType       = POND_STATUS;
    pondMsg.boardId       = ESP32_PondControl;
    pondMsg.waterTemp     = pond->getWaterTemp();
    pondMsg.airTemp       = pond->getAirTemp();
    pondMsg.feedAmount1   = pond->getFeedAmount1();
    pondMsg.feedAmount2   = pond->getFeedAmount2();
    strlcpy(pondMsg.feedTime1, pond->getFeedTime1(), sizeof(pondMsg.feedTime1));
    strlcpy(pondMsg.feedTime2, pond->getFeedTime2(), sizeof(pondMsg.feedTime2));
    pondMsg.timestamp     = millis();
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
        // Re-sync NTP after reconnect
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

    // Sync time via NTP
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
        ArduinoOTA.onStart([]() { Serial.println("OTA update starting..."); });
        ArduinoOTA.onEnd([]()   { Serial.println("\nOTA update done."); });
        ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA error[%u]\n", error); });
        ArduinoOTA.begin();
        Serial.println("ArduinoOTA ready");
    }

    // DS18B20 — non-blocking async conversion
    sensors.begin();
    sensors.setWaitForConversion(false);  // do not block during requestTemperatures()
    eb = new EventBus();
    tempWater = new DS18B20("TempWater", &sensors, addrWater);
    tempAir   = new DS18B20("TempAir",   &sensors, addrAir);
    eb->attach(tempWater);
    eb->attach(tempAir);

    // OLED
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    {
        Serial.println(F("SSD1306 allocation failed"));
        for (;;);
    }
    Wire.setTimeOut(3000);

    // Logger + PondController
    logger = new RemoteLogger(ESP32_PondControl, webServerIP, ESP_UDP_PORT);
    pond   = new PondController("PondCtrl", &display);
    pond->attachLogger(logger);

    logger->log80("Pond Controller started: " + WiFi.localIP().toString());
    Serial.println("Setup done");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop()
{
    esp_task_wdt_reset();

    if (OTA_ENABLED) ArduinoOTA.handle();

    checkWiFiConnection();

    // DS18B20 non-blocking: request conversion every second,
    // then read result after ≥800 ms have passed.
    static unsigned long tempRequestTime = 0;
    static bool tempRequested = false;
    unsigned long now = millis();

    if (!tempRequested)
    {
        sensors.requestTemperatures();
        tempRequestTime = now;
        tempRequested = true;
    }
    else if (now - tempRequestTime >= 800)
    {
        float wt = sensors.getTempC(addrWater);
        float at = sensors.getTempC(addrAir);
        if (wt != DEVICE_DISCONNECTED_C) pond->setWaterTemp(wt);
        if (at != DEVICE_DISCONNECTED_C) pond->setAirTemp(at);
        tempRequested = false;  // trigger next request on next iteration
    }

    eb->onLoop();

    // Call PondController action (feeding time check, display update)
    pond->action();

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
                logger->log80("Event received, code: " + String(ev.eventCode));
                eventstruct e = getEvent();
                e.eventCode  = ev.eventCode;
                e.eventType  = ev.eventType;
                e.val_int    = ev.val_int;
                e.val_float  = ev.val_float;
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
