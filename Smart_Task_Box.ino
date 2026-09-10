/*
  TASK BOX V3 - FINAL FULLY INTEGRATED
  ESP8266 / NodeMCU
  LCD 16x2 I2C @ 0x27
  Touch: D5
  Buzzer: D7
  Branding: CoffeeUnderFlow (https://coffeeunderflow.in/)
*/

#include <ESP8266mDNS.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <time.h>

// =========================
// 1. USER CONFIGURATION
// =========================
const char* WIFI_SSID = "STRANGER'S HOTSPOT";
const char* WIFI_PASS = "nothingisfree";

const char* WEB_USER = "taskbox";
const char* WEB_PASS = "taskbox";
const char* OTA_PASS = "taskbox";

constexpr long TZ_OFFSET_SECONDS = 19800; // IST
constexpr int EEPROM_SIZE = 512;
constexpr uint32_t EEPROM_MAGIC = 0x54424F58UL; // "TBOX"
constexpr uint16_t EEPROM_VERSION = 3;

constexpr uint8_t MAX_TASKS = 4;
constexpr uint8_t TASK_TEXT_LEN = 40;

// Hardware Pins
constexpr uint8_t TOUCH_PIN = D5;
constexpr uint8_t BUZZER_PIN = D7;
constexpr uint8_t LCD_ADDR = 0x27;

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
ESP8266WebServer server(80);

// =========================
// 2. SETTINGS
// =========================
struct Settings {
  uint8_t morningHour = 7;
  uint8_t morningMinute = 0;
  uint8_t eveningHour = 19;
  uint8_t eveningMinute = 0;

  uint32_t alarmTimeoutMs = 600000UL;  // 10 min
  uint8_t alarmDismissTaps = 15;

  uint32_t taskHoldMs = 2000UL;        // 2 sec
  uint32_t nightHoldMs = 20000UL;      // 20 sec (Hardcoded behavior)

  uint32_t rotationDelayMs = 180000UL; // 3 min
  uint32_t rotationSpeedMs = 3000UL;   // 3 sec
};

Settings settings;

// =========================
// 3. TASK STORAGE
// =========================
struct Task {
  char text[TASK_TEXT_LEN + 1];
  char priority;
};

struct PersistedData {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  Settings settings;
  uint8_t taskCount;
  Task tasks[MAX_TASKS];
  char lastPriority;
  uint32_t crc;
};

Task taskQueue[MAX_TASKS];
uint8_t taskCount = 0;
char lastPriority = 'C';
bool storageDirty = false;

// =========================
// 4. CRC32 & EEPROM HANDLING
// =========================
uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; ++i) {
      uint32_t mask = -(crc & 1UL);
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

uint32_t persistedCRC(const PersistedData& data) {
  return crc32(reinterpret_cast<const uint8_t*>(&data), offsetof(PersistedData, crc));
}

void setDefaults() {
  settings = Settings();
  taskCount = 0;
  lastPriority = 'C';
  memset(taskQueue, 0, sizeof(taskQueue));
}

bool validPriority(char p) {
  return p >= 'A' && p <= 'D';
}

bool validTaskText(const String& s) {
  if (s.length() == 0 || s.length() > TASK_TEXT_LEN) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c < 32 || c == 127) return false;
  }
  return true;
}

void sortTasks() {
  for (uint8_t i = 1; i < taskCount; ++i) {
    Task key = taskQueue[i];
    int8_t j = i - 1;
    while (j >= 0 && taskQueue[j].priority > key.priority) {
      taskQueue[j + 1] = taskQueue[j];
      --j;
    }
    taskQueue[j + 1] = key;
  }
}

void clearTaskSlot(uint8_t i) {
  memset(&taskQueue[i], 0, sizeof(Task));
}

void saveStorage() {
  if (!storageDirty) return;

  PersistedData data{};
  data.magic = EEPROM_MAGIC;
  data.version = EEPROM_VERSION;
  data.settings = settings;
  data.taskCount = taskCount;
  memcpy(data.tasks, taskQueue, sizeof(taskQueue));
  data.lastPriority = lastPriority;
  data.crc = persistedCRC(data);

  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, data);
  bool ok = EEPROM.commit();
  EEPROM.end();

  if (ok) storageDirty = false;
}

