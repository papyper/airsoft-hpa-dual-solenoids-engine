#ifndef CONFIGS
#define CONFIGS

#define PREF_NAME "fcu"

#define DEF_FCU_ID "3p100"
#define DEF_FCU_PWD "123456"

const int SOL1_PIN = D10;
const int SOL2_PIN = D3;
const int TRIGGER_PIN = D7;
const int MODE_PIN = D6;
const int SAFE_PIN = D5;
const int LED_PIN = D4;
const int TRIGGER_HALL_PIN = D1;
const int SELECTOR_HALL_PIN = D2;

const uint32_t DEF_CONFIG_HOLD_TIME = 3000000;

#define PWM_FREQ 20000 
#define PWM_RES 8      

#define TRIGGER_DEBOUNCE_MICROS 3000
#define CALIB_DURATION_MS       5000

#define USE_HALL_SELECTOR true
#define SELECTOR_HYST_LIGHT 80
#define SELECTOR_HYST_HEAVY 350
#define DEF_HALL_SAFE  2370
#define DEF_HALL_MODE1 2000
#define DEF_HALL_MODE2 1200

#define USE_HALL_TRIGGER true
#define TRIGGER_FILTER_ALPHA 0.8f  
#define DEF_TRIG_IDLE     2350
#define DEF_TRIG_MAX      1800
#define DEF_TRIG_FIRE_PCT 20
#define DEF_TRIG_REL_PCT  10

#define PROFILE_COUNT 5

#define BLE_NAME               "PaPyPer_FCU"
#define SERVICE_UUID_BASE      "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CONFIG_CHAR_UUID_BASE  "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define STATE_CHAR_UUID_BASE   "1c9441a1-f3b1-4f16-8eb5-7c37a6b72a6b"

#define DEF_SLEEP_TIMEOUT_MS       120000 
#define DEF_WAKE_POLL_INTERVAL_US  400000 

#endif