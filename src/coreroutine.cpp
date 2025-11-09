#include "coreroutine.h"

ESP32Time RTC(0);
#ifdef USE_HW_RTC
ErriezDS3231 hwRTC;
#endif

TaskHandle_t xHandleAlarm = NULL;
BaseType_t xReturnedAlarm;
QueueHandle_t xQueueAlarm = xQueueCreate( 10, sizeof( struct AlarmMessage ) );
TaskHandle_t xHandleEnviroSensor = NULL;

#ifdef USE_LOCAL_WEB_INTERFACE
AsyncWebServer http(80);
AsyncWebSocket ws(PSTR("/ws"));
std::map<uint32_t, bool> wsClientAuthenticationStatus;
std::map<IPAddress, unsigned long> wsClientAuthAttemptTimestrhs; 
std::map<uint32_t, String> wsClientSalts;
SemaphoreHandle_t xSemaphoreWSBroadcast = NULL;
#endif

#ifdef USE_IOT
IoTState iotState;
#ifdef USE_IOT_SECURE
WiFiClientSecure tcpClient;
#else
WiFiClient tcpClient;
#endif
Arduino_MQTT_Client mqttClient(tcpClient);
Provision<> IAPIProv;
Server_Side_RPC<> IAPIRPC;
Shared_Attribute_Update<> IAPISharedAttr;
Attribute_Request<> IAPISharedAttrReq;
Espressif_Updater<> IoTUpdater;
OTA_Firmware_Update<> IAPIOta;
ThingsBoard tb(mqttClient);
#endif

#ifdef USE_SHT3X
SensirionI2cSht3x sht;
#endif

#ifdef USE_SHT4X
SensirionI2cSht4x sht;
#endif

#ifdef USE_BH1750
BH1750 bh1750(LOW);
#endif

#ifdef USE_MAX17048
MAX17048 max17048;
#endif


void reboot(int countDown = 0){
  crashState.plannedRebootCountDown = countDown;
  crashState.fPlannedReboot = true;
}

void coreroutineSetup(){
  #ifdef USE_I2C
  Wire.begin();
  Wire.setClock(400000);
  #endif

  coreroutineCrashStateTruthKeeper(1);
  if(crashState.rtcp < SAFE_CHECKPOINT_TIME){
    crashState.crashCnt++;
    if(crashState.crashCnt >= MAX_CRASH_COUNTER){
        crashState.fSafeMode = true;
        logger->warn(PSTR(__func__), PSTR("** SAFEMODE ACTIVATED **\n"));
    }
  }
  if(crashState.fSafeMode){
    logger->warn(PSTR(__func__), PSTR("** SAFEMODE ACTIVATED **\n"));
  }
  logger->debug(PSTR(__func__), PSTR("Runtime Counter: %d, Crash Counter: %d, Safemode Status: %s\n"), crashState.rtcp, crashState.crashCnt, crashState.fSafeMode ? PSTR("ENABLED") : PSTR("DISABLED"));
  
  if (!crashState.fSafeMode){
    logger->info(PSTR(__func__), PSTR("Hardware ID: %s\n"), config.state.hwid);

    #ifdef USE_WIFI_LOGGER
    wiFiLogger->setConfig(config.state.logIP, config.state.logPort, WIFI_LOGGER_BUFFER_SIZE);
    #endif

    if(xHandleAlarm == NULL){
      xReturnedAlarm = xTaskCreatePinnedToCore(coreroutineAlarmTaskRoutine, PSTR("coreroutineAlarmTaskRoutine"), ALARM_STACKSIZE, NULL, 1, &xHandleAlarm, 1);
      if(xReturnedAlarm == pdPASS){
        logger->warn(PSTR(__func__), PSTR("Task alarmTaskRoutine has been created.\n"));
      }
    }
    if(xHandleEnviroSensor == NULL){
      appState.xReturnedEnviroSensor = xTaskCreatePinnedToCore(coreroutineWaterSensorTaskRoutine, PSTR("enviroSensor"), 4096, NULL, 1, &appState.xHandleEnviroSensor, 1);
      if(appState.xReturnedEnviroSensor == pdPASS){
          logger->warn(PSTR(__func__), PSTR("Task enviroSensor has been created.\n"));
      }
    }
    
    coreroutineSetLEDBuzzer(1, false, 3, 50);

    crashState.rtcp = 0;

    #ifdef USE_MAX17048
    max17048.attatch(Wire);
    coreroutineReadBatteryGauge();
    #endif

    #ifdef USE_LOCAL_WEB_INTERFACE
    if(xSemaphoreWSBroadcast == NULL){xSemaphoreWSBroadcast = xSemaphoreCreateMutex();}
    #endif

    #ifdef USE_IOT
    tb.Set_Buffering_Size(IOT_BUFFERING_SIZE);
    tb.Set_Maximum_Stack_Size(IOT_DEFAULT_MAX_STACK_SIZE);
    tb.Set_Buffer_Size(IOT_RECEIVE_BUFFER_SIZE, IOT_SEND_BUFFER_SIZE);

    logger->debug(PSTR(__func__), PSTR("ThingsBoard buffer size after: %u, stack: %u, resp: %u, send: %u, recv: %u\n"),
      tb.Get_Buffering_Size(),
      tb.Get_Maximum_Stack_Size(),
      tb.Get_Max_Response_Size(),
      tb.Get_Send_Buffer_Size(), 
      tb.Get_Receive_Buffer_Size());

    if(iotState.xSemaphoreThingsboard == NULL){iotState.xSemaphoreThingsboard = xSemaphoreCreateMutex();}
    #ifdef USE_IOT_SECURE
    tcpClient.setCACert(CA_CERT);
    #endif

    tb.Subscribe_API_Implementation(IAPIProv);
    tb.Subscribe_API_Implementation(IAPIRPC);
    tb.Subscribe_API_Implementation(IAPISharedAttr);
    tb.Subscribe_API_Implementation(IAPISharedAttrReq);
    tb.Subscribe_API_Implementation(IAPIOta);
    #endif
  }
  coreroutineSetAlarm(0, 1, 3, 50);
}

void coreroutineLoop(){
    unsigned long now = millis();

    wiFiHelper.run();

    #ifdef USE_WIFI_OTA
    ArduinoOTA.handle();
    #endif

    #ifdef USE_LOCAL_WEB_INTERFACE
    ws.cleanupClients();
    #endif

    #ifdef USE_IOT
    if(WiFi.getMode() == WIFI_MODE_STA && WiFi.isConnected()){
      coreroutineRunIoT();
    }
    #endif

    if( !crashState.crashStateCheckedFlag && (now - crashState.crashStateCheckTimer) > SAFEMODE_CLEAR_DURATION * 1000 ){
      crashState.fSafeMode = false;
      crashState.crashCnt = 0;
      logger->info(PSTR(__func__), PSTR("fSafeMode & Crash Counter cleared! Try to reboot normally.\n"));
      coreroutineCrashStateTruthKeeper(2);
      crashState.crashStateCheckedFlag = true;
    }

    if( (now - crashState.lastRecordedDatetimeSavedTimer) > FSTIMESAVER_INTERVAL * 3600 * 1000 ){
      crashState.lastRecordedDatetimeSavedTimer = now;
      coreroutineCrashStateTruthKeeper(2);
      logger->verbose(PSTR(__func__), PSTR("Crash state saved.\n"));
    }

    if(crashState.fPlannedReboot){
      if( now - crashState.plannedRebootTimer > 1000){
        if(crashState.plannedRebootCountDown <= 0){
          logger->warn(PSTR(__func__), PSTR("Reboting...\n"));
          ESP.restart();
        }
        crashState.plannedRebootCountDown--;
        logger->warn(PSTR(__func__), PSTR("Planned reboot in %d.\n"), crashState.plannedRebootCountDown);
        crashState.plannedRebootTimer = now;
      }
    }

    if(crashState.fStartServices){
      crashState.fStartServices = false;
      coreroutineStartServices();
    }

    if(crashState.fStopServices){
      crashState.fStopServices = false;
      coreroutineStopServices();
    }

    if(crashState.fDoInit){
      crashState.fDoInit = false;
      coreroutineDoInit();
    }

    if(appState.fsyncClientAttributes){
        coreroutineSyncClientAttr(3); // 3 means both ws and iot
        appState.fsyncClientAttributes = false;
        logger->debug(PSTR(__func__), PSTR("Synced client attributes.\n"));
    }

    if(appState.fSaveAppState){
        JsonDocument doc;
        storageConvertAppState(doc, false);
        appStateGC.save(doc);
        appState.fSaveAppState = false;
        logger->debug(PSTR(__func__), PSTR("Saved app state.\n"));
    }

    if (appState.fSaveAllState){
      coreroutineSaveAllStorage();
      appState.fSaveAllState = false;
    }

    #ifdef USE_LOCAL_WEB_INTERFACE
      if(ws.count() > 0 && (now - appState.lastWebBcast > (appConfig.intvWeb * 1000))){
        JsonDocument doc;
        JsonObject sysInfo = doc[PSTR("sysInfo")].to<JsonObject>();

        sysInfo[PSTR("heap")] = ESP.getFreeHeap();
        sysInfo[PSTR("uptime")] = now;
        sysInfo[PSTR("datetime")] = RTC.getDateTime();
        sysInfo[PSTR("rssi")] = wiFiHelper.rssiToPercent(WiFi.RSSI());
        #ifdef USE_MAX17048
        coreroutineReadBatteryGauge();
        sysInfo[PSTR("batt")] = appState.battAccurPercent > 100.0 ? 100.0 : appState.battAccurPercent;
        #else
        sysInfo[PSTR("batt")] = 100;
        #endif

        wsBcast(doc);
        appState.lastWebBcast = now;
      }
    #endif

    #ifdef USE_IOT
      if(now - appState.lastAttrBcast > (appConfig.intvAttr * 1000)){
        JsonDocument doc;

        doc[PSTR("heap")] = ESP.getFreeHeap();
        doc[PSTR("uptime")] = now;
        doc[PSTR("datetime")] = RTC.getDateTime();
        doc[PSTR("rssi")] = wiFiHelper.rssiToPercent(WiFi.RSSI());
        #ifdef USE_MAX17048
        doc[PSTR("batt")] = appState.battAccurPercent > 100.0 ? 100.0 : appState.battAccurPercent;
        #endif
        iotSendAttr(doc);
        appState.lastAttrBcast = now;
      }
    #endif
}

