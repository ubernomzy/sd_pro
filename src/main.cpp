// SD Pro clock - first test firmware (v0.1.0)
//
// Goal of this version: prove we can run our own code on the clock
// without ever losing the ability to update it.
//
//   1. Join home WiFi (or open a recovery hotspot if that fails)
//   2. Keep a password-protected update page running at /update
//   3. Show a status page with editable display pin settings
//   4. Try to draw "Hello" on the 240x240 ST7789 screen
//
// If the screen stays blank, the pin guesses are wrong. Change them on
// the status page and the clock reboots with the new pins - no rebuild.

#include <Arduino.h>
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
  int8_t bl;             // backlight pin (-1 = not connected)
  uint8_t blActiveLow;   // 1 = backlight turns on when the pin is LOW
  uint8_t spiMode;       // ST7789 boards without a CS pin usually need mode 3
  uint8_t rotation;      // 0-3
};

static const uint32_t CONFIG_MAGIC = 0x53445031;  // "SDP1"
static const DisplayConfig DEFAULT_CONFIG = {CONFIG_MAGIC, 0, 2, 5, 1, 3, 0};

DisplayConfig cfg;
bool usingSavedConfig = false;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer updateServer;
Adafruit_ST7789 *tft = nullptr;
bool apMode = false;

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
                     cfg.dc >= 0 && isSafePin(cfg.rst) && isSafePin(cfg.bl) &&
                     cfg.spiMode <= 3 && cfg.rotation <= 3;
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

void initDisplay() {
  setBacklight(true);
  // CS = -1: these clocks usually tie the display's chip-select to ground.
  tft = new Adafruit_ST7789(-1, cfg.dc, cfg.rst);
  tft->init(240, 240, cfg.spiMode);
  tft->setRotation(cfg.rotation);
  tft->setSPISpeed(40000000);
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

void handleRoot() {
  if (!requireLogin()) return;

  String html;
  html.reserve(2600);
  html += F("<!doctype html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>SD Pro</title><style>"
            "body{font-family:sans-serif;max-width:560px;margin:24px auto;padding:0 16px;line-height:1.5}"
            "td{padding:2px 12px 2px 0}input{width:70px}button{margin:4px 4px 4px 0;padding:6px 12px}"
            "</style></head><body><h2>SD Pro custom firmware</h2><table>");
  html += "<tr><td>Version</td><td>" FW_VERSION "</td></tr>";
  html += "<tr><td>Mode</td><td>" + String(apMode ? "Recovery hotspot" : "Home WiFi") + "</td></tr>";
  html += "<tr><td>IP address</td><td>" + currentIp() + "</td></tr>";
  html += "<tr><td>WiFi signal</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>";
  html += "<tr><td>Uptime</td><td>" + String(millis() / 1000) + " s</td></tr>";
  html += "<tr><td>Free memory</td><td>" + String(ESP.getFreeHeap()) + " bytes</td></tr>";
  html += "<tr><td>Firmware size</td><td>" + String(ESP.getSketchSize()) + " bytes</td></tr>";
  html += "<tr><td>Space for next update</td><td>" + String(ESP.getFreeSketchSpace()) + " bytes</td></tr>";
  html += "<tr><td>Pin settings</td><td>" + String(usingSavedConfig ? "Saved" : "Defaults (not saved yet)") + "</td></tr>";
  html += F("</table><h3>Display pins</h3>"
            "<p>Screen blank or garbled? Try different pins. Saving reboots the clock. "
            "Use -1 for not connected. Data (GPIO13) and clock (GPIO14) are fixed.</p>"
            "<form method='post' action='/config'><table>");
  html += "<tr><td>DC pin</td><td>" + pinInput("dc", cfg.dc) + "</td></tr>";
  html += "<tr><td>Reset pin</td><td>" + pinInput("rst", cfg.rst) + "</td></tr>";
  html += "<tr><td>Backlight pin</td><td>" + pinInput("bl", cfg.bl) + "</td></tr>";
  html += "<tr><td>Backlight on when LOW</td><td><input name='bllow' type='number' min='0' max='1' value='" +
          String(cfg.blActiveLow) + "'> (1 = yes)</td></tr>";
  html += "<tr><td>SPI mode</td><td><input name='spi' type='number' min='0' max='3' value='" +
          String(cfg.spiMode) + "'> (0-3)</td></tr>";
  html += "<tr><td>Rotation</td><td><input name='rot' type='number' min='0' max='3' value='" +
          String(cfg.rotation) + "'> (0-3)</td></tr>";
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
  next.bl = server.arg("bl").toInt();
  next.blActiveLow = server.arg("bllow").toInt() ? 1 : 0;
  next.spiMode = constrain(server.arg("spi").toInt(), 0, 3);
  next.rotation = constrain(server.arg("rot").toInt(), 0, 3);

  if (next.dc < 0 || !isSafePin(next.dc) || !isSafePin(next.rst) || !isSafePin(next.bl)) {
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
  drawTestScreen();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleBacklight() {
  if (!requireLogin()) return;
  setBacklight(server.arg("on") == "1");
  server.sendHeader("Location", "/");
  server.send(303);
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

  loadConfig();
  Serial.printf("Display pins: DC=%d RST=%d BL=%d (active %s) SPI mode %d\n", cfg.dc,
                cfg.rst, cfg.bl, cfg.blActiveLow ? "LOW" : "HIGH", cfg.spiMode);

  // Network and update page come first, so a display problem can never
  // stop the clock from accepting the next firmware.
  startWifi();
  startWebServer();
  startNetworkUpdates();

  initDisplay();
  drawTestScreen();
  Serial.println("Ready.");
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  MDNS.update();
}
