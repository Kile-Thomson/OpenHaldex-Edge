#pragma once

#include <OpenHaldexC6_defs.h>
#include <cstring>

// WiFi AP setup and reset helpers. Implemented in OpenHaldexC6_WiFi.cpp.
void setupWiFi();
void disconnectWifi();
void resetWifiPassword(); // clears WiFi password and restarts AP as open network
void resetWifiSsid();     // restores default SSID and restarts AP
void resetWifi();         // clears password AND restores default SSID, restarts AP