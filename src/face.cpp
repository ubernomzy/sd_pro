// SD Pro clock - clock face drawing. See face.h.
//
// Layout (240 x 240, black background). The case hides the bottom pixel or
// two, so nothing is drawn below y = 230.
//
//   Hello                      Mon       <- greeting / day
//   Neehal             28 Jul 2026       <- name / date
//   10:24 AM                 (icon)      <- time / weather icon
//   (pin) Sydney                22°C     <- city / temperature
//                              Sunny     <- condition
//   [ icon  value unit | dial ]          <- rotating card: wind, pressure,
//   [ Label            |      ]             UV index, feels like
//   [      o o o o            ]          <- page dots

#include "face.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Fonts/FreeSans9pt7b.h>
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
constexpr uint16_t NAME_BLUE = rgb(40, 170, 255);
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

// UV index bands (WHO): Low 0-2, Moderate 3-5, High 6-7, Very high 8-10, Extreme 11+
constexpr uint16_t UV_COLOURS[] = {rgb(80, 200, 90), rgb(240, 210, 40), rgb(255, 140, 0),
                                   rgb(230, 50, 50), rgb(170, 90, 230)};
constexpr const char *UV_LEVELS[] = {"Low", "Moderate", "High", "Very high", "Extreme"};

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
constexpr int16_t WEATHER_ICON_Y = 72;

// The rotating card. Left of the divider: icon, value, label. Right: a dial.
constexpr int16_t CARD_X = 4;
constexpr int16_t CARD_Y = 150;
constexpr int16_t CARD_W = 148;
constexpr int16_t CARD_H = 80;
constexpr int16_t CARD_DIVIDER_X = 112;
constexpr int16_t CARD_DIAL_X = CARD_DIVIDER_X + (CARD_X + CARD_W - CARD_DIVIDER_X) / 2;
constexpr int16_t CARD_VALUE_X = CARD_X + 44;
constexpr int16_t CARD_VALUE_BASELINE = CARD_Y + 32;
constexpr int16_t CARD_LABEL_BASELINE = CARD_Y + 60;
constexpr int16_t CARD_DOTS_Y = CARD_Y + CARD_H - 8;

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

// Temperature like "22°C" in FreeSansBold12pt, with a hand-drawn degree
// sign (the fonts are ASCII only). Returns the width it takes up.
int16_t temperatureWidth(Adafruit_GFX &g, int value) {
  char number[8];
  snprintf(number, sizeof(number), "%d", value);
  int16_t x1, cx1;
  uint16_t w, cw;
  measure(g, &FreeSansBold12pt7b, number, x1, w);
  measure(g, &FreeSansBold12pt7b, "C", cx1, cw);
  return (x1 + (int16_t)w) + 9 + (cx1 + (int16_t)cw);
}

// Draws it with the ink starting at x; returns the x where it ends.
int16_t drawTemperature(Adafruit_GFX &g, int16_t x, int16_t baseline, int value, uint16_t colour) {
  char number[8];
  snprintf(number, sizeof(number), "%d", value);
  int16_t x1;
  uint16_t w;
  measure(g, &FreeSansBold12pt7b, number, x1, w);
  int16_t numberEnd = x + x1 + (int16_t)w;
  textAt(g, &FreeSansBold12pt7b, colour, x, baseline, number);
  g.drawCircle(numberEnd + 4, baseline - 15, 2, colour);
  g.drawCircle(numberEnd + 4, baseline - 15, 3, colour);
  return textAt(g, &FreeSansBold12pt7b, colour, numberEnd + 9, baseline, "C");
}

// ---------------------------------------------------------------------------
// Icons
// ---------------------------------------------------------------------------

void drawSun(Adafruit_GFX &g, int16_t cx, int16_t cy, int16_t r) {
  for (int i = 0; i < 8; i++) {
    float a = i * (float)M_PI / 4.0f;
    float s = sinf(a), c = cosf(a);
    thickLine(g, cx + s * (r + 4), cy - c * (r + 4), cx + s * (r + 9), cy - c * (r + 9), SUN_ORANGE);
  }
  g.fillCircle(cx, cy, r, SUN_ORANGE);
  g.fillCircle(cx, cy, r - 2, SUN_YELLOW);
}

