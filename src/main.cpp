// SD Pro clock - custom firmware
//
// v0.1.0: WiFi, password-protected update page, status page, test screen.
// v0.2.0: display pin scan over 72 DC/reset/SPI-mode combinations.
//         Nothing appeared: the display ignored every combination.
// v0.3.0: uses the pins found by disassembling the seller's firmware,
//         including chip-select on GPIO15. Still blank.
// v0.4.0: sends the seller's own ST7789 startup table (panel voltages, gate
//         lines, gamma) and fixes the SPI mode setting: on the ESP8266,
//         SPI_MODE3 is 0x11, so passing the number 3 actually gave mode 1.
//         The scan then worked on every SPI mode 3 step and no mode 0 step.
// v0.4.1: SPI mode 3 is now the default, so the screen works out of the box.
// v0.4.2: rotation 2 is the default: the panel is mounted upside down.
// v0.5.0: the clock face (see face.cpp): greeting and name, date, time from
//         the internet in Sydney time, weather from OpenWeatherMap over HTTPS.
//         Removed PlatformIO-over-WiFi uploads and sdpro.local (mDNS) to keep
//         the firmware under 500KB; updates go through the /update page.
//
// v0.6.0: the bottom card rotates every 5 seconds between wind, pressure,
//         UV index and feels-like temperature. Weather now comes from
//         Open-Meteo (free for non-commercial use, no API key; data licensed
//         CC BY 4.0), since OpenWeatherMap's free feed has no UV index.
// v0.7.0: read-only file browser (/files) for the seller's files still in
//         the clock's storage (GIFs, photos). The pin scan is removed: the
//         display pins are confirmed. Weather is fetched over plain HTTP
//         (public data, and the HTTPS code was 82KB), making room for the
//         file system and, later, the spaceman animation.
// v0.8.0: the seller's spaceman animation (their /0.gif) plays right of the
//         bottom card. Converted on the PC by tools/make_spaceman.py into
//         spaceman_data.h (26KB), so the clock needs no GIF decoder.
//
// Size rule: an update is written beside the running firmware, so each
// version must stay under ~500KB (about half the 1MB firmware area). The
// build checks this (check_size.py).
//
// Pins from the seller's firmware (display setup code at 0x4021a24b):
//   CS = GPIO15, DC = GPIO0, RST = GPIO2, backlight = GPIO5 (on when LOW)
//   SPI data = GPIO13, SPI clock = GPIO14 (hardware SPI)
// Display chip: ST7789 (its startup command table is in the seller's image).

#include <Arduino.h>
#include <new>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <EEPROM.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>

#include "face.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h - copy include/secrets.example.h to include/secrets.h and fill it in."
#endif

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

// ---------------------------------------------------------------------------
// Clock face settings: change these to personalise the clock
// ---------------------------------------------------------------------------

static const char *FACE_GREETING = "Hello";
static const char *FACE_NAME = "Neehal";
static const char *FACE_CITY = "Sydney";

// Where the weather is for (decimal degrees). Sydney CBD; find others at
// https://www.latlong.net
static const char *WEATHER_LATITUDE = "-33.8688";
static const char *WEATHER_LONGITUDE = "151.2093";

// How long each page of the bottom card stays up.
static const uint32_t CARD_PAGE_MS = 5000;

// Sydney time, switching to daylight saving on the first Sunday of October
// and back on the first Sunday of April.
static const char *TIMEZONE = "AEST-10AEDT,M10.1.0,M4.1.0/3";

static const uint32_t WEATHER_REFRESH_MS = 10UL * 60 * 1000;  // every 10 minutes
static const uint32_t WEATHER_RETRY_MS = 60UL * 1000;         // after a failure

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static const char *HOSTNAME = "sdpro";          // name shown in your router's device list
static const char *AP_NAME = "SDPro-Recovery";   // fallback hotspot name
static const char *WEB_USER = "admin";
static const uint32_t WIFI_TIMEOUT_MS = 20000;