#ifdef USE_MAX17048
void coreroutineReadBatteryGauge(){
  unsigned long now = millis();
  appState.fBattSensor = !isnan(max17048.id());
  appState.battADC = max17048.adc();
  appState.battVolt = max17048.voltage();
  appState.battPercent = max17048.percent();
  appState.battAccurPercent = max17048.accuratePercent();

  #ifdef USE_IOT
  if(now - appState.lastBattGaugeSend > appState.intvSendBattGauge * 1000){
    JsonDocument doc;

    doc[PSTR("battADC")] = appState.battADC;
    doc[PSTR("battVolt")] = appState.battVolt;
    doc[PSTR("battPercent")] = appState.battPercent;
    doc[PSTR("battAccurPercent")] = appState.battAccurPercent;
    iotSendTele(doc);
    #endif

    /*logger->debug(PSTR(__func__), PSTR("Battery - ADC: %.2f, Volt: %.2fV, Percent: %.2f%%, Accurate: %.2f%%\n"), 
      appState.battADC, appState.battVolt, appState.battPercent, appState.battAccurPercent);*/
    appState.lastBattGaugeSend = now;
  }
}
#endif

void coreroutineDoInit(){
    logger->warn(PSTR(__func__), PSTR("Starting services setup protocol!\n"));
    if (!MDNS.begin(config.state.hname)) {
    logger->error(PSTR(__func__), PSTR("Error setting up MDNS responder!\n"));
    }
    else{
    logger->debug(PSTR(__func__), PSTR("mDNS responder started at %s\n"), config.state.hname);
    }

    MDNS.addService("http", "tcp", 80);

    #ifdef USE_WIFI_OTA
    if(config.state.fWOTA){
        logger->debug(PSTR(__func__), PSTR("Starting WiFi OTA at %s\n"), config.state.hname);
        ArduinoOTA.setHostname(config.state.hname);
        ArduinoOTA.setPasswordHash(config.state.upass);

        ArduinoOTA.onStart(coreroutineOnWiFiOTAStart);
        ArduinoOTA.onEnd(coreroutineOnWiFiOTAEnd);
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        coreroutineOnWiFiOTAProgress(progress, total);
        });
        ArduinoOTA.onError([](ota_error_t error) {
            coreroutineOnWiFiOTAError(error);
        });
        ArduinoOTA.begin();
    }
    #endif

    #ifdef USE_LOCAL_WEB_INTERFACE
    logger->debug(PSTR(__func__), PSTR("Starting web service...\n"));
    http.serveStatic("/", LittleFS, "/ui").setDefaultFile("index.html");
    http.serveStatic("/css", LittleFS, "/ui/css");
    http.serveStatic("/assets", LittleFS, "/ui/assets");
    http.serveStatic("/locales", LittleFS, "/ui/locales");
    http.on("/damodar", HTTP_GET, [](AsyncWebServerRequest *request){
      request->redirect("/");
    });
    http.on("/gadadar", HTTP_GET, [](AsyncWebServerRequest *request){
      request->redirect("/");
    });
    http.on("/murari", HTTP_GET, [](AsyncWebServerRequest *request){
      request->redirect("/");
    });

    ws.onEvent([](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
        coreroutineOnWsEvent(server, client, type, arg, data, len);
    });

    http.addHandler(&ws);
    http.begin();
    logger->debug(PSTR(__func__), PSTR("Web service started.\n"));
    #endif
}

void coreroutineStartServices(){
    #ifdef USE_WIFI_LOGGER
    logger->addLogger(wiFiLogger);
    #endif
    coreroutineRTCUpdate(0);
    #ifdef USE_WIFI_OTA
    if(config.state.fWOTA){
        logger->debug(PSTR(__func__), PSTR("Starting WiFi OTA at %s\n"), config.state.hname);
        ArduinoOTA.setHostname(config.state.hname);
        ArduinoOTA.setPasswordHash(config.state.upass);

        ArduinoOTA.onStart(coreroutineOnWiFiOTAStart);
        ArduinoOTA.onEnd(coreroutineOnWiFiOTAEnd);
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        coreroutineOnWiFiOTAProgress(progress, total);
        });
        ArduinoOTA.onError([](ota_error_t error) {
            coreroutineOnWiFiOTAError(error);
        });
        ArduinoOTA.begin();
    }
    #endif

    if(!MDNS.begin(config.state.hname)) {
        logger->error(PSTR(__func__), PSTR("Error setting up MDNS responder!\n"));
    }
    else{
        logger->debug(PSTR(__func__), PSTR("mDNS responder started at %s\n"), config.state.hname);
    }

    MDNS.addService("http", "tcp", 80);

    #ifdef USE_LOCAL_WEB_INTERFACE
    if(config.state.fWeb && !crashState.fSafeMode){
      logger->debug(PSTR(__func__), PSTR("Starting web wervice...\n"));
      http.serveStatic("/", LittleFS, "/ui").setDefaultFile("index.html");
      http.serveStatic("/css", LittleFS, "/ui/css");
      http.serveStatic("/assets", LittleFS, "/ui/assets");
      http.serveStatic("/locales", LittleFS, "/ui/locales");
      http.on("/damodar", HTTP_GET, [](AsyncWebServerRequest *request){
        request->redirect("/");
      });
      http.on("/gadadar", HTTP_GET, [](AsyncWebServerRequest *request){
        request->redirect("/");
      });
      http.on("/murari", HTTP_GET, [](AsyncWebServerRequest *request){
        request->redirect("/");
      });

      ws.onEvent([](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
          coreroutineOnWsEvent(server, client, type, arg, data, len);
      });

      http.addHandler(&ws);
      http.begin();
      logger->debug(PSTR(__func__), PSTR("Web service started.\n"));
    }
    #endif
}

void coreroutineStopServices(){
    #ifdef USE_WIFI_LOGGER
    logger->addLogger(wiFiLogger);
    #endif

    #ifdef USE_WIFI_OTA
    if(config.state.fWOTA){
        logger->debug(PSTR(__func__), PSTR("Stopping WiFi OTA...\n"), config.state.hname);
        ArduinoOTA.end();
    }
    #endif

    logger->error(PSTR(__func__), PSTR("Stopping MDNS...\n"));
    MDNS.end();


    #ifdef USE_LOCAL_WEB_INTERFACE
    logger->error(PSTR(__func__), PSTR("Stopping HTTP...\n"));
    http.end();
    #endif
}

void coreroutineCrashStateTruthKeeper(uint8_t direction){
  JsonDocument doc;
  crashState.rtcp = millis();

  if(direction == 1 || direction == 3){
    crashStateConfig.load(doc);
    crashState.rtcp = doc[PSTR("rtcp")];
    crashState.crashCnt = doc[PSTR("crashCnt")];
    crashState.fSafeMode = doc[PSTR("fSafeMode")];
    crashState.lastRecordedDatetime = doc[PSTR("lastRecordedDatetime")];
  } 

  if(direction == 2 || direction == 3){
    doc[PSTR("rtcp")] = crashState.rtcp;
    doc[PSTR("crashCnt")] = crashState.crashCnt;
    doc[PSTR("fSafeMode")] = crashState.fSafeMode;
    doc[PSTR("lastRecordedDatetime")] = RTC.getEpoch();
    crashStateConfig.save(doc);
  }
  


}

