#include <Preferences.h>
#include "pin.h"

// ===== CONFIGURATION MODE TOGGLE =====
const uint32_t CONFIG_HOLD_TIME = 5000000; // 5 seconds

bool configActive = false;
uint32_t safeHoldStart = 0;
bool safeHolding = false;
bool configToggleDone = false;
Preferences prefs;

// ===== FIRING CONFIGURATION =====
#define PROFILE_COUNT 5

struct FireMode {
  uint32_t sol1_open;
  uint32_t after_sol1;
  uint32_t sol2_open;
  uint32_t after_sol2;
  int round_per_trigger;
  int round_per_trigger_release;
  int round_per_second;

  uint32_t t_sol1_off;
  uint32_t t_sol2_on;
  uint32_t t_sol2_off;
  uint32_t t_base_cycle; 
  uint32_t t_cycle_end;  
};

FireMode profiles[PROFILE_COUNT];
int modeSlot[2] = {0, 1};

// ===== STATE MACHINE =====
enum FCUState { 
  S_IDLE, 
  S_SOL1_ACTIVE, 
  S_WAIT_SOL2, 
  S_SOL2_ACTIVE, 
  S_WAIT_CYCLE_END 
};
FCUState fcuState = S_IDLE;

// ===== FIRING VARIABLES =====
uint32_t tStart = 0;
FireMode* currentMode;
int shotsRemaining = 0;

// ===== HARDWARE OUTPUT CACHE =====
bool sol1State = false;
bool sol2State = false;
bool hardwareLedState = false;

// ===== TRIGGER DEBOUNCE =====
bool triggerState = LOW;
bool lastTriggerState = LOW;
bool triggerEdge = false;
bool pendingReleaseFire = false; 
uint32_t lastDebounce = 0;
const uint32_t debounceDelay = 3000;

// ===== SELECTOR =====
int selectorState = -1; // -1: safe, 0: mode 0, 1: mode 1

// ===== LED TIMERS =====
uint32_t ledTimer = 0;
uint32_t ledPauseTimer = 0;
int ledBlinkCount = 0;
bool logicalLedState = false;

// ================= PRE-CALCULATOR =================
void precalcProfile(FireMode& m) {
  uint32_t t = 0;
  
  t += m.sol1_open;
  m.t_sol1_off = t;

  t += m.after_sol1;
  m.t_sol2_on = t;

  t += m.sol2_open;
  m.t_sol2_off = t;

  t += m.after_sol2;
  m.t_base_cycle = t; 

  if (m.round_per_second > 0) {
    uint32_t target = 1000000 / m.round_per_second;
    if (target > t) t = target;
  }
  m.t_cycle_end = t;
}

// ================= CONFIGURATION =================
void loadConfig() {
  prefs.begin("fcu", true);
  for (int i = 0; i < PROFILE_COUNT; i++) {
    String p = "p" + String(i);
    profiles[i].sol1_open = prefs.getUInt((p+"s1").c_str(), 30000);
    profiles[i].after_sol1 = prefs.getUInt((p+"d1").c_str(), 10000);
    profiles[i].sol2_open = prefs.getUInt((p+"s2").c_str(), 40000);
    profiles[i].after_sol2 = prefs.getUInt((p+"d2").c_str(), 10000);
    profiles[i].round_per_trigger = prefs.getInt((p+"rpt").c_str(), i==0 ? 1 : -1);
    profiles[i].round_per_trigger_release = prefs.getInt((p+"rptr").c_str(), 0); 
    profiles[i].round_per_second = prefs.getInt((p+"rps").c_str(), 10);
    
    precalcProfile(profiles[i]);
  }
  modeSlot[0] = prefs.getInt("slot0", 0);
  modeSlot[1] = prefs.getInt("slot1", 1);
  prefs.end();
}

#include "ble_server.h"

// ================= HARDWARE WRITERS =================
void setSol1(bool on) {
  if (sol1State != on) { sol1State = on; digitalWrite(SOL1_PIN, on); }
}
void setSol2(bool on) {
  if (sol2State != on) { sol2State = on; digitalWrite(SOL2_PIN, on); }
}
void setLED(bool on) {
  if (hardwareLedState != on) { hardwareLedState = on; digitalWrite(LED_PIN, on); }
}

// ================= TRIGGER LOGIC =================
void readTrigger() {
  bool reading = !digitalRead(TRIGGER_PIN);
  if (reading != lastTriggerState) lastDebounce = micros();
  
  if ((micros() - lastDebounce) > debounceDelay) {
    if (reading != triggerState) {
      bool risingEdge = (reading == HIGH && triggerState == LOW);
      bool fallingEdge = (reading == LOW && triggerState == HIGH);
      
      triggerState = reading;
      triggerEdge = risingEdge;
      
      if (fallingEdge) pendingReleaseFire = true;
      if (risingEdge) pendingReleaseFire = false; 
    }
  }
  lastTriggerState = reading;
}