void drawMoon(Adafruit_GFX &g, int16_t cx, int16_t cy) {
  g.fillCircle(cx, cy, 13, MOON_PALE);
  g.fillCircle(cx + 7, cy - 5, 11, BLACK);
}

void drawCloud(Adafruit_GFX &g, int16_t cx, int16_t cy, uint16_t c) {
  g.fillCircle(cx - 9, cy + 2, 7, c);
  g.fillCircle(cx + 1, cy - 4, 10, c);
  g.fillCircle(cx + 11, cy + 3, 6, c);
  g.fillRoundRect(cx - 16, cy + 2, 33, 8, 4, c);
}

void drawWeatherIcon(Adafruit_GFX &g, WeatherIcon icon, int16_t cx, int16_t cy) {
  switch (icon) {
    case ICON_SUN:
      drawSun(g, cx, cy, 11);
      break;
    case ICON_MOON:
      drawMoon(g, cx, cy);
      break;
    case ICON_PARTLY_DAY:
      drawSun(g, cx - 6, cy - 7, 8);
      drawCloud(g, cx + 3, cy + 6, CLOUD_GREY);
      break;
    case ICON_PARTLY_NIGHT:
      g.fillCircle(cx - 6, cy - 7, 10, MOON_PALE);
      g.fillCircle(cx - 1, cy - 11, 8, BLACK);
      drawCloud(g, cx + 3, cy + 6, CLOUD_GREY);
      break;
    case ICON_CLOUD:
      drawCloud(g, cx, cy, CLOUD_GREY);
      break;
    case ICON_RAIN:
      drawCloud(g, cx, cy - 6, CLOUD_GREY);
      for (int i = -1; i <= 1; i++) {
        thickLine(g, cx + i * 9 + 2, cy + 8, cx + i * 9 - 2, cy + 16, RAIN_BLUE);
      }
      break;
    case ICON_STORM:
      drawCloud(g, cx, cy - 6, CLOUD_GREY);
      g.fillTriangle(cx + 2, cy + 4, cx - 6, cy + 13, cx + 1, cy + 13, SUN_YELLOW);
      g.fillTriangle(cx - 1, cy + 12, cx + 5, cy + 12, cx - 4, cy + 22, SUN_YELLOW);
      break;
    case ICON_SNOW:
      drawCloud(g, cx, cy - 6, CLOUD_GREY);
      for (int i = -1; i <= 1; i++) {
        g.fillCircle(cx + i * 9, cy + 12 + (i == 0 ? 4 : 0), 2, WHITE);
      }
      break;
    case ICON_MIST:
      for (int i = 0; i < 4; i++) {
        int16_t w = (i % 2) ? 26 : 34;
        g.fillRoundRect(cx - w / 2, cy - 12 + i * 8, w, 3, 1, CLOUD_GREY);
      }
      break;
    default:
      break;
  }
}

// Map pin, 12 x 18, top-left at (x, y).
void drawPin(Adafruit_GFX &g, int16_t x, int16_t y) {
  g.fillCircle(x + 6, y + 6, 6, ACCENT_BLUE);
  g.fillTriangle(x + 1, y + 9, x + 11, y + 9, x + 6, y + 18, ACCENT_BLUE);
  g.fillCircle(x + 6, y + 6, 2, BLACK);
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
  textAt(g, &FreeSans9pt7b, GREY, LEFT, 20, d.greeting);
  textAt(g, &FreeSansBold18pt7b, NAME_BLUE, LEFT - 1, 48, d.name);
  if (d.timeValid) {
    textRight(g, &FreeSansBold9pt7b, WHITE, RIGHT, 20, d.dayName);
    textRight(g, &FreeSans9pt7b, WHITE, RIGHT, 40, d.date);
  } else {
    textRight(g, &FreeSans9pt7b, DIM, RIGHT, 20, "Syncing");
    textRight(g, &FreeSans9pt7b, DIM, RIGHT, 40, "time...");
  }
}

