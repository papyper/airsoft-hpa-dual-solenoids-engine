#include <Preferences.h>
#include "configs.h"

bool configActive = false;
uint32_t safeHoldStart = 0;
bool safeHolding = false;
bool configToggleDone = false;
Preferences prefs;

struct FireMode {
  uint32_t sol1_open;
  uint32_t sol1_peak;     
  int      sol1_hold_pwm; 
  uint32_t after_sol1;

  uint32_t sol2_open;
  uint32_t sol2_peak;     
  int      sol2_hold_pwm; 
  uint32_t after_sol2;

  int round_per_trigger;
  int round_per_trigger_release;
  int round_per_second;

  uint32_t t_sol1_peak_end; 
  uint32_t t_sol1_off;
  uint32_t t_sol2_on;
  uint32_t t_sol2_peak_end; 
  uint32_t t_sol2_off;
  uint32_t t_base_cycle; 
  uint32_t t_cycle_end;  
};

FireMode profiles[PROFILE_COUNT];
int modeSlot[2] = {0, 1};

// ===== STATE MACHINE =====
enum FCUState { 
  S_IDLE, S_SOL1_PEAK, S_SOL1_HOLD, S_WAIT_SOL2, 
  S_SOL2_PEAK, S_SOL2_HOLD, S_WAIT_CYCLE_END 
};
FCUState fcuState = S_IDLE;

// ===== FIRING VARIABLES =====
uint32_t tStart = 0;
FireMode* currentMode;
int shotsRemaining = 0;

// ===== HARDWARE OUTPUT CACHE =====
int sol1CurrentPwm = -1; 
int sol2CurrentPwm = -1;
bool hardwareLedState = false;

// ===== TRIGGER DEBOUNCE =====
bool physicalTriggerState = LOW;
bool triggerState = LOW;
bool lastTriggerState = LOW;
bool triggerEdge = false;
bool pendingReleaseFire = false; 
uint32_t lastDebounce = 0;

// ===== SELECTOR (HALL & SWITCH) =====
int selectorState = -1; // -1: safe, 0: mode 0, 1: mode 1

// Hall Sensor Calibration Centers (Nearest Neighbor)
int safeVal = DEF_HALL_SAFE;
int mode1Val = DEF_HALL_MODE1;
int mode2Val = DEF_HALL_MODE2;

// ===== TRIGGER HALL CALIBRATION & STATE =====
int trigIdleVal = DEF_TRIG_IDLE;
int trigMaxVal = DEF_TRIG_MAX;
int trigFirePct = DEF_TRIG_FIRE_PCT;
int trigRelPct = DEF_TRIG_REL_PCT;

// ===== CALIBRATION VARIABLES =====
volatile int calibState = -1;
volatile uint32_t calibStartTime = 0;
volatile long calibSum = 0;
volatile int calibSamples = 0;
int filteredHall = 0;
bool hallInitialized = false;

// ===== LED TIMERS =====
uint32_t ledTimer = 0;
uint32_t ledPauseTimer = 0;
int ledBlinkCount = 0;
bool logicalLedState = false;