void loadStorage() {
  EEPROM.begin(EEPROM_SIZE);
  PersistedData data{};
  EEPROM.get(0, data);
  EEPROM.end();

  bool valid = true;
  if (data.magic != EEPROM_MAGIC) valid = false;
  if (data.version != EEPROM_VERSION) valid = false;
  if (data.taskCount > MAX_TASKS) valid = false;
  if (data.crc != persistedCRC(data)) valid = false;

  if (!valid) {
    setDefaults();
    storageDirty = true;
    saveStorage();
    return;
  }

  settings = data.settings;
  taskCount = data.taskCount;
  memcpy(taskQueue, data.tasks, sizeof(taskQueue));
  lastPriority = validPriority(data.lastPriority) ? data.lastPriority : 'C';

  for (uint8_t i = 0; i < taskCount; ++i) {
    taskQueue[i].text[TASK_TEXT_LEN] = '\0';
    if (!validPriority(taskQueue[i].priority) || strlen(taskQueue[i].text) > TASK_TEXT_LEN) {
      clearTaskSlot(i);
      for (uint8_t j = i; j + 1 < taskCount; ++j) {
        taskQueue[j] = taskQueue[j + 1];
      }
      clearTaskSlot(taskCount - 1);
      --taskCount;
      --i;
    }
  }
  sortTasks();
}

// =========================
// 5. AUDIO ENGINE
// =========================
enum AudioCue {
  AUDIO_NONE, AUDIO_CLICK, AUDIO_ADDED, AUDIO_COMPLETE,
  AUDIO_HOURLY, AUDIO_NIGHT_ENTER, AUDIO_NIGHT_EXIT
};

AudioCue audioCue = AUDIO_NONE;
uint8_t audioStep = 0;
uint32_t audioStepStarted = 0;

struct Note { uint16_t frequency; uint16_t duration; };

const Note melodyAdded[] = { {523, 80}, {659, 80}, {784, 80}, {1046, 150} };
const Note melodyComplete[] = { {392, 100}, {523, 100}, {659, 250} };
const Note melodyNightEnter[] = { {440, 150}, {330, 250} };
const Note melodyNightExit[] = { {330, 150}, {440, 250} };

void stopAudio() {
  noTone(BUZZER_PIN);
  audioCue = AUDIO_NONE;
  audioStep = 0;
}

void playAudio(int cue) {
  extern bool alarmActive; 
  if (alarmActive) return;
  
  audioCue = (AudioCue)cue;
  audioStep = 0;
  audioStepStarted = millis();
}

void handleAudio() {
  extern bool alarmActive;
  if (audioCue == AUDIO_NONE || alarmActive) return;

  const uint32_t now = millis();

  if (audioCue == AUDIO_CLICK) {
    if (audioStep == 0) {
      tone(BUZZER_PIN, 2500);
      audioStep = 1;
      audioStepStarted = now;
    } else if (now - audioStepStarted >= 15) {
      stopAudio();
    }
    return;
  }

  const Note* notes = nullptr;
  uint8_t noteCount = 0;
  uint16_t gap = 10;

  switch (audioCue) {
    case AUDIO_ADDED: notes = melodyAdded; noteCount = 4; break;
    case AUDIO_COMPLETE: notes = melodyComplete; noteCount = 3; break;
    case AUDIO_NIGHT_ENTER: notes = melodyNightEnter; noteCount = 2; break;
    case AUDIO_NIGHT_EXIT: notes = melodyNightExit; noteCount = 2; break;
    case AUDIO_HOURLY:
      if (audioStep == 0) { tone(BUZZER_PIN, 659); audioStep = 1; audioStepStarted = now; }
      else if (audioStep == 1 && now - audioStepStarted >= 200) { tone(BUZZER_PIN, 523); audioStep = 2; audioStepStarted = now; }
      else if (audioStep == 2 && now - audioStepStarted >= 250) { stopAudio(); }
      return;
    default: stopAudio(); return;
  }

  if (audioStep < noteCount) {
    if (audioStep == 0 || now - audioStepStarted >= notes[audioStep - 1].duration + gap) {
      tone(BUZZER_PIN, notes[audioStep].frequency);
      audioStepStarted = now;
      ++audioStep;
    }
  } else if (now - audioStepStarted >= notes[noteCount - 1].duration) {
    stopAudio();
  }
}

