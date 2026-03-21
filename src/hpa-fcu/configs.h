#ifndef CONFIGS
#define CONFIGS

#define PREF_NAME "fcu"

// ===== SEEED XIAOZHI ESP32-C3 PIN ===== 
const int SOL1_PIN = D10;
const int SOL2_PIN = D3;
const int TRIGGER_PIN = D7;
const int MODE_PIN = D6;
const int SAFE_PIN = D5;
const int LED_PIN = D4;
const int TRIGGER_HALL_PIN = D1;
const int SELECTOR_HALL_PIN = D2;

// ===== CONFIGURATION MODE TOGGLE =====
const uint32_t CONFIG_HOLD_TIME = 5000000; // 5 seconds

// ===== PWM CONFIGURATION (ESP32 Core v3.x.x API) =====
#define PWM_FREQ 20000 // 20kHz
#define PWM_RES 8      // res 8-bit (0 - 255)

// ===== SELECTOR CONFIGURATION =====
// Set to true to use Hall sensor, false for traditional physical switches
#define USE_HALL_SELECTOR true
#define SELECTOR_POLL_MS 50 //50ms, 20 selector read per second

// ===== FIRING CONFIGURATION =====
#define PROFILE_COUNT 5

// ===== BLE =====
#define BLE_NAME               "PaPyPer_FCU"
#define SERVICE_UUID           "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CONFIG_CHAR_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define STATE_CHAR_UUID        "1c9441a1-f3b1-4f16-8eb5-7c37a6b72a6b"

#endif 