void coreroutineRTCUpdate(long ts){
  #ifdef USE_HW_RTC
  crashState.fRTCHwDetected = false;
  if(!hwRTC.begin()){
    logger->error(PSTR(__func__), PSTR("RTC module not found. Any function that requires precise timing will malfunction! \n"));
    logger->warn(PSTR(__func__), PSTR("Trying to recover last recorded time from flash file...! \n"));
    RTC.setTime(crashState.lastRecordedDatetime);
    logger->debug(PSTR(__func__), PSTR("Updated time via last recorded time from flash file: %s\n"), RTC.getDateTime().c_str());
  }
  else{
    crashState.fRTCHwDetected = true;
    hwRTC.setSquareWave(SquareWaveDisable);
  }
  #endif
  if(ts == 0){
    WiFiUDP ntpUDP;
    NTPClient timeClient(ntpUDP, "pool.ntp.org");
    timeClient.setTimeOffset(config.state.gmtOff);
    bool ntpSuccess = timeClient.update();
    if (ntpSuccess){
      long epochTime = timeClient.getEpochTime();
      RTC.setTime(epochTime);
      logger->debug(PSTR(__func__), PSTR("Updated time via NTP: %s GMT Offset:%d (%d) \n"), RTC.getDateTime().c_str(), config.state.gmtOff, config.state.gmtOff / 3600);
      #ifdef USE_HW_RTC
      if(crashState.fRTCHwDetected){
        logger->debug(PSTR(__func__), PSTR("Updating RTC HW from NTP...\n"));
        hwRTC.setDateTime(RTC.getHour(), RTC.getMinute(), RTC.getSecond(), RTC.getDay(), RTC.getMonth()+1, RTC.getYear(), RTC.getDayofWeek());
        logger->debug(PSTR(__func__), PSTR("Updated RTC HW from NTP with epoch %d | H:I:S W D-M-Y. -> %d:%d:%d %d %d-%d-%d\n"), 
        hwRTC.getEpoch(), RTC.getHour(), RTC.getMinute(), RTC.getSecond(), RTC.getDayofWeek(), RTC.getDay(), RTC.getMonth()+1, RTC.getYear());
      }
      #endif
    }else{
      #ifdef USE_HW_RTC
      if(crashState.fRTCHwDetected){
        logger->debug(PSTR(__func__), PSTR("Updating RTC from RTC HW with epoch %d.\n"), hwRTC.getEpoch());
        RTC.setTime(hwRTC.getEpoch());
        logger->debug(PSTR(__func__), PSTR("Updated time via RTC HW: %s GMT Offset:%d (%d) \n"), RTC.getDateTime().c_str(), config.state.gmtOff, config.state.gmtOff / 3600);
      }
      #endif
    }
  }else{
      RTC.setTime(ts);
      logger->debug(PSTR(__func__), PSTR("Updated time via timestrh: %s\n"), RTC.getDateTime().c_str());
  }
}

void coreroutineSetAlarm(uint16_t code, uint8_t color, int32_t blinkCount, uint16_t blinkDelay){
  if( xQueueAlarm != NULL ){
    AlarmMessage alarmMsg;
    alarmMsg.code = code; alarmMsg.color = color; alarmMsg.blinkCount = blinkCount; alarmMsg.blinkDelay = blinkDelay;
    if( xQueueSend( xQueueAlarm, &alarmMsg, ( TickType_t ) 1000 ) != pdPASS )
    {
        logger->debug(PSTR(__func__), PSTR("Failed to set alarm. Queue is full. \n"));
    }
  }
}

void coreroutineAlarmTaskRoutine(void *arg){
  #ifndef USE_CO_MCU
  pinMode(config.state.pinLEDR, OUTPUT);
  pinMode(config.state.pinLEDG, OUTPUT);
  pinMode(config.state.pinLEDB, OUTPUT);
  pinMode(config.state.pinBuzz, OUTPUT);
  #endif
  while(true){
    if( xQueueAlarm != NULL ){
      AlarmMessage alarmMsg;
      if( xQueueReceive( xQueueAlarm,  &( alarmMsg ), ( TickType_t ) 100 ) == pdPASS )
      {
        if(alarmMsg.code > 0){
          JsonDocument doc;
          JsonObject alarm = doc[PSTR("alarm")].to<JsonObject>();
          alarm[PSTR("code")] = alarmMsg.code;
          alarm[PSTR("time")] = RTC.getDateTime();

          #ifdef USE_LOCAL_WEB_INTERFACE
          wsBcast(doc);
          #endif

          #ifdef USE_IOT
          doc.clear();
          doc[PSTR("alarm")] = alarmMsg.code;
          iotSendTele(doc);
          #endif
        }
        coreroutineSetLEDBuzzer(alarmMsg.color, alarmMsg.blinkCount > 0 ? true : false, alarmMsg.blinkCount, alarmMsg.blinkDelay);
        logger->debug(PSTR(__func__), PSTR("Alarm code: %d, color: %d, blinkCount: %d, blinkDelay: %d\n"), alarmMsg.code, alarmMsg.color, alarmMsg.blinkCount, alarmMsg.blinkDelay);
        vTaskDelay((const TickType_t) (alarmMsg.blinkCount * alarmMsg.blinkDelay) / portTICK_PERIOD_MS);
      }
    }
    vTaskDelay((const TickType_t) 100 / portTICK_PERIOD_MS);
  }
}

void coreroutineSetLEDBuzzer(uint8_t color, uint8_t isBlink, int32_t blinkCount, uint16_t blinkDelay){
  uint8_t r, g, b;
  
  switch (color)
  {
  //Auto by network
  case 0:
    if(false){
      r = config.state.LEDOn;
      g = config.state.LEDOn;
      b = config.state.LEDOn;
    }
    else if(WiFi.status() == WL_CONNECTED && WiFi.getMode() == WIFI_MODE_STA){
      r = !config.state.LEDOn;
      g = config.state.LEDOn;
      b = !config.state.LEDOn;
    }
    else if(WiFi.status() == WL_CONNECTED && WiFi.getMode() == WIFI_MODE_AP && WiFi.softAPgetStationNum() > 0){
      r = !config.state.LEDOn;
      g = config.state.LEDOn;
      b = !config.state.LEDOn;
    }
    else{
      r = config.state.LEDOn;
      g = !config.state.LEDOn;
      b = !config.state.LEDOn;
    }
    break;
  //RED
  case 1:
    r = config.state.LEDOn;
    g = !config.state.LEDOn;
    b = !config.state.LEDOn;
    break;
  //GREEN
  case 2:
    r = !config.state.LEDOn;
    g = config.state.LEDOn;
    b = !config.state.LEDOn;
    break;
  //BLUE
  case 3:
    r = !config.state.LEDOn;
    g = !config.state.LEDOn;
    b = config.state.LEDOn;
    break;
  default:
    r = config.state.LEDOn;
    g = config.state.LEDOn;
    b = config.state.LEDOn;
  }

  if(isBlink){
    int32_t blinkCounter = 0;
    while (blinkCounter < blinkCount)
    {
      digitalWrite(config.state.pinLEDR, r);
      digitalWrite(config.state.pinLEDG, g);
      digitalWrite(config.state.pinLEDB, b);
      digitalWrite(config.state.pinBuzz, HIGH);
      //logger->debug(PSTR(__func__), PSTR("Blinking LED and Buzzing, blinkDelay: %d, blinkCount: %d\n"), blinkDelay, blinkCount);
      vTaskDelay(pdMS_TO_TICKS(blinkDelay));
      digitalWrite(config.state.pinLEDR, !r);
      digitalWrite(config.state.pinLEDG, !g);
      digitalWrite(config.state.pinLEDB, !b);
      digitalWrite(config.state.pinBuzz, LOW);
      //logger->debug(PSTR(__func__), PSTR("Stop Blinking LED and Buzzing.\n"));
      vTaskDelay(pdMS_TO_TICKS(blinkDelay));
      blinkCounter++;
    }
  }
  else{
    digitalWrite(config.state.pinLEDR, r);
    digitalWrite(config.state.pinLEDG, g);
    digitalWrite(config.state.pinLEDB, b);
  }
}

void coreroutineSaveAllStorage() {
    JsonDocument doc;

    // Save appConfig
    storageConvertAppConfig(doc, false);
    appConfigGC.save(doc);
    doc.clear();

    // Save appState
    storageConvertAppState(doc, false);
    appStateGC.save(doc);
    doc.clear();

    // Save crashStateConfig
    coreroutineCrashStateTruthKeeper(2);

    // Save main config (UdawaConfig)
    config.save();
}

static void coreroutineFSDownloaderTask(void *arg){
  coreroutineFSDownloader();
  vTaskDelete(NULL);
}