// Display pin settings, saved in EEPROM so they survive a reboot.
// Hardware SPI is fixed on the ESP8266: data = GPIO13, clock = GPIO14.
struct DisplayConfig {
  uint32_t magic;
  int8_t dc;             // data/command pin
  int8_t rst;            // reset pin (-1 = not connected)
  int8_t cs;             // chip-select pin (-1 = not connected)
  int8_t bl;             // backlight pin (-1 = not connected)
  uint8_t blActiveLow;   // 1 = backlight turns on when the pin is LOW
  uint8_t spiMode;       // 0-3; this clock needs 3
  uint8_t rotation;      // 0-3
  uint8_t initTable;     // 1 = seller's startup table, 0 = library's generic one
};

// Bumped whenever the layout changes ("SDP1" v0.1-0.2, "SDP2" v0.3,
// "SDP3" v0.4) so older saved settings are ignored instead of misread.
static const uint32_t CONFIG_MAGIC = 0x53445033;
// Confirmed on the clock (v0.4.0 scan, steps 2/3/6/7): only SPI mode 3 works.
// The panel is mounted upside down, so rotation 2 (180 degrees) is upright.
static const DisplayConfig DEFAULT_CONFIG = {CONFIG_MAGIC, 0, 2, 15, 5, 1, 3, 2, 1};

// The seller's ST7789 startup sequence, copied byte for byte from their
// firmware (file offset 0x6a308 in both v1.0.4 and v1.0.6). Format is the
// Adafruit library's: command count, then per command: command byte,
// argument count (+0x80 if a delay follows), arguments, delay in ms
// (255 = 500ms).
static const uint8_t SELLER_INIT[] PROGMEM = {
  20,
  0x11, 0x80, 255,                              // SLPOUT, wait 500ms
  0x3A, 0x81, 0x55, 10,                         // COLMOD: 16-bit colour
  0x36, 0x01, 0x00,                             // MADCTL
  0x2A, 0x04, 0x00, 0x00, 0x00, 0xF0,           // CASET 0-240
  0x2B, 0x04, 0x00, 0x00, 0x00, 0xF0,           // RASET 0-240
  0xB2, 0x05, 0x0C, 0x0C, 0x00, 0x33, 0x33,     // PORCTRL
  0xB7, 0x01, 0x35,                             // GCTRL
  0xBB, 0x01, 0x1F,                             // VCOMS
  0xC0, 0x01, 0x2C,                             // LCMCTRL
  0xC2, 0x01, 0x01,                             // VDVVRHEN
  0xC3, 0x01, 0x12,                             // VRHS
  0xC4, 0x01, 0x20,                             // VDVS
  0xC6, 0x01, 0x0F,                             // FRCTRL2: 60Hz
  0xD0, 0x02, 0xA4, 0xA1,                       // PWCTRL1
  0xE0, 0x0E, 0xD0, 0x08, 0x11, 0x08, 0x0C, 0x15, 0x39, 0x33, 0x50, 0x36,
              0x13, 0x14, 0x29, 0x2D,           // positive gamma
  0xE1, 0x0E, 0xD0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39, 0x44, 0x51, 0x0B,
              0x16, 0x14, 0x2F, 0x31,           // negative gamma
  0xE4, 0x03, 0x1D, 0x00, 0x00,                 // GATECTRL: 240 gate lines
  0x21, 0x80, 10,                               // INVON
  0x13, 0x80, 10,                               // NORON
  0x29, 0x80, 255,                              // DISPON, wait 500ms
};

// Adafruit_ST7789 keeps displayInit() protected; this exposes it.
class ClockDisplay : public Adafruit_ST7789 {
 public:
  using Adafruit_ST7789::Adafruit_ST7789;
  void runInitTable(const uint8_t *table) { displayInit(table); }
};

// Settings store SPI mode as 0-3, but the ESP8266 SPI library uses
// SPI_MODE0 = 0x00, SPI_MODE1 = 0x01, SPI_MODE2 = 0x10, SPI_MODE3 = 0x11.
uint8_t spiModeConstant(uint8_t mode) {
  switch (mode) {
    case 1: return SPI_MODE1;
    case 2: return SPI_MODE2;
    case 3: return SPI_MODE3;
    default: return SPI_MODE0;
  }
}

