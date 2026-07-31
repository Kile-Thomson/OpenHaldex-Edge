#include <OpenHaldexC6_WiFi.h>
#include <cstring>

// WiFi AP setup and reset helpers. Brings up the soft-AP (open or WPA2 per the
// stored password), advertises mDNS, and provides the password/SSID reset paths
// used by the API. The HTTP server itself is set up separately in
// OpenHaldexC6_API.cpp (setupWebServer).

static void softAPStart()
{
  WiFi.softAPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
  if (strlen(wifiPassword) >= 8)
  {
    WiFi.softAP(wifiHostName, wifiPassword); // password-protected AP
    DEBUG("WiFi AP started with password: %s", wifiHostName);
  }
  else
  {
    WiFi.softAP(wifiHostName); // open network
    DEBUG("WiFi AP started (open): %s", wifiHostName);
  }
}

void setupWiFi()
{
  // Called once from main.cpp at boot. Configures the soft-AP and mDNS here.
  WiFi.hostname(wifiHostName);
  DEBUG("Creating Access Point...");
  WiFi.mode(WIFI_AP);
  softAPStart();
  WiFi.setSleep(false);
  // Aggressive sleep: trim AP TX power to reduce active-WiFi current.
  if (canSleepAggressive)
  {
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
  }
  DEBUG("IP address: 192.168.1.1");

  MDNS.begin("openhaldex");           // openhaldex.local
  MDNS.addService("http", "tcp", 80); // advertise HTTP
}

void disconnectWifi()
{
  rebootWiFi = true;
}

void resetWifiPassword()
{
  memset(wifiPassword, 0, sizeof(wifiPassword)); // clear password -> open network
  rebootWiFi = true;                             // trigger AP restart
  DEBUG("WiFi password cleared - restarting as open AP");
}

void resetWifi()
{
  memset(wifiPassword, 0, sizeof(wifiPassword));                 // clear password -> open network
  memset(wifiSsid, 0, sizeof(wifiSsid));                         // clear SSID
  strncpy(wifiSsid, wifiHostNameDefault, sizeof(wifiSsid) - 1); // restore factory default SSID
  rebootWiFi = true;                                             // trigger AP restart
  DEBUG("WiFi reset to defaults - SSID: %s, open network", wifiSsid);
}

void resetWifiSsid()
{
  memset(wifiSsid, 0, sizeof(wifiSsid));                       // clear
  strncpy(wifiSsid, wifiHostNameDefault, sizeof(wifiSsid) - 1); // restore factory default
  rebootWiFi = true;                                            // trigger AP restart
  DEBUG("WiFi SSID reset to default - restarting AP: %s", wifiSsid);
}