// =========================
// 6. TIME / ALARM MANAGER
// =========================
const char* DAYS[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
bool timeValid = false;
bool ntpStarted = false;
uint32_t lastNtpStart = 0;

bool alarmActive = false;
uint32_t alarmStartMs = 0;
uint8_t alarmTapCount = 0;
String alarmMessage;

int32_t lastMinuteKey = -1;
int32_t lastHourKey = -1;
int32_t lastMorningAlarmDay = -1;
int32_t lastEveningAlarmDay = -1;

uint32_t makeDayKey(const tm& t) {
  return (uint32_t)(t.tm_year + 1900) * 400UL + (uint32_t)t.tm_yday;
}

int32_t makeMinuteKey(const tm& t) {
  return (int32_t)((t.tm_year + 1900) * 366L + t.tm_yday) * 1440L + (int32_t)t.tm_hour * 60L + t.tm_min;
}

void startNtpIfNeeded(bool force = false) {
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  if (!force && ntpStarted && now - lastNtpStart < 30000UL) return;

  configTime(TZ_OFFSET_SECONDS, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  ntpStarted = true;
  lastNtpStart = now;
}

void startAlarm(bool morning, const tm& t) {
  uint32_t dayKey = makeDayKey(t);
  int32_t& lastAlarm = morning ? lastMorningAlarmDay : lastEveningAlarmDay;

  if ((int32_t)dayKey == lastAlarm) return;

  lastAlarm = dayKey;
  alarmActive = true;
  alarmStartMs = millis();
  alarmTapCount = 0;
  alarmMessage = morning ? "SUN IS UP" : "SUN IS DOWN";

  stopAudio();
}

void handleTimeAndAlarms() {
  time_t nowTime = time(nullptr);
  tm timeinfo;
  localtime_r(&nowTime, &timeinfo);

  bool newTimeValid = (timeinfo.tm_year >= 120);

  if (newTimeValid && !timeValid) {
    lastMinuteKey = makeMinuteKey(timeinfo) - 1;
    lastHourKey = makeMinuteKey(timeinfo) / 60 - 1;
  }
  timeValid = newTimeValid;

  if (!timeValid) return;

  const int32_t minuteKey = makeMinuteKey(timeinfo);

  if (minuteKey != lastMinuteKey) {
    lastMinuteKey = minuteKey;

    const bool morning = (timeinfo.tm_hour == settings.morningHour && timeinfo.tm_min == settings.morningMinute);
    const bool evening = (timeinfo.tm_hour == settings.eveningHour && timeinfo.tm_min == settings.eveningMinute);

    if (morning) startAlarm(true, timeinfo);
    else if (evening) startAlarm(false, timeinfo);

    const int32_t hourKey = minuteKey / 60;
    if (hourKey != lastHourKey) {
      lastHourKey = hourKey;
      if (!morning && !evening && !alarmActive) playAudio(AUDIO_HOURLY);
    }
  }

  if (alarmActive) {
    if (millis() - alarmStartMs >= settings.alarmTimeoutMs) {
      alarmActive = false;
      noTone(BUZZER_PIN);
      return;
    }

    static uint32_t lastSirenToggle = 0;
    if (millis() - lastSirenToggle >= 100UL) {
      lastSirenToggle = millis();
      static bool highTone = false;
      highTone = !highTone;
      tone(BUZZER_PIN, highTone ? 800 : 600);
    }
  }
}

// =========================
// 7. TOUCH INPUT & STATE FLAGS
// =========================
bool nightModeActive = false;
bool lastTouchRaw = LOW;
bool stableTouch = LOW;
uint32_t touchDebounceAt = 0;
uint32_t touchStartedAt = 0;
bool holdActionTriggered = false;
bool nightActionTriggered = false;

// Display feedback notification flags
bool completionVisible = false;
uint32_t completionStartedAt = 0;
bool newTaskVisible = false;
uint32_t newTaskStartedAt = 0;

uint8_t taskTapCount = 0;
uint32_t lastTaskTapTime = 0;
uint32_t nightLightOnUntil = 0;

void completeCurrentTask() {
  if (taskCount == 0) return;
  for (uint8_t i = 0; i + 1 < taskCount; ++i) taskQueue[i] = taskQueue[i + 1];
  clearTaskSlot(taskCount - 1);
  --taskCount;
  storageDirty = true;
  completionVisible = true;
  completionStartedAt = millis();
  playAudio(AUDIO_COMPLETE);
}

void onTouchPressed() {
  touchStartedAt = millis();
  holdActionTriggered = false;
  nightActionTriggered = false;
  playAudio(AUDIO_CLICK);

  if (alarmActive) {
    ++alarmTapCount;
    if (alarmTapCount >= settings.alarmDismissTaps) {
      alarmActive = false;
      noTone(BUZZER_PIN);
      playAudio(AUDIO_COMPLETE);
    }
  } 
  else if (nightModeActive) {
    nightLightOnUntil = millis() + 60000UL; // Wake backlight for 1 min
  }
  else {
    uint32_t now = millis();
    if (now - lastTaskTapTime > 1200UL) {
      taskTapCount = 0; 
    }
    taskTapCount++;
    lastTaskTapTime = now;

    if (taskTapCount >= 3) {
      taskTapCount = 0;
      completeCurrentTask();
    }
  }
}

void onTouchReleased() {}

void handleInput() {
  bool raw = digitalRead(TOUCH_PIN);
  uint32_t now = millis();

  if (raw != lastTouchRaw) {
    lastTouchRaw = raw;
    touchDebounceAt = now;
  }

  if (now - touchDebounceAt >= 35UL && raw != stableTouch) {
    stableTouch = raw;
    if (stableTouch) onTouchPressed();
    else onTouchReleased();
  }

  // Night Mode long hold detection (20 sec)
  if (stableTouch && !alarmActive && !holdActionTriggered) {
    uint32_t duration = now - touchStartedAt;
    if (duration >= settings.nightHoldMs) {
      holdActionTriggered = true;
      nightActionTriggered = true;
      nightModeActive = !nightModeActive;
      playAudio(nightModeActive ? AUDIO_NIGHT_ENTER : AUDIO_NIGHT_EXIT);
    }
  }
}

// =========================
// 8. DISPLAY MANAGER
// =========================
byte blockFull[8] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F }; 
byte blockTop[8]  = { 0x1F, 0x1F, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00 }; 
byte blockBot[8]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x1F, 0x1F }; 

