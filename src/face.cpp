// SD Pro clock - clock face drawing. See face.h.
//
// Layout (240 x 240, black background). The case hides the bottom pixel or
// two, so nothing is drawn below y = 230.
//
//   (pin) Sydney               Mon       <- city / day
//                      28 Jul 2026       <- date
//   10:24 AM                 (icon)      <- time / weather icon
//                               22°C     <- temperature
//   [ icon  Label             ]          <- rotating card: wind, pressure,
//   [ Value         |  dial   ] (spaceman)  weather condition, feels like
//   [   o o o o     |         ]          <- page dots

#include "face.h"
#include "spaceman_data.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

namespace {

// ---------------------------------------------------------------------------
// Colours (RGB565)
// ---------------------------------------------------------------------------

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

constexpr uint16_t BLACK = rgb(0, 0, 0);
constexpr uint16_t WHITE = rgb(255, 255, 255);
constexpr uint16_t GREY = rgb(170, 180, 195);
constexpr uint16_t DIM = rgb(90, 100, 115);
constexpr uint16_t ACCENT_BLUE = rgb(50, 150, 255);
constexpr uint16_t SUN_YELLOW = rgb(255, 205, 40);
constexpr uint16_t SUN_ORANGE = rgb(255, 160, 0);
constexpr uint16_t MOON_PALE = rgb(235, 230, 200);
constexpr uint16_t CLOUD_GREY = rgb(200, 205, 215);
constexpr uint16_t RAIN_BLUE = rgb(80, 160, 255);

constexpr uint16_t CARD_FILL = rgb(6, 14, 30);
constexpr uint16_t CARD_EDGE = rgb(35, 95, 170);
constexpr uint16_t WARM_ORANGE = rgb(255, 140, 40);
constexpr uint16_t COOL_BLUE = rgb(90, 170, 255);
constexpr uint16_t THERMO_RED = rgb(240, 80, 60);

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

constexpr int16_t LEFT = 10;
constexpr int16_t RIGHT = 231;  // right edge for right-aligned text

// Time is drawn into a 1-bit off-screen canvas, then copied in one go, so
// the once-a-minute update doesn't flicker. 160 x 46 pixels = 920 bytes.
constexpr int16_t TIME_X = 8;
constexpr int16_t TIME_Y = 54;
constexpr int16_t TIME_W = 160;
constexpr int16_t TIME_H = 46;
constexpr int16_t TIME_BASELINE = 40;  // inside the canvas

constexpr int16_t WEATHER_ICON_X = 205;
constexpr int16_t WEATHER_ICON_Y = 78;
constexpr float WEATHER_ICON_SCALE = 0.75f;  // icons are ~40px at 1.0
constexpr int16_t TEMP_BASELINE = 116;

// The rotating card, below the time and left of the spaceman.
//   [ icon  Label                  ]   <- header row
//   [ Value unit      |    dial    ]   <- body: big value, dial on the right
//   [   o o o o       |            ]   <- page dots
constexpr int16_t CARD_X = 4;
constexpr int16_t CARD_Y = 108;
constexpr int16_t CARD_W = 162;
constexpr int16_t CARD_H = 122;
constexpr int16_t CARD_ICON_Y = CARD_Y + 12;
constexpr int16_t CARD_LABEL_X = CARD_X + 44;
constexpr int16_t CARD_LABEL_BASELINE = CARD_Y + 30;
constexpr int16_t CARD_BODY_Y = CARD_Y + 42;  // top of the body area
constexpr int16_t CARD_DIVIDER_X = CARD_X + CARD_W - 62;
constexpr int16_t CARD_DIAL_X = CARD_DIVIDER_X + (CARD_X + CARD_W - CARD_DIVIDER_X) / 2;
constexpr int16_t CARD_VALUE_X = CARD_X + 10;
constexpr int16_t CARD_VALUE_BASELINE = CARD_Y + 82;
constexpr int16_t CARD_DOTS_Y = CARD_Y + CARD_H - 10;

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

// Ink width and left offset of text in the given font.
void measure(Adafruit_GFX &g, const GFXfont *font, const char *text, int16_t &x1, uint16_t &w) {
  int16_t y1;
  uint16_t h;
  g.setFont(font);
  g.setTextSize(1);
  g.getTextBounds(text, 0, 100, &x1, &y1, &w, &h);
}

// Left-aligned text; y is the baseline (for FreeFonts).
int16_t textAt(Adafruit_GFX &g, const GFXfont *font, uint16_t colour, int16_t x, int16_t y,
               const char *text) {
  g.setFont(font);
  g.setTextSize(1);
  g.setTextColor(colour);
  g.setCursor(x, y);
  g.print(text);
  return g.getCursorX();
}

// Right-aligned so the ink ends at x = right.
void textRight(Adafruit_GFX &g, const GFXfont *font, uint16_t colour, int16_t right, int16_t y,
               const char *text) {
  int16_t x1;
  uint16_t w;
  measure(g, font, text, x1, w);
  textAt(g, font, colour, right - (x1 + (int16_t)w) + 1, y, text);
}

// Centred on x = centre.
void textCentre(Adafruit_GFX &g, const GFXfont *font, uint16_t colour, int16_t centre, int16_t y,
                const char *text) {
  int16_t x1;
  uint16_t w;
  measure(g, font, text, x1, w);
  textAt(g, font, colour, centre - (x1 + (int16_t)w / 2), y, text);
}

// A line two pixels thick.
void thickLine(Adafruit_GFX &g, float x0, float y0, float x1, float y1, uint16_t c) {
  g.drawLine(lroundf(x0), lroundf(y0), lroundf(x1), lroundf(y1), c);
  if (fabsf(x1 - x0) > fabsf(y1 - y0)) {
    g.drawLine(lroundf(x0), lroundf(y0) + 1, lroundf(x1), lroundf(y1) + 1, c);
  } else {
    g.drawLine(lroundf(x0) + 1, lroundf(y0), lroundf(x1) + 1, lroundf(y1), c);
  }
}

// Temperature like "22°C", with a hand-drawn degree sign (the fonts are
// ASCII only). The ring is sized from the font's digit height.
struct DegreeRing {
  int16_t up, radius;
};
DegreeRing degreeRing(Adafruit_GFX &g, const GFXfont *font) {
  int16_t x1, y1;
  uint16_t w, h;
  g.setFont(font);
  g.setTextSize(1);
  g.getTextBounds("0", 0, 100, &x1, &y1, &w, &h);
  int16_t radius = h >= 22 ? 4 : 3;
  return {(int16_t)(h - radius), radius};
}

// Width it takes up.
int16_t temperatureWidth(Adafruit_GFX &g, int value, const GFXfont *font) {
  char number[8];
  snprintf(number, sizeof(number), "%d", value);
  DegreeRing ring = degreeRing(g, font);
  int16_t x1, cx1;
  uint16_t w, cw;
  measure(g, font, number, x1, w);
  measure(g, font, "C", cx1, cw);
  return (x1 + (int16_t)w) + ring.radius * 2 + 4 + (cx1 + (int16_t)cw);
}

// Draws it with the ink starting at x; returns the x where it ends.
int16_t drawTemperature(Adafruit_GFX &g, int16_t x, int16_t baseline, int value, uint16_t colour,
                        const GFXfont *font) {
  char number[8];
  snprintf(number, sizeof(number), "%d", value);
  DegreeRing ring = degreeRing(g, font);
  int16_t x1;
  uint16_t w;
  measure(g, font, number, x1, w);
  int16_t numberEnd = x + x1 + (int16_t)w;
  textAt(g, font, colour, x, baseline, number);
  int16_t cx = numberEnd + 1 + ring.radius, cy = baseline - ring.up;
  g.drawCircle(cx, cy, ring.radius, colour);
  g.drawCircle(cx, cy, ring.radius - 1, colour);
  return textAt(g, font, colour, numberEnd + ring.radius * 2 + 4, baseline, "C");
}

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------

// Weather icons take a scale k: at 1.0 they are about 40 x 40 pixels.
int16_t sc(float v, float k) { return (int16_t)lroundf(v * k); }

void drawSun(Adafruit_GFX &g, int16_t cx, int16_t cy, int16_t r, float k = 1.0f) {
  for (int i = 0; i < 8; i++) {
    float a = i * (float)M_PI / 4.0f;
    float s = sinf(a), c = cosf(a);
    thickLine(g, cx + s * (r + 4 * k), cy - c * (r + 4 * k), cx + s * (r + 9 * k),
              cy - c * (r + 9 * k), SUN_ORANGE);
  }
  g.fillCircle(cx, cy, r, SUN_ORANGE);
  g.fillCircle(cx, cy, r - 2, SUN_YELLOW);
}

void drawMoon(Adafruit_GFX &g, int16_t cx, int16_t cy, float k) {
  g.fillCircle(cx, cy, sc(13, k), MOON_PALE);
  g.fillCircle(cx + sc(7, k), cy - sc(5, k), sc(11, k), BLACK);
}

void drawCloud(Adafruit_GFX &g, int16_t cx, int16_t cy, uint16_t c, float k) {
  g.fillCircle(cx - sc(9, k), cy + sc(2, k), sc(7, k), c);
  g.fillCircle(cx + sc(1, k), cy - sc(4, k), sc(10, k), c);
  g.fillCircle(cx + sc(11, k), cy + sc(3, k), sc(6, k), c);
  g.fillRoundRect(cx - sc(16, k), cy + sc(2, k), sc(33, k), sc(8, k), sc(4, k), c);
}

void drawWeatherIcon(Adafruit_GFX &g, WeatherIcon icon, int16_t cx, int16_t cy, float k) {
  switch (icon) {
    case ICON_SUN:
      drawSun(g, cx, cy, sc(11, k), k);
      break;
    case ICON_MOON:
      drawMoon(g, cx, cy, k);
      break;
    case ICON_PARTLY_DAY:
      drawSun(g, cx - sc(6, k), cy - sc(7, k), sc(8, k), k);
      drawCloud(g, cx + sc(3, k), cy + sc(6, k), CLOUD_GREY, k);
      break;
    case ICON_PARTLY_NIGHT:
      g.fillCircle(cx - sc(6, k), cy - sc(7, k), sc(10, k), MOON_PALE);
      g.fillCircle(cx - sc(1, k), cy - sc(11, k), sc(8, k), BLACK);
      drawCloud(g, cx + sc(3, k), cy + sc(6, k), CLOUD_GREY, k);
      break;
    case ICON_CLOUD:
      drawCloud(g, cx, cy, CLOUD_GREY, k);
      break;
    case ICON_RAIN:
      drawCloud(g, cx, cy - sc(6, k), CLOUD_GREY, k);
      for (int i = -1; i <= 1; i++) {
        thickLine(g, cx + (i * 9 + 2) * k, cy + 8 * k, cx + (i * 9 - 2) * k, cy + 16 * k, RAIN_BLUE);
      }
      break;
    case ICON_STORM:
      drawCloud(g, cx, cy - sc(6, k), CLOUD_GREY, k);
      g.fillTriangle(cx + sc(2, k), cy + sc(4, k), cx - sc(6, k), cy + sc(13, k), cx + sc(1, k),
                     cy + sc(13, k), SUN_YELLOW);
      g.fillTriangle(cx - sc(1, k), cy + sc(12, k), cx + sc(5, k), cy + sc(12, k), cx - sc(4, k),
                     cy + sc(20, k), SUN_YELLOW);
      break;
    case ICON_SNOW:
      drawCloud(g, cx, cy - sc(6, k), CLOUD_GREY, k);
      for (int i = -1; i <= 1; i++) {
        g.fillCircle(cx + sc(i * 9, k), cy + sc(12 + (i == 0 ? 4 : 0), k), 2, WHITE);
      }
      break;
    case ICON_MIST:
      for (int i = 0; i < 4; i++) {
        int16_t w = sc((i % 2) ? 26 : 34, k);
        g.fillRoundRect(cx - w / 2, cy + sc(-12 + i * 8, k), w, 3, 1, CLOUD_GREY);
      }
      break;
    default:
      break;
  }
}

// Map pin, 10 x 15, top-left at (x, y).
void drawPin(Adafruit_GFX &g, int16_t x, int16_t y) {
  g.fillCircle(x + 5, y + 5, 5, ACCENT_BLUE);
  g.fillTriangle(x + 1, y + 8, x + 9, y + 8, x + 5, y + 15, ACCENT_BLUE);
  g.fillCircle(x + 5, y + 5, 2, BLACK);
}

// Three wind streaks with curled ends, about 30 x 22, top-left at (x, y).
void drawWindIcon(Adafruit_GFX &g, int16_t x, int16_t y) {
  const uint16_t c = ACCENT_BLUE;
  for (int t = 0; t < 2; t++) {  // two pixels thick
    // top streak, curling up
    g.drawFastHLine(x, y + 6 + t, 20, c);
    g.drawCircleHelper(x + 20, y + 2 + t, 4, 0x2 | 0x4, c);
    // middle streak (longest), curling down
    g.drawFastHLine(x + 3, y + 12 + t, 23, c);
    g.drawCircleHelper(x + 26, y + 16 + t, 4, 0x2 | 0x4, c);
    // bottom streak
    g.drawFastHLine(x, y + 18 + t, 14, c);
  }
}

// Small pressure gauge, about 28 x 20, top-left at (x, y).
void drawGaugeIcon(Adafruit_GFX &g, int16_t x, int16_t y) {
  const int16_t cx = x + 14, cy = y + 18;
  for (int t = 0; t < 2; t++) g.drawCircleHelper(cx, cy, 13 - t, 0x1 | 0x2, ACCENT_BLUE);
  g.drawFastHLine(cx - 13, cy, 27, ACCENT_BLUE);
  thickLine(g, cx, cy, cx + 7, cy - 8, WHITE);
  g.fillCircle(cx, cy, 2, WHITE);
}

// Thermometer, about 12 x 24, top-left at (x, y).
void drawThermometer(Adafruit_GFX &g, int16_t x, int16_t y) {
  g.drawRoundRect(x + 3, y, 7, 18, 3, GREY);
  g.fillCircle(x + 6, y + 19, 5, THERMO_RED);
  g.fillRect(x + 5, y + 7, 3, 12, THERMO_RED);
}

// Compass ring with a direction arrow.
void drawCompass(Adafruit_GFX &g, int16_t cx, int16_t cy, int16_t r, int degrees) {
  g.drawCircle(cx, cy, r, DIM);
  for (int i = 0; i < 4; i++) {  // N/E/S/W ticks
    float a = i * (float)M_PI / 2.0f;
    g.drawLine(cx + sinf(a) * (r - 3), cy - cosf(a) * (r - 3), cx + sinf(a) * r,
               cy - cosf(a) * r, GREY);
  }
  g.setFont(nullptr);
  g.setTextSize(1);
  g.setTextColor(GREY);
  g.setCursor(cx - 2, cy - r + 5);
  g.print("N");
  if (degrees < 0) return;

  float a = degrees * (float)M_PI / 180.0f;
  float s = sinf(a), c = cosf(a);
  float tipX = cx + s * (r - 5), tipY = cy - c * (r - 5);
  float backX = cx - s * 7, backY = cy + c * 7;
  float notchX = cx - s * 2, notchY = cy + c * 2;
  float leftX = backX - c * 6, leftY = backY - s * 6;
  float rightX = backX + c * 6, rightY = backY + s * 6;
  g.fillTriangle(tipX, tipY, leftX, leftY, notchX, notchY, ACCENT_BLUE);
  g.fillTriangle(tipX, tipY, rightX, rightY, notchX, notchY, rgb(30, 110, 210));
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void drawHeader(Adafruit_GFX &g, const FaceData &d) {
  drawPin(g, LEFT, 10);
  textAt(g, &FreeSansBold9pt7b, WHITE, LEFT + 15, 24, d.city);
  if (d.timeValid) {
    textRight(g, &FreeSansBold12pt7b, WHITE, RIGHT, 24, d.dayName);
    textRight(g, &FreeSans12pt7b, WHITE, RIGHT, 48, d.date);
  } else {
    textRight(g, &FreeSans12pt7b, DIM, RIGHT, 24, "Syncing");
    textRight(g, &FreeSans12pt7b, DIM, RIGHT, 48, "time...");
  }
}

// Small text in the built-in 5x7 font; y is the top of the text.
void smallText(Adafruit_GFX &g, uint16_t colour, int16_t x, int16_t y, const char *text) {
  g.setFont(nullptr);
  g.setTextSize(1);
  g.setTextColor(colour);
  g.setCursor(x, y);
  g.print(text);
}

void smallTextCentre(Adafruit_GFX &g, uint16_t colour, int16_t centre, int16_t y, const char *text) {
  smallText(g, colour, centre - (int16_t)strlen(text) * 3, y, text);
}

void drawWeather(Adafruit_GFX &g, const FaceData &d) {
  if (!d.weatherValid) {
    textCentre(g, &FreeSansBold12pt7b, DIM, WEATHER_ICON_X, WEATHER_ICON_Y + 8, "--");
    smallTextCentre(g, DIM, WEATHER_ICON_X, TEMP_BASELINE - 10,
                    d.weatherMessage ? d.weatherMessage : "...");
    return;
  }
  drawWeatherIcon(g, d.icon, WEATHER_ICON_X, WEATHER_ICON_Y, WEATHER_ICON_SCALE);
  drawTemperature(g, RIGHT + 1 - temperatureWidth(g, d.tempC, &FreeSansBold12pt7b), TEMP_BASELINE,
                  d.tempC, WHITE, &FreeSansBold12pt7b);
}

// Header row: the page name next to its icon. Returns where the text ends.
int16_t cardLabel(Adafruit_GFX &g, const char *label) {
  return textAt(g, &FreeSans9pt7b, GREY, CARD_LABEL_X, CARD_LABEL_BASELINE, label);
}

// The big number in the body, with its unit after it if there's room.
void cardValue(Adafruit_GFX &g, const char *value, const char *unit) {
  int16_t end = textAt(g, &FreeSansBold18pt7b, WHITE, CARD_VALUE_X, CARD_VALUE_BASELINE, value);
  if (unit && end + 3 + (int16_t)strlen(unit) * 6 < CARD_DIVIDER_X - 2) {
    smallText(g, GREY, end + 3, CARD_VALUE_BASELINE - 7, unit);
  }
}

void drawWindPage(Adafruit_GFX &g, const FaceData &d) {
  drawWindIcon(g, CARD_X + 8, CARD_ICON_Y);
  cardLabel(g, "Wind speed");
  char value[8] = "--";
  if (d.weatherValid) snprintf(value, sizeof(value), "%d", d.windKmh);
  cardValue(g, value, "km/h");

  drawCompass(g, CARD_DIAL_X, CARD_BODY_Y + 24, 20, d.weatherValid ? d.windDeg : -1);
  const char *point = (d.weatherValid && d.windDeg >= 0) ? compassPoint(d.windDeg) : "--";
  textCentre(g, &FreeSansBold9pt7b, WHITE, CARD_DIAL_X, CARD_BODY_Y + 64, point);
}

void drawPressurePage(Adafruit_GFX &g, const FaceData &d) {
  drawGaugeIcon(g, CARD_X + 8, CARD_ICON_Y);
  int16_t end = cardLabel(g, "Pressure");
  smallText(g, DIM, end + 4, CARD_LABEL_BASELINE - 7, "hPa");
  char value[8] = "--";
  if (d.weatherValid) snprintf(value, sizeof(value), "%d", d.pressureHpa);
  cardValue(g, value, nullptr);

  // Half-circle dial from 980 hPa (left) to 1040 hPa (right).
  const int16_t cx = CARD_DIAL_X, cy = CARD_BODY_Y + 40, r = 22;
  g.drawCircleHelper(cx, cy, r, 0x1 | 0x2, DIM);
  for (int i = 0; i <= 4; i++) {
    float a = (float)M_PI * (1.0f - i / 4.0f);  // left to right over the top
    g.drawLine(cx + cosf(a) * (r - 4), cy - sinf(a) * (r - 4), cx + cosf(a) * r,
               cy - sinf(a) * r, GREY);
  }
  if (!d.weatherValid) return;
  float t = (d.pressureHpa - 980) / 60.0f;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  float a = (float)M_PI * (1.0f - t);
  thickLine(g, cx, cy, cx + cosf(a) * (r - 5), cy - sinf(a) * (r - 5), WHITE);
  g.fillCircle(cx, cy, 2, WHITE);
  const char *level = d.pressureHpa < 1006 ? "LOW" : d.pressureHpa > 1020 ? "HIGH" : "NORMAL";
  smallTextCentre(g, WHITE, cx, cy + 8, level);
}

// Weather condition: the whole body is the text, on two lines if needed.
void drawConditionPage(Adafruit_GFX &g, const FaceData &d) {
  drawCloud(g, CARD_X + 22, CARD_ICON_Y + 10, ACCENT_BLUE, 0.6f);
  cardLabel(g, "Weather");
  const char *text = d.weatherValid ? d.condition : "--";
  const int16_t maxWidth = CARD_W - 14;  // to 4px inside the right edge
  int16_t x1;
  uint16_t w;
  measure(g, &FreeSansBold18pt7b, text, x1, w);
  if (x1 + (int16_t)w <= maxWidth) {
    textAt(g, &FreeSansBold18pt7b, WHITE, CARD_VALUE_X, CARD_VALUE_BASELINE, text);
    return;
  }
  // Split at the first space: "Partly cloudy" -> "Partly" / "cloudy".
  char first[16];
  snprintf(first, sizeof(first), "%s", text);
  char *space = strchr(first, ' ');
  const GFXfont *font = &FreeSansBold18pt7b;
  if (space) {
    *space = '\0';
    const char *second = space + 1;
    uint16_t w2;
    measure(g, font, first, x1, w);
    int16_t w1 = x1 + (int16_t)w;
    measure(g, font, second, x1, w2);
    if (w1 > maxWidth || x1 + (int16_t)w2 > maxWidth) font = &FreeSansBold12pt7b;
    textAt(g, font, WHITE, CARD_VALUE_X, CARD_BODY_Y + 22, first);
    textAt(g, font, WHITE, CARD_VALUE_X, CARD_BODY_Y + 54, second);
  } else {
    textAt(g, &FreeSansBold12pt7b, WHITE, CARD_VALUE_X, CARD_VALUE_BASELINE, text);
  }
}

void drawFeelsLikePage(Adafruit_GFX &g, const FaceData &d) {
  drawThermometer(g, CARD_X + 14, CARD_ICON_Y - 2);
  cardLabel(g, "Feels like");
  if (d.weatherValid) {
    drawTemperature(g, CARD_VALUE_X, CARD_VALUE_BASELINE, d.feelsLikeC, WHITE, &FreeSansBold18pt7b);
  } else {
    cardValue(g, "--", nullptr);
  }

  // How it compares with the actual temperature.
  if (!d.weatherValid) return;
  const int16_t cx = CARD_DIAL_X, cy = CARD_BODY_Y + 12;
  int diff = d.feelsLikeC - d.tempC;
  char text[8];
  if (diff == 0) {
    g.fillRect(cx - 8, cy - 4, 16, 3, GREY);
    g.fillRect(cx - 8, cy + 2, 16, 3, GREY);
    smallTextCentre(g, GREY, cx, cy + 24, "same");
  } else {
    uint16_t colour = diff > 0 ? WARM_ORANGE : COOL_BLUE;
    if (diff > 0) {
      g.fillTriangle(cx, cy - 9, cx - 9, cy + 5, cx + 9, cy + 5, colour);
    } else {
      g.fillTriangle(cx, cy + 7, cx - 9, cy - 7, cx + 9, cy - 7, colour);
    }
    snprintf(text, sizeof(text), "%+d", diff);
    int16_t x1;
    uint16_t w;
    measure(g, &FreeSansBold12pt7b, text, x1, w);
    int16_t x = cx - (x1 + (int16_t)w + 8) / 2;
    int16_t end = textAt(g, &FreeSansBold12pt7b, colour, x, cy + 38, text);
    g.drawCircle(end + 4, cy + 23, 2, colour);
    g.drawCircle(end + 4, cy + 23, 3, colour);
    smallTextCentre(g, GREY, cx, cy + 46, diff > 0 ? "warmer" : "cooler");
  }
}

void drawCard(Adafruit_GFX &g, const FaceData &d) {
  g.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 9, CARD_FILL);
  g.drawRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 9, CARD_EDGE);
  if (d.cardPage != CARD_CONDITION) {  // the condition page uses the full width
    g.drawFastVLine(CARD_DIVIDER_X, CARD_BODY_Y, CARD_Y + CARD_H - 12 - CARD_BODY_Y, CARD_EDGE);
  }

