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
  CARD_UV,
  CARD_FEELS_LIKE,
  CARD_PAGE_COUNT,
};

struct FaceData {
  // Fixed text (set in main.cpp)
  const char *greeting;   // "Hello"
  const char *name;       // "Neehal"
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
  float uvIndex;
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

// 8-point compass name ("N", "NE", ...) for a wind direction in degrees.
const char *compassPoint(int degrees);
