// Copy this file to include/secrets.h and fill in your own values.
// secrets.h is listed in .gitignore, so it never gets pushed to GitHub.

#pragma once

// Your home WiFi. Must be the 2.4GHz network (the ESP8266 has no 5GHz).
#define WIFI_SSID "your-2.4GHz-wifi-name"
#define WIFI_PASS "your-wifi-password"

// Password for the update page and the settings page.
// Username is always: admin
#define OTA_PASSWORD "choose-a-password"

// Recovery hotspot. If the clock can't join your WiFi within 20 seconds,
// it creates a WiFi network called "SDPro-Recovery" with this password
// (at least 8 characters). Join it and open http://192.168.4.1
#define AP_PASSWORD "choose-8-plus-chars"