  switch (d.cardPage) {
    case CARD_PRESSURE: drawPressurePage(g, d); break;
    case CARD_CONDITION: drawConditionPage(g, d); break;
    case CARD_FEELS_LIKE: drawFeelsLikePage(g, d); break;
    default: drawWindPage(g, d); break;
  }

  // Page dots, centred under the left part of the card.
  const int16_t spacing = 9;
  const int16_t first = (CARD_X + CARD_DIVIDER_X) / 2 - spacing * (CARD_PAGE_COUNT - 1) / 2;
  for (int i = 0; i < CARD_PAGE_COUNT; i++) {
    bool active = i == d.cardPage;
    g.fillCircle(first + i * spacing, CARD_DOTS_Y, 2, active ? ACCENT_BLUE : DIM);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void drawFaceTime(Adafruit_GFX &gfx, const FaceData &d) {
  static GFXcanvas1 canvas(TIME_W, TIME_H);
  canvas.fillScreen(0);
  canvas.setTextWrap(false);

  char time[8];
  const char *ampm = "";
  if (d.timeValid) {
    int h12 = d.hour24 % 12;
    if (h12 == 0) h12 = 12;
    snprintf(time, sizeof(time), "%d:%02d", h12, d.minute);
    ampm = d.hour24 < 12 ? "AM" : "PM";
  } else {
    strcpy(time, "--:--");
  }
  int16_t end = textAt(canvas, &FreeSansBold24pt7b, 1, 0, TIME_BASELINE, time);
  textAt(canvas, &FreeSansBold12pt7b, 1, end + 4, TIME_BASELINE, ampm);

  gfx.drawBitmap(TIME_X, TIME_Y, canvas.getBuffer(), TIME_W, TIME_H, WHITE, BLACK);
}

void drawFace(Adafruit_GFX &gfx, const FaceData &d) {
  gfx.fillScreen(BLACK);
  gfx.setTextWrap(false);
  drawHeader(gfx, d);
  drawFaceTime(gfx, d);
  drawWeather(gfx, d);
  drawCard(gfx, d);
}

void drawFaceCard(Adafruit_GFX &gfx, const FaceData &d) {
  gfx.setTextWrap(false);
  drawCard(gfx, d);
}

const char *compassPoint(int degrees) {
  static const char *POINTS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = ((degrees % 360 + 360) % 360 + 22) / 45;
  return POINTS[index % 8];
}

// ----- Spaceman -----

static_assert(SPACEMAN_W == SPACEMAN_H && SPACEMAN_W <= 80, "drawSpaceman's row buffer is 80 pixels");

uint8_t spacemanFrameCount() { return SPACEMAN_FRAMES; }
uint16_t spacemanFrameMs() { return SPACEMAN_FRAME_MS; }
int16_t spacemanSize() { return SPACEMAN_W; }

SpacemanFrame::SpacemanFrame(uint8_t frame)
    : pos_(pgm_read_word(&SPACEMAN_OFFSETS[frame % SPACEMAN_FRAMES])), colour_(0) {}

// Each packed byte is (shade << 4) | (run length - 1): 16 greys, runs of
// 1-16 pixels. Runs carry on from one row to the next.
void SpacemanFrame::nextRow(uint16_t *pixels) {
  for (int16_t x = 0; x < SPACEMAN_W; x++) {
    if (left_ == 0) {
      uint8_t b = pgm_read_byte(&SPACEMAN_RLE[pos_++]);
      uint8_t v = (b >> 4) * 17;  // shade 0-15 -> 0-255
      colour_ = rgb(v, v, v);
      left_ = (b & 0x0F) + 1;
    }
    pixels[x] = colour_;
    left_--;
  }
}
