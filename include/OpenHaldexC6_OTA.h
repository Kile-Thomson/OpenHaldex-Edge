#pragma once

#include <OpenHaldexC6_defs.h>

void setupOTA();

// Fail-closed auth gate for the flash/control endpoints. Sends 503 when no
// password is set, 401 challenge otherwise - caller must return on false.
bool requireOtaAuth(AsyncWebServerRequest *request);
bool isOtaCredentialProvisioned(); // true once a password is set (NVS or build-time)

// Fail-closed analyzer-injection gate. True only when an OTA credential is
// provisioned, evaluated once per analyzer TCP connection. Resolves the
// credential in task-local storage so it never touches requireOtaAuth's shared
// static; only the boolean crosses into the analyzer task.
bool analyzerInjectionPermitted();

bool isSystemSafeForOTA();
void confirmFirmwareValidity();
bool needsFirmwareConfirmation();
bool isOTAUpdateInProgress();
String getFirmwareVersion();