// ================= PRE-CALCULATOR =================
void precalcProfile(FireMode& m) {
  uint32_t t = 0;
  
  uint32_t actual_sol1_peak = (m.sol1_peak > m.sol1_open) ? m.sol1_open : m.sol1_peak;
  m.t_sol1_peak_end = t + actual_sol1_peak;
  t += m.sol1_open;
  m.t_sol1_off = t;

  t += m.after_sol1;
  m.t_sol2_on = t;

  uint32_t actual_sol2_peak = (m.sol2_peak > m.sol2_open) ? m.sol2_open : m.sol2_peak;
  m.t_sol2_peak_end = t + actual_sol2_peak;
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
  prefs.begin(PREF_NAME, true);
  for (int i = 0; i < PROFILE_COUNT; i++) {
    String p = "p" + String(i);
    profiles[i].sol1_open = prefs.getUInt((p+"s1").c_str(), 30000);
    profiles[i].sol1_peak = prefs.getUInt((p+"p1").c_str(), 5000); 
    profiles[i].sol1_hold_pwm = prefs.getInt((p+"h1").c_str(), 100); 
    profiles[i].after_sol1 = prefs.getUInt((p+"d1").c_str(), 10000);
    
    profiles[i].sol2_open = prefs.getUInt((p+"s2").c_str(), 40000);
    profiles[i].sol2_peak = prefs.getUInt((p+"p2").c_str(), 5000);
    profiles[i].sol2_hold_pwm = prefs.getInt((p+"h2").c_str(), 100);
    profiles[i].after_sol2 = prefs.getUInt((p+"d2").c_str(), 10000);
    
    profiles[i].round_per_trigger = prefs.getInt((p+"rpt").c_str(), i==0 ? 1 : -1);
    profiles[i].round_per_trigger_release = prefs.getInt((p+"rptr").c_str(), 0); 
    profiles[i].round_per_second = prefs.getInt((p+"rps").c_str(), 10);
    
    precalcProfile(profiles[i]);
  }
  modeSlot[0] = prefs.getInt("slot0", 0);
  modeSlot[1] = prefs.getInt("slot1", 1);

  safeVal = prefs.getInt("hs_val", DEF_HALL_SAFE);
  mode1Val = prefs.getInt("hm1_val", DEF_HALL_MODE1);
  mode2Val = prefs.getInt("hm2_val", DEF_HALL_MODE2);

  trigIdleVal = prefs.getInt("th_idle", DEF_TRIG_IDLE);
  trigMaxVal = prefs.getInt("th_max", DEF_TRIG_MAX);
  trigFirePct = prefs.getInt("th_fpct", DEF_TRIG_FIRE_PCT);
  trigRelPct = prefs.getInt("th_rpct", DEF_TRIG_REL_PCT);
  
  prefs.end();
}

#include "ble_server.h"

// ================= (CALIBRATION) =================
void startCalibration(int stateIndex) {
  if (calibState != -1) return; 
  
  calibStartTime = millis();
  calibSum = 0;
  calibSamples = 0;
  
  Serial.printf("Starting async calibration for state %d...\n", stateIndex);
  
  calibState = stateIndex; 
}

void handleCalibration() {
  static uint32_t lastCalibRead = 0;
  if (millis() - lastCalibRead < 10) return; // Sample every 10ms
  lastCalibRead = millis();

  int pin = (calibState == 3 || calibState == 4) ? TRIGGER_HALL_PIN : SELECTOR_HALL_PIN;
  calibSum += analogRead(pin);
  calibSamples++;

  if (millis() - calibStartTime >= CALIB_DURATION_MS) {
    int finalVal = (calibSamples > 0) ? (calibSum / calibSamples) : analogRead(pin);

    prefs.begin(PREF_NAME, false);
    if (calibState == 0) {
      safeVal = finalVal;
      prefs.putInt("hs_val", finalVal);
    } else if (calibState == 1) {
      mode1Val = finalVal;
      prefs.putInt("hm1_val", finalVal);
    } else if (calibState == 2) {
      mode2Val = finalVal;
      prefs.putInt("hm2_val", finalVal);
    } else if (calibState == 3) {
      trigIdleVal = finalVal;
      prefs.putInt("th_idle", finalVal);
    } else if (calibState == 4) {
      trigMaxVal = finalVal;
      prefs.putInt("th_max", finalVal);
    }
    prefs.end();
    
    Serial.printf("Calibrated state %d -> Center Val: %d\n", calibState, finalVal);

    int lastState = calibState;
    calibState = -1;

    safeHolding = false;

    updateConfigCharacteristic();
    sendCalibrationDoneBLE(lastState);
  }
}

// ================= (NORMAL OPERATION) =================
void handleSelectorNormal() {
  int raw = analogRead(SELECTOR_HALL_PIN);

  // ===== FILTER (EMA - Integer Math) =====
  if (!hallInitialized) {
    filteredHall = raw;
    hallInitialized = true;
  } else {
    filteredHall = (filteredHall * 3 + raw) / 4; // Alpha 0.25
  }

  int hall = filteredHall;

  // ===== SORT VALUES =====
  int v0 = safeVal;
  int v1 = mode1Val;
  int v2 = mode2Val;

  if (v0 > v1) { int t = v0; v0 = v1; v1 = t; }
  if (v1 > v2) { int t = v1; v1 = v2; v2 = t; }
  if (v0 > v1) { int t = v0; v0 = v1; v1 = t; }

  // ===== MIDPOINT =====
  int b1 = (v0 + v1) / 2;
  int b2 = (v1 + v2) / 2;

  // ===== MAP STATE =====
  int state0 = (v0 == safeVal) ? -1 : (v0 == mode1Val ? 0 : 1);
  int state1 = (v1 == safeVal) ? -1 : (v1 == mode1Val ? 0 : 1);
  int state2 = (v2 == safeVal) ? -1 : (v2 == mode1Val ? 0 : 1);

  int newState = selectorState; 

  // ===== HYSTERESIS LOGIC =====
  switch (selectorState) {
    case -1: // SAFE
      if (hall > b1 + HALL_HYSTERESIS) {
        newState = (hall <= b2) ? state1 : state2;
      }
      break;

    case 0: // MODE 0
      if (hall < b1 - HALL_HYSTERESIS) {
        newState = state0;
      } else if (hall > b2 + HALL_HYSTERESIS) {
        newState = state2;
      }
      break;

    case 1: // MODE 1
      if (hall < b2 - HALL_HYSTERESIS) {
        newState = (hall <= b1) ? state0 : state1;
      }
      break;

    default:
      if (hall <= b1) newState = state0;
      else if (hall <= b2) newState = state1;
      else newState = state2;
      break;
  }

  selectorState = newState;
}

