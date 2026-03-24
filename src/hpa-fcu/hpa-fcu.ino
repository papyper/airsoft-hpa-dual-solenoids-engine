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

uint32_t shotCount = 0;

// ===== FIRING VARIABLES =====
uint32_t tStart = 0;
FireMode* currentMode;
int shotsRemaining = 0;
int trigFireThreshold = 0;
int trigRelThreshold = 0;
bool trigInverted = false;

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

void precalcTrigger() {
  int range = trigMaxVal - trigIdleVal;
  trigInverted = (range < 0);
  
  trigFireThreshold = trigIdleVal + (range * trigFirePct) / 100;
  trigRelThreshold  = trigIdleVal + (range * trigRelPct) / 100;
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

  precalcTrigger();
  
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

void handleHallCalibration() {
  static uint32_t lastCalibRead = 0;
  if (millis() - lastCalibRead < 10) return; // Sample every 10ms
  lastCalibRead = millis();

  int pin = (calibState == 3 || calibState == 4) ? TRIGGER_HALL_PIN : SELECTOR_HALL_PIN;
  calibSum += analogRead(pin);
  calibSamples++;

  if (millis() - calibStartTime >= CALIB_DURATION_MS) {
    int finalVal = (calibSamples > 0) ? (calibSum / calibSamples) : analogRead(pin);

    prefs.begin(PREF_NAME, false);
    if (calibState == 0) { // Safe
      safeVal = finalVal;
      prefs.putInt("hs_val", finalVal);
    } else if (calibState == 1) { // Mode 1
      mode1Val = finalVal;
      prefs.putInt("hm1_val", finalVal);
    } else if (calibState == 2) { // Mode 2
      mode2Val = finalVal;
      prefs.putInt("hm2_val", finalVal);
    } else if (calibState == 3) { // Trigger Idle
      trigIdleVal = finalVal;
      prefs.putInt("th_idle", finalVal);
    } else if (calibState == 4) { // Trigger Full
      trigMaxVal = finalVal;
      prefs.putInt("th_max", finalVal);
    }
    prefs.end();
    
    Serial.printf("Calibrated state %d -> Center Val: %d\n", calibState, finalVal);

    int lastState = calibState;
    calibState = -1;

    safeHolding = false;

    precalcTrigger();

    updateConfigCharacteristic();
    sendCalibrationDoneBLE(lastState);
  }
}

// ================= (NORMAL OPERATION) =================
void handleHallSelector() {
  int raw = analogRead(SELECTOR_HALL_PIN);

  if (!hallInitialized) {
    filteredHall = raw;
    hallInitialized = true;
  } else {
    filteredHall = (filteredHall * 3 + raw) / 4;
  }
  int hall = filteredHall;

  int dSafe  = abs(hall - safeVal);
  int dMode1 = abs(hall - mode1Val);
  int dMode2 = abs(hall - mode2Val);

  if (selectorState == -1) { 
    dMode1 += SELECTOR_HYST_LIGHT;
    dMode2 += SELECTOR_HYST_HEAVY;
  } 
  else if (selectorState == 0) { 
    dSafe  += SELECTOR_HYST_LIGHT;
    dMode2 += SELECTOR_HYST_HEAVY;
  } 
  else if (selectorState == 1) { 
    dMode1 += SELECTOR_HYST_HEAVY;
    dSafe  += SELECTOR_HYST_HEAVY;
  }

  if (dSafe <= dMode1 && dSafe <= dMode2) selectorState = -1;
  else if (dMode1 <= dSafe && dMode1 <= dMode2) selectorState = 0;
  else selectorState = 1;
}

// ================= SELECTOR MASTER =================
void readSelector() {
  bool safe = false;

  if (!USE_HALL_SELECTOR) {
    safe = (digitalRead(SAFE_PIN) == LOW);
    if(safe) selectorState = -1;
    else selectorState = (digitalRead(MODE_PIN) == LOW) ? 1 : 0;
  } else {
    handleHallSelector();
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
    else { filteredTrigHall = (filteredTrigHall * 3 + rawHall) / 4; }
    
    int hall = filteredTrigHall;
    
    if (triggerState == LOW) {
      if (!trigInverted) isPulled = (hall >= trigFireThreshold);
      else               isPulled = (hall <= trigFireThreshold);
    } else {
      if (!trigInverted) isPulled = (hall > trigRelThreshold);
      else               isPulled = (hall < trigRelThreshold);
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

  shotCount++;

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

  shotCount++;
  
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
    if (hardwareLedState) setLED(false);
    logicalLedState = false;
    ledBlinkCount = 0;
    return;
  }
  
  if (ledBlinkCount < (selectorState + 1)) {
    if (now - ledTimer > 150) {
      ledTimer = now; 
      logicalLedState = !logicalLedState; 
      setLED(logicalLedState);
      
      if (!logicalLedState) { 
        ledBlinkCount++;
        if (ledBlinkCount >= (selectorState + 1)) {
          ledPauseTimer = now; 
        }
      }
    }
  } else {
    setLED(false);
    if (now - ledPauseTimer > 2000) {
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
    handleHallCalibration();
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