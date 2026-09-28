# SD Pro clock: custom firmware

Custom firmware for the SD Pro WiFi weather clock (ESP8266, 4MB flash,
1.54" 240x240 ST7789 LCD). Forked from JUZIPi-tech/SD_PRO, which only
publishes compiled firmware. The seller's files are kept in `original/`.

## Current version: 0.8.0 (spaceman)

Progress so far:

- v0.1.0 runs on the clock: joins WiFi, update page and status page work
- Backlight confirmed: GPIO5, on when LOW
- v0.2.0 scanned 72 DC/reset/SPI-mode combinations: display stayed blank
- Disassembling the seller's firmware showed the display setup code
  (at `0x4021a24b`) drives chip-select on GPIO15, which we never drove,
  and contains the ST7789 startup table (at file offset `0x6a308`)
- v0.3.0 used those exact pins: still blank
- The pin order in the seller's code matches the Adafruit display library,
  so the seller uses the same library; the difference is their startup
  table, which sets panel voltages and gate lines the generic one doesn't
- v0.4.0 sends the seller's exact table and fixes an SPI mode bug
  (on the ESP8266, `SPI_MODE3` is `0x11`, so "mode 3" had meant mode 1)
- **The display works.** The v0.4.0 scan drew on every SPI mode 3 step and
  no mode 0 step; chip-select and reset made no difference. SPI mode 3 was
  the real requirement, hidden by the mode bug since v0.1.0
- v0.4.1 makes SPI mode 3 the default
- The image was upside down: the panel is mounted rotated 180 degrees.
  v0.4.2 makes rotation 2 the default
- v0.5.0 adds the clock face: greeting and name, date, time from the
  internet (Sydney time, daylight saving handled), weather from
  OpenWeatherMap over HTTPS (icon, temperature, condition, wind speed and
  direction, comfort). Network uploads from PlatformIO and `sdpro.local`
  were removed to keep the firmware under 500KB
- v0.6.0: the comfort card and country name are gone; the bottom card
  rotates every 5 seconds between wind (speed + compass), pressure
  (hPa + dial), UV index (level + colour scale) and feels-like
  temperature (vs actual). Weather now comes from
  [Open-Meteo](https://open-meteo.com/): free for non-commercial use, no
  API key, data licensed CC BY 4.0 (credited on the status page).
  OpenWeatherMap's free feed has no UV index
- v0.7.0: read-only file browser at `/files`, to download the seller's
  GIFs and photos that are still in the clock's storage (the spaceman
  animation is one of them). The file system is mounted with auto-format
  off, and nothing writes to it. To make room: the pin scan is gone (its
  job is done) and weather uses plain HTTP instead of HTTPS (public data;
  the HTTPS code never checked certificates and cost 82KB). Firmware is
  now about 431KB
- v0.8.0: the seller's spaceman animation (`/0.gif` on the clock, saved
  as `assets/spaceman.gif`) plays in the gap right of the bottom card,
  at the GIF's own speed (20 frames, 20ms each). `tools/make_spaceman.py`
  converts it on the PC into 16 greys, run-length packed
  (`src/spaceman_data.h`, 26KB), so the clock needs no GIF decoder and
  almost no memory. Firmware is about 458KB

## Personalise it

The top of `src/main.cpp` has a settings block:

```cpp
static const char *FACE_GREETING = "Hello";
static const char *FACE_NAME = "Neehal";
static const char *FACE_CITY = "Sydney";
static const char *WEATHER_LATITUDE = "-33.8688";
static const char *WEATHER_LONGITUDE = "151.2093";
static const uint32_t CARD_PAGE_MS = 5000;
static const char *TIMEZONE = "AEST-10AEDT,M10.1.0,M4.1.0/3";
```

The layout itself is in `src/face.cpp` (positions, colours, fonts, icons).

To swap the animation: replace `assets/spaceman.gif` with another GIF of
at most 80x80 pixels (square), run `python3 tools/make_spaceman.py`, and
rebuild. Colour is dropped (16 greys), and keep an eye on the size check.

## Features

- Clock face: name, date, time, weather, a card rotating between wind,
  pressure, UV index and feels-like temperature, and the animated spaceman
- Joins your 2.4GHz home WiFi. If it can't within 20 seconds, it opens a
  recovery hotspot `SDPro-Recovery` at http://192.168.4.1
- Password-protected firmware update page at `/update` (user `admin`)
- Status page at `/` showing time sync, weather status, IP, memory,
  firmware size and display pins, with buttons to switch between the
  clock face and a test screen and to refresh the weather
- Display pins can be changed on the status page without rebuilding
- Files page at `/files`: lists everything in the clock's 3MB storage;
  click a file to open or download it. Read-only

Display pins, from the seller's firmware:

| Signal    | GPIO | Evidence                                   |
|-----------|------|--------------------------------------------|
| MOSI      | 13   | Hardware SPI                               |
| SCLK      | 14   | Hardware SPI                               |
| CS        | 15   | Seller code: `pinMode(15, OUTPUT)`, HIGH   |
| DC        | 0    | Seller code: `pinMode(0, OUTPUT)`, HIGH    |
| Reset     | 2    | Seller code: pulses GPIO2 HIGH-LOW-HIGH    |
| Backlight | 5    | Confirmed on the clock (on when LOW)       |
| SPI mode  | 3    | Confirmed on the clock (mode 0 stays blank)|
| Rotation  | 2    | Panel is mounted upside down               |

Checked on the clock with v0.4.x: image upright, colour order correct
(red, green, blue), no offset. The case covers the bottom pixel or two,
so keep content a few pixels clear of the bottom edge.

Display chip: ST7789, 240x240. The seller's startup table (file offset
`0x6a308`, identical in v1.0.4 and v1.0.6) is copied into `SELLER_INIT`
in `src/main.cpp`; the status page can switch back to the generic table.

## Build

1. Copy `include/secrets.example.h` to `include/secrets.h` and fill in
   your WiFi name, WiFi password, update password and recovery password.
   `secrets.h` is in `.gitignore` and must never be committed. (Weather
   needs no key.)
2. Build: `pio run -e sdpro`
3. The firmware file is `.pio/build/sdpro/firmware.bin` (about 458KB).
   The build fails on purpose if it goes over 500KB (`check_size.py`).

## Flash

- First time (from the seller's firmware): the seller's update page only
  accepts file names starting with `SDP`, so copy it first:
  `cp .pio/build/sdpro/firmware.bin SDPro_custom.bin`
  then upload that file from the Firmware Update section of the settings page.
- After that (from this firmware): open `http://<clock-ip>/update`,
  log in as `admin`, choose `firmware.bin`, upload.

## Rules

1. Every version must keep the `/update` page (`updateServer.setup(...)`).
   Without it the clock can never be updated again.
2. Keep firmware under 500KB. An update is written beside the running
   firmware inside the ~1MB firmware area (`eagle.flash.4m3m.ld`), so each
   version can use about half. `check_size.py` enforces this at build time.
3. Only merge to `main` after a version has been tested on the clock.