// ================= SELECTOR MASTER =================
void readSelector() {
  bool safe = false;

  if (!USE_HALL_SELECTOR) {
    safe = (digitalRead(SAFE_PIN) == LOW);
    if(safe) selectorState = -1;
    else selectorState = (digitalRead(MODE_PIN) == LOW) ? 1 : 0;
  } else {
    handleSelectorNormal();
    safe = (selectorState == -1);
  }

  if (safe && (physicalTriggerState == HIGH || triggerState == HIGH)) {
    if (!safeHolding) {
      safeHolding = true;
      safeHoldStart = micros();
      configToggleDone = false;
    } else if (!configToggleDone && (micros() - safeHoldStart >= CONFIG_HOLD_TIME)) {
      if (configActive) stopBLE();
      else startBLE();
      configToggleDone = true;
    }
  } else safeHolding = false;
}

// ================= HARDWARE WRITERS =================
void setSol1PWM(int pwm) {
  if (sol1CurrentPwm != pwm) { 
    sol1CurrentPwm = pwm;
    ledcWrite(SOL1_PIN, pwm); 
  }
}

void setSol2PWM(int pwm) {
  if (sol2CurrentPwm != pwm) { 
    sol2CurrentPwm = pwm;
    ledcWrite(SOL2_PIN, pwm); 
  }
}

void setLED(bool on) {
  if (hardwareLedState != on) { 
    hardwareLedState = on; 
    digitalWrite(LED_PIN, on); 
  }
}

// ================= TRIGGER LOGIC =================
void readTrigger() {
  bool physicalReading = !digitalRead(TRIGGER_PIN);
  
  static bool lastPhysReading = LOW;
  static uint32_t lastPhysDebounce = 0;
  if (physicalReading != lastPhysReading) lastPhysDebounce = micros();
  if ((micros() - lastPhysDebounce) > TRIGGER_DEBOUNCE_MICROS) {
    physicalTriggerState = physicalReading;
  }
  lastPhysReading = physicalReading;

  bool isPulled = false;

  if (USE_HALL_TRIGGER) {
    int rawHall = analogRead(TRIGGER_HALL_PIN);
    static int filteredTrigHall = 0;
    static bool trigHallInit = false;
    if (!trigHallInit) { filteredTrigHall = rawHall; trigHallInit = true; }
    else { filteredTrigHall = (filteredTrigHall * 3 + rawHall * 1) / 4; }
    
    int hall = filteredTrigHall;
    
    int range = trigMaxVal - trigIdleVal;
    int pct = 0;
    if (range != 0) {
      pct = ((hall - trigIdleVal) * 100) / range;
    }
    
    if (range < 0) { // reverse handling
      pct = ((trigIdleVal - hall) * 100) / (-range);
    }
    
    if (triggerState == LOW) {
       if (pct >= trigFirePct) isPulled = true;
       else isPulled = false;
    } else {
       if (pct <= trigRelPct) isPulled = false;
       else isPulled = true;
    }
  } else {
    isPulled = physicalTriggerState;
  }

  if (isPulled != lastTriggerState) lastDebounce = micros();
  
  if ((micros() - lastDebounce) > TRIGGER_DEBOUNCE_MICROS) {
    if (isPulled != triggerState) {
      bool risingEdge = (isPulled == true && triggerState == false);
      bool fallingEdge = (isPulled == false && triggerState == true);
      
      triggerState = isPulled;
      if (risingEdge) triggerEdge = true;
      
      if (fallingEdge) pendingReleaseFire = true;
      if (risingEdge) pendingReleaseFire = false; 
    }
  }
  lastTriggerState = isPulled;
}

