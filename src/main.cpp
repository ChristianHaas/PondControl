#include <Arduino.h>
#include <basecomp.h>
#include <event.h>
#include <digitalOut.h>
#include <onewire.h>
#include <DallasTemperature.h>
#include <DS18B20.h>
#include <WiFi.h>
#include <WiFiUDP.h>
#include <esp_task_wdt.h>
#include "rtc_wdt.h"     // IDF 5.x path (was soc/rtc_wdt.h on IDF 4.x)
#include <time.h>
#include <esp_sntp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "commonstruct.h"
#include "udp_config.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <ArduinoOTA.h>
#include "ota_config.h"
#include <Logger.h>
#include "PondController.h"

// ── Independent RTC hardware watchdog ─────────────────────────────────────────
// The RTC WDT is a silicon-level watchdog in the always-on RTC domain. It resets
// the chip by hardware even if the CPU is fully wedged or interrupts are disabled
// — the most robust recovery available. loop() must call feedHwWatchdog() at
// least every 15 s (also fed during OTA) or the device hard-resets.
#define RTC_WDT_TIMEOUT_MS 15000

void initHwWatchdog()
{
    rtc_wdt_protect_off();
    rtc_wdt_disable();
    rtc_wdt_set_length_of_reset_signal(RTC_WDT_SYS_RESET_SIG, RTC_WDT_LENGTH_3_2us);
    rtc_wdt_set_stage(RTC_WDT_STAGE0, RTC_WDT_STAGE_ACTION_RESET_SYSTEM);
    rtc_wdt_set_time(RTC_WDT_STAGE0, RTC_WDT_TIMEOUT_MS);
    rtc_wdt_enable();
    rtc_wdt_protect_on();
}

inline void feedHwWatchdog() { rtc_wdt_feed(); }

// Pause/resume the RTC WDT around OTA so a slow/stalled upload can never reset
// the chip mid-flash. On a successful OTA the device reboots (WDT re-inits in
// setup); resume is only needed if the OTA fails.
void pauseHwWatchdog()  { rtc_wdt_protect_off(); rtc_wdt_disable(); rtc_wdt_protect_on(); }
void resumeHwWatchdog() { initHwWatchdog(); }

// ── GPIO pin assignments ───────────────────────────────────────────────────────
#define PIN_RELAY_PUMP1    25   // Relay Pump1  (invers: LOW = relay ON)
#define PIN_RELAY_PUMP2    26   // Relay Pump2
#define PIN_RELAY_FEEDER   27   // Relay Feeder
#define PIN_RELAY_AIRPUMP  18   // Relay AirPump

// ── Sensor IDs (used in DS18B20 events to identify which sensor fired) ────────
#define SENSOR_ID_WATER 1
#define SENSOR_ID_AIR   2

// ── DS18B20 – both sensors share one OneWire bus (pin 4) ─────────────────────
// Replace these addresses with the actual addresses found by a bus scan.
OneWire oneWire(4);
DallasTemperature sensors(&oneWire);
DeviceAddress addrWater = {0x28, 0xFF, 0xFA, 0x87, 0x60, 0x17, 0x05, 0x14};
DeviceAddress addrAir   = {0x28, 0x78, 0xAC, 0xAE, 0x63, 0x20, 0x01, 0x57};

// ── Network ───────────────────────────────────────────────────────────────────
IPAddress webServerIP(192, 168, 68, ESP32_WebServer_IP);
WiFiUDP   udp;

// ── Objects ───────────────────────────────────────────────────────────────────
RemoteLogger   *logger;
PondController *pond;
EventBus       *eb;
DS18B20        *tempWater;
DS18B20        *tempAir;
DigitalOut     *relayPump1;
DigitalOut     *relayPump2;
DigitalOut     *relayFeeder;
DigitalOut     *relayAirPump;

AsyncWebServer server(OTA_PORT);

static uint32_t messageCounter = 0;
struct_pond_message    pondMsg;
struct_pond_settings   incomingSettings;

// ── Loop-Timing-Statistik (24-h-Fenster) ─────────────────────────────────────
static unsigned long loopLastUs     = 0;   // micros() beim letzten loop()-Aufruf
static uint64_t      loopSumUs      = 0;   // Summe aller Dauern im aktuellen Fenster
static uint32_t      loopCount      = 0;   // Anzahl gemessener Intervalle
static uint32_t      loopMaxUs      = 0;   // Maximum im aktuellen Fenster
static unsigned long loopWindowStart = 0;  // millis() beim letzten Reset

