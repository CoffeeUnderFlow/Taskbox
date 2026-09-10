# 📦 TaskBox - Fully Integrated Smart Task Console

An ESP8266-powered smart task management device featuring an I2C LCD, capacitive touch controls, a buzzer audio engine, and a secure local web dashboard. Designed and built by **[CoffeeUnderFlow](https://coffeeunderflow.in/)**.

---

## ✨ Features

- **Smart Task Queue:** Supports up to 4 prioritized tasks (Levels A–D) with automatic sorting and smooth text scrolling.
- **Touch-to-Complete:** Easily complete active tasks via a simple double-tap gesture on the touch sensor.
- **EEPROM Persistence & Safety:** Securely saves tasks and settings across reboots, protected by a custom `magic` header check and **CRC32 data integrity validation**.
- **Secure Web Console:** Fully responsive, dark-mode web interface protected by HTTP Basic Authentication. Manage tasks and device settings remotely over Wi-Fi.
- **NTP Time & Alarms:** Automatically synchronizes time via NTP, featuring customizable Morning and Evening alarms with interactive touch-dismissal.
- **Night Mode:** Long-hold the touch sensor (20s) to toggle Night Mode, which dims the backlight and displays a minimalist clock-and-task view.
- **OTA Updates:** Supports Over-The-Air firmware updates for easy maintenance.

---

## 🛠️ Hardware Requirements

* **Microcontroller:** NodeMCU ESP8266
* **Display:** 16x2 I2C LCD Display (Address: `0x27`)
* **Touch Sensor:** Capacitive Touch Module (e.g., TTP223) connected to **D5**
* **Audio:** Active/Passive Buzzer connected to **D7**

---

## 📌 Pinout Mapping

| Component | ESP8266 Pin | Notes |
| :--- | :--- | :--- |
| **Touch Sensor** | `D5` | Input pin for touch gestures |
| **Buzzer** | `D7` | Output pin for audio cues & alarms |
| **LCD SDA** | `D2` | I2C Data line |
| **LCD SCL** | `D1` | I2C Clock line |

---

## ⚙️ Configuration

Before compiling and uploading the sketch in the Arduino IDE, update your local user configuration credentials in `Taskbox.ino`:

```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* WEB_USER = "taskbox";
const char* WEB_PASS = "taskbox";
const char* OTA_PASS = "taskbox";


## 🚀 Getting Started

1. Clone or download this repository into your Arduino sketchbook directory.
2. Ensure you have the required libraries installed (`LiquidCrystal_I2C`, `ESP8266WiFi`, etc.).
3. Open `Taskbox.ino` in the Arduino IDE.
4. Select your **NodeMCU 1.0 (ESP-12E Module)** board and correct COM port.
5. Compile and upload to your ESP8266!

---

## 📄 License & Credits

Developed by **CoffeeUnderFlow**. Visit [coffeeunderflow.in](https://coffeeunderflow.in/) for more projects.