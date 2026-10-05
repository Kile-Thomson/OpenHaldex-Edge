#pragma once

#include <OpenHaldexC6_defs.h>

// Web access boundary (decisions live in OpenHaldexC6_Access.h, host-tested).
// setupWebAccess() must run before any other route is added: its handler has
// to be first in the server's list so it sees every request.
void setupWebAccess();

// True once a WiFi AP password of WPA2 length is set. Until then the AP is in
// first-run setup mode (setup page only) and the home-network side answers nothing.
bool isDeviceProvisioned();

// Analyzer (GVRET / SLCAN) host-to-CAN injection gate, evaluated once per
// connection: allowed only on a protected AP and not for a home-network client.
// network = false for the wired serial session; for a TCP client pass true and
// the local address it connected to (an unknown address counts as home network).
bool analyzerInjectionPermitted(bool network, uint32_t clientLocalIp);
