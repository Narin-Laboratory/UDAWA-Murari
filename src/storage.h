#ifndef STORAGE_H
#define STORAGE_H

#include "main.h"

struct CrashState{
    unsigned long rtcp = 0;
    int crashCnt = 0;
    bool fSafeMode = false;
    unsigned long crashStateCheckTimer = millis();
    bool crashStateCheckedFlag = false;
    unsigned long plannedRebootTimer = millis();
    unsigned int plannedRebootCountDown = 0;
    bool fPlannedReboot = false;
    bool fRTCHwDetected = false;
    unsigned long lastRecordedDatetime = 0;
    unsigned long lastRecordedDatetimeSavedTimer = 0;
    bool fStartServices = false;
    bool fStopServices = false;
    bool fDoInit = false;
};

// New structs based on prompt and old.h
struct TimerConfig {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    unsigned long duration; // Duration in seconds

    TimerConfig() : hour(0), minute(0), second(0), duration(0) {}
    TimerConfig(uint8_t h, uint8_t m, uint8_t s, uint16_t d) : hour(h), minute(m), second(s), duration(d) {}
};

struct AppConfig {
    uint8_t s1tx;
    uint8_t s1rx;
    unsigned long intvWeb;
    unsigned long intvAttr;
    unsigned long intvTele;
    int maxWatt;
    bool relayON;
    bool fEnviroSensorDummy;
    unsigned long enviroSensorAlarmTimer;
    // Safe thresholds for enviro sensors
    float tempSafeHigh;
    float tempSafeLow;
    float rhSafeHigh;
    float rhSafeLow;
};

struct AppState {
    bool fSHTSensor = false;
    bool fBHSensor = false;
    TaskHandle_t xHandleEnviroSensor = NULL;
    BaseType_t xReturnedEnviroSensor;
    unsigned long enviroSensorTaskRoutineLastActivity = 0;
    bool fSaveAppState = false;
    bool fsyncClientAttributes = false;
    bool fSaveAllState = false;
    bool fPanic = false;

    // Transient state variables, moved from coreroutineLoop
    bool panic_action_taken = false;
    unsigned long lastWebBcast = 0;
    unsigned long lastAttrBcast = 0;
};

extern UdawaConfig config;
extern CrashState crashState;
extern GenericConfig crashStateConfig;

// New extern declarations
extern AppConfig appConfig;
extern AppState appState;

extern GenericConfig appConfigGC;
extern GenericConfig appStateGC;

void storageSetup();
void storageConvertAppConfig(JsonDocument &doc, bool direction, bool load_defaults = false);
void storageConvertAppState(JsonDocument &doc, bool direction, bool load_defaults = false);
void storageConvertUdawaConfig(JsonDocument &doc, bool direction, bool load_defaults = false);

#endif