void setupCustomChars() {
  lcd.createChar(0, blockFull);
  lcd.createChar(1, blockTop);
  lcd.createChar(2, blockBot);
}

char cachedRow0[17] = "";
char cachedRow1[17] = "";

void writeRowIfChanged(uint8_t row, const char* text) {
  char rowBuffer[17];
  memset(rowBuffer, ' ', 16);
  rowBuffer[16] = '\0';

  size_t len = strlen(text);
  if (len > 16) len = 16;
  memcpy(rowBuffer, text, len);

  char* cache = (row == 0) ? cachedRow0 : cachedRow1;
  if (strncmp(cache, rowBuffer, 17) == 0) return;

  strncpy(cache, rowBuffer, 17);
  lcd.setCursor(0, row);
  lcd.print(rowBuffer);
}

void formatTaskRow(char* out, size_t outSize, uint8_t index) {
  if (index >= taskCount) {
    snprintf(out, outSize, "No active tasks");
    return;
  }

  char full[64];
  snprintf(full, sizeof(full), "%c:%s", taskQueue[index].priority, taskQueue[index].text);
  size_t len = strlen(full);

  if (len <= 16) {
    snprintf(out, outSize, "%s", full);
    return;
  }

  uint32_t scrollCycle = (millis() / 400UL);
  int maxScroll = (int)len - 16;
  int cycleLength = maxScroll + 7;
  int pos = scrollCycle % cycleLength;
  if (pos > maxScroll) pos = maxScroll;

  strncpy(out, full + pos, 16);
  out[16] = '\0';
}

uint32_t lastRotationAt = 0;
uint32_t rotationStepAt = 0;
uint8_t rotationIndex = 0;
bool rotating = false;

void resetRotation() {
  rotating = false;
  rotationIndex = 0;
  lastRotationAt = millis();
  rotationStepAt = millis();
}