void coreroutineFSDownloader() {
    coreroutineSaveAllStorage();
    // We need a WiFiClient for ArduinoHttpClient
    WiFiClient wifi;

    // --- New URL Parsing Logic ---
    char host[128];
    char path[128];
    uint16_t port = 80; // Default to port 80

    // Use sscanf to parse the URL from your configuration
    // This format string looks for "http://", then captures the host (up to '/'),
    // an optional port (:port), and the rest as the path.
    if (sscanf(config.state.binURL, "http://%99[^:]:%hu/%99[^\n]", host, &port, path) == 3) {
        // Successfully parsed host, port, and path
    } else if (sscanf(config.state.binURL, "http://%99[^/]/%99[^\n]", host, path) == 2) {
        // Successfully parsed host and path, port remains 80
    } else {
        logger->error(PSTR(__func__), PSTR("Failed to parse URL: %s\n"), config.state.binURL);
        return;
    }
    // Prepend a '/' to the path if it's missing, as required by HttpClient
    if (path[0] != '/') {
        String tempPath = String("/") + String(path);
        strncpy(path, tempPath.c_str(), sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0'; // Ensure null termination
    }
    // --- End of New URL Parsing Logic ---

    // Create the HttpClient instance with the WiFiClient and parsed server details
    HttpClient http(wifi, host, port);

    logger->info(PSTR(__func__), PSTR("Downloading SPIFFS from host: %s, port: %d, path: %s\n"), host, port, path);

    // Make the GET request
    http.get(path);

    // ... (the rest of your function remains exactly the same) ...
    
    // Get the status code from the response
    int httpCode = http.responseStatusCode();

    int64_t updateSize = 0;
    String contentType;
    String acceptRange;

    if (httpCode == 200) { // HTTP_CODE_OK
        while (http.headerAvailable()) {
            String headerName = http.readHeaderName();
            String headerValue = http.readHeaderValue();

            if (headerName.equalsIgnoreCase("Content-Length")) {
                updateSize = headerValue.toInt();
            } else if (headerName.equalsIgnoreCase("Content-Type")) {
                contentType = headerValue;
            } else if (headerName.equalsIgnoreCase("Accept-Ranges")) {
                acceptRange = headerValue;
            }
        }

        if (acceptRange == "bytes") {
            logger->info(PSTR(__func__), PSTR("This server supports resume!\n"));
        } else {
            logger->info(PSTR(__func__), PSTR("This server does not support resume!\n"));
        }

    } else {
        logger->error(PSTR(__func__), PSTR("Server responded with HTTP Status %d.\n"), httpCode);
        return;
    }

    updateSize = http.contentLength();

    if (updateSize <= 0) {
        logger->error(PSTR(__func__), PSTR("Response is empty or invalid! updateSize: %d, contentType: %s\n"), (int)updateSize, contentType.c_str());
        reboot(3);
        return;
    }

    logger->info(PSTR(__func__), PSTR("updateSize: %d, contentType: %s\n"), (int)updateSize, contentType.c_str());
    
    bool canBegin = Update.begin(updateSize, U_SPIFFS);

    if (!canBegin) {
        logger->warn(PSTR(__func__), PSTR("Not enough space to begin OTA, partition size mismatch?\n"));
        Update.abort();
        reboot(3);
        return;
    }

    Update.onProgress([](size_t progress, size_t size) {
        JsonDocument doc;
        int calculatedProgress = progress + (size * 0.90);
        if(calculatedProgress > size){calculatedProgress = size; doc[PSTR("FSUpdate")][PSTR("status")] = PSTR("finished");}
        else{doc[PSTR("FSUpdate")][PSTR("status")] = PSTR("downloading");}
        doc[PSTR("FSUpdate")][PSTR("progress")] = calculatedProgress;
        doc[PSTR("FSUpdate")][PSTR("total")] = size;
        wsBcast(doc);
    });

    logger->info(PSTR(__func__), PSTR("Begin LittleFS OTA. This may take 2 - 5 mins to complete. Things might be quiet for a while.. Patience!\n"));
    JsonDocument doc;
    doc[PSTR("FSUpdate")][PSTR("status")] = PSTR("start");
    wsBcast(doc);
    doc.clear();

    size_t written = Update.writeStream(http);

    if (written == updateSize) {
        logger->info(PSTR(__func__), PSTR("Written : %d successfully.\n"), (int)written);
    } else {
        logger->warn(PSTR(__func__), PSTR("Written only : %d / %d. Premature end of stream? Error: %s\n"), (int)written, (int)updateSize, Update.errorString());
        Update.abort();
        coreroutineSaveAllStorage();
        doc[PSTR("FSUpdate")][PSTR("status")] = PSTR("fail");
        doc[PSTR("FSUpdate")][PSTR("error")] = Update.errorString();
        wsBcast(doc);
        reboot(3);
        return;
    }

    if (!Update.end()) {
        logger->warn(PSTR(__func__), PSTR("An Update Error Occurred: %d\n"), Update.getError());
        coreroutineSaveAllStorage();
        doc[PSTR("FSUpdate")][PSTR("status")] = PSTR("fail");
        doc[PSTR("FSUpdate")][PSTR("error")] = Update.getError();
        wsBcast(doc);
        reboot(3);
        return;
    }
    
    if (Update.isFinished()) {
        logger->info(PSTR(__func__), PSTR("Update completed successfully.\n"));
        coreroutineSaveAllStorage();
        doc[PSTR("FSUpdate")][PSTR("status")] = PSTR("success");
        wsBcast(doc);
        delay(1000); // Ensure configuration is saved before reboot
        reboot(3);
    } else {
        coreroutineSaveAllStorage();
        logger->warn(PSTR(__func__), PSTR("Update not finished! Something went wrong!\n"));
        doc[PSTR("FSUpdate")][PSTR("status")] = PSTR("fail");
        doc[PSTR("FSUpdate")][PSTR("error")] = "Update not finished";
        wsBcast(doc);
        reboot(3);
    }

    reboot(3);
}

#ifdef USE_WIFI_OTA
void coreroutineOnWiFiOTAStart(){
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
    type = "sketch";
    } else { // U_SPIFFS
    type = "filesystem";
    }
    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    LittleFS.end();
    logger->debug(PSTR(""), PSTR("Start updating %s.\n"), type.c_str());
}

void coreroutineOnWiFiOTAEnd(){
    logger->debug(PSTR(__func__), PSTR("\n Finished.\n"));
}

void coreroutineOnWiFiOTAProgress(unsigned int progress, unsigned int total){
    logger->debug(PSTR(__func__), PSTR("Progress: %u%%\n"), (progress / (total / 100)));
}

void coreroutineOnWiFiOTAError(ota_error_t error){
    logger->error(PSTR(__func__), PSTR("Error[%u]: "), error);
    if (error == OTA_AUTH_ERROR) {
    logger->error(PSTR(""),PSTR("Auth Failed\n"));
    } else if (error == OTA_BEGIN_ERROR) {
    logger->error(PSTR(""),PSTR("Begin Failed\n"));
    } else if (error == OTA_CONNECT_ERROR) {
    logger->error(PSTR(""),PSTR("Connect Failed\n"));
    } else if (error == OTA_RECEIVE_ERROR) {
    logger->error(PSTR(""),PSTR("Receive Failed\n"));
    } else if (error == OTA_END_ERROR) {
    logger->error(PSTR(""),PSTR("End Failed\n"));
    }
}
#endif

#ifdef USE_LOCAL_WEB_INTERFACE
String hmacSha256(String htP, String salt) {
  char outputBuffer[65]; // 2 characters per byte + null terminator

  // Convert input strings to UTF-8 byte arrays 
  std::vector<uint8_t> apiKeyUtf8(htP.begin(), htP.end());
  std::vector<uint8_t> saltUtf8(salt.begin(), salt.end());

  // Calculate the HMAC
  unsigned char hmac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1); // Set HMAC mode
  mbedtls_md_hmac_starts(&ctx, apiKeyUtf8.data(), apiKeyUtf8.size());
  mbedtls_md_hmac_update(&ctx, saltUtf8.data(), saltUtf8.size());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx); 

  // Convert the hash to a hex string (with leading zeros)
  for (int i = 0; i < 32; i++) {
    sprintf(&outputBuffer[i * 2], "%02x", hmac[i]);
  }

  // Null terminate the string
  outputBuffer[64] = '\0';
  return String(outputBuffer);  
}


void coreroutineOnWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  IPAddress clientIP = client->remoteIP();
  JsonDocument doc;
  switch(type) {
    case WS_EVT_DISCONNECT:
    {
      logger->verbose(PSTR(__func__), PSTR("Client disconnected.\n"));
      wsClientAuthenticationStatus.erase(client->id());
      wsClientAuthAttemptTimestrhs.erase(clientIP);
      wsClientSalts.erase(client->id());  // Remove salt on disconnect
      break;     
    }
    case WS_EVT_CONNECT:
    {
      logger->verbose(PSTR(__func__), PSTR("New client arrived [%s]\n"), clientIP.toString().c_str());
      wsClientAuthenticationStatus[client->id()] = false;
      wsClientAuthAttemptTimestrhs[clientIP] = millis();

      if(config.state.fInit){
        // Generate a random salt
        unsigned char salt[16];
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context ctr_drbg;
        const char *pers = "ws_salt";

        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, strlen(pers));
        mbedtls_ctr_drbg_random(&ctr_drbg, salt, sizeof(salt));
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);

        String saltHex;
        for (int i = 0; i < sizeof(salt); i++) {
            char hex[3];
            sprintf(hex, "%02x", salt[i]);
            saltHex += hex;
        }
        
        // Store the salt for this client
        wsClientSalts[client->id()] = saltHex;

        // Send salt to the client
          doc.clear();
          doc[PSTR("setSalt")][PSTR("salt")] = saltHex;
          doc[PSTR("setSalt")][PSTR("name")] = config.state.name;
          doc[PSTR("setSalt")][PSTR("model")] = config.state.model;
          doc[PSTR("setSalt")][PSTR("group")] = config.state.group;
          String message;
          serializeJson(doc, message);
          doc.clear();
          client->text(message);
          break;
        }
      break;
    }
    case WS_EVT_DATA:
    {
      doc.clear();
      DeserializationError err = deserializeJson(doc, (const char*)data, len);
      /*if(err != DeserializationError::Ok){
        logger->error(PSTR(__func__), PSTR("Failed to parse JSON.\n"));
        return;
      }*/
      // If client is not authenticated, check credentials
      if(!wsClientAuthenticationStatus[client->id()] && config.state.fInit) {
        logger->verbose(PSTR(__func__), PSTR("Client is NOT authenticated (%i) AND fInit is TRUE (%i)\n"), wsClientAuthenticationStatus[client->id()], config.state.fInit);
        unsigned long currentTime = millis();
        unsigned long lastAttemptTime = wsClientAuthAttemptTimestrhs[clientIP];

        /**if (currentTime - lastAttemptTime < 1000) {
          // Too many attempts in short time, block this IP for blockInterval
          //_wsClientAuthAttemptTimestrhs[clientIP] = currentTime + WS_BLOCKED_DURATION - WS_RATE_LIMIT_INTERVAL;
          logger->verbose(PSTR(__func__), PSTR("Too many authentication attempts. Blocking for %d seconds. Rate limit %d.\n"), WS_BLOCKED_DURATION / 1000, WS_RATE_LIMIT_INTERVAL);
          //client->close();
          return;
        }**/

        if (err != DeserializationError::Ok) {
          //client->printf(PSTR("{\"status\": {\"code\": 400, \"msg\": \"Bad request.\"}}"));
          //_wsClientAuthAttemptTimestrhs[clientIP] = currentTime;
          return;
        }
        else{
          if(doc[PSTR("auth")][PSTR("salt")] == nullptr || doc[PSTR("auth")][PSTR("hash")] == nullptr){
            //client->printf(PSTR("{\"status\": {\"code\": 400, \"msg\": \"Bad request.\"}}"));
            //_wsClientAuthAttemptTimestrhs[clientIP] = currentTime;
            return;
          }

          String clientAuth = doc[PSTR("auth")][PSTR("hash")].as<String>();
          String clientSalt = doc[PSTR("auth")][PSTR("salt")].as<String>();
          //logger->debug(PSTR(__func__), PSTR("\n\tserver: %s\n\tclient: %s\n\tkey: %s\n\tsalt: %s\n"), _auth.c_str(), auth.c_str(), config.state.htP, salt.c_str());
          
          if (wsClientSalts[client->id()] == clientSalt) {
              // Compute expected HMAC with stored salt
              String expectedAuth = hmacSha256(config.state.htP, wsClientSalts[client->id()]);

              // Check if the HMACs match
              //logger->verbose(PSTR(__func__), PSTR("\nhtP:\t%s \n\nclientAuth:\t%s\n\nexpectedAuth:\t%s\n"), config.state.htP, clientAuth.c_str(), expectedAuth.c_str());
              if (clientAuth == expectedAuth) {
                  wsClientAuthenticationStatus[client->id()] = true;
                  logger->verbose(PSTR(__func__), PSTR("Client authenticated successfully.\n"));
                  client->printf(PSTR("{\"status\": {\"code\": 200, \"msg\": \"Authorized.\", \"model\": \"%s\"}}"), config.state.model);
              } else {
                  logger->warn(PSTR(__func__), PSTR("Authentication failed.\n"));
                  client->printf(PSTR("{\"status\": {\"code\": 401, \"msg\": \"Authorization failed.\", \"model\": \"%s\"}}"), config.state.model);
              }
          } else {
              logger->warn(PSTR(__func__), PSTR("Salt mismatch or expired.\n"));
              client->printf(PSTR("{\"status\": {\"code\": 401, \"msg\": \"Salt mismatch or expired.\", \"model\": \"%s\"}}"), config.state.model);
          }
          // Update timestrh for rate limiting
          wsClientAuthAttemptTimestrhs[clientIP] = currentTime;
          return;
        }
      }
      else {
        // The client is already authenticated or fInit is false, you can process the received data
        //...

      if (doc[PSTR("setConfig")].is<JsonObject>()) {
        if (doc[PSTR("setConfig")][PSTR("cfg")].is<JsonObject>()) {
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("wssid")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("wssid")].as<const char*>()) > 0) {
          strlcpy(config.state.wssid, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("wssid")].as<const char*>(), sizeof(config.state.wssid));
          logger->debug(PSTR(__func__), PSTR("wssid: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("wssid")].as<const char*>());
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("wpass")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("wpass")].as<const char*>()) > 0) {
          strlcpy(config.state.wpass, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("wpass")].as<const char*>(), sizeof(config.state.wpass));
          logger->debug(PSTR(__func__), PSTR("wpass: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("wpass")].as<const char*>());
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("gmtOff")].is<int>()) {
          config.state.gmtOff = doc[PSTR("setConfig")][PSTR("cfg")][PSTR("gmtOff")].as<int>();
          logger->debug(PSTR(__func__), PSTR("gmtOff: %d\n"), config.state.gmtOff);  // Display as integer
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("group")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("group")].as<const char*>()) > 0) {
          strlcpy(config.state.group, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("group")].as<const char*>(), sizeof(config.state.group));
          logger->debug(PSTR(__func__), PSTR("group: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("group")].as<const char*>());
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("name")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("name")].as<const char*>()) > 0) {
          strlcpy(config.state.name, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("name")].as<const char*>(), sizeof(config.state.name));
          logger->debug(PSTR(__func__), PSTR("name: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("name")].as<const char*>());
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("hname")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("hname")].as<const char*>()) > 0) {
          strlcpy(config.state.hname, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("hname")].as<const char*>(), sizeof(config.state.hname));
          logger->debug(PSTR(__func__), PSTR("hname: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("hname")].as<const char*>());
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("htP")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("htP")].as<const char*>()) > 0) {
          strlcpy(config.state.htP, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("htP")].as<const char*>(), sizeof(config.state.htP));
          logger->debug(PSTR(__func__), PSTR("htP: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("htP")].as<const char*>());
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("binURL")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("binURL")].as<const char*>()) > 0) {
          strlcpy(config.state.binURL, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("binURL")].as<const char*>(), sizeof(config.state.binURL));
          logger->debug(PSTR(__func__), PSTR("binURL: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("binURL")].as<const char*>());
          }
          #ifdef USE_IOT
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("tbAddr")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("tbAddr")].as<const char*>()) > 0) {
          strlcpy(config.state.tbAddr, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("tbAddr")].as<const char*>(), sizeof(config.state.tbAddr));
          logger->debug(PSTR(__func__), PSTR("tbAddr: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("tbAddr")].as<const char*>());
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("tbPort")].is<uint16_t>()) {
          config.state.tbPort = doc[PSTR("setConfig")][PSTR("cfg")][PSTR("tbPort")].as<uint16_t>();
          logger->debug(PSTR(__func__), PSTR("tbPort: %d\n"), config.state.tbPort); 
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("fIoT")].is<bool>()) {
          config.state.fIoT = doc[PSTR("setConfig")][PSTR("cfg")][PSTR("fIoT")].as<bool>();
          logger->debug(PSTR(__func__), PSTR("fIoT: %d\n"), config.state.fIoT); 
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("provDK")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("provDK")].as<const char*>()) > 0) {
          strlcpy(config.state.provDK, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("provDK")].as<const char*>(), sizeof(config.state.provDK));
          //logger->debug(PSTR(__func__), PSTR("provDK: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("provDK")].as<const char*>());
          }
          if (doc[PSTR("setConfig")][PSTR("cfg")][PSTR("provDS")].is<const char*>() && strlen(doc[PSTR("setConfig")][PSTR("cfg")][PSTR("provDS")].as<const char*>()) > 0) {
          strlcpy(config.state.provDS, doc[PSTR("setConfig")][PSTR("cfg")][PSTR("provDS")].as<const char*>(), sizeof(config.state.provDS));
          //logger->debug(PSTR(__func__), PSTR("provDS: %s\n"), doc[PSTR("setConfig")][PSTR("cfg")][PSTR("provDS")].as<const char*>());
          }
          #endif
        }
        config.save();
        }

        else if(doc[PSTR("getConfig")].is<const char*>()){
          coreroutineSyncClientAttr(1);
        }

        else if(doc[PSTR("getAvailableWiFi")].is<const char*>()){
            JsonDocument doc;
            File file = LittleFS.open(PSTR("/WiFiList.json"), FILE_READ);
            if (file) {
            // Deserialize the file's content into a temporary JsonDocument
            JsonDocument wifiListDoc;
            DeserializationError error = deserializeJson(wifiListDoc, file);
            file.close();

            if (error == DeserializationError::Ok) {
              // Create the final structure
              doc[PSTR("WiFiList")] = wifiListDoc.as<JsonVariant>();
              wsBcast(doc);
            } else {
              logger->error(PSTR(__func__), PSTR("Failed to parse WiFiList.json: %s\n"), error.c_str());
            }
            } else {
            logger->error(PSTR(__func__), PSTR("Could not open WiFiList.json for reading.\n"));
            }
        }

        else if(doc[PSTR("setFInit")].is<JsonObject>()){
          if(doc[PSTR("setFInit")][PSTR("fInit")].is<bool>()){
            coreroutineSetFInit(doc[PSTR("setFInit")][PSTR("fInit")].as<bool>());
            coreroutineSyncClientAttr(1);
          }
          reboot(3);
        }

        else if(doc[PSTR("setRTCUpdate")].is<JsonObject>()){
          if(doc[PSTR("setRTCUpdate")][PSTR("ts")].is<unsigned long>()){
            coreroutineRTCUpdate(doc[PSTR("setRTCUpdate")][PSTR("ts")].as<unsigned long>());
          }
        }

        else if(doc[PSTR("reboot")].is<int>()){
          reboot(doc[PSTR("reboot")].as<int>());
        }

        else if(doc[PSTR("FSUpdate")].is<bool>()){
          xTaskCreate(
              coreroutineFSDownloaderTask,    // Function that implements the task.
              "FSDownloader",                 // Text name for the task.
              8192,                           // Stack size in words, not bytes.
              NULL,                           // Parameter passed into the task.
              1,                              // Priority at which the task is created.
              NULL                            // Used to pass out the created task's handle.
          );
        }

       
      }
      doc.clear();
    }
    break;
    case WS_EVT_ERROR:
      {
        logger->warn(PSTR(__func__), PSTR("ws [%u] error\n"), client->id());
        return;
      }
      break;	
  }
}

