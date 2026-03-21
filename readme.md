# PaPyPer HPA FCU ⚡

PaPyPer HPA FCU is an open-source Fire Control Unit designed for HPA (High Pressure Air) Airsoft replicas, powered by the **ESP32-C3** microcontroller (optimized for boards like the Seeed Studio XIAO).

This project features wireless configuration via **Bluetooth Low Energy (BLE)** directly through a web browser (Web Bluetooth API), completely eliminating the need for physical LCD screens or third-party mobile apps.

---

## 🌟 Key Features

* **Independent Dual Solenoid Control:** Supports separate timing adjustments for two solenoids (Nozzle & Poppet).
* **Solenoid Protection (Peak & Hold PWM):** Automatically reduces the holding current (Hold PWM) after the initial high-power opening phase (Peak), preventing solenoids from overheating during heavy use.
* **5 Customizable Profiles:** Pre-configure up to 5 different firing profiles, saved permanently to the ESP32's non-volatile memory (Preferences).
* **Multiple Firing Modes:** Supports Semi-Auto, Full-Auto, Burst (Rounds per pull), and Binary trigger (Rounds per release).
* **Web BLE Dashboard:** A modern web interface that allows you to:
  * Easily adjust settings on the fly.
  * Monitor Trigger and Selector states in real-time.
* **Secure BLE Activation:** Bluetooth is off by default to save battery and prevent unauthorized access. It is only activated via a specific hardware input sequence.

---

## ⚙️ Pinout & Wiring

Designed for the **Seeed Studio XIAO ESP32-C3** (or equivalent ESP32-C3 boards):

| Name | Pin | Function | Configuration |
| :--- | :--- | :--- | :--- |
| **SOL1** | `D10` | Solenoid 1 Output (Requires MOSFET) | PWM (20kHz, 8-bit) |
| **SOL2** | `D3` | Solenoid 2 Output (Requires MOSFET) | PWM (20kHz, 8-bit) |
| **TRIGGER** | `D7` | Trigger Switch (Micro/Optical) | Input Pullup (LOW = Fired) |
| **MODE** | `D6` | Fire Selector | Input Pullup |
| **SAFE** | `D5` | Safe Selector | Input Pullup |
| **LED** | `D4` | Status LED | Output |
| **TRIG_HALL**| `D2` | Hall Effect Trigger (Optional) | Input |
| **SEL_HALL** | `D3` | Hall Effect Selector (Optional) | Input |

---

## 🚀 Usage & Configuration

### 1. Activating BLE Mode
To conserve battery and ensure security during gameplay, Bluetooth is disabled by default. To turn it on/off:
1. Switch the fire selector to **SAFE**.
2. Squeeze and hold the **Trigger** for about **5 seconds**.
3. The LED will begin to blink, indicating the FCU is broadcasting under the name `PaPyPer_FCU`.

### 2. Connecting to the Web Dashboard
1. Open the `ble_configs.html` file (or host it locally/online) using a browser that supports the **Web Bluetooth API** (e.g., Google Chrome, Microsoft Edge, Opera). *Note: Safari currently does not support this feature.*
2. Click the **Connect** button.
3. Select `PaPyPer_FCU` from the browser's device pairing popup.
4. Once connected, the live Trigger/Selector status will display, and you can begin modifying your profiles.

### 3. Understanding Firing Parameters

Each profile allows you to fine-tune the following parameters (all time values are in Microseconds - µs):

#### Solenoid 1 & 2 Settings
* **Sol 1 / 2 Open (µs):** Total time the solenoid valve remains open.
* **Sol 1 / 2 Peak (µs):** The initial time full voltage (100% Duty Cycle) is applied to overcome physical/spring resistance.
* **Sol 1 / 2 Hold PWM (0-255):** The reduced current level maintained after the peak time. Set from `0 - 255` (e.g., 100 ~ 40% power) to keep the valve open without overheating.
* **Delay After Sol (µs):** The delay/dwell time before the system proceeds to the next step.

#### Firing Mode Settings
* **Rounds Per Pull:** The number of rounds fired when the trigger is pulled. Ex:
  * Set `1` = Semi
  * Set `3` = 3-round burst
  * Set `-1` = Full Auto.
* **Rounds Per Release:** The number of rounds fired when the trigger is released (Used for Binary trigger setups).
* **Rounds Per Second (RPS):** The global cycle time limiter. It dictates the maximum rate of fire for Full-Auto and Burst modes. In Semi-Auto, it acts as a forced delay between individual shots, effectively capping how fast the trigger can be spammed. *(Note: Binary trigger release shots deliberately bypass this delay for immediate response).*

After adjusting your values, click **Save Configuration**. The parameters are sent instantly via BLE and saved to the ESP32's flash memory.

---

## 🛠 Build Requirements
* **IDE:** Arduino IDE (v2.x) or VSCode with PlatformIO.
* **Required Libraries:** * `Preferences` (Built into the ESP32 core)
  * `NimBLE-Arduino` (Install via Library Manager; highly recommended for better RAM optimization compared to the standard BLE library).