// ================= FIRING STATE MACHINE =================
void startFire(FireMode* m, int shots) {
  currentMode = m;
  shotsRemaining = shots; 

  tStart = micros();
  setSol1PWM(255); 
  setSol2PWM(0);
  
  fcuState = S_SOL1_PEAK;
  updateFire(); 
}

void nextShot() {
  if (currentMode->round_per_trigger == -1 && triggerState == LOW) {
    fcuState = S_IDLE; setSol1PWM(0); setSol2PWM(0); return;
  }
  
  if (shotsRemaining > 0) shotsRemaining--;
  
  if (shotsRemaining == 0) {
    fcuState = S_IDLE; setSol1PWM(0); setSol2PWM(0); return;
  }
  
  tStart = micros();
  setSol1PWM(255); 
  setSol2PWM(0);
  fcuState = S_SOL1_PEAK;
}

void updateFire() {
  if (fcuState == S_IDLE) return;
  
  uint32_t dt = micros() - tStart; 

  if (fcuState == S_SOL1_PEAK) {
    if (dt >= currentMode->t_sol1_off) {
      setSol1PWM(0); fcuState = S_WAIT_SOL2;
    } else if (dt >= currentMode->t_sol1_peak_end) {
      setSol1PWM(currentMode->sol1_hold_pwm); fcuState = S_SOL1_HOLD;
    }
  }

  if (fcuState == S_SOL1_HOLD && dt >= currentMode->t_sol1_off) {
    setSol1PWM(0); fcuState = S_WAIT_SOL2;
  }
  
  if (fcuState == S_WAIT_SOL2 && dt >= currentMode->t_sol2_on) {
    setSol2PWM(255); fcuState = S_SOL2_PEAK;
  }
  
  if (fcuState == S_SOL2_PEAK) {
    if (dt >= currentMode->t_sol2_off) {
      setSol2PWM(0); fcuState = S_WAIT_CYCLE_END;
    } else if (dt >= currentMode->t_sol2_peak_end) {
      setSol2PWM(currentMode->sol2_hold_pwm); fcuState = S_SOL2_HOLD;
    }
  }

  if (fcuState == S_SOL2_HOLD && dt >= currentMode->t_sol2_off) {
    setSol2PWM(0); fcuState = S_WAIT_CYCLE_END;
  }

  if (fcuState == S_WAIT_CYCLE_END) {
    bool bypassRPS = (shotsRemaining == 0 && pendingReleaseFire);
    uint32_t targetWaitTime = bypassRPS ? currentMode->t_base_cycle : currentMode->t_cycle_end;
    if (dt >= targetWaitTime) nextShot(); 
  }
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

  ledcAttach(SOL1_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(SOL2_PIN, PWM_FREQ, PWM_RES);
  ledcWrite(SOL1_PIN, 0); 
  ledcWrite(SOL2_PIN, 0);

  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  pinMode(MODE_PIN, INPUT_PULLUP);
  pinMode(SAFE_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(TRIGGER_HALL_PIN, INPUT);
  pinMode(SELECTOR_HALL_PIN, INPUT);

  loadConfig();
}

void loop() {
  if (calibState != -1) {
    handleCalibration();
    return;
  }

  readTrigger();
  readSelector();
  updateLED();

  if (configActive) sendLiveStatesBLE();

  if (selectorState == -1) {
    fcuState = S_IDLE; 
    setSol1PWM(0); 
    setSol2PWM(0);
    pendingReleaseFire = false;
    triggerEdge = false;
    return;
  }

  int modeIndex = selectorState;
  FireMode* m = &profiles[modeSlot[modeIndex]];

  if (fcuState == S_IDLE && !configActive) {
    if (m->round_per_trigger == -1) {
      if (triggerState == HIGH) startFire(m, -1); 
      pendingReleaseFire = false;
      triggerEdge = false;
    } else {
      if (triggerEdge) {
        triggerEdge = false;
        startFire(m, m->round_per_trigger); 
      } else if (pendingReleaseFire && m->round_per_trigger_release > 0) {
        startFire(m, m->round_per_trigger_release); 
        pendingReleaseFire = false; 
      } else if (pendingReleaseFire) {
        pendingReleaseFire = false; 
      }
    }
  }
  
  updateFire();
}