void wsBcast(JsonDocument &doc){
  if(config.state.fWeb){
    if( xSemaphoreWSBroadcast != NULL){
      if( xSemaphoreTake( xSemaphoreWSBroadcast, ( TickType_t ) 1000 ) == pdTRUE )
      {
        String buffer;
        serializeJson(doc, buffer);
        ws.textAll(buffer);
        //logger->verbose(PSTR(__func__), PSTR("Broadcasting message: %s\n"), buffer.c_str());
        xSemaphoreGive( xSemaphoreWSBroadcast );
      }
      else
      {
        logger->verbose(PSTR(__func__), PSTR("No semaphore available.\n"));
      }
    }
  }
}
#endif

void coreroutineSyncClientAttr(uint8_t direction){
  JsonDocument doc;
  String ip = WiFi.localIP().toString();
  
  if(direction == 1 || direction == 3){ // Send to client
    #ifdef USE_LOCAL_WEB_INTERFACE
    // Send original attr and cfg objects for UI compatibility
    doc.clear();
    JsonObject attr = doc[PSTR("attr")].to<JsonObject>();
    attr[PSTR("ipad")] = ip.c_str();
    attr[PSTR("compdate")] = COMPILED;
    attr[PSTR("fmTitle")] = CURRENT_FIRMWARE_TITLE;
    attr[PSTR("fmVersion")] = CURRENT_FIRMWARE_VERSION;
    attr[PSTR("stamac")] = WiFi.macAddress();
    attr[PSTR("apmac")] = WiFi.softAPmacAddress();
    attr[PSTR("flFree")] = ESP.getFreeSketchSpace();
    attr[PSTR("fwSize")] = ESP.getSketchSize();
    attr[PSTR("flSize")] = ESP.getFlashChipSize();
    attr[PSTR("dSize")] = LittleFS.totalBytes();
    attr[PSTR("dUsed")] = LittleFS.usedBytes();
    attr[PSTR("sdkVer")] = ESP.getSdkVersion();
    wsBcast(doc);

    doc.clear();
    JsonObject cfg = doc[PSTR("cfg")].to<JsonObject>();
    cfg[PSTR("hwid")] = config.state.hwid;
    cfg[PSTR("name")] = config.state.name;
    cfg[PSTR("model")] = config.state.model;
    cfg[PSTR("group")] = config.state.group;
    cfg[PSTR("gmtOff")] = config.state.gmtOff;
    cfg[PSTR("hname")] = config.state.hname;
    cfg[PSTR("htP")] = config.state.htP;
    cfg[PSTR("wssid")] = config.state.wssid;
    cfg[PSTR("wpass")] = config.state.wpass;
    cfg[PSTR("fInit")] = config.state.fInit;
    cfg[PSTR("binURL")] = config.state.binURL;
    cfg[PSTR("fIoT")] = config.state.fIoT;
    cfg[PSTR("tbAddr")] = config.state.tbAddr;
    cfg[PSTR("tbPort")] = config.state.tbPort;
    wsBcast(doc);

    // Send new app-specific configs
    doc.clear();
    storageConvertAppConfig(doc, false);
    wsBcast(doc);

    doc.clear();
    storageConvertAppState(doc, false);
    wsBcast(doc);

    doc.clear();
    storageConvertUdawaConfig(doc, false);
    wsBcast(doc);
    #endif
  }

  if(direction == 2 || direction == 3){ // Send to IoT
      #ifdef USE_IOT
      // Send appConfig
      doc.clear();
      storageConvertAppConfig(doc, false);
      iotSendAttr(doc);

      // Send appState
      doc.clear();
      storageConvertAppState(doc, false);
      iotSendAttr(doc);

      // Send udawaConfig
      doc.clear();
      storageConvertUdawaConfig(doc, false);
      iotSendAttr(doc);
      #endif
  }
}

void coreroutineSetFInit(bool fInit){
  config.state.fInit = fInit;
  config.save();

  #ifdef USE_LOCAL_WEB_INTERFACE
    if(config.state.fWeb && !crashState.fSafeMode){
      JsonDocument doc;
      doc[PSTR("setFinishedSetup")][PSTR("fInit")] = config.state.fInit;
      wsBcast(doc);
    }
  #endif
}