void updateLoopStats()
{
    unsigned long now = micros();
    if (loopLastUs == 0) { loopLastUs = now; loopWindowStart = millis(); return; }
    uint32_t dt = (uint32_t)(now - loopLastUs);
    loopLastUs = now;

    if (millis() - loopWindowStart >= 86400000UL) {  // 24 h abgelaufen → Fenster neu
        loopSumUs = 0; loopCount = 0; loopMaxUs = 0;
        loopWindowStart = millis();
    }
    loopSumUs += dt;
    loopCount++;
    if (dt > loopMaxUs) loopMaxUs = dt;
}

String buildStatusPage()
{
    float meanUs = (loopCount > 0) ? (float)loopSumUs / loopCount : 0.0f;

    unsigned long uptimeSec = millis() / 1000;
    unsigned long h = uptimeSec / 3600;
    unsigned long m = (uptimeSec % 3600) / 60;
    unsigned long s = uptimeSec % 60;

    char timeBuf[20] = "--:--:--";
    struct tm ti;
    if (getLocalTime(&ti, 0))
        strftime(timeBuf, sizeof(timeBuf), "%d.%m.%Y %H:%M:%S", &ti);

    String html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta http-equiv='refresh' content='5'>"
        "<title>PondControl</title>"
        "<style>"
        "body{font-family:monospace;margin:20px;background:#f8f8f8;}"
        "h2{color:#224466;} h3{color:#336;margin-top:1.2em;}"
        "table{border-collapse:collapse;min-width:320px;}"
        "td{padding:3px 14px;border-bottom:1px solid #ddd;}"
        "td:first-child{color:#555;}"
        ".on{color:green;font-weight:bold;} .off{color:#aaa;}"
        "</style></head><body>"
        "<h2>PondControl</h2>"
        "<p>Build: " + String(__DATE__) + " " + String(__TIME__) + "<br>"
        "Zeit: " + String(timeBuf) + "<br>"
        "Uptime: " + String(h) + "h " + String(m) + "m " + String(s) + "s<br>"
        "WiFi: " + WiFi.localIP().toString() + " &nbsp; RSSI: " + String(WiFi.RSSI()) + " dBm</p>"

        "<h3>Temperaturen</h3><table>"
        "<tr><td>Wasser</td><td>" + String(pond ? pond->getWaterTemp() : -99.0f, 1) + " °C</td></tr>"
        "<tr><td>Luft</td><td>"   + String(pond ? pond->getAirTemp()   : -99.0f, 1) + " °C</td></tr>"
        "</table>"

        "<h3>Relais</h3><table>"
        "<tr><td>Pumpe 1 (Skimmer)</td><td class='" + String(pond && pond->isPump1On()  ? "on'>AN" : "off'>AUS") + "</td></tr>"
        "<tr><td>Pumpe 2</td><td class='"            + String(pond && pond->isPump2On()  ? "on'>AN" : "off'>AUS") + "</td></tr>"
        "<tr><td>Futterautomat</td><td class='"      + String(pond && pond->isFeederOn() ? "on'>AN" : "off'>AUS") + "</td></tr>"
        "<tr><td>Luftpumpe</td><td class='"          + String(pond && pond->isAirPumpOn()? "on'>AN" : "off'>AUS") + "</td></tr>"
        "</table>"

        "<h3>Fütterung</h3><table>"
        "<tr><td>Menge 1 / Zeit</td><td>" + String(pond ? pond->getFeedAmount1() : 0) + " g &nbsp; " + String(pond ? pond->getFeedTime1() : "--:--") + "</td></tr>"
        "<tr><td>Menge 2 / Zeit</td><td>" + String(pond ? pond->getFeedAmount2() : 0) + " g &nbsp; " + String(pond ? pond->getFeedTime2() : "--:--") + "</td></tr>"
        "<tr><td>Pumpe-Pause</td><td>"    + String(pond ? pond->getPumpOffMinutes() : 0) + " min</td></tr>"
        "</table>"

        "<h3>Loop-Statistik (" + String((millis() - loopWindowStart) / 60000UL) + " min Fenster, max 24 h)</h3><table>"
        "<tr><td>Mittlere Dauer</td><td>" + String(meanUs, 0)        + " µs (" + String(meanUs / 1000.0f, 2) + " ms)</td></tr>"
        "<tr><td>Maximale Dauer</td><td>" + String(loopMaxUs)        + " µs (" + String(loopMaxUs / 1000.0f, 2) + " ms)</td></tr>"
        "<tr><td>Messungen</td><td>"      + String(loopCount)        + "</td></tr>"
        "</table>"

        "<h3>Kommunikation</h3><table>"
        "<tr><td>Gesendete Pakete</td><td>" + String(messageCounter) + "</td></tr>"
        "</table>"

        "<p style='margin-top:2em'><a href='/update'>OTA Update</a></p>"
        "</body></html>";
    return html;
}

