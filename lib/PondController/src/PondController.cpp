#include "PondController.h"
#include <commonstruct.h>

PondController::PondController(String name, DigitalOut *relayPump1, DigitalOut *relayPump2, DigitalOut *relayFeeder)
    : BaseComp(name), _relayPump1(relayPump1), _relayPump2(relayPump2), _relayFeeder(relayFeeder)
{
    loadSettings();
    setInterval(0, 1000);  // call action() every second via EventBus

    // Normal state: Pump1 on, Pump2 off, feeder off
    _relayPump1->setOutput(true);
    _relayPump2->setOutput(false);
    _relayFeeder->setOutput(false);
}

// ── Settings persistence ──────────────────────────────────────────────────────

void PondController::loadSettings()
{
    _prefs.begin("pond", true);
    _feedAmount1    = _prefs.getInt("feed1", 0);
    _feedAmount2    = _prefs.getInt("feed2", 0);
    _pumpOffMinutes = _prefs.getInt("pumpOff", 300);
    strlcpy(_feedTime1, _prefs.getString("time1", "08:00").c_str(), sizeof(_feedTime1));
    strlcpy(_feedTime2, _prefs.getString("time2", "18:00").c_str(), sizeof(_feedTime2));
    _prefs.end();
}

void PondController::saveSettings()
{
    _prefs.begin("pond", false);
    _prefs.putInt("feed1",   _feedAmount1);
    _prefs.putInt("feed2",   _feedAmount2);
    _prefs.putInt("pumpOff", _pumpOffMinutes);
    _prefs.putString("time1", _feedTime1);
    _prefs.putString("time2", _feedTime2);
    _prefs.end();
    log80("Settings saved: feed1=" + String(_feedAmount1) +
          "g feed2=" + String(_feedAmount2) +
          "g time1=" + String(_feedTime1) +
          " time2=" + String(_feedTime2) +
          " pumpOff=" + String(_pumpOffMinutes) + "min");
}

void PondController::applySettings(int feed1, int feed2, const char* time1, const char* time2, int pumpOffMinutes)
{
    _feedAmount1    = constrain(feed1, 0, 100);
    _feedAmount2    = constrain(feed2, 0, 100);
    _pumpOffMinutes = constrain(pumpOffMinutes, 0, 600);
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

// ── Feeding ───────────────────────────────────────────────────────────────────

void PondController::feedNow(int amount)
{
    if (amount <= 0) return;

    // Pump1 off, Pump2 on for 5 hours during feeding
    _relayPump1->setOutput(false);
    _relayPump2->setOutput(true);
    _pumpRestoreTime = millis() + (unsigned long)_pumpOffMinutes * 60UL * 1000UL;

    // Feeder on for amount seconds (1g = 1s), auto-off via DigitalOut timer
    _relayFeeder->setOnInMillis(amount * 1000);

    log80("Feeding: " + String(amount) + "g, Pump1 off for " + String(_pumpOffMinutes) + "min");
}

// ── Feeding time check ────────────────────────────────────────────────────────

void PondController::checkFeedingTime(struct tm &ti)
{
    char now[6];
    snprintf(now, sizeof(now), "%02d:%02d", ti.tm_hour, ti.tm_min);
    int doy = ti.tm_yday;

    if (strcmp(now, _feedTime1) == 0 && _feedAmount1 > 0 && doy != _lastFedDoy1)
    {
        _lastFedDoy1 = doy;
        feedNow(_feedAmount1);
    }

    if (strcmp(now, _feedTime2) == 0 && _feedAmount2 > 0 && doy != _lastFedDoy2)
    {
        _lastFedDoy2 = doy;
        feedNow(_feedAmount2);
    }
}

// ── Periodic action (every second via EventBus) ───────────────────────────────

void PondController::action()
{
    // Restore normal pump state after 5-hour feeding pause
    if (_pumpRestoreTime > 0 && millis() >= _pumpRestoreTime)
    {
        _relayPump2->setOutput(false);
        _relayPump1->setOutput(true);
        _pumpRestoreTime = 0;
        log80("Pumps restored: Pump1 on, Pump2 off");
    }

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0))  // timeout=0: non-blocking
        checkFeedingTime(timeinfo);
}
