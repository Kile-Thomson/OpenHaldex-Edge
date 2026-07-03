#pragma once

#include <OpenHaldexC6_defs.h>

void setupOTA();

// Fail-closed auth gate for the flash/control endpoints. Sends 503 when no
// password is set, 401 challenge otherwise - caller must return on false.
bool requireOtaAuth(AsyncWebServerRequest *request);
bool isOtaCredentialProvisioned(); // true once a password is set (NVS or build-time)

bool isSystemSafeForOTA();
void confirmFirmwareValidity();
bool needsFirmwareConfirmation();
bool isOTAUpdateInProgress();
String getFirmwareVersion();