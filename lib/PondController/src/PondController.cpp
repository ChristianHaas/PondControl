#include "PondController.h"
#include <commonstruct.h>

PondController::PondController(String name, Adafruit_SSD1306 *display, bool displayOk)
    : BaseComp(name), _display(display), _displayOk(displayOk)
{
    loadSettings();
    setInterval(0, 1000);  // call action() every second
}

// ── Settings persistence ──────────────────────────────────────────────────────

void PondController::loadSettings()
{
    _prefs.begin("pond", true);
    _feedAmount1 = _prefs.getInt("feed1", 0);
    _feedAmount2 = _prefs.getInt("feed2", 0);
    strlcpy(_feedTime1, _prefs.getString("time1", "08:00").c_str(), sizeof(_feedTime1));
    strlcpy(_feedTime2, _prefs.getString("time2", "18:00").c_str(), sizeof(_feedTime2));
    _prefs.end();
}

void PondController::saveSettings()
{
    _prefs.begin("pond", false);
    _prefs.putInt("feed1", _feedAmount1);
    _prefs.putInt("feed2", _feedAmount2);
    _prefs.putString("time1", _feedTime1);
    _prefs.putString("time2", _feedTime2);
    _prefs.end();
    log80("Settings saved: feed1=" + String(_feedAmount1) +
          "g feed2=" + String(_feedAmount2) +
          "g time1=" + String(_feedTime1) +
          " time2=" + String(_feedTime2));
}

void PondController::applySettings(int feed1, int feed2, const char* time1, const char* time2)
{
    _feedAmount1 = constrain(feed1, 0, 100);
    _feedAmount2 = constrain(feed2, 0, 100);
    strlcpy(_feedTime1, time1, sizeof(_feedTime1));
    strlcpy(_feedTime2, time2, sizeof(_feedTime2));
    saveSettings();  // persist and log once
}

void PondController::setFeedAmount1(int v)
{
    _feedAmount1 = constrain(v, 0, 100);
    saveSettings();
}

void PondController::setFeedAmount2(int v)
{
    _feedAmount2 = constrain(v, 0, 100);
    saveSettings();
}

void PondController::setFeedTime1(const char* t)
{
    strlcpy(_feedTime1, t, sizeof(_feedTime1));
    saveSettings();
}

void PondController::setFeedTime2(const char* t)
{
    strlcpy(_feedTime2, t, sizeof(_feedTime2));
    saveSettings();
}

// ── Event handling ────────────────────────────────────────────────────────────

void PondController::handleEvent(eventstruct e)
{
    // Temperature events from DS18B20 — sensor distinguished by e.val_int (ID)
    if (e.eventType == event_type_DS18B20)
    {
        if (e.eventCode == event_code_DS18B20_TEMP)
        {
            if (e.val_int == POND_SENSOR_ID_WATER)
                _waterTemp = e.val_float;
            else if (e.val_int == POND_SENSOR_ID_AIR)
                _airTemp = e.val_float;
        }
        return;
    }

    // Pond-specific events
    if (e.eventType == event_type_pond)
    {
        switch (e.eventCode)
        {
        case event_code_pond_update_settings:
            // settings applied via applySettings() from main.cpp
            break;
        }
    }
}

// ── Feeding time check ────────────────────────────────────────────────────────

void PondController::checkFeedingTime(struct tm &ti)
{
    // Build "HH:MM" string for current time
    char now[6];
    snprintf(now, sizeof(now), "%02d:%02d", ti.tm_hour, ti.tm_min);
    int doy = ti.tm_yday;   // day-of-year, resets to 0 on Jan 1

    if (strcmp(now, _feedTime1) == 0 && _feedAmount1 > 0 && doy != _lastFedDoy1)
    {
        _lastFedDoy1 = doy;
        log80("Feeding 1: " + String(_feedAmount1) + "g at " + String(now));
    }

    if (strcmp(now, _feedTime2) == 0 && _feedAmount2 > 0 && doy != _lastFedDoy2)
    {
        _lastFedDoy2 = doy;
        log80("Feeding 2: " + String(_feedAmount2) + "g at " + String(now));
    }
}

// ── Periodic action (every second) ───────────────────────────────────────────

void PondController::action()
{
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
        checkFeedingTime(timeinfo);

    displayStatus();
}

// ── OLED display ──────────────────────────────────────────────────────────────

void PondController::displayStatus()
{
    if (!_displayOk) return;
    struct tm timeinfo;
    bool timeValid = getLocalTime(&timeinfo);

    _display->clearDisplay();
    _display->setTextSize(1);
    _display->setTextColor(WHITE);
    _display->setCursor(0, 0);

    if (timeValid)
    {
        char buf[9];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        _display->println(buf);
    }
    else
    {
        _display->println("Time: --:--:--");
    }

    _display->setTextSize(2);
    _display->setCursor(0, 12);
    if (_waterTemp > -90)
        _display->println("W:" + String(_waterTemp, 1));
    else
        _display->println("W: ---");

    _display->setCursor(0, 32);
    if (_airTemp > -90)
        _display->println("L:" + String(_airTemp, 1));
    else
        _display->println("L: ---");

    _display->setTextSize(1);
    _display->setCursor(0, 52);
    _display->println(String(_feedTime1) + " " + String(_feedAmount1) + "g  " +
                      String(_feedTime2) + " " + String(_feedAmount2) + "g");
    _display->display();
}
