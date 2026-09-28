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
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h - copy include/secrets.example.h to include/secrets.h and fill it in."
#endif

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static const char *HOSTNAME = "sdpro";          // reachable as sdpro.local
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

// ---------------------------------------------------------------------------
// Pin scan
// ---------------------------------------------------------------------------

// With the seller's pins known, the scan only tries a few variants around
// them: chip-select driven or not, SPI mode 0 or 3, reset driven or not.
static const int8_t CS_CANDIDATES[] = {15, -1};
static const uint8_t MODE_CANDIDATES[] = {0, 3};
static const int8_t RST_CANDIDATES[] = {2, -1};
static const int8_t SCAN_DC = 0;

static const uint32_t SCAN_STEP_MS = 4000;  // seller's table alone takes ~1s
static const int MAX_STEPS = 16;

struct ScanStep {
  int8_t dc;
  int8_t rst;
  int8_t cs;
  uint8_t mode;
};

ScanStep scanSteps[MAX_STEPS];
int scanStepCount = 0;

bool scanRunning = false;
int scanCurrent = -1;          // step currently shown on screen, -1 = none
uint32_t scanLastChange = 0;

// Remembers the running step across an unexpected reset, so a pin that
// resets the chip can be identified and skipped.
struct ScanMarker {
  uint32_t magic;
  int32_t step;
};
static const uint32_t SCAN_MARKER_MAGIC = 0x5343414E;  // "SCAN"
static const uint32_t SCAN_MARKER_OFFSET = 32;          // RTC memory block
int scanInterruptedAt = -1;

void buildScanSteps() {
  scanStepCount = 0;
  for (int8_t cs : CS_CANDIDATES) {
    for (uint8_t mode : MODE_CANDIDATES) {
      for (int8_t rst : RST_CANDIDATES) {
        if (scanStepCount >= MAX_STEPS) continue;
        scanSteps[scanStepCount++] = {SCAN_DC, rst, cs, mode};
      }
    }
  }
}

void writeScanMarker(int step) {
  ScanMarker marker = {step >= 0 ? SCAN_MARKER_MAGIC : 0, step};
  ESP.rtcUserMemoryWrite(SCAN_MARKER_OFFSET, reinterpret_cast<uint32_t *>(&marker),
                         sizeof(marker));
}

void readScanMarker() {
  ScanMarker marker;
  ESP.rtcUserMemoryRead(SCAN_MARKER_OFFSET, reinterpret_cast<uint32_t *>(&marker),
                        sizeof(marker));
  if (marker.magic == SCAN_MARKER_MAGIC && marker.step >= 0 && marker.step < MAX_STEPS) {
    scanInterruptedAt = marker.step;
  }
  writeScanMarker(-1);  // clear it
}

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
// deleted each time, so a long scan can't fragment the ESP8266's small heap.
alignas(ClockDisplay) static uint8_t tftStorage[sizeof(ClockDisplay)];

