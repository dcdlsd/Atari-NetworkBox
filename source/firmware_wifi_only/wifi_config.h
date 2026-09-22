#pragma once

// Optional development fallback. Leave both values empty for a distributable
// firmware: it will open NetworkBox-Setup until a user saves credentials.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define WIFI_AUTH CYW43_AUTH_WPA2_AES_PSK
