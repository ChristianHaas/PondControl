#ifndef POND_CONTROLLER_H
#define POND_CONTROLLER_H

#include <BaseComp.h>
#include <DigitalOut.h>
#include <Preferences.h>
#include <time.h>
#include <EventType.h>

// Sensor IDs — must match the IDs set via DS18B20::setId() in main.cpp
#define POND_SENSOR_ID_WATER 1
#define POND_SENSOR_ID_AIR   2

class PondController : public BaseComp
{
public:
    PondController(String name, DigitalOut *relayPump1, DigitalOut *relayPump2, DigitalOut *relayFeeder);

    void action();
    void handleEvent(eventstruct e);
    void feedNow(int amount);   // Pump1 off, Pump2 on for 5h, feeder on for amount seconds

    // Getters
    float getWaterTemp()       const { return _waterTemp; }
    float getAirTemp()         const { return _airTemp; }
    int   getFeedAmount1()     const { return _feedAmount1; }
    int   getFeedAmount2()     const { return _feedAmount2; }
    const char* getFeedTime1() const { return _feedTime1; }
    const char* getFeedTime2() const { return _feedTime2; }

    // Apply all settings at once — persists and logs only once
    void applySettings(int feed1, int feed2, const char* time1, const char* time2, int pumpOffMinutes);

    int  getPumpOffMinutes() const { return _pumpOffMinutes; }
    bool isPump1On()         const { return _relayPump1->getStatus(); }
    bool isPump2On()         const { return _relayPump2->getStatus(); }
    bool isFeederOn()        const { return _relayFeeder->getStatus(); }

    // Individual setters (persist to flash — use applySettings when changing multiple)
    void setFeedAmount1(int v);
    void setFeedAmount2(int v);
    void setFeedTime1(const char* t);
    void setFeedTime2(const char* t);

private:
    void saveSettings();
    void loadSettings();
    void checkFeedingTime(struct tm &ti);

    DigitalOut       *_relayPump1;
    DigitalOut       *_relayPump2;
    DigitalOut       *_relayFeeder;
    unsigned long     _pumpRestoreTime = 0;  // millis() when Pump1 resumes and Pump2 stops

    Preferences _prefs;

    float _waterTemp = -99.0f;
    float _airTemp   = -99.0f;

    int  _feedAmount1 = 0;
    int  _feedAmount2 = 0;
    char _feedTime1[6] = "08:00";
    char _feedTime2[6] = "18:00";

    int  _pumpOffMinutes = 300;   // default 5 hours

    // feeding-time dedup: store last day-of-year each slot was triggered
    int  _lastFedDoy1 = -1;
    int  _lastFedDoy2 = -1;
};

#endif