DisplayConfig cfg;
bool usingSavedConfig = false;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer updateServer;
ClockDisplay *tft = nullptr;
bool apMode = false;

// What the screen is showing. The clock face updates itself only in FACE.
enum Screen { SCREEN_FACE, SCREEN_TEST };
Screen screen = SCREEN_FACE;

FaceData face;
bool faceNeedsRedraw = true;
int lastMinuteShown = -1;
int lastDayShown = -1;
int localDay = -1;            // day of the year in Sydney time, for spotting midnight
uint32_t lastCardChange = 0;
uint8_t spacemanFrame = 0;
uint32_t lastSpacemanFrame = 0;

bool fsMounted = false;

uint32_t nextWeatherAt = 0;
String weatherStatus = "Not fetched yet";

// ---------------------------------------------------------------------------
// Config storage
// ---------------------------------------------------------------------------

// GPIO6-11 are wired to the flash chip; touching them crashes the ESP8266.
// GPIO13/14 are reserved for the display's SPI bus.
bool isSafePin(int pin) {
  const int allowed[] = {-1, 0, 1, 2, 3, 4, 5, 12, 15, 16};
  for (int p : allowed) {
    if (p == pin) return true;
  }
  return false;
}

void loadConfig() {
  EEPROM.begin(64);
  EEPROM.get(0, cfg);
  usingSavedConfig = cfg.magic == CONFIG_MAGIC && isSafePin(cfg.dc) &&
                     cfg.dc >= 0 && isSafePin(cfg.rst) && isSafePin(cfg.cs) && isSafePin(cfg.bl) &&
                     cfg.spiMode <= 3 && cfg.rotation <= 3 && cfg.initTable <= 1;
  if (!usingSavedConfig) cfg = DEFAULT_CONFIG;  // nothing is written until you press Save
}

