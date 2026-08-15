#include <OpenHaldexC6_WiFi.h>
#include <DNSServer.h>
#include <cstring>

// WiFi AP setup and reset helpers. Brings up the soft-AP (open or WPA2 per the
// stored password), advertises mDNS, and provides the password/SSID reset paths
// used by the API. The HTTP server itself is set up separately in
// OpenHaldexC6_API.cpp (setupWebServer).

// Captive-DNS responder. The soft-AP hands out its own IP (192.168.1.1) as the
// DNS server via DHCP, but arduino-esp32 runs no DNS process by default - so a
// phone's connectivity-check lookup (connectivitycheck.gstatic.com, etc.) hits
// 192.168.1.1:53 and nothing answers. The lookup then hangs, the phone sits in
// a "validating" state, and it pulls its default route onto our AP while it
// waits - killing the phone's cellular data (calls, messages, OTA).
//
// This DNSServer answers every query with 192.168.1.1, so the probe HTTP GET
// actually reaches the web server, where is_captive_probe() replies with a bare
// 404 (no 204, no redirect). The phone reads that as "reachable, but no
// internet, not a captive portal" and keeps cellular as its data route. The DNS
// answer is what makes the existing 404 handler reachable at all.
static DNSServer dnsServer;
static bool dnsRunning = false;

void dnsStart()
{
  if (dnsRunning)
  {
    return;
  }
  // "*" matches every hostname; TTL 0 so the phone never caches the answer.
  dnsServer.setTTL(0);
  dnsServer.start(53, "*", IPAddress(192, 168, 1, 1));
  dnsRunning = true;
}

void dnsStop()
{
  if (!dnsRunning)
  {
    return;
  }
  dnsServer.stop();
  dnsRunning = false;
}

void dnsProcess()
{
  if (dnsRunning)
  {
    dnsServer.processNextRequest();
  }
}

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

  dnsStart(); // answer phone connectivity-check DNS so it keeps cellular alive
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