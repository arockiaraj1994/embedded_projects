/*
 * Expense Summary — ESP32 + 1.54" 3-Color E-Paper (200x200, B/W/Red)
 *
 * Fetches monthly expense data from a Google Apps Script proxy and
 * displays a table of card-wise spending (monthly + today).
 * Deep-sleeps 1 hour between refreshes; quiet from 11 PM to 6 AM.
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

#define APPS_SCRIPT_URL "https://script.google.com/macros/s/AKfycbw8yW6GF7_-wA3INtIRS5PfpKNUjc6PgGyhtHicJUYelTeIYsNrbcz6oCZLTw2GCBGj/exec"
#define API_KEY         "199401"

#define EPD_CS   5
#define EPD_DC   17
#define EPD_RST  16
#define EPD_BUSY 4

#define SLEEP_MINUTES    60
#define WIFI_TIMEOUT_SEC 15
#define MAX_CARDS        6

#define NIGHT_START_HOUR 23  // 11 PM
#define NIGHT_END_HOUR    6  // 6 AM

static const char* NTP_SERVER = "pool.ntp.org";
static const long  IST_OFFSET = 19800;  // UTC+5:30

// ——— Display ———
GxEPD2_3C<GxEPD2_154_Z90c, GxEPD2_154_Z90c::HEIGHT> display(
  GxEPD2_154_Z90c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ——— Data ———
struct CardItem {
  String id;
  String name;
  int32_t monthly;
  int32_t today;
};

struct ExpenseResult {
  String month;
  CardItem cards[MAX_CARDS];
  int  count;
  bool success;
  bool unchanged;
};

RTC_DATA_ATTR uint32_t lastPayloadHash = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;

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

// Indian-style comma formatting: 1,23,456
String formatAmount(int32_t val) {
  if (val < 0) return "-" + formatAmount(-val);
  if (val < 1000) return String(val);

  String num = String(val);
  int len = num.length();
  String result;

  // Last 3 digits
  result = num.substring(len - 3);
  int pos = len - 3;

  // Remaining digits in groups of 2
  while (pos > 0) {
    int start = (pos - 2 >= 0) ? pos - 2 : 0;
    result = num.substring(start, pos) + "," + result;
    pos = start;
  }
  return result;
}

// Build "Mar-2026" from current NTP time
String buildMonthParam() {
  struct tm ti;
  if (!getLocalTime(&ti, 5000)) return "Jan-2000";
  return String(MONTH_NAMES[ti.tm_mon]) + "-" + String(1900 + ti.tm_year);
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
// Fetch expenses from Apps Script
// ——————————————————————————————————————————————

ExpenseResult fetchExpenses() {
  ExpenseResult r = {};

  String month = buildMonthParam();
  String url = String(APPS_SCRIPT_URL)
    + "?action=esp32&month=" + month
    + "&key=" + API_KEY;

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

  r.month = doc["month"].as<String>();

  JsonArray cards = doc["cards"].as<JsonArray>();
  int total = min((int)cards.size(), MAX_CARDS);

  for (int i = 0; i < total; i++) {
    JsonObject c = cards[i];
    r.cards[i].id      = c["id"].as<String>();
    r.cards[i].name    = c["name"].as<String>();
    r.cards[i].monthly = c["monthly"].as<int32_t>();
    r.cards[i].today   = c["today"].as<int32_t>();
  }

  r.count   = total;
  r.success = true;

  Serial.printf("Got %d cards for %s\n", r.count, r.month.c_str());
  return r;
}

// ——————————————————————————————————————————————
// Display: table layout constants
// ——————————————————————————————————————————————

static const int COL1_X =  2;   // Card name start
static const int DIV1_X = 90;   // First vertical divider
static const int COL2_X = 92;   // Monthly column start
static const int DIV2_X = 150;  // Second vertical divider
static const int COL3_X = 152;  // Today column start
static const int TABLE_R = 198; // Right edge

// Right-align text at xRight using built-in font (6px per char at size 1)
void drawRightAligned(int xRight, int y, const String &text) {
  int w = text.length() * 6;
  display.setCursor(xRight - w, y);
  display.print(text);
}

// ——————————————————————————————————————————————
// Display: expense table
// ——————————————————————————————————————————————

void drawScreen(const ExpenseResult &r) {
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
    display.print("EXPENSES");

    display.setFont(&FreeSans9pt7b);
    display.getTextBounds(r.month.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(195 - tbw, 18);
    display.print(r.month);

    display.drawLine(0, 26, 200, 26, GxEPD_BLACK);

    // ── Table header row ──
    int tableTop = 30;
    display.setFont(NULL);
    display.setTextSize(1);
    display.setTextColor(GxEPD_RED);

    display.setCursor(COL1_X + 2, tableTop + 3);
    display.print("Card");
    drawRightAligned(DIV2_X - 4, tableTop + 3, "Monthly");
    drawRightAligned(TABLE_R - 2, tableTop + 3, "Today");

    int headerBottom = tableTop + 14;
    display.drawLine(0, headerBottom, 200, headerBottom, GxEPD_BLACK);

    // Vertical dividers spanning the table
    int tableBottom = headerBottom + r.count * 18 + 2;

    display.drawLine(DIV1_X, tableTop, DIV1_X, tableBottom, GxEPD_BLACK);
    display.drawLine(DIV2_X, tableTop, DIV2_X, tableBottom, GxEPD_BLACK);

    // ── Data rows ──
    display.setTextColor(GxEPD_BLACK);
    int32_t totalMonthly = 0;
    int32_t totalToday   = 0;

    for (int i = 0; i < r.count; i++) {
      int rowY = headerBottom + 4 + i * 18;

      display.setCursor(COL1_X + 2, rowY);
      display.print(r.cards[i].name);

      drawRightAligned(DIV2_X - 4, rowY, formatAmount(r.cards[i].monthly));
      drawRightAligned(TABLE_R - 2, rowY, formatAmount(r.cards[i].today));

      totalMonthly += r.cards[i].monthly;
      totalToday   += r.cards[i].today;

      if (i < r.count - 1) {
        int sepY = headerBottom + (i + 1) * 18;
        for (int x = 0; x < 200; x += 3) {
          display.drawPixel(x, sepY, GxEPD_BLACK);
        }
      }
    }

    // ── Totals row ──
    display.drawLine(0, tableBottom, 200, tableBottom, GxEPD_BLACK);
    int totY = tableBottom + 4;
    display.setTextColor(GxEPD_RED);
    display.setCursor(COL1_X + 2, totY);
    display.print("TOTAL");
    drawRightAligned(DIV2_X - 4, totY, formatAmount(totalMonthly));
    drawRightAligned(TABLE_R - 2, totY, formatAmount(totalToday));

    display.drawLine(DIV1_X, tableBottom, DIV1_X, tableBottom + 14, GxEPD_BLACK);
    display.drawLine(DIV2_X, tableBottom, DIV2_X, tableBottom + 14, GxEPD_BLACK);

    int totalBottom = tableBottom + 16;
    display.drawLine(0, totalBottom, 200, totalBottom, GxEPD_BLACK);

    // ── Footer: last update ──
    display.setTextColor(GxEPD_BLACK);
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
      char buf[24];
      strftime(buf, sizeof(buf), "Updated: %H:%M IST", &ti);
      display.setCursor(5, 192);
      display.print(buf);
    }

  } while (display.nextPage());
}

// ——————————————————————————————————————————————
// Display: night mode screen
// ——————————————————————————————————————————————

void drawNightMode(int mins) {
  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(5, 18);
    display.print("EXPENSES");
    display.drawLine(0, 26, 200, 26, GxEPD_BLACK);

    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    const char* msg = "Night Mode";
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(msg, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor((200 - tbw) / 2, 100);
    display.print(msg);

    display.setFont(NULL);
    display.setTextSize(1);
    char buf[32];
    snprintf(buf, sizeof(buf), "Wake at %d:00 (%dh %dm)", NIGHT_END_HOUR, mins / 60, mins % 60);
    int w = strlen(buf) * 6;
    display.setCursor((200 - w) / 2, 118);
    display.print(buf);

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
    display.print("EXPENSES");
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
  Serial.println("\n--- Expense Summary ---");

  display.init(115200);

  if (!connectWiFi()) {
    drawError("No WiFi");
    enterDeepSleep(SLEEP_MINUTES);
    return;
  }

  bootCount++;
  Serial.printf("Boot #%u\n", bootCount);

  struct tm now;
  if (getLocalTime(&now, 0)) {
    Serial.printf("Time: %02d:%02d IST\n", now.tm_hour, now.tm_min);
  }

  int nightMins = nightSleepMinutes();
  if (nightMins > 0 && bootCount > 1) {
    Serial.printf("Night mode, sleeping %d min until %d:00\n", nightMins, NIGHT_END_HOUR);
    drawNightMode(nightMins);
    enterDeepSleep(nightMins);
    return;
  }

  ExpenseResult expenses = fetchExpenses();

  if (expenses.success && expenses.unchanged) {
    Serial.println("No change, skip display refresh");
  } else if (expenses.success) {
    Serial.printf("Drawing %d cards...\n", expenses.count);
    drawScreen(expenses);
    Serial.println("Display updated");
  } else {
    Serial.println("Fetch failed, showing error");
    drawError("API Error");
  }

  enterDeepSleep(SLEEP_MINUTES);
}

void loop() {
}
