// SD Pro clock - clock face drawing.
//
// Draws the whole face onto any Adafruit_GFX surface: the real ST7789 on
// the clock, or an off-screen canvas on a PC to render a preview image.
// Nothing in here talks to WiFi or the time server; main.cpp gathers the
// data into a FaceData and hands it over.

#pragma once

#include <Adafruit_GFX.h>

enum WeatherIcon : uint8_t {
  ICON_NONE,
  ICON_SUN,
  ICON_MOON,
  ICON_PARTLY_DAY,
  ICON_PARTLY_NIGHT,
  ICON_CLOUD,
  ICON_RAIN,
  ICON_STORM,
  ICON_SNOW,
  ICON_MIST,
};

// The bottom card cycles through these, in this order.
enum CardPage : uint8_t {
  CARD_WIND,
  CARD_PRESSURE,
  CARD_CONDITION,
  CARD_FEELS_LIKE,
  CARD_PAGE_COUNT,
};

struct FaceData {
  // Fixed text (set in main.cpp)
  const char *city;       // "Sydney"

  // Time (from the internet)
  bool timeValid;
  int hour24;             // 0-23
  int minute;             // 0-59
  char dayName[4];        // "Mon"
  char date[16];          // "28 Jul 2026"

  // Weather (from Open-Meteo)
  bool weatherValid;
  const char *weatherMessage;  // shown instead of weather when not valid
  int tempC;
  char condition[16];     // "Sunny"
  WeatherIcon icon;
  int windKmh;
  int windDeg;            // 0-359, direction the wind comes from; -1 = unknown
  int pressureHpa;        // sea-level pressure
  int feelsLikeC;

  // Which card page is showing
  CardPage cardPage;
};

// Clears the screen and draws everything.
void drawFace(Adafruit_GFX &gfx, const FaceData &d);

// Redraws only the time (flicker-free), for the once-a-minute update.
void drawFaceTime(Adafruit_GFX &gfx, const FaceData &d);

// Redraws only the bottom card, for switching pages.
void drawFaceCard(Adafruit_GFX &gfx, const FaceData &d);

// ----- Spaceman animation (the seller's /0.gif), right of the bottom card -----

constexpr int16_t SPACEMAN_X = 171;  // in the gap right of the card
constexpr int16_t SPACEMAN_Y = 146;  // centred under the temperature

uint8_t spacemanFrameCount();
uint16_t spacemanFrameMs();          // how long each frame shows, from the GIF
int16_t spacemanSize();              // width = height (80)

// Unpacks one frame of the animation a row at a time.
class SpacemanFrame {
 public:
  explicit SpacemanFrame(uint8_t frame);
  void nextRow(uint16_t *pixels);    // fills spacemanSize() colours

 private:
  uint32_t pos_;       // next byte of the packed data
  uint16_t colour_;    // colour of the current run
  uint8_t left_ = 0;   // pixels left in the current run
};

// Draws one frame. A template so the display's own fast drawRGBBitmap is
// used (the generic one sends pixels one at a time).
template <class Gfx>
void drawSpaceman(Gfx &gfx, uint8_t frame) {
  uint16_t row[80];
  SpacemanFrame f(frame);
  for (int16_t y = 0; y < spacemanSize(); y++) {
    f.nextRow(row);
    gfx.drawRGBBitmap(SPACEMAN_X, SPACEMAN_Y + y, row, spacemanSize(), 1);
  }
}

// 8-point compass name ("N", "NE", ...) for a wind direction in degrees.
const char *compassPoint(int degrees);