void saveConfig() {
  cfg.magic = CONFIG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void setBacklight(bool on) {
  if (cfg.bl < 0) return;
  pinMode(cfg.bl, OUTPUT);
  bool level = cfg.blActiveLow ? !on : on;
  digitalWrite(cfg.bl, level ? HIGH : LOW);
}

// (Re)creates the display driver with the given pins.
// The driver lives in a fixed memory slot rather than being new'd and
// deleted each time, so re-creating it can't fragment the ESP8266's small heap.
alignas(ClockDisplay) static uint8_t tftStorage[sizeof(ClockDisplay)];

void startDisplay(int8_t dc, int8_t rst, int8_t cs, uint8_t mode) {
  if (tft) {
    tft->~ClockDisplay();
    tft = nullptr;
  }
  // With a CS pin, the driver raises and lowers it around every transfer,
  // which is what the seller's firmware does on GPIO15. Without one, hold
  // GPIO15 low so the display stays selected.
  if (cs < 0 && dc != 15 && rst != 15) {
    pinMode(15, OUTPUT);
    digitalWrite(15, LOW);
  }
  tft = new (tftStorage) ClockDisplay(cs, dc, rst);
  tft->init(240, 240, spiModeConstant(mode));  // pins, reset pulse, generic table
  if (cfg.initTable) tft->runInitTable(SELLER_INIT);
  tft->setRotation(cfg.rotation);
  tft->setSPISpeed(27000000);  // conservative; ST7789 is rated for more
}

String currentIp() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

void drawTestScreen() {
  if (!tft) return;
  tft->fillScreen(ST77XX_BLACK);

  // Colour bars: if these show in the wrong order, the colour mode is off.
  tft->fillRect(0, 0, 80, 40, ST77XX_RED);
  tft->fillRect(80, 0, 80, 40, ST77XX_GREEN);
  tft->fillRect(160, 0, 80, 40, ST77XX_BLUE);

  tft->setTextWrap(false);
  tft->setTextColor(ST77XX_WHITE);
  tft->setTextSize(3);
  tft->setCursor(12, 70);
  tft->print("Hello ");
  tft->print(FACE_NAME);

  tft->setTextSize(2);
  tft->setTextColor(ST77XX_YELLOW);
  tft->setCursor(12, 130);
  tft->print(apMode ? "Recovery mode" : "Custom firmware");
  tft->setCursor(12, 155);
  tft->print("v" FW_VERSION);
  tft->setCursor(12, 180);
  tft->print(currentIp());

  // Corner markers check the image isn't shifted or cut off.
  tft->drawRect(0, 0, 240, 240, ST77XX_WHITE);
  tft->fillRect(0, 232, 8, 8, ST77XX_CYAN);
  tft->fillRect(232, 232, 8, 8, ST77XX_MAGENTA);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// Fills the time and date fields of `face` from the system clock, which the
// ESP8266 keeps in sync with internet time servers after configTime().
void updateFaceTime() {
  time_t now = time(nullptr);
  face.timeValid = now > 1700000000;  // before sync the clock reads 1970
  if (!face.timeValid) return;

  struct tm t;
  localtime_r(&now, &t);
  static const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  face.hour24 = t.tm_hour;
  face.minute = t.tm_min;
  localDay = t.tm_year * 400 + t.tm_yday;
  snprintf(face.dayName, sizeof(face.dayName), "%s", DAYS[t.tm_wday]);
  snprintf(face.date, sizeof(face.date), "%d %s %d", t.tm_mday, MONTHS[t.tm_mon],
           t.tm_year + 1900);
}

// ---------------------------------------------------------------------------
// Weather
// ---------------------------------------------------------------------------

// Turns a WMO weather code (as used by Open-Meteo) into an icon and label.
// Codes: https://open-meteo.com/en/docs (section "WMO Weather interpretation codes")
void describeWeather(int code, bool day) {
  const char *label = "Cloudy";
  WeatherIcon icon = ICON_CLOUD;
  switch (code) {
    case 0:
      label = day ? "Sunny" : "Clear", icon = day ? ICON_SUN : ICON_MOON;
      break;
    case 1:
      label = day ? "Mostly sunny" : "Mostly clear", icon = day ? ICON_PARTLY_DAY : ICON_PARTLY_NIGHT;
      break;
    case 2:
      label = "Partly cloudy", icon = day ? ICON_PARTLY_DAY : ICON_PARTLY_NIGHT;
      break;
    case 3:
      label = "Cloudy", icon = ICON_CLOUD;
      break;
    case 45: case 48:
      label = "Fog", icon = ICON_MIST;
      break;
    case 51: case 53: case 55: case 56: case 57:
      label = "Drizzle", icon = ICON_RAIN;
      break;
    case 61: case 63: case 66: case 80: case 81:
      label = code >= 80 ? "Showers" : "Rain", icon = ICON_RAIN;
      break;
    case 65: case 67: case 82:
      label = "Heavy rain", icon = ICON_RAIN;
      break;
    case 71: case 73: case 75: case 77: case 85: case 86:
      label = "Snow", icon = ICON_SNOW;
      break;
    case 95: case 96: case 99:
      label = "Storm", icon = ICON_STORM;
      break;
  }
  snprintf(face.condition, sizeof(face.condition), "%s", label);
  face.icon = icon;
}

// Downloads current weather from Open-Meteo. Returns true if the face
// needs redrawing. Free, non-commercial use: under 10,000 calls a day;
// this makes 144 (one every 10 minutes).
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    weatherStatus = "Waiting for WiFi";
    nextWeatherAt = millis() + WEATHER_RETRY_MS;
    return false;
  }

  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + WEATHER_LATITUDE +
               "&longitude=" + WEATHER_LONGITUDE +
               "&current=temperature_2m,apparent_temperature,is_day,weather_code,"
               "pressure_msl,wind_speed_10m,wind_direction_10m,uv_index"
               "&wind_speed_unit=kmh&timezone=auto";

  // Plain HTTP: the data is public weather and nothing secret is sent.
  // (The HTTPS version didn't check certificates anyway, so it protected
  // nothing more, and it cost 82KB of firmware and ~20KB of memory.)
  WiFiClient client;
  HTTPClient http;
  http.useHTTP10(true);  // plain (not chunked) response, easier to stream-parse
  http.setTimeout(8000);
  bool changed = false;
  if (!http.begin(client, url)) {
    weatherStatus = "Could not start request";
  } else {
    int code = http.GET();
    if (code == 200) {
      // Only keep the fields we use, so the response fits in little memory.
      JsonDocument filter;
      JsonObject want = filter["current"].to<JsonObject>();
      for (const char *key : {"temperature_2m", "apparent_temperature", "is_day", "weather_code",
                              "pressure_msl", "wind_speed_10m", "wind_direction_10m", "uv_index"}) {
        want[key] = true;
      }

      JsonDocument doc;
      DeserializationError err =
          deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      JsonObject now = doc["current"];
      if (err) {
        weatherStatus = String("Could not read response: ") + err.c_str();
      } else if (now.isNull()) {
        weatherStatus = "Response had no current weather";
      } else {
        face.tempC = lroundf(now["temperature_2m"] | 0.0f);
        face.feelsLikeC = lroundf(now["apparent_temperature"] | 0.0f);
        describeWeather(now["weather_code"] | 3, (now["is_day"] | 1) == 1);
        face.pressureHpa = lroundf(now["pressure_msl"] | 0.0f);
        face.windKmh = lroundf(now["wind_speed_10m"] | 0.0f);
        face.windDeg = now["wind_direction_10m"] | -1;
        face.uvIndex = now["uv_index"] | 0.0f;
        face.weatherValid = true;
        changed = true;

        char when[6] = "--:--";
        if (face.timeValid) snprintf(when, sizeof(when), "%02d:%02d", face.hour24, face.minute);
        weatherStatus = String("OK at ") + when + ": " + face.tempC + " C (feels " +
                        face.feelsLikeC + "), " + face.condition + ", wind " + face.windKmh +
                        " km/h " + (face.windDeg >= 0 ? compassPoint(face.windDeg) : "") + ", " +
                        face.pressureHpa + " hPa, UV " + String(face.uvIndex, 1);
      }
    } else if (code == 400) {
      weatherStatus = "Request rejected (400). Check WEATHER_LATITUDE / WEATHER_LONGITUDE.";
    } else if (code == 429) {
      weatherStatus = "Too many requests (429). Will retry.";
    } else {
      weatherStatus = String("Request failed: ") + code + " " + HTTPClient::errorToString(code);
    }
    http.end();
  }

  if (changed) {
    nextWeatherAt = millis() + WEATHER_REFRESH_MS;
  } else {
    nextWeatherAt = millis() + WEATHER_RETRY_MS;
    if (!face.weatherValid) {  // keep showing old data if we have some
      face.weatherMessage = "unavailable";
      changed = true;
    }
  }
  Serial.println("Weather: " + weatherStatus);
  return changed;
}

