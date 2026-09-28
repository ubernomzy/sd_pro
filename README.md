# SD Pro clock: custom firmware

Custom firmware for the SD Pro WiFi weather clock (ESP8266, 4MB flash,
1.54" 240x240 ST7789 LCD). Forked from JUZIPi-tech/SD_PRO, which only
publishes compiled firmware. The seller's files are kept in `original/`.

## Current version: 0.5.0 (clock face)

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

## Personalise it

The top of `src/main.cpp` has a settings block:

```cpp
static const char *FACE_GREETING = "Hello";
static const char *FACE_NAME = "Neehal";
static const char *FACE_CITY = "Sydney";
static const char *WEATHER_QUERY = "Sydney,AU";
static const char *TIMEZONE = "AEST-10AEDT,M10.1.0,M4.1.0/3";
```

The layout itself is in `src/face.cpp` (positions, colours, fonts, icons).

## Features

- Clock face: name, date, time, weather, wind, comfort
- Joins your 2.4GHz home WiFi. If it can't within 20 seconds, it opens a
  recovery hotspot `SDPro-Recovery` at http://192.168.4.1
- Password-protected firmware update page at `/update` (user `admin`)
- Status page at `/` showing time sync, weather status, IP, memory,
  firmware size and display pins, with buttons to switch between the
  clock face and a test screen and to refresh the weather
- Display pins can be changed on the status page without rebuilding
- Pin scan: tries 8 variants of the seller's pins (chip-select on/off,
  SPI mode 0/3, reset on/off), showing a big step number on screen.
  `/scan/table` lists the pins for every step

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
   your WiFi name, WiFi password, update password, recovery password and
   (optional) OpenWeatherMap key. `secrets.h` is in `.gitignore` and must
   never be committed.
2. Build: `pio run -e sdpro`
3. The firmware file is `.pio/build/sdpro/firmware.bin` (about 490KB).
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
