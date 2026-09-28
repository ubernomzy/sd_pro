// SD Pro clock - clock face drawing. See face.h.
//
// Layout (240 x 240, black background). The case hides the bottom pixel or
// two, so nothing is drawn below y = 230.
//
//   Hello                      Mon       <- greeting / day
//   Neehal             28 Jul 2026       <- name / date
//   10:24 AM                 (icon)      <- time / weather icon
//   (pin) Sydney                22°C     <- city / temperature
//         Australia            Sunny     <- country / condition
//   [ wind  18 km/h | compass ] [ leaf ] <- wind card / comfort card
//   [ Wind speed    |   NE    ] [Pleasant]
//                                [Climate ]

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

constexpr uint16_t CARD_BLUE_FILL = rgb(6, 14, 30);
constexpr uint16_t CARD_BLUE_EDGE = rgb(35, 95, 170);
constexpr uint16_t CARD_GREEN_FILL = rgb(6, 24, 14);
constexpr uint16_t CARD_GREEN_EDGE = rgb(45, 170, 85);
constexpr uint16_t LEAF_GREEN = rgb(70, 205, 95);
constexpr uint16_t LEAF_VEIN = rgb(20, 90, 40);
constexpr uint16_t CARD_AMBER_FILL = rgb(28, 18, 4);
constexpr uint16_t CARD_AMBER_EDGE = rgb(190, 130, 30);
constexpr uint16_t LEAF_AMBER = rgb(235, 170, 50);
constexpr uint16_t LEAF_AMBER_VEIN = rgb(110, 70, 10);

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

constexpr int16_t CARD_Y = 150;
constexpr int16_t CARD_H = 80;
constexpr int16_t WIND_CARD_X = 4;
constexpr int16_t WIND_CARD_W = 148;
constexpr int16_t COMFORT_CARD_X = 156;
constexpr int16_t COMFORT_CARD_W = 80;
constexpr int16_t WIND_DIVIDER_X = 112;

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

// Leaf, filled, pointing up-right, about 24 x 24, centred at (cx, cy).
void drawLeaf(Adafruit_GFX &g, int16_t cx, int16_t cy, uint16_t fill, uint16_t vein) {
  // A lens shape (two overlapping circles) along a 45-degree axis.
  const float halfLength = 11.0f, halfWidth = 5.5f;
  const float radius = (halfLength * halfLength + halfWidth * halfWidth) / (2 * halfWidth);
  const float offset = radius - halfWidth;
  const float k = 0.70710678f;
  for (int dy = -12; dy <= 12; dy++) {
    for (int dx = -12; dx <= 12; dx++) {
      float u = (dx - dy) * k;  // along the leaf (up-right)
      float v = (dx + dy) * k;  // across the leaf
      float a = v - offset, b = v + offset;
      if (u * u + a * a <= radius * radius && u * u + b * b <= radius * radius) {
        g.drawPixel(cx + dx, cy + dy, fill);
      }
    }
  }
  // Centre vein and a short stem out of the bottom-left tip.
  g.drawLine(cx - 7, cy + 7, cx + 6, cy - 6, vein);
  thickLine(g, cx - 8, cy + 8, cx - 12, cy + 12, fill);
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
  textAt(g, &FreeSans9pt7b, GREY, LEFT + 18, 134, d.country);
}

void drawWeather(Adafruit_GFX &g, const FaceData &d) {
  if (!d.weatherValid) {
    textRight(g, &FreeSans9pt7b, DIM, RIGHT, 116, "Weather");
    textRight(g, &FreeSans9pt7b, DIM, RIGHT, 134, d.weatherMessage ? d.weatherMessage : "...");
    return;
  }
  drawWeatherIcon(g, d.icon, WEATHER_ICON_X, WEATHER_ICON_Y);

  // Temperature with a hand-drawn degree sign (the fonts are ASCII only).
  char number[8];
  snprintf(number, sizeof(number), "%d", d.tempC);
  int16_t x1, cx1;
  uint16_t w, cw;
  measure(g, &FreeSansBold12pt7b, number, x1, w);
  measure(g, &FreeSansBold12pt7b, "C", cx1, cw);
  const int16_t degreeGap = 9;
  int16_t cX = RIGHT - (cx1 + (int16_t)cw) + 1;
  int16_t numberX = cX - degreeGap - (x1 + (int16_t)w);
  textAt(g, &FreeSansBold12pt7b, WHITE, numberX, 116, number);
  g.drawCircle(cX - degreeGap / 2 - 1, 116 - 15, 2, WHITE);
  g.drawCircle(cX - degreeGap / 2 - 1, 116 - 15, 3, WHITE);
  textAt(g, &FreeSansBold12pt7b, WHITE, cX, 116, "C");

  textRight(g, &FreeSans9pt7b, GREY, RIGHT, 134, d.condition);
}