void handleDisplay() {
  // 1. New Task Notification Feedback -> Force Backlight ON
  if (newTaskVisible) {
    lcd.backlight();
    if (millis() - newTaskStartedAt < 2000UL) {
      char row0[17];
      if (timeValid) {
        time_t nowTime = time(nullptr);
        tm t; localtime_r(&nowTime, &t);
        snprintf(row0, sizeof(row0), "%02d:%02d:%02d %s %02d", t.tm_hour, t.tm_min, t.tm_sec, DAYS[t.tm_wday], t.tm_mday);
      } else {
        snprintf(row0, sizeof(row0), "New Task Added!");
      }
      writeRowIfChanged(0, row0);
      writeRowIfChanged(1, " * NEW TASK! * ");
      return;
    }
    newTaskVisible = false;
  }

  // 2. Task Completion Feedback -> Force Backlight ON
  if (completionVisible) {
    lcd.backlight(); 
    if (millis() - completionStartedAt < 2000UL) {
      char row0[17];
      if (timeValid) {
        time_t nowTime = time(nullptr);
        tm t; localtime_r(&nowTime, &t);
        snprintf(row0, sizeof(row0), "%02d:%02d:%02d %s %02d", t.tm_hour, t.tm_min, t.tm_sec, DAYS[t.tm_wday], t.tm_mday);
      } else {
        snprintf(row0, sizeof(row0), "Task Completed!");
      }
      writeRowIfChanged(0, row0);
      writeRowIfChanged(1, "  * WELL DONE! *  ");
      return;
    }
    completionVisible = false;
  }

  // 3. Alarm Active -> Force Backlight ON
  if (alarmActive) {
    lcd.backlight();
    char row0[17], row1[17];
    if (timeValid) {
      time_t nowTime = time(nullptr);
      tm t; localtime_r(&nowTime, &t);
      snprintf(row0, sizeof(row0), "%02d:%02d:%02d %s %02d", t.tm_hour, t.tm_min, t.tm_sec, DAYS[t.tm_wday], t.tm_mday);
    } else snprintf(row0, sizeof(row0), "ALARM");

    snprintf(row1, sizeof(row1), "TAPS %u/%u", alarmTapCount, settings.alarmDismissTaps);
    writeRowIfChanged(0, row0);
    writeRowIfChanged(1, row1);
    return;
  }

  // 4. Night Mode State
  if (nightModeActive && timeValid) {
    if (millis() < nightLightOnUntil) {
      lcd.backlight(); 
    } else {
      lcd.noBacklight(); 
    }

    time_t nowTime = time(nullptr);
    tm t; localtime_r(&nowTime, &t);
    char row0[17], row1[17];
    
    snprintf(row0, sizeof(row0), "   %02d:%02d:%02d   ", t.tm_hour, t.tm_min, t.tm_sec);
    formatTaskRow(row1, sizeof(row1), 0);
    
    writeRowIfChanged(0, row0);
    writeRowIfChanged(1, row1);
    return;
  } else {
    lcd.backlight(); 
  }

  // 5. Normal Operating State
  char row0[17], row1[17];
  if (timeValid) {
    time_t nowTime = time(nullptr);
    tm t; localtime_r(&nowTime, &t);
    snprintf(row0, sizeof(row0), "%02d:%02d:%02d %s %02d", t.tm_hour, t.tm_min, t.tm_sec, DAYS[t.tm_wday], t.tm_mday);
  } else snprintf(row0, sizeof(row0), "Syncing Clock...");

  if (taskCount == 0) {
    snprintf(row1, sizeof(row1), "No active tasks");
    writeRowIfChanged(0, row0);
    writeRowIfChanged(1, row1);
    return;
  }

  if (!rotating && taskCount > 1 && millis() - lastRotationAt >= settings.rotationDelayMs) {
    rotating = true;
    rotationIndex = 1;
    rotationStepAt = millis();
  }

  uint8_t displayIndex = 0;
  if (rotating) {
    if (rotationIndex >= taskCount) resetRotation();
    else {
      displayIndex = rotationIndex;
      if (millis() - rotationStepAt >= settings.rotationSpeedMs) {
        rotationStepAt = millis();
        ++rotationIndex;
        if (rotationIndex >= taskCount) { resetRotation(); displayIndex = 0; }
      }
    }
  }

  formatTaskRow(row1, sizeof(row1), displayIndex);
  writeRowIfChanged(0, row0);
  writeRowIfChanged(1, row1);
}