// ---------------------------------------------------------------------------
// File system (the seller's GIFs, photos and settings are still in it)
// ---------------------------------------------------------------------------

// Mounts the 3MB file system READ-ONLY in practice: nothing here writes to
// it. Auto-format is switched off, because the library's default is to
// wipe the file system if it can't mount it - that would erase the
// seller's files.
void mountFileSystem() {
  LittleFSConfig fsConfig;
  fsConfig.setAutoFormat(false);
  LittleFS.setConfig(fsConfig);
  fsMounted = LittleFS.begin();
  Serial.println(fsMounted ? "File system mounted" : "File system could not be mounted");
}

const char *contentTypeFor(const String &path) {
  if (path.endsWith(".gif")) return "image/gif";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".bmp")) return "image/bmp";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".txt")) return "text/plain";
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  return "application/octet-stream";
}

// Adds a table row per file under `path`, including sub-folders.
void listFiles(String &html, const String &path, int depth) {
  Dir dir = LittleFS.openDir(path);
  while (dir.next()) {
    String full = path + dir.fileName();
    if (dir.isDirectory()) {
      html += "<tr><td colspan='2'><b>" + full + "/</b></td></tr>";
      if (depth < 4) listFiles(html, full + "/", depth + 1);
    } else {
      html += "<tr><td><a href='/files/get?path=" + full + "'>" + full + "</a></td><td>" +
              String(dir.fileSize()) + "</td></tr>";
    }
  }
}

void showFace() {
  screen = SCREEN_FACE;
  faceNeedsRedraw = true;
}