// ── NTP ───────────────────────────────────────────────────────────────────────
// Timezone: CET (UTC+1) with automatic DST to CEST (UTC+2)
// POSIX TZ string covers both winter and summer time
#define TZ_STRING "CET-1CEST,M3.5.0,M10.5.0/3"

const char* ntpServer      = "192.168.68.1";    // router NTP (kept as fallback)
const char* ntpServer2     = "de.pool.ntp.org";
const long  gmtOffset_sec  = 3600;
const int   daylightOffset = 3600;

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
    pondMsg.pump1State      = pond->isPump1On();
    pondMsg.pump2State      = pond->isPump2On();
    pondMsg.feederState     = pond->isFeederOn();
    pondMsg.airPumpState    = pond->isAirPumpOn();
    pondMsg.pumpOffMinutes  = pond->getPumpOffMinutes();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0))  // timeout=0: non-blocking, never stalls the loop
        snprintf(pondMsg.currentTime, sizeof(pondMsg.currentTime), "%02d:%02d:%02d",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    else
        strlcpy(pondMsg.currentTime, "--:--:--", sizeof(pondMsg.currentTime));
    pondMsg.timestamp      = millis();
    pondMsg.messageCounter = ++messageCounter;

    udp.beginPacket(webServerIP, ESP_UDP_PORT);
    udp.write((uint8_t *)&pondMsg, sizeof(pondMsg));
    if (!udp.endPacket())
        Serial.println("UDP send failed");
}

// Forward declaration
bool syncTimeFromWebServer();

// ── WiFi watchdog ─────────────────────────────────────────────────────────────
// Faellt das Netz aus, wird NICHT neu gestartet: der Controller arbeitet weiter
// und sucht im Hintergrund die Verbindung. (Der frueher hier stehende Reboot nach
// 2 min stammte aus einer Absturzserie, deren Ursache Schaltspitzen der Relais
// waren - die sind seit dem RC-Glied an den Relais weg. Bei einem echten
// Netzausfall schadet ein Reboot nur: er loescht den Betriebszustand, obwohl die
// Steuerung ohne Netz einwandfrei weiterlaufen koennte.)
//
// Gegen den Fall, dass nicht das Netz weg ist, sondern der WLAN-Stack des ESP
// haengt (WiFi.reconnect() kehrt dann nie in den verbundenen Zustand zurueck),
// hilft die Eskalationsstufe: alle 10 erfolglosen Versuche wird der Stack
// komplett aus- und wieder eingeschaltet - dieselbe Wirkung wie ein Reboot, aber
// ohne Verlust des Betriebszustands.
#define WIFI_RETRY_FAST_MS     10000UL    // erste Minute: alle 10 s
#define WIFI_RETRY_SLOW_MS     60000UL    // danach: jede Minute
#define WIFI_STACK_RESET_EVERY 10         // nach so vielen Versuchen Stack neu aufsetzen