void drawWindCard(Adafruit_GFX &g, const FaceData &d) {
  g.fillRoundRect(WIND_CARD_X, CARD_Y, WIND_CARD_W, CARD_H, 9, CARD_BLUE_FILL);
  g.drawRoundRect(WIND_CARD_X, CARD_Y, WIND_CARD_W, CARD_H, 9, CARD_BLUE_EDGE);
  g.drawFastVLine(WIND_DIVIDER_X, CARD_Y + 10, CARD_H - 20, CARD_BLUE_EDGE);

  drawWindIcon(g, WIND_CARD_X + 8, CARD_Y + 12);

  char speed[8];
  if (d.weatherValid) {
    snprintf(speed, sizeof(speed), "%d", d.windKmh);
  } else {
    strcpy(speed, "--");
  }
  int16_t end = textAt(g, &FreeSansBold12pt7b, WHITE, WIND_CARD_X + 44, CARD_Y + 32, speed);
  if (end + 26 < WIND_DIVIDER_X) {
    g.setFont(nullptr);
    g.setTextSize(1);
    g.setTextColor(GREY);
    g.setCursor(end + 3, CARD_Y + 25);
    g.print("km/h");
  }
  textAt(g, &FreeSans9pt7b, GREY, WIND_CARD_X + 8, CARD_Y + 62, "Wind speed");

  const int16_t compassX = WIND_DIVIDER_X + (WIND_CARD_X + WIND_CARD_W - WIND_DIVIDER_X) / 2;
  drawCompass(g, compassX, CARD_Y + 30, 17, d.weatherValid ? d.windDeg : -1);
  const char *point = (d.weatherValid && d.windDeg >= 0) ? compassPoint(d.windDeg) : "--";
  textCentre(g, &FreeSansBold9pt7b, WHITE, compassX, CARD_Y + 68, point);
}

void drawComfortCard(Adafruit_GFX &g, const FaceData &d) {
  // Green when pleasant, amber otherwise, neutral blue-grey with no data.
  uint16_t fill = CARD_BLUE_FILL, edge = CARD_BLUE_EDGE, leaf = DIM, vein = BLACK;
  if (d.weatherValid && d.comfortGood) {
    fill = CARD_GREEN_FILL, edge = CARD_GREEN_EDGE, leaf = LEAF_GREEN, vein = LEAF_VEIN;
  } else if (d.weatherValid) {
    fill = CARD_AMBER_FILL, edge = CARD_AMBER_EDGE, leaf = LEAF_AMBER, vein = LEAF_AMBER_VEIN;
  }
  g.fillRoundRect(COMFORT_CARD_X, CARD_Y, COMFORT_CARD_W, CARD_H, 9, fill);
  g.drawRoundRect(COMFORT_CARD_X, CARD_Y, COMFORT_CARD_W, CARD_H, 9, edge);
  const int16_t centre = COMFORT_CARD_X + COMFORT_CARD_W / 2;
  drawLeaf(g, centre, CARD_Y + 22, leaf, vein);
  textCentre(g, &FreeSansBold9pt7b, WHITE, centre, CARD_Y + 56,
             d.weatherValid ? d.comfort : "--");
  textCentre(g, &FreeSans9pt7b, GREY, centre, CARD_Y + 73, "Climate");
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
  drawWindCard(gfx, d);
  drawComfortCard(gfx, d);
}

void describeComfort(int tempC, int humidity, char *label, size_t labelSize, bool &good) {
  const char *text;
  good = false;
  if (humidity >= 80 && tempC >= 20) {
    text = "Humid";
  } else if (tempC < 10) {
    text = "Cold";
  } else if (tempC < 16) {
    text = "Cool";
  } else if (tempC <= 26) {
    text = "Pleasant";
    good = true;
  } else if (tempC <= 32) {
    text = "Warm";
  } else {
    text = "Hot";
  }
  snprintf(label, labelSize, "%s", text);
}

const char *compassPoint(int degrees) {
  static const char *POINTS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = ((degrees % 360 + 360) % 360 + 22) / 45;
  return POINTS[index % 8];
}
