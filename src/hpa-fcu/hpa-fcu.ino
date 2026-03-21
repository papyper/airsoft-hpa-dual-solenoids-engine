#include <Preferences.h>
#include "configs.h"

bool configActive = false;
uint32_t safeHoldStart = 0;
bool safeHolding = false;
bool configToggleDone = false;
Preferences prefs;

struct FireMode {
  // Solenoid 1
  uint32_t sol1_open;
  uint32_t sol1_peak;     
  int      sol1_hold_pwm; 
  uint32_t after_sol1;

  // Solenoid 2
  uint32_t sol2_open;
  uint32_t sol2_peak;     
  int      sol2_hold_pwm; 
  uint32_t after_sol2;

  int round_per_trigger;
  int round_per_trigger_release;
  int round_per_second;

  // Pre-calculated times
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
  S_IDLE, 
  S_SOL1_PEAK, 
  S_SOL1_HOLD, 
  S_WAIT_SOL2, 
  S_SOL2_PEAK, 
  S_SOL2_HOLD, 
  S_WAIT_CYCLE_END 
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
  
  // Solenoid 1
  uint32_t actual_sol1_peak = (m.sol1_peak > m.sol1_open) ? m.sol1_open : m.sol1_peak;
  m.t_sol1_peak_end = t + actual_sol1_peak;
  t += m.sol1_open;
  m.t_sol1_off = t;

  t += m.after_sol1;
  m.t_sol2_on = t;

  // Solenoid 2
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
  prefs.end();
}


#include "ble_server.h"

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

  // --- SOLENOID 1 LOGIC ---
  if (fcuState == S_SOL1_PEAK) {
    if (dt >= currentMode->t_sol1_off) {
      setSol1PWM(0);
      fcuState = S_WAIT_SOL2;
    } 
    else if (dt >= currentMode->t_sol1_peak_end) {
      setSol1PWM(currentMode->sol1_hold_pwm);
      fcuState = S_SOL1_HOLD;
    }
  }

  if (fcuState == S_SOL1_HOLD && dt >= currentMode->t_sol1_off) {
    setSol1PWM(0);
    fcuState = S_WAIT_SOL2;
  }
  
  // --- WAIT BETWEEN 2 SOLENOID ---
  if (fcuState == S_WAIT_SOL2 && dt >= currentMode->t_sol2_on) {
    setSol2PWM(255); 
    fcuState = S_SOL2_PEAK;
  }
  
  // --- SOLENOID 2 LOGIC ---
  if (fcuState == S_SOL2_PEAK) {
    if (dt >= currentMode->t_sol2_off) {
      setSol2PWM(0);
      fcuState = S_WAIT_CYCLE_END;
    } 
    else if (dt >= currentMode->t_sol2_peak_end) {
      setSol2PWM(currentMode->sol2_hold_pwm);
      fcuState = S_SOL2_HOLD;
    }
  }

  if (fcuState == S_SOL2_HOLD && dt >= currentMode->t_sol2_off) {
    setSol2PWM(0);
    fcuState = S_WAIT_CYCLE_END;
  }

  // --- CYCLE END ---
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

  ledcAttach(SOL1_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(SOL2_PIN, PWM_FREQ, PWM_RES);

  ledcWrite(SOL1_PIN, 0); 
  ledcWrite(SOL2_PIN, 0);

  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  pinMode(MODE_PIN, INPUT_PULLUP);
  pinMode(SAFE_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(TRIGGER_HALL_PIN, INPUT);
  
  loadConfig();
}

void loop() {
  readTrigger();
  readSelector();
  updateLED();

  if (configActive) {
    sendLiveStatesBLE();
  }

  if (selectorState == -1) {
    fcuState = S_IDLE; 
    setSol1PWM(0); 
    setSol2PWM(0);
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