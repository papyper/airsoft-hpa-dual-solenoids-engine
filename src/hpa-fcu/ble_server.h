#ifndef BLE_CONFIG_H
#define BLE_CONFIG_H

#include <NimBLEDevice.h>

NimBLEServer* pServer = NULL;
NimBLECharacteristic* pConfigCharacteristic = NULL;
NimBLECharacteristic* pStateCharacteristic = NULL;
bool deviceConnected = false; 

// Helper function to split CSV strings
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
    pConfigCharacteristic->setValue((uint8_t*)csv.c_str(), csv.length());
}

class ConfigCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string rawValue = pCharacteristic->getValue();
        if (rawValue.length() > 0) {
            String value = String(rawValue.c_str());
            Serial.print("BLE Save Value: ");
            Serial.println(value);

            prefs.begin(PREF_NAME, false);
            
            modeSlot[0] = getValue(value, ',', 0).toInt();
            modeSlot[1] = getValue(value, ',', 1).toInt();
            prefs.putInt("slot0", modeSlot[0]);
            prefs.putInt("slot1", modeSlot[1]);

            int vIdx = 2;
            for (int i = 0; i < PROFILE_COUNT; i++) {
                String p = "p" + String(i);
                
                // Solenoid 1
                prefs.putUInt((p+"s1").c_str(), getValue(value, ',', vIdx++).toInt()); // sol1_open
                prefs.putUInt((p+"p1").c_str(), getValue(value, ',', vIdx++).toInt()); // sol1_peak
                prefs.putInt((p+"h1").c_str(), getValue(value, ',', vIdx++).toInt());  // sol1_hold_pwm
                prefs.putUInt((p+"d1").c_str(), getValue(value, ',', vIdx++).toInt()); // after_sol1
                
                // Solenoid 2
                prefs.putUInt((p+"s2").c_str(), getValue(value, ',', vIdx++).toInt()); // sol2_open
                prefs.putUInt((p+"p2").c_str(), getValue(value, ',', vIdx++).toInt()); // sol2_peak
                prefs.putInt((p+"h2").c_str(), getValue(value, ',', vIdx++).toInt());  // sol2_hold_pwm
                prefs.putUInt((p+"d2").c_str(), getValue(value, ',', vIdx++).toInt()); // after_sol2
                
                // Firing Mode
                prefs.putInt((p+"rpt").c_str(), getValue(value, ',', vIdx++).toInt());
                prefs.putInt((p+"rptr").c_str(), getValue(value, ',', vIdx++).toInt());
                prefs.putInt((p+"rps").c_str(), getValue(value, ',', vIdx++).toInt());
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
      Serial.print("BLE Client Address: ");
      Serial.println(connInfo.getAddress().toString().c_str());
  };

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.print("BLE Disconnected! Reason code: ");
    Serial.println(reason);
    NimBLEDevice::startAdvertising(); 
  }
};

void startBLE() {
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setMTU(512);
  
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); 

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);

  pConfigCharacteristic = pService->createCharacteristic(
                             CONFIG_CHAR_UUID,
                             NIMBLE_PROPERTY::READ |
                             NIMBLE_PROPERTY::WRITE
                           );
  pConfigCharacteristic->setCallbacks(new ConfigCallbacks());

  pStateCharacteristic = pService->createCharacteristic(
                             STATE_CHAR_UUID,
                             NIMBLE_PROPERTY::READ | 
                             NIMBLE_PROPERTY::NOTIFY
                           );
  
  updateConfigCharacteristic();
  pStateCharacteristic->setValue("Safe,Idle"); 

  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  
  pAdvertising->setName(BLE_NAME);
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start(); 
  
  configActive = true; 
  Serial.println("BLE Started.");
}

void stopBLE() {
  if (pServer != NULL) {
    NimBLEDevice::deinit(true);
  }
  configActive = false;
  deviceConnected = false;
}

void sendLiveStatesBLE() {
  static bool lastSafe = false;
  static int lastMode = -1;
  static bool lastFired = false;

  bool safe = selectorState == -1;
  int modeIndex = selectorState;
  bool fired = (triggerState == HIGH);

  bool stateChanged = (safe != lastSafe || modeIndex != lastMode || fired != lastFired);
  
  lastSafe = safe;
  lastMode = modeIndex;
  lastFired = fired;

  if (stateChanged && pStateCharacteristic != NULL) {
      String selStr;
      if (safe) selStr = "Safe";
      else if (modeIndex == 0) selStr = "Mode 1";
      else selStr = "Mode 2";

      String trigStr = fired ? "Fired" : "Idle";
      String stateStr = selStr + "," + trigStr;

      pStateCharacteristic->setValue((uint8_t*)stateStr.c_str(), stateStr.length());
      pStateCharacteristic->notify();
      Serial.println("BLE Notify: " + stateStr);
  }
}

#endif