// Called every loop: keeps the clock face current without redrawing more
// than needed (a full redraw flickers briefly; the time alone doesn't).
void updateFace() {
  if ((int32_t)(millis() - nextWeatherAt) >= 0) {
    if (fetchWeather()) faceNeedsRedraw = true;
  }
  if (screen != SCREEN_FACE || !tft) return;

  updateFaceTime();
  int minute = face.timeValid ? face.hour24 * 60 + face.minute : -2;
  int day = face.timeValid ? localDay : -2;
  if (day != lastDayShown) faceNeedsRedraw = true;  // midnight (or time just synced)

  if (faceNeedsRedraw) {
    drawFace(*tft, face);
    drawSpaceman(*tft, spacemanFrame);
    faceNeedsRedraw = false;
    lastCardChange = millis();
  } else {
    if (minute != lastMinuteShown) drawFaceTime(*tft, face);
    if (millis() - lastCardChange >= CARD_PAGE_MS) {
      face.cardPage = (CardPage)((face.cardPage + 1) % CARD_PAGE_COUNT);
      drawFaceCard(*tft, face);
      lastCardChange = millis();
    }
  }
  // Spaceman: next frame when it's due. Each frame takes a few ms to send;
  // if the clock was busy (e.g. fetching weather) it just carries on.
  if (millis() - lastSpacemanFrame >= spacemanFrameMs()) {
    lastSpacemanFrame = millis();
    spacemanFrame = (spacemanFrame + 1) % spacemanFrameCount();
    drawSpaceman(*tft, spacemanFrame);
  }
  lastMinuteShown = minute;
  lastDayShown = day;
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

void startWifi() {
  WiFi.persistent(false);   // don't wear out flash by re-saving WiFi details
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("Connecting to %s", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    return;
  }

  // Couldn't join home WiFi: open a hotspot so the update page stays
  // reachable at http://192.168.4.1. The clock keeps retrying home WiFi.
  apMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME, AP_PASSWORD);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi failed. Recovery hotspot '%s' at %s\n", AP_NAME,
                WiFi.softAPIP().toString().c_str());
}

// ---------------------------------------------------------------------------
// Web pages
// ---------------------------------------------------------------------------

bool requireLogin() {
  if (server.authenticate(WEB_USER, OTA_PASSWORD)) return true;
  server.requestAuthentication();
  return false;
}

