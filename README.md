# SD Pro clock: custom firmware

Custom firmware for the SD Pro WiFi weather clock (ESP8266, 4MB flash,
1.54" 240x240 ST7789 LCD). Forked from JUZIPi-tech/SD_PRO, which only
publishes compiled firmware. The seller's files are kept in `original/`.

## Current version: 0.2.0 (display pin scan)

Progress so far:

- v0.1.0 runs on the clock: joins WiFi, update page and status page work
- Backlight confirmed: GPIO5, on when LOW
- Display still blank with DC 0 / reset 2 (SPI modes 3 and 0) and with no reset pin
- v0.2.0 adds a pin scan on the status page to find the right DC/reset pins

## Features

- Joins your 2.4GHz home WiFi. If it can't within 20 seconds, it opens a
  recovery hotspot `SDPro-Recovery` at http://192.168.4.1
- Password-protected firmware update page at `/update` (user `admin`)
- Status page at `/` showing IP, memory, firmware size and display pins
- Draws "Hello Hal", colour bars and the IP address on the screen
- Display pins can be changed on the status page without rebuilding
- Pin scan (v0.2.0): cycles through 72 DC/reset/SPI-mode combinations,
  3 seconds each, showing a big step number on screen. `/scan/table`
  lists the pins for every step

Display pins are guesses until confirmed on the real clock:

| Signal    | GPIO | Status                   |
|-----------|------|--------------------------|
| MOSI      | 13   | Fixed hardware SPI       |
| SCLK      | 14   | Fixed hardware SPI       |
| DC        | 0    | Guess                    |
| Reset     | 2    | Guess                    |
| Backlight | 5    | Confirmed (on when LOW)  |
| CS        | none | Guess (tied to ground)   |

## Build

1. Copy `include/secrets.example.h` to `include/secrets.h` and fill in
   your WiFi name, WiFi password, update password and recovery password.
   `secrets.h` is in `.gitignore` and must never be committed.
2. Build: `pio run -e sdpro`
3. The firmware file is `.pio/build/sdpro/firmware.bin` (about 360KB).

## Flash

- First time (from the seller's firmware): the seller's update page only
  accepts file names starting with `SDP`, so copy it first:
  `cp .pio/build/sdpro/firmware.bin SDPro_custom.bin`
  then upload that file from the Firmware Update section of the settings page.
- After that (from this firmware): open `http://<clock-ip>/update`,
  log in as `admin`, choose `firmware.bin`, upload. Or from the terminal:
  `SDPRO_OTA_PASSWORD='...' pio run -e sdpro_wifi -t upload`

## Rules

1. Every version must keep the `/update` page (`updateServer.setup(...)`).
   Without it the clock can never be updated again.
2. Keep firmware under about 500KB. The flash layout (`eagle.flash.4m3m.ld`)
   leaves roughly half of the 1MB firmware area for each update.
3. Only merge to `main` after a version has been tested on the clock.