void checkWiFiConnection()
{
    static unsigned long lastCheck         = 0;
    static unsigned long disconnectedSince = 0;
    static unsigned long lastAttempt       = 0;
    static uint32_t      attempts          = 0;
    static bool          wasDisconnected   = false;

    if (millis() - lastCheck < 10000) return;
    lastCheck = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
        if (!wasDisconnected)
        {
            wasDisconnected   = true;
            disconnectedSince = millis();
            lastAttempt       = 0;
            attempts          = 0;
            Serial.println("WiFi weg - Reconnect laeuft (kein Reboot)");
            if (logger) logger->log80("WiFi weg - Reconnect laeuft (kein Reboot)");
        }

        unsigned long warte = (millis() - disconnectedSince < 60000UL)
                            ? WIFI_RETRY_FAST_MS : WIFI_RETRY_SLOW_MS;
        if (lastAttempt && millis() - lastAttempt < warte) return;
        lastAttempt = millis();
        attempts++;

        if (attempts % WIFI_STACK_RESET_EVERY == 0)
        {
            Serial.println("WiFi-Stack neu aufsetzen (Versuch " + String(attempts) + ")");
            if (logger) logger->log80("WiFi-Stack neu aufsetzen (Versuch " + String(attempts) + ")");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            delay(200);
            WiFi.mode(WIFI_STA);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
        else
        {
            WiFi.disconnect(false);
            WiFi.reconnect();
        }
    }
    else if (wasDisconnected)
    {
        wasDisconnected = false;
        udp.stop();
        udp.begin(ESP_UDP_PORT);
        syncTimeFromWebServer();
        String meldung = "WiFi wieder da nach " + String((millis() - disconnectedSince) / 1000)
                       + " s: " + WiFi.localIP().toString();
        Serial.println(meldung);
        if (logger) logger->log80(meldung);
    }
}

// ── Time sync from HomeWebServerV2 ────────────────────────────────────────────
bool syncTimeFromWebServer()
{
    HTTPClient http;
    http.begin("http://192.168.68.200/api/time");
    http.setTimeout(3000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("Time sync HTTP error: %d\n", code);
        http.end();
        return false;
    }
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, http.getString()) != DeserializationError::Ok) {
        Serial.println("Time sync JSON parse error");
        http.end();
        return false;
    }
    http.end();

    long epoch = doc["epoch"] | 0L;
    if (epoch < 1000000000L) {
        Serial.println("Time sync: invalid epoch from server");
        return false;
    }

    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, nullptr);

    struct tm ti;
    getLocalTime(&ti, 0);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
    Serial.println("Time synced from web server: " + String(buf));
    if (logger) logger->log80("Time synced: " + String(buf));
    return true;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.printf("\n=== PondControl build %s %s ===\n", __DATE__, __TIME__);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    // Watch the idle task of every core present (0b1 single-core, 0b11 dual-core)
    // idle_core_mask = 0: do NOT watch idle tasks. Arduino loop() busy-runs and
    // never yields to IDLE1, which would falsely trip the watchdog. We watch the
    // loop task itself via esp_task_wdt_add(NULL) below — that catches real hangs.
    esp_task_wdt_config_t twdtConfig = {
        .timeout_ms     = 30000,
        .idle_core_mask = 0,
        .trigger_panic  = true };
    esp_task_wdt_reconfigure(&twdtConfig);   // TWDT already inited by core 3.x
#else
    esp_task_wdt_init(30, true);