String pinInput(const char *name, int value) {
  return String("<input name='") + name + "' type='number' min='-1' max='16' value='" +
         value + "'>";
}

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRoot() {
  if (!requireLogin()) return;

  String html;
  html.reserve(4200);
  html += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>SD Pro</title><style>"
            "body{font-family:sans-serif;max-width:560px;margin:24px auto;padding:0 16px;line-height:1.5}"
            "td{padding:2px 12px 2px 0}input{width:70px}button{margin:4px 4px 4px 0;padding:6px 12px}"
            ".note{padding:8px 12px;border:1px solid #c90;border-radius:6px}"
            "</style></head><body><h2>SD Pro custom firmware</h2>");

  // ----- Clock face -----
  html += F("<h3>Clock face</h3><table>");
  html += "<tr><td>Time</td><td>" +
          String(face.timeValid ? String(face.dayName) + " " + face.date + ", " +
                                      (face.hour24 < 10 ? "0" : "") + face.hour24 + ":" +
                                      (face.minute < 10 ? "0" : "") + face.minute
                                : String("not synced yet")) +
          "</td></tr>";
  html += "<tr><td>Weather</td><td>" + weatherStatus +
          "<br><small>Weather data by <a href='https://open-meteo.com/'>Open-Meteo.com</a> "
          "(CC BY 4.0)</small></td></tr>";
  html += "<tr><td>Screen</td><td>" +
          String(screen == SCREEN_FACE ? "Clock face" : "Test screen") +
          "</td></tr></table>"
          "<form method='post' action='/face' style='display:inline'><button>Show clock face</button></form>"
          "<form method='post' action='/weather' style='display:inline'><button>Refresh weather</button></form>"
          "<p><a href='/files'>Files on the clock</a> (GIFs, photos and settings left by the seller's firmware)</p>";

  // ----- Status -----
  html += F("<h3>Status</h3><table>");
  html += "<tr><td>Version</td><td>" FW_VERSION "</td></tr>";
  html += "<tr><td>Mode</td><td>" + String(apMode ? "Recovery hotspot" : "Home WiFi") + "</td></tr>";
  html += "<tr><td>IP address</td><td>" + currentIp() + "</td></tr>";
  html += "<tr><td>WiFi signal</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  html += "<tr><td>Uptime</td><td>" + String(millis() / 1000) + " s</td></tr>";
  html += "<tr><td>Free memory</td><td>" + String(ESP.getFreeHeap()) + " bytes</td></tr>";
  html += "<tr><td>Firmware size</td><td>" + String(ESP.getSketchSize()) + " bytes</td></tr>";
  html += "<tr><td>Space for next update</td><td>" + String(ESP.getFreeSketchSpace()) + " bytes</td></tr>";
  html += "<tr><td>Pin settings</td><td>" + String(usingSavedConfig ? "Saved" : "Defaults (not saved yet)") + "</td></tr>";

  // ----- Manual pins -----
  html += F("</table><h3>Display pins</h3>"
            "<p>Defaults are the pins found in the seller's firmware. Use -1 for not "
            "connected. Data (GPIO13) and clock (GPIO14) are fixed. "
            "Saving reboots the clock.</p>"
            "<form method='post' action='/config'><table>");
  html += "<tr><td>DC pin</td><td>" + pinInput("dc", cfg.dc) + "</td></tr>";
  html += "<tr><td>Reset pin</td><td>" + pinInput("rst", cfg.rst) + "</td></tr>";
  html += "<tr><td>Chip-select pin</td><td>" + pinInput("cs", cfg.cs) + "</td></tr>";
  html += "<tr><td>Backlight pin</td><td>" + pinInput("bl", cfg.bl) + "</td></tr>";
  html += "<tr><td>Backlight on when LOW</td><td><input name='bllow' type='number' min='0' max='1' value='" +
          String(cfg.blActiveLow) + "'> (1 = yes)</td></tr>";
  html += "<tr><td>SPI mode</td><td><input name='spi' type='number' min='0' max='3' value='" +
          String(cfg.spiMode) + "'> (0-3)</td></tr>";
  html += "<tr><td>Rotation</td><td><input name='rot' type='number' min='0' max='3' value='" +
          String(cfg.rotation) + "'> (0-3)</td></tr>";
  html += "<tr><td>Startup table</td><td><input name='init' type='number' min='0' max='1' value='" +
          String(cfg.initTable) + "'> (1 = seller's, 0 = generic)</td></tr>";
  html += F("</table><button type='submit'>Save and reboot</button></form>"
            "<h3>Actions</h3>"
            "<form method='post' action='/test' style='display:inline'><button>Show test screen</button></form>"
            "<form method='post' action='/backlight?on=1' style='display:inline'><button>Backlight on</button></form>"
            "<form method='post' action='/backlight?on=0' style='display:inline'><button>Backlight off</button></form>"
            "<form method='post' action='/reboot' style='display:inline'><button>Reboot</button></form>"
            "<p><a href='/update'>Upload new firmware</a></p></body></html>");
  server.send(200, "text/html", html);
}

void handleConfig() {
  if (!requireLogin()) return;

  DisplayConfig next = cfg;
  next.dc = server.arg("dc").toInt();
  next.rst = server.arg("rst").toInt();
  next.cs = server.arg("cs").toInt();
  next.bl = server.arg("bl").toInt();
  next.blActiveLow = server.arg("bllow").toInt() ? 1 : 0;
  next.spiMode = constrain(server.arg("spi").toInt(), 0, 3);
  next.rotation = constrain(server.arg("rot").toInt(), 0, 3);
  next.initTable = server.arg("init").toInt() ? 1 : 0;

  if (next.dc < 0 || !isSafePin(next.dc) || !isSafePin(next.rst) || !isSafePin(next.cs) ||
      !isSafePin(next.bl)) {
    server.send(400, "text/plain",
                "Rejected: allowed pins are -1, 0, 1, 2, 3, 4, 5, 12, 15, 16 (DC can't be -1).");
    return;
  }

  cfg = next;
  saveConfig();
  server.send(200, "text/html",
              "<meta http-equiv='refresh' content='8;url=/'>Saved. Rebooting... this page reloads in 8 seconds.");
  delay(500);
  ESP.restart();
}

