/*
 * Smart Tasks — ESP32 + 1.54" 3-Color E-Paper (200x200, B/W/Red)
 *
 * Fetches pending Google Tasks via a Google Apps Script proxy and
 * displays up to 4 tasks with title, due date, and notes.
 * Deep-sleeps 10 minutes between refreshes.
 *
 * Hardware wiring (ESP32 VSPI):
 *   E-Paper  ->  ESP32
 *   VCC      ->  3.3V
 *   GND      ->  GND
 *   DIN      ->  GPIO 23 (MOSI)
 *   CLK      ->  GPIO 18 (SCK)
 *   CS       ->  GPIO 5
 *   D/C      ->  GPIO 17
 *   RST      ->  GPIO 16
 *   BUSY     ->  GPIO 4
 *
 * Libraries: GxEPD2, ArduinoJson (v7), Adafruit GFX
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

// ——— Configuration ———
#define WIFI_SSID "Arockiaraj_Jio"
#define WIFI_PASS "11119999"

#define APPS_SCRIPT_URL "https://script.google.com/macros/s/AKfycbwxTm7jPPOOo2S106XIP0oekUL8fCJR8N8thH5S5XxvN0KB5JyhkqXHwgZP9wxlLZcgYw/exec"
#define API_KEY         "f47ac10b-58cc-4372-a567-0e02b2c3d479"

#define EPD_CS   5
#define EPD_DC   17
#define EPD_RST  16
#define EPD_BUSY 4

#define SLEEP_MINUTES    10
#define WIFI_TIMEOUT_SEC 15
#define MAX_DISPLAY_TASKS 4

#define NIGHT_START_HOUR 22  // 10 PM
#define NIGHT_END_HOUR    6  // 6 AM

static const char* NTP_SERVER = "pool.ntp.org";
static const long  IST_OFFSET = 19800;  // UTC+5:30

// ——— Display ———
GxEPD2_3C<GxEPD2_154_Z90c, GxEPD2_154_Z90c::HEIGHT> display(
  GxEPD2_154_Z90c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ——— Data ———
struct TaskItem {
  String title;
  String due;    // "Mar 10" or ""
  String notes;  // first line, truncated
  bool   overdue;
};

struct TaskResult {
  TaskItem items[MAX_DISPLAY_TASKS];
  int      count;
  bool     success;
  bool     unchanged;
};

RTC_DATA_ATTR uint32_t lastPayloadHash = 0;

// ——————————————————————————————————————————————
// Helpers
// ——————————————————————————————————————————————

static const char* MONTH_NAMES[] = {
  "Jan","Feb","Mar","Apr","May","Jun",
  "Jul","Aug","Sep","Oct","Nov","Dec"
};

uint32_t hashPayload(const char* s) {
  uint32_t h = 5381;
  while (*s) h = ((h << 5) + h) + (uint8_t)*s++;
  return h;
}

// "2026-03-10" -> "Mar 10", also sets overdue flag
String formatDue(const char* iso, bool &overdue) {
  overdue = false;
  if (!iso || strlen(iso) < 10) return "";

  int y = atoi(iso);
  int m = atoi(iso + 5);
  int d = atoi(iso + 8);
  if (m < 1 || m > 12) return String(iso);

  struct tm now;
  if (getLocalTime(&now, 0)) {
    int nowY = now.tm_year + 1900;
    int nowM = now.tm_mon + 1;
    int nowD = now.tm_mday;
    if (y < nowY || (y == nowY && m < nowM) || (y == nowY && m == nowM && d < nowD)) {
      overdue = true;
    }
  }

  String s = String(MONTH_NAMES[m - 1]) + " " + String(d);
  time_t nowEpoch = time(nullptr);
  struct tm *tmNow = localtime(&nowEpoch);
  if (tmNow && y != 1900 + tmNow->tm_year) {
    s += ", " + String(y);
  }
  return s;
}

// ——————————————————————————————————————————————
// WiFi + NTP
// ——————————————————————————————————————————————

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > (unsigned long)WIFI_TIMEOUT_SEC * 1000UL) {
      Serial.println("WiFi timeout");
      return false;
    }
    delay(250);
  }
  Serial.print("WiFi OK  IP: ");
  Serial.println(WiFi.localIP());

  configTime(IST_OFFSET, 0, NTP_SERVER);
  struct tm ti;
  getLocalTime(&ti, 5000);
  return true;
}

// Returns minutes to sleep until NIGHT_END_HOUR, or 0 if daytime
int nightSleepMinutes() {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return 0;

  int hour = ti.tm_hour;
  int min  = ti.tm_min;

  bool isNight = (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
  if (!isNight) return 0;

  int wakeHour = NIGHT_END_HOUR;
  int minsUntilWake;
  if (hour >= NIGHT_START_HOUR) {
    minsUntilWake = (24 - hour + wakeHour) * 60 - min;
  } else {
    minsUntilWake = (wakeHour - hour) * 60 - min;
  }
  if (minsUntilWake < 1) minsUntilWake = 1;
  return minsUntilWake;
}

// ——————————————————————————————————————————————
// Fetch tasks from Apps Script
// ——————————————————————————————————————————————

TaskResult fetchTasks() {
  TaskResult r = {};

  String url = String(APPS_SCRIPT_URL) + "?key=" + API_KEY;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, url);
  http.addHeader("Accept", "application/json");
  http.setTimeout(15000);

  Serial.println("GET " + url);
  int code = http.GET();
  Serial.printf("HTTP %d  size=%d\n", code, http.getSize());

  if (code != HTTP_CODE_OK) {
    http.end();
    return r;
  }

  String payload = http.getString();
  http.end();
  Serial.printf("Payload %d bytes\n", payload.length());

  uint32_t h = hashPayload(payload.c_str());
  if (h == lastPayloadHash) {
    Serial.println("Data unchanged, skipping refresh");
    r.success   = true;
    r.unchanged = true;
    return r;
  }
  lastPayloadHash = h;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return r;
  }

  if (!doc["error"].isNull()) {
    Serial.println("API: " + doc["error"].as<String>());
    return r;
  }

  JsonArray tasks = doc["tasks"].as<JsonArray>();
  int total = min((int)tasks.size(), MAX_DISPLAY_TASKS);

  for (int i = 0; i < total; i++) {
    JsonObject t = tasks[i];

    r.items[i].title = t["title"].as<String>();

    bool ov = false;
    if (t["due"].is<const char*>()) {
      r.items[i].due = formatDue(t["due"].as<const char*>(), ov);
    }
    r.items[i].overdue = ov;

    if (t["notes"].is<const char*>()) {
      String n = t["notes"].as<String>();
      int nl = n.indexOf('\n');
      if (nl >= 0) n = n.substring(0, nl);
      if (n.length() > 30) n = n.substring(0, 27) + "...";
      r.items[i].notes = n;
    }
  }

  r.count   = total;
  r.success = true;

  Serial.printf("Got %d tasks\n", r.count);
  return r;
}

// ——————————————————————————————————————————————
// Display: main task screen
// ——————————————————————————————————————————————

void drawScreen(const TaskResult &r) {
  int16_t  tbx, tby;
  uint16_t tbw, tbh;

  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // ── Header ──
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(5, 18);
    display.print("TASKS");

    String countStr = String(r.count) + " item" + (r.count != 1 ? "s" : "");
    display.setFont(&FreeSans9pt7b);
    display.getTextBounds(countStr.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(195 - tbw, 18);
    display.print(countStr);

    display.drawLine(0, 26, 200, 26, GxEPD_BLACK);
    display.setTextWrap(false);

    // ── Footer: last update ──
    display.setFont(NULL);
    display.setTextSize(1);
    display.setTextColor(GxEPD_BLACK);
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
      char buf[24];
      strftime(buf, sizeof(buf), "Updated: %H:%M IST", &ti);
      display.setCursor(5, 192);
      display.print(buf);
    }

    // ── Tasks ──
    if (r.count == 0) {
      display.setFont(&FreeSans9pt7b);
      display.setTextColor(GxEPD_BLACK);
      const char* msg = "No pending tasks";
      display.getTextBounds(msg, 0, 0, &tbx, &tby, &tbw, &tbh);
      display.setCursor((200 - tbw) / 2, 105);
      display.print(msg);
    } else {
      int yBase = 30;
      int blockH = (185 - yBase) / min(r.count, MAX_DISPLAY_TASKS);
      if (blockH > 38) blockH = 38;

      for (int i = 0; i < r.count; i++) {
        int yOff = yBase + i * blockH;

        // Bullet
        display.fillCircle(9, yOff + 9, 2, GxEPD_BLACK);

        // Title — FreeSans9pt (next size up from built-in 1)
        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(16, yOff + 14);
        display.print(r.items[i].title);

        // Detail line: due | notes — built-in size 1
        display.setFont(NULL);
        display.setTextSize(1);

        String detail;
        if (r.items[i].due.length() > 0) {
          detail = r.items[i].due;
          if (r.items[i].notes.length() > 0) {
            detail += " | " + r.items[i].notes;
          }
        } else if (r.items[i].notes.length() > 0) {
          detail = r.items[i].notes;
        } else {
          detail = "No date";
        }

        display.setTextColor(r.items[i].overdue ? GxEPD_RED : GxEPD_BLACK);
        display.setCursor(16, yOff + 26);
        display.print(detail);

        // Separator
        if (i < r.count - 1) {
          int sepY = yOff + blockH - 2;
          display.drawLine(5, sepY, 195, sepY, GxEPD_BLACK);
        }
      }
    }

  } while (display.nextPage());
}

// ——————————————————————————————————————————————
// Display: error screen
// ——————————————————————————————————————————————

void drawError(const char* msg) {
  int16_t  tbx, tby;
  uint16_t tbw, tbh;

  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(5, 18);
    display.print("TASKS");
    display.drawLine(0, 26, 200, 26, GxEPD_BLACK);

    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_RED);
    display.getTextBounds(msg, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor((200 - tbw) / 2, 110);
    display.print(msg);

  } while (display.nextPage());
}

// ——————————————————————————————————————————————
// Deep sleep
// ——————————————————————————————————————————————

void enterDeepSleep(int minutes) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  display.hibernate();
  esp_sleep_enable_timer_wakeup((uint64_t)minutes * 60ULL * 1000000ULL);
  Serial.printf("Deep sleep %d min...\n", minutes);
  Serial.flush();
  esp_deep_sleep_start();
}

// ——————————————————————————————————————————————
// Entry point
// ——————————————————————————————————————————————

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Smart Tasks ---");

  display.init(115200);

  if (!connectWiFi()) {
    drawError("No WiFi");
    enterDeepSleep(SLEEP_MINUTES);
    return;
  }

  int nightMins = nightSleepMinutes();
  if (nightMins > 0) {
    Serial.printf("Night mode, sleeping %d min until %d:00\n", nightMins, NIGHT_END_HOUR);
    enterDeepSleep(nightMins);
    return;
  }

  TaskResult tasks = fetchTasks();

  if (tasks.success && tasks.unchanged) {
    Serial.println("No change, skip display");
  } else if (tasks.success) {
    drawScreen(tasks);
  } else {
    drawError("API Error");
  }

  enterDeepSleep(SLEEP_MINUTES);
}

void loop() {
}