// =========================
// 9. WEB SERVER & HANDLERS
// =========================
String escapeHTML(const char* input) {
  String out;
  out.reserve(strlen(input) + 16);
  while (*input) {
    switch (*input) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += *input;
    }
    ++input;
  }
  return out;
}

bool requireAuth() {
  if (server.authenticate(WEB_USER, WEB_PASS)) return true;
  server.requestAuthentication();
  return false;
}

void handleRoot() {
  if (!requireAuth()) return;

  String html;
  html.reserve(4000);

  html += F("<!DOCTYPE html><html lang='en'><head>");
  html += F("<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>");
  html += F("<meta http-equiv='refresh' content='10'>");
  html += F("<title>Task Box Console | CoffeeUnderFlow</title>");
  html += F("<style>");
  html += F(":root{--bg-base:#09090b;--card-bg:#121215;--border-color:#27272a;--text-main:#f4f4f5;--text-muted:#a1a1aa;--accent:#f59e0b;--accent-hover:#d97706;--danger:#ef4444;}");
  html += F("body{font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background-color:var(--bg-base);color:var(--text-main);max-width:520px;margin:0 auto;padding:20px 16px;line-height:1.5;}");
  html += F(".brand-header{display:flex;justify-content:space-between;align-items:flex-end;border-bottom:1px solid var(--border-color);padding-bottom:14px;margin-bottom:24px;}");
  html += F(".brand-title{font-size:22px;font-weight:900;letter-spacing:0.05em;text-transform:uppercase;color:var(--accent);line-height:1.1;}");
  html += F(".brand-sub{display:block;font-size:12px;font-weight:500;color:var(--text-muted);text-transform:none;letter-spacing:normal;margin-top:2px;}");
  html += F(".brand-sub a{color:var(--text-muted);text-decoration:none;transition:color 0.2s;}.brand-sub a:hover{color:var(--accent);text-decoration:underline;}");
  html += F(".header-console{font-size:15px;font-weight:600;color:var(--text-muted);letter-spacing:-0.01em;text-align:right;}");
  html += F(".card{background-color:var(--card-bg);border:1px solid var(--border-color);border-radius:14px;padding:20px;margin-bottom:20px;box-shadow:0 10px 15px -3px rgba(0,0,0,0.5);}");
  html += F(".card h3{font-size:16px;font-weight:600;margin-top:0;margin-bottom:16px;color:var(--text-main);}");
  html += F(".form-group{margin-bottom:16px;}");
  html += F("label{display:block;font-size:13px;font-weight:500;color:var(--text-muted);margin-bottom:6px;}");
  html += F("input[type=text],input[type=number]{width:100%;padding:12px 14px;border-radius:8px;border:1px solid var(--border-color);background-color:var(--bg-base);color:var(--text-main);font-size:15px;box-sizing:border-box;outline:none;transition:border-color 0.2s;}");
  html += F("input[type=text]:focus,input[type=number]:focus{border-color:var(--accent);}");
  html += F(".priority-selector{display:flex;gap:8px;}.priority-option{flex:1;cursor:pointer;}.priority-option input{display:none;}");
  html += F(".priority-badge{display:block;text-align:center;padding:10px;background-color:var(--bg-base);border:1px solid var(--border-color);border-radius:8px;font-weight:700;font-size:14px;color:var(--text-muted);transition:all 0.2s;}");
  html += F(".priority-option input:checked+.priority-badge{background-color:var(--accent);border-color:var(--accent);color:#000;}");
  html += F("button,input[type=submit]{width:100%;background-color:var(--accent);color:#000;border:none;padding:12px;border-radius:8px;font-size:15px;font-weight:700;cursor:pointer;transition:background-color 0.2s;}");
  html += F("button:hover,input[type=submit]:hover{background-color:var(--accent-hover);}");
  html += F(".task-item{display:flex;align-items:center;justify-content:space-between;background-color:var(--bg-base);padding:10px 14px;border-radius:8px;margin-bottom:8px;border:1px solid var(--border-color);}");
  html += F(".task-content{display:flex;align-items:center;gap:12px;overflow:hidden;}.task-prio{background:var(--accent);color:#000;padding:2px 8px;border-radius:6px;font-size:13px;font-weight:800;}");
  html += F(".task-text{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:14px;}.delete-btn{color:var(--danger);background:none;border:none;font-size:13px;font-weight:600;cursor:pointer;padding:4px 8px;width:auto;}");
  html += F(".delete-btn:hover{text-decoration:underline;background:none;}.time-row{display:flex;align-items:center;gap:8px;}.time-row span{color:var(--text-muted);font-weight:bold;}");
  html += F("</style></head><body>");

  html += F("<div class='brand-header'><div class='brand-title'>TASKBOX<span class='brand-sub'>by <a href='https://coffeeunderflow.in/' target='_blank'>CoffeeUnderFlow</a></span></div><div class='header-console'>Task Console</div></div>");

  html += F("<div class='card'><h3>Add New Task</h3><form method='POST' action='/setTask'>");
  html += F("<div class='form-group'><input type='text' name='task' maxlength='40' placeholder='Task description...' required></div>");
  html += F("<div class='form-group'><label>Priority Level</label><div class='priority-selector'>");

  const char priorities[] = {'A','B','C','D'};
  for (uint8_t i = 0; i < 4; ++i) {
    html += "<label class='priority-option'><input type='radio' name='pri' value='";
    html += priorities[i];
    if (lastPriority == priorities[i]) html += F("' checked>");
    else html += F("'>");
    html += "<span class='priority-badge'>";
    html += priorities[i];
    html += F("</span></label>");
  }

  html += F("</div></div><input type='submit' value='Send to Box'></form></div>");

  html += F("<div class='card'><h3>Live Queue</h3>");
  if (taskCount == 0) {
    html += F("<p style='color:var(--text-muted);font-size:14px;margin:0;'>No active tasks.</p>");
  } else {
    for (uint8_t i = 0; i < taskCount; ++i) {
      html += F("<div class='task-item'><div class='task-content'><span class='task-prio'>");
      html += taskQueue[i].priority;
      html += F("</span><span class='task-text'>");
      html += escapeHTML(taskQueue[i].text);
      html += F("</span></div><a href='/deleteTask?id=");
      html += i;
      html += F("' class='delete-btn' onclick=\"return confirm('Delete this task?')\">delete</a></div>");
    }
  }
  html += F("</div>");

  html += F("<div class='card'><h3>Device Settings</h3><form method='POST' action='/updateSettings'>");
  
  html += F("<div class='form-group'><label>Morning Alarm (Hour : Minute)</label><div class='time-row'>");
  html += F("<input type='number' name='mH' min='0' max='23' value='") + String(settings.morningHour) + F("'>");
  html += F("<span>:</span>");
  html += F("<input type='number' name='mM' min='0' max='59' value='") + String(settings.morningMinute) + F("'>");
  html += F("</div></div>");

  html += F("<div class='form-group'><label>Evening Alarm (Hour : Minute)</label><div class='time-row'>");
  html += F("<input type='number' name='eH' min='0' max='23' value='") + String(settings.eveningHour) + F("'>");
  html += F("<span>:</span>");
  html += F("<input type='number' name='eM' min='0' max='59' value='") + String(settings.eveningMinute) + F("'>");
  html += F("</div></div>");

  html += F("<input type='submit' value='Save Settings'></form></div></body></html>");

  server.send(200, "text/html", html);
}