void startDisplay(int8_t dc, int8_t rst, int8_t cs, uint8_t mode) {
  if (tft) {
    tft->~ClockDisplay();
    tft = nullptr;
  }
  // With a CS pin, the driver raises and lowers it around every transfer,
  // which is what the seller's firmware does on GPIO15. Without one, hold
  // GPIO15 low so a previous step can't leave the display deselected.
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
  tft->setTextSize(4);
  tft->setCursor(12, 70);
  tft->print("Hello Hal");

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

void drawScanStep(int step) {
  if (!tft) return;
  // Alternate the background so consecutive steps are easy to tell apart.
  static const uint16_t COLOURS[] = {ST77XX_BLUE, ST77XX_RED, ST77XX_GREEN, ST77XX_MAGENTA};
  tft->fillScreen(COLOURS[step % 4]);
  tft->setTextWrap(false);
  tft->setTextColor(ST77XX_WHITE);
  tft->setTextSize(10);
  String label = String(step);
  int16_t x = (240 - (int16_t)label.length() * 60) / 2;
  tft->setCursor(x < 0 ? 0 : x, 80);
  tft->print(label);
  tft->setTextSize(2);
  tft->setCursor(40, 200);
  tft->print("Pin scan step");
}

void showScanStep(int step) {
  const ScanStep &s = scanSteps[step];
  scanCurrent = step;
  writeScanMarker(step);  // if the next lines reset the chip, we'll know which step did it
  Serial.printf("Scan step %d: DC=%d RST=%d CS=%d mode=%d\n", step, s.dc, s.rst, s.cs, s.mode);
  startDisplay(s.dc, s.rst, s.cs, s.mode);
  drawScanStep(step);
  writeScanMarker(-1);
}

void stopScan() {
  scanRunning = false;
  writeScanMarker(-1);
}

void updateScan() {
  if (!scanRunning || millis() - scanLastChange < SCAN_STEP_MS) return;
  scanLastChange = millis();
  int next = scanCurrent + 1;
  if (next >= scanStepCount) {
    stopScan();
    return;
  }
  showScanStep(next);
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

String stepDescription(int step) {
  const ScanStep &s = scanSteps[step];
  return "DC " + String(s.dc) + ", reset " + String(s.rst) + ", CS " + String(s.cs) +
         ", SPI mode " + String(s.mode);
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
  if (scanRunning) html += F("<meta http-equiv='refresh' content='2'>");
  html += F("<title>SD Pro</title><style>"
            "body{font-family:sans-serif;max-width:560px;margin:24px auto;padding:0 16px;line-height:1.5}"
            "td{padding:2px 12px 2px 0}input{width:70px}button{margin:4px 4px 4px 0;padding:6px 12px}"
            ".note{padding:8px 12px;border:1px solid #c90;border-radius:6px}"
            "</style></head><body><h2>SD Pro custom firmware</h2>");

  // ----- Pin scan -----
  html += F("<h3>Display pin scan</h3>");
  if (scanInterruptedAt >= 0) {
    html += "<p class='note'>The last scan stopped when the clock restarted during step " +
            String(scanInterruptedAt) + " (" + stepDescription(scanInterruptedAt) +
            "). Those pins probably reset the chip. Resume to continue after it.</p>";
  }
  if (scanRunning) {
    html += "<p><b>Scanning: step " + String(scanCurrent) + " of " + String(scanStepCount - 1) +
            "</b><br>" + (scanCurrent >= 0 ? stepDescription(scanCurrent) : String("starting")) +
            "</p><p>Watch the clock. When a big number appears, note it and press Stop.</p>"
            "<form method='post' action='/scan/stop'><button>Stop</button></form>";
  } else {
    html += F("<p>Tries 8 variants of the seller's pins (chip-select, SPI mode, reset) "
              "with the startup table chosen below, about 4 seconds each. "
              "When one works, the clock shows a big step number.</p>"
              "<form method='post' action='/scan/start' style='display:inline'><button>Start scan</button></form>");
    if (scanInterruptedAt >= 0) {
      html += "<form method='post' action='/scan/start?from=" + String(scanInterruptedAt + 1) +
              "' style='display:inline'><button>Resume from step " + String(scanInterruptedAt + 1) +
              "</button></form>";
    }
    if (scanCurrent >= 0) {
      html += "<p>Last step shown: " + String(scanCurrent) + " (" + stepDescription(scanCurrent) + ")</p>";
    }
    html += F("<form method='post' action='/scan/show'>Step number: "
              "<input name='step' type='number' min='0' max='15'> "
              "<button>Show this step</button> "
              "<button formaction='/scan/save'>Save this step's pins and reboot</button></form>"
              "<p><a href='/scan/table'>List of all steps</a></p>");
  }

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
            "<form method='post' action='/test' style='display:inline'><button>Redraw test screen</button></form>"
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

// Returns the requested step number, or -1 (after sending an error) if invalid.
int stepArg() {
  String raw = server.arg("step");
  raw.trim();
  int step = raw.toInt();
  if (raw.length() == 0 || step < 0 || step >= scanStepCount) {
    server.send(400, "text/plain", "Enter a step number between 0 and " + String(scanStepCount - 1) + ".");
    return -1;
  }
  return step;
}

void handleScanStart() {
  if (!requireLogin()) return;
  int from = server.hasArg("from") ? server.arg("from").toInt() : 0;
  if (from < 0 || from >= scanStepCount) from = 0;
  scanInterruptedAt = -1;
  scanCurrent = from - 1;
  scanRunning = true;
  scanLastChange = millis() - SCAN_STEP_MS;  // show the first step straight away
  redirectHome();
}

void handleScanStop() {
  if (!requireLogin()) return;
  stopScan();
  redirectHome();
}

void handleScanShow() {
  if (!requireLogin()) return;
  int step = stepArg();
  if (step < 0) return;
  stopScan();
  showScanStep(step);
  redirectHome();
}

void handleScanSave() {
  if (!requireLogin()) return;
  int step = stepArg();
  if (step < 0) return;
  const ScanStep &s = scanSteps[step];
  cfg.dc = s.dc;
  cfg.rst = s.rst;
  cfg.cs = s.cs;
  cfg.spiMode = s.mode;
  saveConfig();
  server.send(200, "text/html",
              "<meta http-equiv='refresh' content='8;url=/'>Saved step " + String(step) + " (" +
                  stepDescription(step) + "). Rebooting... this page reloads in 8 seconds.");
  delay(500);
  ESP.restart();
}

void handleScanTable() {
  if (!requireLogin()) return;
  String html;
  html.reserve(5200);
  html += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Scan steps</title>"
            "<style>body{font-family:sans-serif;max-width:560px;margin:24px auto;padding:0 16px}"
            "td,th{padding:2px 14px 2px 0;text-align:left}</style></head><body>"
            "<p><a href='/'>Back</a></p><h3>Pin scan steps</h3><table>"
            "<tr><th>Step</th><th>DC</th><th>Reset</th><th>CS</th><th>SPI mode</th></tr>");
  for (int i = 0; i < scanStepCount; i++) {
    const ScanStep &s = scanSteps[i];
    html += "<tr><td>" + String(i) + "</td><td>" + String(s.dc) + "</td><td>" + String(s.rst) +
            "</td><td>" + String(s.cs) + "</td><td>" + String(s.mode) + "</td></tr>";
  }
  html += F("</table></body></html>");
  server.send(200, "text/html", html);
}

void handleTest() {
  if (!requireLogin()) return;
  stopScan();
  startDisplay(cfg.dc, cfg.rst, cfg.cs, cfg.spiMode);
  drawTestScreen();
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

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/scan/start", HTTP_POST, handleScanStart);
  server.on("/scan/stop", HTTP_POST, handleScanStop);
  server.on("/scan/show", HTTP_POST, handleScanShow);
  server.on("/scan/save", HTTP_POST, handleScanSave);
  server.on("/scan/table", HTTP_GET, handleScanTable);
  server.on("/test", HTTP_POST, handleTest);
  server.on("/backlight", HTTP_POST, handleBacklight);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });

  // The most important line in this file: without it the clock can't be updated again.
  updateServer.setup(&server, "/update", WEB_USER, OTA_PASSWORD);
  server.begin();
}

void startNetworkUpdates() {
  // Lets PlatformIO upload over WiFi: pio run -e sdpro_wifi -t upload
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    stopScan();
    if (!tft) return;
    tft->fillScreen(ST77XX_BLACK);
    tft->setTextColor(ST77XX_YELLOW);
    tft->setTextSize(3);
    tft->setCursor(20, 105);
    tft->print("Updating...");
  });
  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("SD Pro custom firmware v" FW_VERSION);

  readScanMarker();
  buildScanSteps();
  loadConfig();
  Serial.printf("Display pins: DC=%d RST=%d CS=%d BL=%d (active %s) SPI mode %d, %s table\n",
                cfg.dc, cfg.rst, cfg.cs, cfg.bl, cfg.blActiveLow ? "LOW" : "HIGH", cfg.spiMode,
                cfg.initTable ? "seller's" : "generic");

  // Network and update page come first, so a display problem can never
  // stop the clock from accepting the next firmware.
  startWifi();
  startWebServer();
  startNetworkUpdates();

  setBacklight(true);
  startDisplay(cfg.dc, cfg.rst, cfg.cs, cfg.spiMode);
  drawTestScreen();
  Serial.println("Ready.");
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  MDNS.update();
  updateScan();
}
