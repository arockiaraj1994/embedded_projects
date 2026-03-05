/*
 * Nifty 50 Ticker — ESP32 + 1.54" 3-Color E-Paper (200x200, B/W/Red)
 *
 * Fetches live Nifty 50 index data from Yahoo Finance, displays it with
 * color-coded change indicators, and deep-sleeps between 5-minute refreshes.
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
 * Libraries: GxEPD2, ArduinoJson (v6), Adafruit GFX
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

// ——— Configuration ———
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASS "YOUR_PASSWORD"

#define EPD_CS   5
#define EPD_DC   17
#define EPD_RST  16
#define EPD_BUSY 4

#define SLEEP_MINUTES    5
#define WIFI_TIMEOUT_SEC 15

static const char* API_URL    = "https://query1.finance.yahoo.com/v8/finance/chart/%5ENSEI";
static const char* USER_AGENT = "Mozilla/5.0 (ESP32; rv:1.0)";
static const char* NTP_SERVER = "pool.ntp.org";
static const long  IST_OFFSET = 19800;  // UTC+5:30

// ——— Display ———
GxEPD2_3C<GxEPD2_154_Z90c, GxEPD2_154_Z90c::HEIGHT> display(
  GxEPD2_154_Z90c(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ——— Data ———
struct NiftyData {
  float price;
  float prevClose;
  float dayHigh;
  float dayLow;
  float change;
  float changePct;
  bool  marketOpen;
  bool  success;
};

// Survives deep sleep — holds last successful fetch
RTC_DATA_ATTR float lastPrice      = 0;
RTC_DATA_ATTR float lastChange     = 0;
RTC_DATA_ATTR float lastChangePct  = 0;
RTC_DATA_ATTR float lastPrevClose  = 0;
RTC_DATA_ATTR float lastDayHigh   = 0;
RTC_DATA_ATTR float lastDayLow    = 0;
RTC_DATA_ATTR bool  lastMarketOpen = false;
RTC_DATA_ATTR bool  hasLastData    = false;

// ——————————————————————————————————————————————
// Helpers
// ——————————————————————————————————————————————

// Indian comma grouping: 12,34,567.89
String formatIndian(float value, int decimals) {
  bool neg = value < 0;
  if (neg) value = -value;

  unsigned long intPart = (unsigned long)value;

  String decStr;
  if (decimals > 0) {
    float frac = value - (float)intPart;
    for (int i = 0; i < decimals; i++) frac *= 10.0f;
    unsigned long decVal = (unsigned long)(frac + 0.5f);
    decStr = ".";
    String d = String(decVal);
    while ((int)d.length() < decimals) d = "0" + d;
    decStr += d;
  }

  String intStr = String(intPart);
  int len = intStr.length();
  String grouped;

  if (len <= 3) {
    grouped = intStr;
  } else {
    grouped = intStr.substring(len - 3);
    int pos = len - 3;
    while (pos > 0) {
      int start = (pos >= 2) ? pos - 2 : 0;
      grouped = intStr.substring(start, pos) + "," + grouped;
      pos = start;
    }
  }

  return (neg ? "-" : "") + grouped + decStr;
}

// ——————————————————————————————————————————————
// WiFi + NTP
// ——————————————————————————————————————————————

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
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

// ——————————————————————————————————————————————
// Yahoo Finance fetch
// ——————————————————————————————————————————————

NiftyData fetchNifty() {
  NiftyData d = {};

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, API_URL);
  http.addHeader("User-Agent", USER_AGENT);
  http.addHeader("Accept", "application/json");
  http.setTimeout(15000);

  Serial.println("GET " + String(API_URL));
  int code = http.GET();
  Serial.printf("HTTP %d  size=%d\n", code, http.getSize());

  if (code != HTTP_CODE_OK) {
    http.end();
    return d;
  }

  String payload = http.getString();
  http.end();
  Serial.printf("Payload %d bytes\n", payload.length());

  // Filter: keep only chart.result[0].meta to save RAM
  JsonDocument filter;
  filter["chart"]["result"][0]["meta"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
    doc, payload, DeserializationOption::Filter(filter));

  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return d;
  }

  JsonObject meta = doc["chart"]["result"][0]["meta"];
  if (meta.isNull()) {
    Serial.println("Missing meta");
    return d;
  }
  Serial.printf("Price=%.2f PrevClose=%.2f\n",
    (float)(meta["regularMarketPrice"] | 0.0f),
    (float)(meta["chartPreviousClose"] | 0.0f));

  d.price     = meta["regularMarketPrice"]   | 0.0f;
  d.prevClose = meta["chartPreviousClose"]    | 0.0f;
  d.dayHigh   = meta["regularMarketDayHigh"]  | 0.0f;
  d.dayLow    = meta["regularMarketDayLow"]   | 0.0f;

  if (d.prevClose > 0) {
    d.change    = d.price - d.prevClose;
    d.changePct = (d.change / d.prevClose) * 100.0f;
  }

  String state = meta["marketState"] | "CLOSED";
  d.marketOpen = (state == "REGULAR");
  d.success    = (d.price > 0);

  if (d.success) {
    lastPrice      = d.price;
    lastChange     = d.change;
    lastChangePct  = d.changePct;
    lastPrevClose  = d.prevClose;
    lastDayHigh    = d.dayHigh;
    lastDayLow     = d.dayLow;
    lastMarketOpen = d.marketOpen;
    hasLastData    = true;
  }

  return d;
}

// ——————————————————————————————————————————————
// Display: main screen
// ——————————————————————————————————————————————

void drawScreen(float price, float change, float changePct,
                float dayHigh, float dayLow, float prevClose,
                bool marketOpen, bool offline) {

  bool neg = (change < 0);
  uint16_t chgColor = neg ? GxEPD_RED : GxEPD_BLACK;

  String priceStr = formatIndian(price, 2);
  String chgStr   = (neg ? "" : "+") + formatIndian(change, 2);
  String pctStr   = "(" + String(changePct, 2) + "%)";
  String highStr  = "H: " + formatIndian(dayHigh, 0);
  String lowStr   = "L: " + formatIndian(dayLow, 0);
  String prevStr  = "Prev: " + formatIndian(prevClose, 0);

  String timeStr;
  if (offline) {
    timeStr = "Offline";
  } else {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
      char buf[16];
      strftime(buf, sizeof(buf), "%H:%M IST", &ti);
      timeStr = "Updated: " + String(buf);
    } else {
      timeStr = "Updated: --:--";
    }
  }

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
    display.print("NIFTY 50");

    const char* badge = marketOpen ? "OPEN" : "CLOSED";
    uint16_t badgeClr = marketOpen ? GxEPD_BLACK : GxEPD_RED;
    display.setTextColor(badgeClr);
    display.getTextBounds(badge, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(195 - tbw, 18);
    display.print(badge);

    display.drawLine(0, 26, 200, 26, GxEPD_BLACK);

    // ── Price ──
    display.setFont(&FreeSansBold18pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.getTextBounds(priceStr.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor((200 - tbw) / 2, 68);
    display.print(priceStr);

    // ── Change line with arrow ──
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(chgColor);

    String changeLine = chgStr + " " + pctStr;
    display.getTextBounds(changeLine.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
    int16_t lineW = tbw + 14;                 // text + triangle + gap
    int16_t startX = (200 - lineW) / 2;

    // Triangle (▲ or ▼)
    int16_t triCx = startX + 4;
    int16_t triCy = 89;
    if (neg) {
      display.fillTriangle(triCx - 4, triCy - 4,
                           triCx + 4, triCy - 4,
                           triCx,     triCy + 4, chgColor);
    } else {
      display.fillTriangle(triCx - 4, triCy + 4,
                           triCx + 4, triCy + 4,
                           triCx,     triCy - 4, chgColor);
    }

    display.setCursor(startX + 14, 95);
    display.print(changeLine);

    display.drawLine(0, 115, 200, 115, GxEPD_BLACK);

    // ── High / Low ──
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(5, 137);
    display.print(highStr);
    display.getTextBounds(lowStr.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(195 - tbw, 137);
    display.print(lowStr);

    // ── Previous close ──
    display.setCursor(5, 158);
    display.print(prevStr);

    display.drawLine(0, 170, 200, 170, GxEPD_BLACK);

    // ── Timestamp / Offline ──
    display.setTextColor(offline ? GxEPD_RED : GxEPD_BLACK);
    display.setCursor(5, 188);
    display.print(timeStr);

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
    display.print("NIFTY 50");
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

void enterDeepSleep() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  display.hibernate();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60ULL * 1000000ULL);
  Serial.println("Deep sleep...");
  Serial.flush();
  esp_deep_sleep_start();
}

// ——————————————————————————————————————————————
// Entry point
// ——————————————————————————————————————————————

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Nifty 50 Ticker ---");

  display.init(115200);

  if (!connectWiFi()) {
    drawError("No WiFi");
    enterDeepSleep();
    return;
  }

  NiftyData data = fetchNifty();

  if (data.success) {
    drawScreen(data.price, data.change, data.changePct,
               data.dayHigh, data.dayLow, data.prevClose,
               data.marketOpen, false);
  } else if (hasLastData) {
    drawScreen(lastPrice, lastChange, lastChangePct,
               lastDayHigh, lastDayLow, lastPrevClose,
               lastMarketOpen, true);
  } else {
    drawError("API Error");
  }

  enterDeepSleep();
}

void loop() {
}
