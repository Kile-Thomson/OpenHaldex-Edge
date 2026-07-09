#pragma once

#include <OpenHaldexC6_defs.h>

void setupOTA();

// True once a WiFi AP password (>= 8 chars) is set. The AP password is the
// single auth boundary; until it is set the AP runs open and the dashboard
// forces the first-run password page.
bool isDeviceProvisioned();

// Fail-closed analyzer-injection gate. True only when the AP is protected,
// evaluated once per analyzer TCP connection. host->device CAN transmit is
// dropped on an open AP; passive sniffing is unaffected.
bool analyzerInjectionPermitted();

bool isSystemSafeForOTA();
void confirmFirmwareValidity();
bool needsFirmwareConfirmation();
bool isOTAUpdateInProgress();
String getFirmwareVersion();