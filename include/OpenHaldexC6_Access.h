#pragma once

// Who may talk to the web server, as pure decisions (no Arduino, no lwIP) so
// the native tests pin the security boundary.
//
// Model: the WiFi AP password is the one credential.
//  - A request that arrives on the device's own AP interface is trusted: the
//    caller already joined the WPA2 network.
//  - A request that arrives on the STA interface (the home network in bridge
//    mode) is not. It must carry HTTP Basic auth, user "admin", password = the
//    AP password, on every route.
//  - Until a password of WPA2 length is set the device is unprovisioned: the AP
//    answers only the setup page and the password endpoint, and the STA side
//    answers nothing, because there is no password to check against yet.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
  ACCESS_ALLOW = 0,          // serve the request normally
  ACCESS_SETUP_REDIRECT,     // unprovisioned AP client: send it to /setup
  ACCESS_DENY_UNPROVISIONED, // STA request while no password exists: refuse
  ACCESS_CHALLENGE           // STA request without valid credentials: 401 + WWW-Authenticate
} access_decision_t;

// true when a request did not come in on our own AP. A zero address on either
// side (AP down, address unknown) counts as "not the AP": fail closed.
static inline bool access_via_sta(uint32_t local_ip, uint32_t softap_ip) {
  if (local_ip == 0 || softap_ip == 0) return true;
  return local_ip != softap_ip;
}

// The only paths an unprovisioned AP client may reach (query string ignored).
static inline bool access_is_setup_path(const char *url) {
  if (url == nullptr) return false;
  size_t n = 0;
  while (url[n] != '\0' && url[n] != '?' && url[n] != '#') n++;
  if (n == 6 && strncmp(url, "/setup", 6) == 0) return true;
  if (n == 9 && strncmp(url, "/api/wifi", 9) == 0) return true;
  return false;
}

// path_open_when_unprovisioned: setup path or a phone connectivity probe.
// creds_ok: Basic auth matched admin / AP password (only meaningful via STA).
static inline access_decision_t access_decide(bool via_sta, bool provisioned,
                                              bool creds_ok, bool path_open_when_unprovisioned) {
  if (via_sta) {
    if (!provisioned) return ACCESS_DENY_UNPROVISIONED;
    return creds_ok ? ACCESS_ALLOW : ACCESS_CHALLENGE;
  }
  if (provisioned) return ACCESS_ALLOW;
  return path_open_when_unprovisioned ? ACCESS_ALLOW : ACCESS_SETUP_REDIRECT;
}

// Host-to-CAN injection from the analyzer (GVRET / SLCAN): allowed only on a
// protected AP and never for a client that came in through the home network.
// Passive sniffing is not gated by this.
static inline bool access_analyzer_injection(bool provisioned, bool via_sta) {
  return provisioned && !via_sta;
}

// A WiFi password is acceptable for the AP only if it is WPA2 length. Empty is
// not acceptable: an open AP is never a valid state to set.
static inline bool access_ap_password_valid(const char *pw) {
  if (pw == nullptr) return false;
  size_t n = strlen(pw);
  return n >= 8 && n <= 63;
}
