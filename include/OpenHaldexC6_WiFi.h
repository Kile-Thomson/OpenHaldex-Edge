#pragma once

#include <OpenHaldexC6_defs.h>
#include <cstring>

// WiFi AP setup and reset helpers. Implemented in OpenHaldexC6_WiFi.cpp.
void setupWiFi();
void disconnectWifi();

// Captive-DNS responder (resolves every query to 192.168.1.1) so phone OS
// connectivity-check probes reach the web server and get a bare 404, letting the
// phone conclude "no internet" and keep its own cellular data alive.
void dnsStart();
void dnsStop();
void dnsProcess(); // pump once per loop() iteration
void resetWifiPassword(); // clears WiFi password and restarts AP as open network
void resetWifiSsid();     // restores default SSID and restarts AP
void resetWifi();         // clears password AND restores default SSID, restarts AP