void coreroutineWaterSensorTaskRoutine(void *arg) {
  float lux, rh, temp;

  // Variables for telemetry data aggregation
  float lux_sum = 0, rh_sum = 0, temp_sum = 0;
  float lux_min_period = 999, lux_max_period = 0;
  float rh_min_period = 999, rh_max_period = 0;
  float temp_min_period = 999, temp_max_period = 0;
  long reading_count = 0;

  unsigned long timerTelemetry = millis();
  unsigned long timerAttribute = millis();
  unsigned long timerWebIface = millis();
  unsigned long timerAlarm = millis();

  Wire.begin();
  #ifdef USE_SHT3X
  sht.begin(Wire, SHT30_I2C_ADDR_44);
  sht.stopMeasurement();
  delay(1);
  sht.softReset();
  delay(100);
  uint16_t aStatusRegister = 0u;
  static int16_t error = sht.readStatusRegister(aStatusRegister);
  if (error != NO_ERROR) {
    logger->error(PSTR(__func__), PSTR("Error trying to execute readStatusRegister(): %d \n"), error);
    appState.fSHTSensor = false;
  }
  else{
    logger->debug(PSTR(__func__), PSTR("Success executed readStatusRegister.\n"));
    appState.fSHTSensor = true;
  }
  logger->debug(PSTR(__func__), PSTR("aStatusRegister: %u\n"), aStatusRegister);
  error = sht.startPeriodicMeasurement(REPEATABILITY_MEDIUM, MPS_ONE_PER_SECOND);
  if (error != NO_ERROR) {
    logger->error(PSTR(__func__), PSTR("Error trying to execute startPeriodicMeasurement(): %d \n"), error);
    appState.fSHTSensor = false;
  }
  else{
    logger->debug(PSTR(__func__), PSTR("Success executed startPeriodicMeasurement.\n"));
    appState.fSHTSensor = true;
  }
  #else
  sht.begin(Wire, SHT40_I2C_ADDR_44);
  appState.fSHTSensor = true;
  #endif

  #ifdef USE_BH1750
  bh1750.begin(ModeContinuous, ResolutionMid);
  appState.fBHSensor = true;
  #endif

  while (true) {
    bool fSHTFailureReadings = false;
    bool fBHFailureReadings = false;

    if (appConfig.fEnviroSensorDummy) {
      appState.fSHTSensor = true;
      appState.fBHSensor = true;
      
      // Generate dummy temperature reading (e.g., 20-30°C)
      temp = 20.0 + (rand() % 100 / 10.0); // 20.0 - 30.0°C
      
      // Generate dummy relative humidity reading (e.g., 40-80%)
      rh = 40.0 + (rand() % 400 / 10.0); // 40.0 - 80.0%
      
      // Generate dummy lux reading (e.g., 100-1000 lux)
      lux = 100.0 + (rand() % 900); // 100 - 1000 lux

    } else {
      #ifdef USE_BH1750
      bh1750.startConversion();
      appState.fBHSensor = true;
      if (appState.fBHSensor) {
        bh1750.waitForCompletion();
        lux = bh1750.read();
        appState.fBHSensor = true;
        fBHFailureReadings = false;
      } else {
        appState.fBHSensor = false;
        fBHFailureReadings = true;
      }
      #endif

      #if defined(USE_SHT3X) || defined(USE_SHT4X)
      if (sht.blockingReadMeasurement(temp, rh) == NO_ERROR) {
        appState.fSHTSensor = true;
        fSHTFailureReadings = false;
      } else {
        appState.fSHTSensor = false;
        fSHTFailureReadings = true;
      }
      #endif
    }

    if (appState.fSHTSensor && !fSHTFailureReadings) {
      if (isnan(temp) || isnan(rh) || rh < 0.0 || rh > 100.0 || temp < -40.0 ||
        temp > 125.0) {
        fSHTFailureReadings = true;
      }
    }

    if (appState.fBHSensor && !fBHFailureReadings) {
      if (isnan(lux) || lux < 0.0 || lux > 65535.0) {
        fBHFailureReadings = true;
      }
    }

    unsigned long now = millis();
    
    // Accumulate readings for aggregation
    if (appState.fBHSensor && !fBHFailureReadings) {
      lux_sum += lux;
      if (lux < lux_min_period) lux_min_period = lux;
      if (lux > lux_max_period) lux_max_period = lux;
    }
    
    if (appState.fSHTSensor && !fSHTFailureReadings) {
      rh_sum += rh;
      temp_sum += temp;
      
      if (rh < rh_min_period) rh_min_period = rh;
      if (rh > rh_max_period) rh_max_period = rh;
      if (temp < temp_min_period) temp_min_period = temp;
      if (temp > temp_max_period) temp_max_period = temp;
    }
    
    if ((appState.fSHTSensor && !fSHTFailureReadings) || (appState.fBHSensor && !fBHFailureReadings)) {
      reading_count++;
    }

    /*if (appState.fSHTSensor && !fSHTFailureReadings) {
      logger->debug(PSTR(__func__), PSTR("SHT Reading - Temp: %.2f°C, RH: %.2f%%\n"), temp, rh);
    }
    if (appState.fBHSensor && !fBHFailureReadings) {
      logger->debug(PSTR(__func__), PSTR("BH1750 Reading - Lux: %.2f\n"), lux);
    }*/

    JsonDocument doc;

    #ifdef USE_LOCAL_WEB_INTERFACE
    if ((now - timerWebIface) > (appConfig.intvWeb * 1000)) {
      #if defined(USE_SHT3X) || defined(USE_SHT4X)
      if (appState.fSHTSensor && !fSHTFailureReadings) {
        JsonObject shtSensor = doc[PSTR("shtSensor")].to<JsonObject>();
        shtSensor[PSTR("rh")] = rh;
        shtSensor[PSTR("temp")] = temp;
        wsBcast(doc);
        doc.clear();
      }
      #endif

      #ifdef USE_BH1750
      if (appState.fBHSensor && !fBHFailureReadings) {
        JsonObject bhSensor = doc[PSTR("bhSensor")].to<JsonObject>();
        bhSensor[PSTR("lux")] = lux;
        wsBcast(doc);
        doc.clear();
      }
      #endif

      timerWebIface = now;
    }
    #endif

    #ifdef USE_IOT
    if ((now - timerAttribute) > (appConfig.intvAttr * 1000)) {
      #if defined(USE_SHT3X) || defined(USE_SHT4X)
      if (appState.fSHTSensor && !fSHTFailureReadings) {
        doc[PSTR("_temp")] = temp;
        doc[PSTR("_rh")] = rh;
        iotSendAttr(doc);
        doc.clear();
      }
      #endif

      #ifdef USE_BH1750
      if (appState.fBHSensor && !fBHFailureReadings) {
        doc[PSTR("_lux")] = lux;
        iotSendAttr(doc);
        doc.clear();
      }
      #endif
      
      timerAttribute = now;
    }

    if ((now - timerTelemetry) > (appConfig.intvTele * 1000)) {
      if (reading_count > 0) {
        #ifdef USE_BH1750
        if (appState.fBHSensor) {
          doc[PSTR("lux_avg")] = lux_sum / reading_count;
          doc[PSTR("lux_min")] = lux_min_period;
          doc[PSTR("lux_max")] = lux_max_period;
        }
        #endif

        #if defined(USE_SHT3X) || defined(USE_SHT4X)
        if (appState.fSHTSensor) {
          doc[PSTR("rh_avg")] = rh_sum / reading_count;
          doc[PSTR("rh_min")] = rh_min_period;
          doc[PSTR("rh_max")] = rh_max_period;

          doc[PSTR("temp_avg")] = temp_sum / reading_count;
          doc[PSTR("temp_min")] = temp_min_period;
          doc[PSTR("temp_max")] = temp_max_period;
        }
        #endif

        iotSendTele(doc);
        doc.clear();

        // Reset accumulators for next period
        lux_sum = 0; rh_sum = 0; temp_sum = 0;
        lux_min_period = 999999; lux_max_period = 0;
        rh_min_period = 999999; rh_max_period = 0;
        temp_min_period = 999999; temp_max_period = 0;
        reading_count = 0;
      }
      timerTelemetry = now;
    }
    #endif

    if ((now - timerAlarm) > (appConfig.enviroSensorAlarmTimer * 1000)) {
      // Raise per-sensor failure/initialization alarms based on standardized codes (see main.h)
      // Weather (SHTx): use ALARM_WEATHER_* constants
      // Light (BH1750): use ALARM_LIGHT_* constants

      // Initialization / reading failure (per sensor)
      if (!appState.fSHTSensor || fSHTFailureReadings) {
        coreroutineSetAlarm(ALARM_WEATHER_SENSOR_INIT_FAIL, 1, 5, 1000);
      }
      if (!appState.fBHSensor || fBHFailureReadings) {
        coreroutineSetAlarm(ALARM_LIGHT_SENSOR_INIT_FAIL, 1, 5, 1000);
      }

      // Range validations (only when we have valid readings)
      if (appState.fSHTSensor && !fSHTFailureReadings) {
        // Absolute validity based on sensor capability
        if (rh < rhSafeLow || rh > rhSafeHigh) {
          coreroutineSetAlarm(ALARM_WEATHER_HUMID_OUT_OF_RANGE, 1, 5, 1000);
        } else {
          // Safe thresholds
          if (rh > appConfig.rhSafeHigh) coreroutineSetAlarm(ALARM_WEATHER_HUMID_EXCEED_HIGH, 1, 5, 1000);
          if (rh < appConfig.rhSafeLow)  coreroutineSetAlarm(ALARM_WEATHER_HUMID_BELOW_LOW, 1, 5, 1000);
        }

        if (temp < tempSafeLow || temp > tempSafeHigh) {
          coreroutineSetAlarm(ALARM_WEATHER_TEMP_OUT_OF_RANGE, 1, 5, 1000);
        } else {
          if (temp > appConfig.tempSafeHigh) coreroutineSetAlarm(ALARM_WEATHER_TEMP_EXCEED_HIGH, 1, 5, 1000);
          if (temp < appConfig.tempSafeLow)  coreroutineSetAlarm(ALARM_WEATHER_TEMP_BELOW_LOW, 1, 5, 1000);
        }
      }
      if (appState.fBHSensor && !fBHFailureReadings) {
        if (lux < 0 || lux > 65535) coreroutineSetAlarm(ALARM_LIGHT_SENSOR_MEAS_ABNORMAL, 1, 5, 1000);
      }

      timerAlarm = now;
    }

    appState.enviroSensorTaskRoutineLastActivity = millis();
    vTaskDelay((const TickType_t)1000 / portTICK_PERIOD_MS);
  }
}

#ifdef USE_IOT
void coreroutineProcessTBProvResp(const JsonDocument &data){
  if( iotState.xSemaphoreThingsboard != NULL && WiFi.isConnected() && !config.state.provSent && tb.connected()){
    if( xSemaphoreTake( iotState.xSemaphoreThingsboard, ( TickType_t ) 1000 ) == pdTRUE )
    {
      constexpr char CREDENTIALS_TYPE[] PROGMEM = "credentialsType";
      constexpr char CREDENTIALS_VALUE[] PROGMEM = "credentialsValue";
      String _data;
      serializeJson(data, _data);
      logger->verbose(PSTR(__func__),PSTR("Received device provision response: %s\n"), _data.c_str());

      if (strncmp(data["status"], "SUCCESS", strlen("SUCCESS")) != 0) {
        logger->error(PSTR(__func__),PSTR("Provision response contains the error: (%s)\n"), data["errorMsg"].as<const char*>());
      }
      else
      {
        if (strncmp(data[CREDENTIALS_TYPE], PSTR("ACCESS_TOKEN"), strlen(PSTR("ACCESS_TOKEN"))) == 0) {
          strlcpy(config.state.accTkn, data[CREDENTIALS_VALUE].as<std::string>().c_str(), sizeof(config.state.accTkn));
          config.state.provSent = true;  
          config.save();
          logger->verbose(PSTR(__func__),PSTR("Access token provision response saved.\n"));
        }
        else if (strncmp(data[CREDENTIALS_TYPE], PSTR("MQTT_BASIC"), strlen(PSTR("MQTT_BASIC"))) == 0) {
          /*auto credentials_value = data[CREDENTIALS_VALUE].as<JsonObjectConst>();
          credentials.client_id = credentials_value[CLIENT_ID].as<std::string>();
          credentials.username = credentials_value[CLIENT_USERNAME].as<std::string>();
          credentials.password = credentials_value[CLIENT_PASSWORD].as<std::string>();*/
        }
        else {
          logger->warn(PSTR(__func__),PSTR("Unexpected provision credentialsType: (%s)\n"), data[CREDENTIALS_TYPE].as<const char*>());

        }
      }

      // Disconnect from the cloud client connected to the provision account, because it is no longer needed the device has been provisioned
      // and we can reconnect to the cloud with the newly generated credentials.
      if (tb.connected()) {
        tb.disconnect();
      }
      xSemaphoreGive( iotState.xSemaphoreThingsboard );
    }
    else
    {
      logger->error(PSTR(__func__), PSTR("No semaphore available.\n"));
    }
  }
}

void coreroutineIoTProvRequestTimedOut(){
  logger->verbose(PSTR(__func__), PSTR("Provisioning request timed out.\n"));
}

void iotFinishedCallback(const bool & success){
  if(success){
    logger->info(PSTR(__func__), PSTR("IoT OTA Update done!\n"));
    reboot(10);
  }
  else{
    logger->warn(PSTR(__func__), PSTR("IoT OTA Update failed!\n"));
    reboot(10);
  }
}

void iotProgressCallback(const size_t & currentChunk, const size_t & totalChuncks){
   if( xSemaphoreTake( iotState.xSemaphoreThingsboard, ( TickType_t ) 5000 ) == pdTRUE ) {
    logger->debug(PSTR(__func__), PSTR("IoT OTA Progress: %.2f%%\n"),  static_cast<float>(currentChunk * 100U) / totalChuncks);
    xSemaphoreGive( iotState.xSemaphoreThingsboard );   
  }
}

void iotUpdateStartingCallback(){
  logger->info(PSTR(__func__), PSTR("IoT OTA Update starting...\n"));
  // It's a good practice to stop services that might interfere with the update
  // For exrhle, web server or other network services.
  // coreroutineStopServices(); // You might want to call this if needed.
  //LittleFS.end(); // Unmount filesystem to be safe
}

