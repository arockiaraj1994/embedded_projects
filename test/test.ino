/*
 * ESP32 + 1.54" E-Paper Display
 *
 * Required libraries (install via Arduino Library Manager):
 *   - GxEPD2 (by Jean-Marc Zingg)
 *   - Adafruit GFX Library
 *
 * Wiring (ESP32 VSPI default: SCK=18, MISO=19, MOSI=23):
 *   E-Paper pin  ->  ESP32 GPIO
 *   -----------     ----------
 *   VCC          ->  3.3V
 *   GND          ->  GND
 *   DIN (MOSI)   ->  23
 *   CLK (SCK)    ->  18
 *   CS           ->  5
 *   D/C          ->  17
 *   RST          ->  16
 *   BUSY         ->  4
 *
 * If your display shows red or wrong colors: you have a 3-color (B/W/Red) panel - use GxEPD2_154_Z90c.
 * If your display doesn't work: change EPD_CS, EPD_DC, EPD_RST, EPD_BUSY to match your wiring.
 */

#include <GxEPD2_3C.h>
#include <Adafruit_GFX.h>

// Pin configuration - change these if your wiring differs
#define EPD_CS   5
#define EPD_DC   17
#define EPD_RST  16
#define EPD_BUSY 4

// GDEH0154Z90 200x200 3-color (black/white/red) - use this for panels that show red
GxEPD2_3C<GxEPD2_154_Z90c, GxEPD2_154_Z90c::HEIGHT> display(
  GxEPD2_154_Z90c(/*CS=*/ EPD_CS, /*DC=*/ EPD_DC, /*RST=*/ EPD_RST, /*BUSY=*/ EPD_BUSY)
);

void setup() {
  display.init(115200);
  display.setRotation(1);
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(3);
  display.setCursor((display.width() - 108) / 2, (display.height() - 48) / 2 - 5);
  display.print("ALFRED");
  display.setTextSize(2);
  display.setCursor((display.width() - 156) / 2, (display.height() - 48) / 2 + 28);
  display.print("is a good boy");
  display.display();
  display.hibernate();
}

void loop() {
  delay(60000);
}