#endif
    esp_task_wdt_add(NULL);   // subscribe the loop task (required before esp_task_wdt_reset)

    // Independent timer-based watchdog — fires even if main task is blocked
    initHwWatchdog();

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

    // configTime() must come first — it internally resets TZ based on its offset args
    configTime(0, 0, ntpServer, ntpServer2);
    // Set correct timezone AFTER configTime() so it isn't overwritten
    setenv("TZ", TZ_STRING, 1);
    tzset();
    // Sync time from HomeWebServerV2 (local HTTP — no NTP/DNS/port-123 needed)
    syncTimeFromWebServer();

    udp.begin(ESP_UDP_PORT);
    Serial.printf("UDP listening on port %d\n", ESP_UDP_PORT);

    // OTA
    if (OTA_ENABLED)
    {
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                  { request->send(200, "text/html", buildStatusPage()); });
        server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request)
                  { request->send(200, "text/plain", "Resetting now..."); ESP.restart(); });
        ElegantOTA.setAuth(OTA_USERNAME, OTA_PASSWORD);
        ElegantOTA.begin(&server);
        ElegantOTA.onStart([]() { pauseHwWatchdog(); });
        ElegantOTA.onEnd([](bool success) {
            if (success) { delay(500); ESP.restart(); }
            else         { resumeHwWatchdog(); }   // failed — re-arm watchdog
        });
        server.begin();
        Serial.println("HTTP server started");

        ArduinoOTA.setHostname(OTA_HOSTNAME);
        ArduinoOTA.setPassword(OTA_PASSWORD);
        ArduinoOTA.onStart([]() {
            Serial.println("OTA update starting...");
            pauseHwWatchdog();        // RTC WDT off for the whole upload
            esp_task_wdt_reset();
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            esp_task_wdt_reset();     // keep the 30s task WDT happy while loop() is blocked
            Serial.printf("OTA progress: %u%%\r", progress * 100 / total);
        });
        ArduinoOTA.onEnd([]()   { Serial.println("\nOTA update done."); });
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("OTA error[%u]\n", error);
            resumeHwWatchdog();   // failed — re-arm watchdog
        });
        ArduinoOTA.begin();
        Serial.println("ArduinoOTA ready");
    }

    // EventBus + components
    sensors.begin();
    eb = new EventBus();
    logger = new RemoteLogger(ESP32_PondControl, webServerIP, ESP_UDP_PORT);

    // Scan the OneWire bus and report found DS18B20 addresses to serial AND the
    // web UI logs, so the real address can be copied into addrWater/addrAir.
    int deviceCount = sensors.getDeviceCount();
    Serial.printf("DS18B20 devices found: %d\n", deviceCount);
    logger->log80("DS18B20 devices found: " + String(deviceCount));
    for (int i = 0; i < deviceCount; i++) {
        DeviceAddress addr;
        if (sensors.getAddress(addr, i)) {
            char hex[64];
            snprintf(hex, sizeof(hex),
                "S%d 0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X",
                i, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
            Serial.println(hex);
            logger->log80(String(hex));
        }
    }

    tempWater = new DS18B20("TempWater", &sensors, addrWater);
    tempWater->setId(SENSOR_ID_WATER);

    tempAir = new DS18B20("TempAir", &sensors, addrAir);
    tempAir->setId(SENSOR_ID_AIR);

    // Relays: invers=true (active LOW — standard relay module wiring)
    relayPump1   = new DigitalOut("Pump1",    PIN_RELAY_PUMP1,   false, true);
    relayPump2   = new DigitalOut("Pump2",    PIN_RELAY_PUMP2,   false, true);
    relayFeeder  = new DigitalOut("Feeder",   PIN_RELAY_FEEDER,  false, true);
    relayAirPump = new DigitalOut("AirPump",  PIN_RELAY_AIRPUMP, false, true);

    pond   = new PondController("PondCtrl", relayPump1, relayPump2, relayFeeder, relayAirPump);
    pond->attachLogger(logger);

    // All components on the EventBus
    // DS18B20: emits temperature events
    // Relays: need onLoop() called so setOnInMillis() timer works
    // PondController: listens for events and calls action() every second
    eb->attach(tempWater);
    eb->attach(tempAir);
    eb->attach(relayPump1);
    eb->attach(relayPump2);
    eb->attach(relayFeeder);
    eb->attach(relayAirPump);
    eb->attachListener(pond);

    logger->log80("Pond Controller started: " + WiFi.localIP().toString());
    logger->log80("Build " + String(__DATE__) + " " + String(__TIME__));
    Serial.println("Setup done");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop()
{
    updateLoopStats();  // muss ganz am Anfang stehen — misst die Zeit seit dem letzten Aufruf
    esp_task_wdt_reset();
    feedHwWatchdog();   // independent of TWDT — restarts if loop stops for 15 s

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
                                    incomingSettings.feedTime2,
                                    incomingSettings.pumpOffMinutes);
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

    // Periodic time re-sync from web server (every hour, or until first successful sync)
    static unsigned long lastTimeSync = 0;
    static bool timeSynced = false;
    unsigned long syncInterval = timeSynced ? 3600000UL : 30000UL; // retry every 30s until synced
    if (millis() - lastTimeSync >= syncInterval) {
        if (syncTimeFromWebServer()) timeSynced = true;
        lastTimeSync = millis();
    }

    // Periodic status broadcast
    static unsigned long lastSendTime = 0;
    if (millis() - lastSendTime >= UDP_SEND_INTERVAL)
    {
        sendPondStatus();
        lastSendTime = millis();
    }
}