// ================= FIRING STATE MACHINE =================
void startFire(FireMode* m, int shots) {
  currentMode = m;
  shotsRemaining = shots; 

  tStart = micros();
  setSol1(true);
  setSol2(false);
  
  fcuState = S_SOL1_ACTIVE;
  updateFire(); 
}

void nextShot() {
  if (currentMode->round_per_trigger == -1 && triggerState == LOW) {
    fcuState = S_IDLE; setSol1(false); setSol2(false); return;
  }
  
  if (shotsRemaining > 0) shotsRemaining--;
  
  if (shotsRemaining == 0) {
    fcuState = S_IDLE; setSol1(false); setSol2(false); return;
  }
  
  tStart = micros();
  setSol1(true);
  setSol2(false);
  fcuState = S_SOL1_ACTIVE;
}

void updateFire() {
  if (fcuState == S_IDLE) return;
  
  uint32_t dt = micros() - tStart; 

  if (fcuState == S_SOL1_ACTIVE && dt >= currentMode->t_sol1_off) {
    setSol1(false);
    fcuState = S_WAIT_SOL2;
  }
  
  if (fcuState == S_WAIT_SOL2 && dt >= currentMode->t_sol2_on) {
    setSol2(true);
    fcuState = S_SOL2_ACTIVE;
  }
  
  if (fcuState == S_SOL2_ACTIVE && dt >= currentMode->t_sol2_off) {
    setSol2(false);
    fcuState = S_WAIT_CYCLE_END;
  }
  
  if (fcuState == S_WAIT_CYCLE_END) {
    bool bypassRPS = (shotsRemaining == 0 && pendingReleaseFire);
    uint32_t targetWaitTime = bypassRPS ? currentMode->t_base_cycle : currentMode->t_cycle_end;

    if (dt >= targetWaitTime) {
      nextShot(); 
    }
  }
}

// ================= SELECTOR =================
void readSelector() {
  bool safe = !digitalRead(SAFE_PIN) == LOW;
  if(safe){
    selectorState = -1;
  }else{
    selectorState = !digitalRead(MODE_PIN) ? 1 : 0;
  }
  if (safe && triggerState == HIGH) {
    if (!safeHolding) {
      safeHolding = true;
      safeHoldStart = micros();
      configToggleDone = false;
    } else if (!configToggleDone && (micros() - safeHoldStart >= CONFIG_HOLD_TIME)) {
      if (configActive) {
        stopBLE();
      } else {
        startBLE();
      }
      configToggleDone = true;
    }
  } else safeHolding = false;
}

// ================= LED INDICATORS =================
void updateLED() {
  uint32_t now = millis();
  
  if (configActive) {
    if (now - ledTimer > 100) {
      ledTimer = now; 
      logicalLedState = !logicalLedState; 
      setLED(logicalLedState);
    }
    return; 
  }

  if (selectorState == -1) {
    setLED(false); 
    return;
  }

  if (ledBlinkCount < (selectorState + 1)) {
    if (now - ledTimer > 150) {
      ledTimer = now; 
      logicalLedState = !logicalLedState; 
      setLED(logicalLedState);
      if (!logicalLedState) ledBlinkCount++;
    }
  } else {
    setLED(false);
    if (now - ledPauseTimer > 2000) {
      ledPauseTimer = now; 
      ledBlinkCount = 0;
    }
  }
}

// ================= MAIN SETUP & LOOP =================
void setup() {
  Serial.begin(115200);
  pinMode(SOL1_PIN, OUTPUT);
  pinMode(SOL2_PIN, OUTPUT);
  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  pinMode(MODE_PIN, INPUT_PULLUP);
  pinMode(SAFE_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  pinMode(TRIGGER_HALL_PIN, INPUT);
  
  loadConfig();
}

void testHallSensor() {
    int value = analogRead(TRIGGER_HALL_PIN);

    Serial.print("Hall value: ");
    Serial.println(value);

    delay(100);
}

void loop() {
  //testHallSensor();
  readTrigger();
  readSelector();
  updateLED();

  // Execute Network Managers
  if (configActive) {
    sendLiveStatesBLE();
  }

  if (selectorState == -1) {
    fcuState = S_IDLE; 
    setSol1(false); 
    setSol2(false);
    pendingReleaseFire = false; 
    return;
  }

  int modeIndex = selectorState;
  FireMode* m = &profiles[modeSlot[modeIndex]];

  if (fcuState == S_IDLE && !configActive) {
    if (m->round_per_trigger == -1) {
      if (triggerState == HIGH) startFire(m, -1); 
      pendingReleaseFire = false; 
    } else {
      if (triggerEdge) {
        startFire(m, m->round_per_trigger); 
      } else if (pendingReleaseFire && m->round_per_trigger_release > 0) {
        startFire(m, m->round_per_trigger_release); 
        pendingReleaseFire = false; 
      } else if (pendingReleaseFire) {
        pendingReleaseFire = false; 
      }
    }
  }
  
  triggerEdge = false;
  updateFire();
}