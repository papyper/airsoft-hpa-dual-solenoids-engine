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
bool triggerState = LOW;
bool lastTriggerState = LOW;
bool triggerEdge = false;
bool pendingReleaseFire = false; 
uint32_t lastDebounce = 0;
const uint32_t debounceDelay = 3000;

// ===== SELECTOR (HALL & SWITCH) =====
int selectorState = -1; // -1: safe, 0: mode 0, 1: mode 1
float smoothedHall = -1; // Realtime EMA filter to prevent lag

// Hall Sensor Calibration Centers (Nearest Neighbor)
int safeVal = 2400;
int mode1Val = 2000;
int mode2Val = 1500;

// ===== CALIBRATION VARIABLES =====
volatile int calibState = -1;
volatile uint32_t calibStartTime = 0;
volatile long calibSum = 0;
volatile int calibSamples = 0;
const uint32_t CALIB_DURATION = 5000;

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

  safeVal = prefs.getInt("hs_val", 2400);
  mode1Val = prefs.getInt("hm1_val", 2000);
  mode2Val = prefs.getInt("hm2_val", 1500);
  
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

  calibSum += analogRead(SELECTOR_HALL_PIN);
  calibSamples++;

  if (millis() - calibStartTime >= CALIB_DURATION) {
    int finalVal = (calibSamples > 0) ? (calibSum / calibSamples) : analogRead(SELECTOR_HALL_PIN);

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
    }
    prefs.end();
    
    Serial.printf("Calibrated state %d -> Center Val: %d\n", calibState, finalVal);

    int lastState = calibState;
    calibState = -1;

    updateConfigCharacteristic();
    sendCalibrationDoneBLE(lastState);
  }
}

// ================= (NORMAL OPERATION) =================
void handleSelectorNormal() {
  int rawHall = analogRead(SELECTOR_HALL_PIN);
  
  // Realtime EMA filter: Lọc mượt nhiễu điện
  if (smoothedHall < 0) smoothedHall = rawHall;
  else smoothedHall = smoothedHall * 0.8 + rawHall * 0.2;

  // 1. Tính toán khoảng cách (gap) giữa các mốc
  int gapSafeM1 = abs(safeVal - mode1Val);
  int gapM1M2   = abs(mode1Val - mode2Val);

  // 2. Định nghĩa "Bán kính hút" (Capture Radius) cho từng mốc
  // Hệ số 0.35 nghĩa là vùng nhận diện chiếm 35% khoảng cách.
  // Bạn phải gạt qua 65% quãng đường thì nó mới chịu nhảy số, triệt tiêu vụ non-linear.
  int rSafe = gapSafeM1 * 0.35;
  int rM1   = min(gapSafeM1, gapM1M2) * 0.35; // Mode 1 nằm giữa nên lấy gap nhỏ hơn cho an toàn
  int rM2   = gapM1M2 * 0.35;

  // 3. Kiểm tra xem có đang nằm chắc chắn trong vùng nào không
  if (abs(smoothedHall - safeVal) <= rSafe) {
    selectorState = -1; // Safe
  } else if (abs(smoothedHall - mode1Val) <= rM1) {
    selectorState = 0;  // Mode 1
  } else if (abs(smoothedHall - mode2Val) <= rM2) {
    selectorState = 1;  // Mode 2
  }
  
  // LƯU Ý: Nếu giá trị smoothedHall rơi vào khoảng giữa (lớn hơn 35% nhưng chưa tới mốc kia),
  // khối lệnh if-else trên sẽ bị bỏ qua -> selectorState KHÔNG bị ghi đè, 
  // nó tự động giữ nguyên trạng thái cũ (Hysteresis).
}

// ================= SELECTOR MASTER =================
void readSelector() {
  bool safe = false;

  if (!USE_HALL_SELECTOR) {
    safe = !digitalRead(SAFE_PIN) == LOW;
    if(safe) selectorState = -1;
    else selectorState = !digitalRead(MODE_PIN) ? 1 : 0;
  } else {
    if (calibState != -1) {
      handleCalibration();
    } else {
      handleSelectorNormal();
    }
    
    safe = (selectorState == -1);
  }

  if (safe && triggerState == HIGH) {
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

  // Khởi tạo bộ lọc với giá trị ngay lúc bật máy
  smoothedHall = analogRead(SELECTOR_HALL_PIN);
  
  loadConfig();
}

void loop() {
  readTrigger();
  readSelector();
  updateLED();

  if (configActive) sendLiveStatesBLE();

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