void handleSetTask() {
  if (!requireAuth()) return;

  if (!server.hasArg("task") || !server.hasArg("pri")) {
    server.send(400, "text/plain", "Missing task or priority");
    return;
  }

  String text = server.arg("task");
  char priority = server.arg("pri").charAt(0);

  if (!validPriority(priority) || !validTaskText(text)) {
    server.send(400, "text/plain", "Invalid task or priority");
    return;
  }

  lastPriority = priority;
  bool replaced = false;

  for (uint8_t i = 0; i < taskCount; ++i) {
    if (taskQueue[i].priority == priority) {
      text.toCharArray(taskQueue[i].text, sizeof(taskQueue[i].text));
      replaced = true;
      break;
    }
  }

  if (!replaced) {
    if (taskCount >= MAX_TASKS) {
      server.send(409, "text/plain", "Queue full");
      return;
    }
    text.toCharArray(taskQueue[taskCount].text, sizeof(taskQueue[taskCount].text));
    taskQueue[taskCount].priority = priority;
    ++taskCount;
  }

  sortTasks();
  storageDirty = true;
  resetRotation();
  playAudio(AUDIO_ADDED);

  newTaskVisible = true;
  newTaskStartedAt = millis();
  
  nightLightOnUntil = millis() + 60000UL; 
  lcd.backlight();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDeleteTask() {
  if (!requireAuth()) return;

  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "Missing id");
    return;
  }

  int id = server.arg("id").toInt();
  if (id < 0 || id >= taskCount) {
    server.send(404, "text/plain", "Task not found");
    return;
  }

  for (uint8_t i = id; i + 1 < taskCount; ++i) taskQueue[i] = taskQueue[i + 1];
  clearTaskSlot(taskCount - 1);
  --taskCount;

  storageDirty = true;
  resetRotation();
  completionVisible = true;
  completionStartedAt = millis();
  playAudio(AUDIO_COMPLETE);

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleUpdateSettings() {
  if (!requireAuth()) return;

  if (server.hasArg("mH")) settings.morningHour = server.arg("mH").toInt();
  if (server.hasArg("mM")) settings.morningMinute = server.arg("mM").toInt();
  if (server.hasArg("eH")) settings.eveningHour = server.arg("eH").toInt();
  if (server.hasArg("eM")) settings.eveningMinute = server.arg("eM").toInt();

  storageDirty = true; 
  playAudio(AUDIO_ADDED); 

  server.sendHeader("Location", "/");
  server.send(303);
}