void coreroutineRunIoT(){
  if(!config.state.provSent){
      if (tb.connect(config.state.tbAddr, PSTR("provision"), config.state.tbPort)) {
        const Provision_Callback provisionCallback(Access_Token(), &coreroutineProcessTBProvResp, config.state.provDK, config.state.provDS, config.state.hwid, IOT_PROVISIONING_TIMEOUT * 1000000, &coreroutineIoTProvRequestTimedOut);
        if(IAPIProv.Provision_Request(provisionCallback))
        {
          logger->info(PSTR(__func__),PSTR("Connected to provisioning server: %s:%d. ID: %s \n"),  
            config.state.tbAddr, config.state.tbPort, config.state.hwid);
        }
        else{
          logger->warn(PSTR(__func__), PSTR("Provision request failed: %s:%d. ID:%S\n"), config.state.tbAddr, config.state.tbPort, config.state.hwid);
        }
      }
      else
      {
        logger->warn(PSTR(__func__),PSTR("Failed to connect to provisioning server: %s:%d\n"),  config.state.tbAddr, config.state.tbPort);
      }
      unsigned long timer = millis();
      while(true){
        tb.loop();
        if(config.state.provSent || (millis() - timer) > 10000){break;}
        vTaskDelay((const TickType_t)10 / portTICK_PERIOD_MS);
      }
    }
    else{
      if(!tb.connected() && WiFi.isConnected())
      {
        logger->warn(PSTR(__func__),PSTR("IoT disconnected!\n"));
        //onTbDisconnectedCb();
        logger->info(PSTR(__func__),PSTR("Connecting to broker %s:%d\n"), config.state.tbAddr, config.state.tbPort);
        uint8_t tbDisco = 0;
        const uint8_t maxRetries = 12;
        const TickType_t retryDelay = 5000 / portTICK_PERIOD_MS;
        while(!tb.connect(config.state.tbAddr, config.state.accTkn, config.state.tbPort, config.state.hwid)){  
          tbDisco++;
          logger->warn(PSTR(__func__),PSTR("Failed to connect to IoT Broker %s (%d)\n"), config.state.tbAddr, tbDisco);
          if(tbDisco >= maxRetries){
            config.state.provSent = false;
            tbDisco = 0;
            break;
          }
          vTaskDelay(retryDelay); 
        }

        if(tb.connected()){
          if(!iotState.fSharedAttributesSubscribed){
            Shared_Attribute_Callback coreroutineThingsboardSharedAttributesUpdateCallback(
                [](const JsonVariantConst& data) {
                    //logger->verbose(PSTR(__func__), PSTR("Received shared attribute update(s): \n"));
                    //serializeJson(data, Serial);

                    JsonDocument doc;
                    DeserializationError error = deserializeJson(doc, data.as<String>());
                    if (error) {
                        logger->error(PSTR(__func__), PSTR("Failed to parse shared attributes: %s\n"), error.c_str());
                        return;
                    }

                    storageConvertAppConfig(doc, true);
                    storageConvertAppState(doc, true);
                    storageConvertUdawaConfig(doc, true);
                }
            );
            iotState.fSharedAttributesSubscribed = IAPISharedAttr.Shared_Attributes_Subscribe(coreroutineThingsboardSharedAttributesUpdateCallback);
            if (iotState.fSharedAttributesSubscribed){
              logger->verbose(PSTR(__func__), PSTR("Thingsboard shared attributes update subscribed successfuly.\n"));
            }
            else{
              logger->warn(PSTR(__func__), PSTR("Failed to subscribe Thingsboard shared attributes update.\n"));
            }
          }
          
          if(!iotState.fRebootRPCSubscribed){
            RPC_Callback rebootCallback("reboot", [](const JsonVariantConst& params, JsonDocument& result) {
                int countdown = 0;
                if (params.is<int>()) {
                    countdown = params.as<int>();
                }
                reboot(countdown);
                result["status"] = "success";
            });
            iotState.fRebootRPCSubscribed = IAPIRPC.RPC_Subscribe(rebootCallback); // Pass the callback directly
            if(iotState.fRebootRPCSubscribed){
              logger->verbose(PSTR(__func__), PSTR("reboot RPC subscribed successfuly.\n"));
            }
            else{
              logger->warn(PSTR(__func__), PSTR("Failed to subscribe reboot RPC.\n"));
            }
          }

          if(!iotState.fStateSaveRPCSubscribed){
            RPC_Callback stateSaveCallback("stateSave", [](const JsonVariantConst& params, JsonDocument& result) {
                appState.fSaveAllState = true;
                result["status"] = "success";
            });
            iotState.fStateSaveRPCSubscribed = IAPIRPC.RPC_Subscribe(stateSaveCallback); // Pass the callback directly
            if(iotState.fStateSaveRPCSubscribed){
              logger->verbose(PSTR(__func__), PSTR("stateSave RPC subscribed successfuly.\n"));
            }
            else{
              logger->warn(PSTR(__func__), PSTR("Failed to subscribe stateSave RPC.\n"));
            }
          }

          if(!iotState.fFSUpdateRPCSubscribed){
            RPC_Callback fsUpdateCallback("FSUpdate", [](const JsonVariantConst& params, JsonDocument& result) {
                xTaskCreate(
                    coreroutineFSDownloaderTask,    // Function that implements the task.
                    "FSDownloader",                 // Text name for the task.
                    8192,                           // Stack size in words, not bytes.
                    NULL,                           // Parameter passed into the task.
                    1,                              // Priority at which the task is created.
                    NULL                            // Used to pass out the created task's handle.
                );
                result["status"] = "success";
            });
            iotState.fFSUpdateRPCSubscribed = IAPIRPC.RPC_Subscribe(fsUpdateCallback); // Pass the callback directly
            if(iotState.fFSUpdateRPCSubscribed){
              logger->verbose(PSTR(__func__), PSTR("FSUpdate RPC subscribed successfuly.\n"));
            }
            else{
              logger->warn(PSTR(__func__), PSTR("Failed to subscribe FSUpdate RPC.\n"));
            }
          }

          if(!iotState.fSyncAttributeRPCSubscribed){
            RPC_Callback syncAttributeCallback("syncAttribute", [](const JsonVariantConst& params, JsonDocument& result) {
              appState.fsyncClientAttributes = true;
              result["status"] = "success";
            });
            iotState.fSyncAttributeRPCSubscribed = IAPIRPC.RPC_Subscribe(syncAttributeCallback);
            if(iotState.fSyncAttributeRPCSubscribed){
              logger->verbose(PSTR(__func__), PSTR("syncAttribute RPC subscribed successfully.\n"));
            }
            else{
              logger->warn(PSTR(__func__), PSTR("Failed to subscribe syncAttribute RPC.\n"));
            }
          }

          #ifdef USE_IOT_OTA
          const OTA_Update_Callback coreroutineIoTUpdaterOTACallback(CURRENT_FIRMWARE_TITLE, CURRENT_FIRMWARE_VERSION, &IoTUpdater, &iotFinishedCallback, &iotProgressCallback, &iotUpdateStartingCallback, IOT_OTA_UPDATE_FAILURE_RETRY, IOT_OTA_UPDATE_PACKET_SIZE);
          if (IAPIOta.Start_Firmware_Update(coreroutineIoTUpdaterOTACallback)) {
              logger->debug(PSTR(__func__), PSTR("Initial firmware update check started.\n"));
          } else {
              logger->error(PSTR(__func__), PSTR("Initial firmware update check failed to start.\n"));
          }
          if (IAPIOta.Subscribe_Firmware_Update(coreroutineIoTUpdaterOTACallback)) {
              logger->debug(PSTR(__func__), PSTR("Subscribed to firmware updates.\n"));
          } else {
              logger->error(PSTR(__func__), PSTR("Failed to subscribe to firmware updates.\n"));
          }
          #endif

          coreroutineSyncClientAttr(2);
          coreroutineSetLEDBuzzer(3, false, 3, 50);         
          logger->info(PSTR(__func__),PSTR("IoT Connected!\n"));
        }
      }
    }

    tb.loop();
}


bool iotSendAttr(JsonDocument &doc){
  bool res = false;
  if( iotState.xSemaphoreThingsboard != NULL && WiFi.isConnected() && config.state.provSent && tb.connected() && config.state.accTkn != NULL){
    if( xSemaphoreTake( iotState.xSemaphoreThingsboard, ( TickType_t ) 10000 ) == pdTRUE )
    {
      res = tb.Send_Attribute_Json(doc);
      xSemaphoreGive( iotState.xSemaphoreThingsboard );
    }
    else
    {
      logger->verbose(PSTR(__func__), PSTR("No semaphore available.\n"));
    }
  }
  return res;
}

bool iotSendTele(JsonDocument &doc){
  bool res = false;
  if( iotState.xSemaphoreThingsboard != NULL && WiFi.isConnected() && config.state.provSent && tb.connected() && config.state.accTkn != NULL){
    if( xSemaphoreTake( iotState.xSemaphoreThingsboard, ( TickType_t ) 10000 ) == pdTRUE )
    {
      res = tb.Send_Telemetry_Json(doc);
      xSemaphoreGive( iotState.xSemaphoreThingsboard );
    }
    else
    {
      logger->verbose(PSTR(__func__), PSTR("No semaphore available.\n"));
    }
  }
  return res;
}
#endif