void handleTest() {
  if (!requireLogin()) return;
  screen = SCREEN_TEST;
  startDisplay(cfg.dc, cfg.rst, cfg.cs, cfg.spiMode);
  drawTestScreen();
  redirectHome();
}

void handleFace() {
  if (!requireLogin()) return;
  showFace();
  redirectHome();
}

void handleWeather() {
  if (!requireLogin()) return;
  nextWeatherAt = millis();  // fetch on the next loop
  redirectHome();
}

void handleBacklight() {
  if (!requireLogin()) return;
  setBacklight(server.arg("on") == "1");
  redirectHome();
}

void handleReboot() {
  if (!requireLogin()) return;
  server.send(200, "text/html", "<meta http-equiv='refresh' content='8;url=/'>Rebooting...");
  delay(500);
  ESP.restart();
}

void handleFiles() {
  if (!requireLogin()) return;
  String html;
  html.reserve(3000);
  html += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Files</title>"
            "<style>body{font-family:sans-serif;max-width:640px;margin:24px auto;padding:0 16px}"
            "td{padding:2px 14px 2px 0}</style></head><body><p><a href='/'>Back</a></p>"
            "<h3>Files on the clock</h3>");
  if (!fsMounted) {
    html += F("<p>The file system could not be read. Nothing was changed.</p></body></html>");
    server.send(200, "text/html", html);
    return;
  }
  FSInfo info;
  LittleFS.info(info);
  html += "<p>Used " + String(info.usedBytes) + " of " + String(info.totalBytes) +
          " bytes. Click a file to open or download it (read-only).</p>"
          "<table><tr><th align='left'>File</th><th align='left'>Bytes</th></tr>";
  listFiles(html, "/", 0);
  html += F("</table></body></html>");
  server.send(200, "text/html", html);
}

void handleFileGet() {
  if (!requireLogin()) return;
  String path = server.arg("path");
  if (!fsMounted || !path.startsWith("/") || !LittleFS.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  File f = LittleFS.open(path, "r");
  String name = path.substring(path.lastIndexOf('/') + 1);
  server.sendHeader("Content-Disposition", "inline; filename=\"" + name + "\"");
  server.streamFile(f, contentTypeFor(path));
  f.close();
}

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/test", HTTP_POST, handleTest);
  server.on("/face", HTTP_POST, handleFace);
  server.on("/weather", HTTP_POST, handleWeather);
  server.on("/files", HTTP_GET, handleFiles);
  server.on("/files/get", HTTP_GET, handleFileGet);
  server.on("/backlight", HTTP_POST, handleBacklight);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });

  // The most important line in this file: without it the clock can't be updated again.
  updateServer.setup(&server, "/update", WEB_USER, OTA_PASSWORD);
  server.begin();
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("SD Pro custom firmware v" FW_VERSION);

  loadConfig();
  Serial.printf("Display pins: DC=%d RST=%d CS=%d BL=%d (active %s) SPI mode %d, %s table\n",
                cfg.dc, cfg.rst, cfg.cs, cfg.bl, cfg.blActiveLow ? "LOW" : "HIGH", cfg.spiMode,
                cfg.initTable ? "seller's" : "generic");

  // Network and update page come first, so a display problem can never
  // stop the clock from accepting the next firmware.
  mountFileSystem();
  startWifi();
  startWebServer();
  configTime(TIMEZONE, "pool.ntp.org", "time.google.com");

  face.greeting = FACE_GREETING;
  face.name = FACE_NAME;
  face.city = FACE_CITY;
  face.weatherMessage = "loading...";
  face.windDeg = -1;
  nextWeatherAt = millis() + 3000;  // give the time sync a moment first

  setBacklight(true);
  startDisplay(cfg.dc, cfg.rst, cfg.cs, cfg.spiMode);
  showFace();
  Serial.println("Ready.");
}

void loop() {
  server.handleClient();
  updateFace();
}