// =========================
// 10. NETWORK / SYSTEM LOGIC
// =========================
uint32_t lastWifiRetry = 0;
bool wifiWasConnected = false;

void setupOTA() {
  ArduinoOTA.setHostname("taskbox");
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    noTone(BUZZER_PIN);
    lcd.backlight(); 
    writeRowIfChanged(0, "SYSTEM UPDATE");
    writeRowIfChanged(1, "Please wait...");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total == 0) return;
    char row[17];
    unsigned int pct = (progress * 100UL) / total;
    snprintf(row, sizeof(row), "Progress %3u%%", pct);
    writeRowIfChanged(1, row);
  });
  ArduinoOTA.onEnd([]() {
    writeRowIfChanged(0, "UPDATE COMPLETE");
    writeRowIfChanged(1, "Rebooting...");
  });
  ArduinoOTA.begin();
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/setTask", HTTP_POST, handleSetTask);
  server.on("/deleteTask", HTTP_GET, handleDeleteTask);
  server.on("/updateSettings", HTTP_POST, handleUpdateSettings); 

  server.onNotFound([]() {
    if (!requireAuth()) return;
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
}

void setupNetwork() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Start mDNS responder so "taskbox.local" or "taskbox" works
  if (MDNS.begin("taskbox")) {
    Serial.println("mDNS responder started: http://taskbox.local");
  }

  setupOTA();
  setupWeb();
}

void maintainNetwork() {
  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      startNtpIfNeeded(true);
    }
    MDNS.update();
    ArduinoOTA.handle();
    server.handleClient();
  } else {
    wifiWasConnected = false;
    if (millis() - lastWifiRetry >= 15000UL) {
      WiFi.reconnect();
      lastWifiRetry = millis();
    }
  }
}

// =========================
// 11. SETUP / LOOP
// =========================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\nStarting TaskBox...");

  pinMode(TOUCH_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN);

  Wire.begin(D2, D1);
  Wire.setClock(100000);

  lcd.init();
  lcd.backlight();
  setupCustomChars();
  writeRowIfChanged(0, "TaskBox By C_");
  writeRowIfChanged(1, "Loading...");

  loadStorage();
  setupNetwork();

  // Print IP address to Serial once connected
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("Web Dashboard IP: http://");
  Serial.println(WiFi.localIP());

  lastRotationAt = millis();
  lastWifiRetry = millis();

  delay(300);
  writeRowIfChanged(0, "Hello There! :)");
  writeRowIfChanged(1, "Welcome Back");

  playAudio(AUDIO_ADDED);
}

void loop() {
  maintainNetwork();
  startNtpIfNeeded();

  handleTimeAndAlarms();
  handleInput();
  handleDisplay();
  handleAudio();

  saveStorage();
}