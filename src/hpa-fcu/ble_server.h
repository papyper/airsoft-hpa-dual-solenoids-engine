#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

#include <NimBLEDevice.h>

NimBLEServer* pServer = NULL;
NimBLECharacteristic* pConfigCharacteristic = NULL;
NimBLECharacteristic* pStateCharacteristic = NULL;
bool deviceConnected = false; 
bool bleInitialized = false;

String makeUUID(String baseStr, String id, String pwd) {
    String str = id + pwd;
    String cleanBase = baseStr;
    cleanBase.replace("-", "");
    uint8_t bytes[16];
    for (int i = 0; i < 16; i++) {
        String hexByte = cleanBase.substring(i * 2, i * 2 + 2);
        bytes[i] = (uint8_t)strtol(hexByte.c_str(), NULL, 16);
    }
    for (int i = 0; i < str.length(); i++) {
        uint8_t c = str.charAt(i);
        bytes[i % 16] = (bytes[i % 16] ^ c) & 0xFF;
        bytes[(i + 1) % 16] = (bytes[(i + 1) % 16] + c) & 0xFF;
    }
    char hex[37];
    sprintf(hex, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5],
            bytes[6], bytes[7],
            bytes[8], bytes[9],
            bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return String(hex);
}

String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = { 0, -1 };
  int maxIndex = data.length() - 1;

  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
        found++;
        strIndex[0] = strIndex[1] + 1;
        strIndex[1] = (i == maxIndex) ? i+1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void updateConfigCharacteristic() {
    String csv = String(modeSlot[0]) + "," + String(modeSlot[1]);
    for (int i = 0; i < PROFILE_COUNT; i++) {
        FireMode &m = profiles[i];
        csv += "," + String(m.sol1_open) + "," + String(m.sol1_peak) + "," + String(m.sol1_hold_pwm) + "," + String(m.after_sol1) + "," +
               String(m.sol2_open) + "," + String(m.sol2_peak) + "," + String(m.sol2_hold_pwm) + "," + String(m.after_sol2) + "," +
               String(m.round_per_trigger) + "," + String(m.round_per_trigger_release) + "," +
               String(m.round_per_second);
    }
    csv += "," + String(safeVal) + "," + String(mode1Val) + "," + String(mode2Val) + "," +
           String(trigIdleVal) + "," + String(trigMaxVal) + "," + String(trigFirePct) + "," + String(trigRelPct) + "," +
           String(enable_pnh1 ? 1 : 0) + "," + String(enable_pnh2 ? 1 : 0) + "," +
           String(configHoldTime) + "," + String(sleepTimeoutMs) + "," + String(wakePollIntervalUs) + "," + 
           fcuPwd;
           
    if(pConfigCharacteristic != NULL) {
        pConfigCharacteristic->setValue((uint8_t*)csv.c_str(), csv.length());
    }
}

void sendLiveStatesBLE() {
  static bool lastSafe = false;
  static int lastMode = -1;
  static bool lastFired = false;
  static uint32_t lastShotCount = 0;
  static uint32_t lastHallSend = 0;

  bool safe = selectorState == -1;
  int modeIndex = selectorState;
  bool fired = (triggerState == HIGH);

  bool stateChanged = (safe != lastSafe || modeIndex != lastMode || fired != lastFired || shotCount != lastShotCount);
  
  bool timeToSendHall = (USE_HALL_SELECTOR || USE_HALL_TRIGGER) && (millis() - lastHallSend >= 100);

  if ((stateChanged || timeToSendHall) && pStateCharacteristic != NULL) {
      lastSafe = safe;
      lastMode = modeIndex;
      lastFired = fired;
      lastShotCount = shotCount;
      if (timeToSendHall) lastHallSend = millis();

      String selStr;
      if (safe) selStr = "Safe";
      else if (modeIndex == 0) selStr = "Mode 1";
      else selStr = "Mode 2";

      String trigStr = fired ? "Fired" : "Idle";
      String stateStr = selStr + "," + trigStr + "," + String(shotCount);

      if (USE_HALL_SELECTOR || USE_HALL_TRIGGER) {
          stateStr += "," + String(analogRead(SELECTOR_HALL_PIN)) + "," + String(analogRead(TRIGGER_HALL_PIN));
      }

      pStateCharacteristic->setValue((uint8_t*)stateStr.c_str(), stateStr.length());
      pStateCharacteristic->notify();
  }
}

void sendCalibrationDoneBLE(int state) {
  if (pStateCharacteristic != NULL && deviceConnected) {
      String msg = "CAL_DONE," + String(state) + 
                   "," + String(safeVal) + 
                   "," + String(mode1Val) + 
                   "," + String(mode2Val) +
                   "," + String(trigIdleVal) +
                   "," + String(trigMaxVal);
                   
      pStateCharacteristic->setValue((uint8_t*)msg.c_str(), msg.length());
      pStateCharacteristic->notify();
  }
}

class ConfigCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string rawValue = pCharacteristic->getValue();
        if (rawValue.length() > 0) {
            String value = String(rawValue.c_str());
            Serial.print("BLE Received: ");
            Serial.println(value);

            if (value.startsWith("CAL,")) {
                int stateToCalibrate = value.substring(4).toInt();
                startCalibration(stateToCalibrate); 
                return;
            }

            if (value == "RST_CNT") {
                shotCount = 0;
                sendLiveStatesBLE();
                return;
            }

            prefs.begin(PREF_NAME, false);
            
            modeSlot[0] = getValue(value, ',', 0).toInt();
            modeSlot[1] = getValue(value, ',', 1).toInt();
            prefs.putInt("slot0", modeSlot[0]);
            prefs.putInt("slot1", modeSlot[1]);

            int vIdx = 2;
            for (int i = 0; i < PROFILE_COUNT; i++) {
                String p = "p" + String(i);
                
                prefs.putUInt((p+"s1").c_str(), getValue(value, ',', vIdx++).toInt()); 
                prefs.putUInt((p+"p1").c_str(), getValue(value, ',', vIdx++).toInt()); 
                prefs.putInt((p+"h1").c_str(), getValue(value, ',', vIdx++).toInt());  
                prefs.putUInt((p+"d1").c_str(), getValue(value, ',', vIdx++).toInt()); 
                
                prefs.putUInt((p+"s2").c_str(), getValue(value, ',', vIdx++).toInt()); 
                prefs.putUInt((p+"p2").c_str(), getValue(value, ',', vIdx++).toInt()); 
                prefs.putInt((p+"h2").c_str(), getValue(value, ',', vIdx++).toInt());  
                prefs.putUInt((p+"d2").c_str(), getValue(value, ',', vIdx++).toInt()); 
                
                prefs.putInt((p+"rpt").c_str(), getValue(value, ',', vIdx++).toInt());
                prefs.putInt((p+"rptr").c_str(), getValue(value, ',', vIdx++).toInt());
                prefs.putInt((p+"rps").c_str(), getValue(value, ',', vIdx++).toInt());
            }

            int tf = getValue(value, ',', vIdx++).toInt();
            int tr = getValue(value, ',', vIdx++).toInt();
            if (tf > 0) prefs.putInt("th_fpct", tf); 
            if (tr > 0) prefs.putInt("th_rpct", tr);

            String pnh1Str = getValue(value, ',', vIdx++);
            if (pnh1Str != "") prefs.putBool("en_pnh1", pnh1Str.toInt() == 1);

            String pnh2Str = getValue(value, ',', vIdx++);
            if (pnh2Str != "") prefs.putBool("en_pnh2", pnh2Str.toInt() == 1);

            String holdStr = getValue(value, ',', vIdx++);
            if (holdStr != "") prefs.putUInt("cfg_hold", holdStr.toInt());

            String sleepStr = getValue(value, ',', vIdx++);
            if (sleepStr != "") prefs.putUInt("slp_tout", sleepStr.toInt());

            String wakeStr = getValue(value, ',', vIdx++);
            if (wakeStr != "") prefs.putUInt("wake_poll", wakeStr.toInt());

            String pwdStr = getValue(value, ',', vIdx++);
            if (pwdStr != "") {
                prefs.putString("fcu_pwd", pwdStr);
                fcuPwd = pwdStr;
            }

            prefs.end();
            loadConfig();
            updateConfigCharacteristic();
        }
    }
};

class ServerCallbacks: public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
      deviceConnected = true;
      Serial.print("BLE Client Connected.");
  };

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    if (configActive) {
        NimBLEDevice::startAdvertising(); 
    }
  }
};

ConfigCallbacks configCallbacksInst;
ServerCallbacks serverCallbacksInst;

void startBLE() {
  if (!bleInitialized) {
      NimBLEDevice::init(BLE_NAME "_" DEF_FCU_ID);
      NimBLEDevice::setMTU(512);
      NimBLEDevice::setPower(ESP_PWR_LVL_P9); 

      pServer = NimBLEDevice::createServer();
      pServer->setCallbacks(&serverCallbacksInst);
      
      String srvUUID = makeUUID(SERVICE_UUID_BASE, DEF_FCU_ID, fcuPwd);
      String cfgUUID = makeUUID(CONFIG_CHAR_UUID_BASE, DEF_FCU_ID, fcuPwd);
      String sttUUID = makeUUID(STATE_CHAR_UUID_BASE, DEF_FCU_ID, fcuPwd);

      NimBLEService *pService = pServer->createService(srvUUID.c_str());

      pConfigCharacteristic = pService->createCharacteristic(
                                   cfgUUID.c_str(),
                                   NIMBLE_PROPERTY::READ |
                                   NIMBLE_PROPERTY::WRITE
                                 );
      pConfigCharacteristic->setCallbacks(&configCallbacksInst);

      pStateCharacteristic = pService->createCharacteristic(
                                   sttUUID.c_str(),
                                   NIMBLE_PROPERTY::READ | 
                                   NIMBLE_PROPERTY::NOTIFY
                                 );
      
      updateConfigCharacteristic();
      pStateCharacteristic->setValue("Safe,Idle,0"); 

      pService->start();
      bleInitialized = true;
      
      NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
      pAdvertising->setName(BLE_NAME "_" DEF_FCU_ID);
      pAdvertising->addServiceUUID(srvUUID.c_str());
      pAdvertising->start(); 
  } else {
      NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
      pAdvertising->start(); 
  }
  
  configActive = true; 
}

void stopBLE() {
  configActive = false;
  setLED(false);
  logicalLedState = false;
  ledBlinkCount = 0;
  
  if (bleInitialized) {
      NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
      if (pAdvertising != nullptr) {
          pAdvertising->stop();
      }
      if (pServer != NULL) {
          std::vector<uint16_t> clients = pServer->getPeerDevices();
          for (size_t i = 0; i < clients.size(); i++) {
              pServer->disconnect(clients[i]);
          }
      }
  }
  
  deviceConnected = false;
}

#endif