void drawLocation(Adafruit_GFX &g, const FaceData &d) {
  drawPin(g, LEFT, 104);
  textAt(g, &FreeSansBold9pt7b, WHITE, LEFT + 18, 116, d.city);
}

void drawWeather(Adafruit_GFX &g, const FaceData &d) {
  if (!d.weatherValid) {
    textRight(g, &FreeSans9pt7b, DIM, RIGHT, 116, "Weather");
    textRight(g, &FreeSans9pt7b, DIM, RIGHT, 134, d.weatherMessage ? d.weatherMessage : "...");
    return;
  }
  drawWeatherIcon(g, d.icon, WEATHER_ICON_X, WEATHER_ICON_Y);

  drawTemperature(g, RIGHT + 1 - temperatureWidth(g, d.tempC), 116, d.tempC, WHITE);

  textRight(g, &FreeSans9pt7b, GREY, RIGHT, 134, d.condition);
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

// The big number on the left, with its unit after it if there's room.
void cardValue(Adafruit_GFX &g, const char *value, const char *unit) {
  int16_t end = textAt(g, &FreeSansBold12pt7b, WHITE, CARD_VALUE_X, CARD_VALUE_BASELINE, value);
  if (unit && end + 3 + (int16_t)strlen(unit) * 6 < CARD_DIVIDER_X - 2) {
    smallText(g, GREY, end + 3, CARD_VALUE_BASELINE - 7, unit);
  }
}

void cardLabel(Adafruit_GFX &g, const char *label, uint16_t colour = GREY) {
  textAt(g, &FreeSans9pt7b, colour, CARD_X + 8, CARD_LABEL_BASELINE, label);
}

void drawWindPage(Adafruit_GFX &g, const FaceData &d) {
  drawWindIcon(g, CARD_X + 8, CARD_Y + 12);
  char value[8] = "--";
  if (d.weatherValid) snprintf(value, sizeof(value), "%d", d.windKmh);
  cardValue(g, value, "km/h");
  cardLabel(g, "Wind speed");

  drawCompass(g, CARD_DIAL_X, CARD_Y + 30, 17, d.weatherValid ? d.windDeg : -1);
  const char *point = (d.weatherValid && d.windDeg >= 0) ? compassPoint(d.windDeg) : "--";
  textCentre(g, &FreeSansBold9pt7b, WHITE, CARD_DIAL_X, CARD_Y + 68, point);
}

void drawPressurePage(Adafruit_GFX &g, const FaceData &d) {
  drawGaugeIcon(g, CARD_X + 8, CARD_Y + 12);
  char value[8] = "--";
  if (d.weatherValid) snprintf(value, sizeof(value), "%d", d.pressureHpa);
  cardValue(g, value, "hPa");
  int16_t end = textAt(g, &FreeSans9pt7b, GREY, CARD_X + 8, CARD_LABEL_BASELINE, "Pressure");
  if (d.weatherValid && d.pressureHpa >= 1000) {  // unit didn't fit after 4 digits
    smallText(g, DIM, end + 4, CARD_LABEL_BASELINE - 7, "hPa");
  }

  // Half-circle dial from 980 hPa (left) to 1040 hPa (right).
  const int16_t cx = CARD_DIAL_X, cy = CARD_Y + 38, r = 15;
  g.drawCircleHelper(cx, cy, r, 0x1 | 0x2, DIM);
  for (int i = 0; i <= 4; i++) {
    float a = (float)M_PI * (1.0f - i / 4.0f);  // left to right over the top
    g.drawLine(cx + cosf(a) * (r - 3), cy - sinf(a) * (r - 3), cx + cosf(a) * r,
               cy - sinf(a) * r, GREY);
  }
  if (!d.weatherValid) return;
  float t = (d.pressureHpa - 980) / 60.0f;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  float a = (float)M_PI * (1.0f - t);
  thickLine(g, cx, cy, cx + cosf(a) * (r - 4), cy - sinf(a) * (r - 4), WHITE);
  g.fillCircle(cx, cy, 2, WHITE);
  const char *level = d.pressureHpa < 1006 ? "LOW" : d.pressureHpa > 1020 ? "HIGH" : "NORMAL";
  smallTextCentre(g, WHITE, cx, CARD_Y + 48, level);
}

void drawUvPage(Adafruit_GFX &g, const FaceData &d) {
  drawSun(g, CARD_X + 22, CARD_Y + 22, 5);
  int band = 0;
  char value[8] = "--";
  if (d.weatherValid) {
    int uv = lroundf(d.uvIndex);
    snprintf(value, sizeof(value), "%d", uv);
    band = uv <= 2 ? 0 : uv <= 5 ? 1 : uv <= 7 ? 2 : uv <= 10 ? 3 : 4;
  }
  cardValue(g, value, "UV");
  if (d.weatherValid) {
    cardLabel(g, UV_LEVELS[band], UV_COLOURS[band]);
  } else {
    cardLabel(g, "UV index");
  }

  // Five-band colour scale, low at the bottom, with a marker on the current band.
  const int16_t barX = CARD_DIAL_X - 2, barTop = CARD_Y + 11, segment = 10;
  for (int i = 0; i < 5; i++) {
    int16_t y = barTop + (4 - i) * segment;
    g.fillRect(barX, y, 10, segment - 2, UV_COLOURS[i]);
  }
  if (d.weatherValid) {
    int16_t y = barTop + (4 - band) * segment + (segment - 2) / 2;
    g.fillTriangle(barX - 8, y - 4, barX - 8, y + 4, barX - 2, y, WHITE);
  }
}

void drawFeelsLikePage(Adafruit_GFX &g, const FaceData &d) {
  drawThermometer(g, CARD_X + 14, CARD_Y + 8);
  if (d.weatherValid) {
    drawTemperature(g, CARD_VALUE_X, CARD_VALUE_BASELINE, d.feelsLikeC, WHITE);
  } else {
    cardValue(g, "--", nullptr);
  }
  cardLabel(g, "Feels like");

  // How it compares with the actual temperature.
  if (!d.weatherValid) return;
  const int16_t cx = CARD_DIAL_X, cy = CARD_Y + 20;
  int diff = d.feelsLikeC - d.tempC;
  char text[8];
  if (diff == 0) {
    g.fillRect(cx - 7, cy - 4, 14, 3, GREY);
    g.fillRect(cx - 7, cy + 2, 14, 3, GREY);
    smallTextCentre(g, GREY, cx, CARD_Y + 38, "same");
  } else {
    uint16_t colour = diff > 0 ? WARM_ORANGE : COOL_BLUE;
    if (diff > 0) {
      g.fillTriangle(cx, cy - 8, cx - 8, cy + 4, cx + 8, cy + 4, colour);
    } else {
      g.fillTriangle(cx, cy + 6, cx - 8, cy - 6, cx + 8, cy - 6, colour);
    }
    snprintf(text, sizeof(text), "%+d", diff);
    int16_t x1;
    uint16_t w;
    measure(g, &FreeSansBold9pt7b, text, x1, w);
    int16_t x = cx - (x1 + (int16_t)w + 6) / 2;
    int16_t end = textAt(g, &FreeSansBold9pt7b, colour, x, CARD_Y + 44, text);
    g.drawCircle(end + 3, CARD_Y + 34, 2, colour);
    smallTextCentre(g, GREY, cx, CARD_Y + 52, diff > 0 ? "warmer" : "cooler");
  }
}

void drawCard(Adafruit_GFX &g, const FaceData &d) {
  g.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 9, CARD_FILL);
  g.drawRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 9, CARD_EDGE);
  g.drawFastVLine(CARD_DIVIDER_X, CARD_Y + 10, CARD_H - 20, CARD_EDGE);

  switch (d.cardPage) {
    case CARD_PRESSURE: drawPressurePage(g, d); break;
    case CARD_UV: drawUvPage(g, d); break;
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
  drawLocation(gfx, d);
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
