#include <OpenHaldexC6_OTA.h>
#include <OpenHaldexC6_Calculations.h> // select_ota_password / ota_credential_configured / analyzer_injection_allowed

//static AsyncWebServer *otaServer = nullptr;

// ============================================================================
// SAFETY-CRITICAL: Configuration
// ============================================================================

// Optional build-time OTA password (-D OPENHALDEX_OTA_PASSWORD="..."). Defaults
// to empty so no credential ever ships in the source - the device fails closed
// until one is set via /setup or at build time.
#ifndef OPENHALDEX_OTA_PASSWORD
#define OPENHALDEX_OTA_PASSWORD ""
#endif

// OTA partition labels (must match partition table)
#define OTA_PARTITION_LABEL_0 "ota_0"
#define OTA_PARTITION_LABEL_1 "ota_1"

// Safety check timeout (ms) - how long to wait for safety conditions
#define OTA_SAFETY_CHECK_TIMEOUT_MS 5000

// ============================================================================
// SAFETY-CRITICAL: State Variables
// ============================================================================

static const char *TAG = "OTA";
static AsyncWebServer *otaServer = nullptr;
static bool otaUpdateInProgress = false;
static esp_ota_handle_t otaHandle = 0;
static const esp_partition_t *otaPartition = nullptr;

// Firmware confirmation flag - set to true only after all safety checks pass
static bool firmwareConfirmed = false;

// Max stored password length. The setup/rotation endpoints reject anything
// longer, so the static buffer below never truncates a stored credential.
#define OTA_PASSWORD_MAX_LEN 63

// ============================================================================
// SAFETY-CRITICAL: Resolve the effective OTA credential
// ============================================================================
// An NVS-provisioned password (otaCred/otaPass) wins over the build-time
// default; empty means unset. Uses its own Preferences handle - the shared
// global `pref` is held open on another namespace by the writeEEP task.
static const char *resolveOtaCredential() {
  static char effective[OTA_PASSWORD_MAX_LEN + 1];

  Preferences otaPref;
  String nvsValue = "";
  if (otaPref.begin("otaCred", true)) {
    nvsValue = otaPref.getString("otaPass", "");
    otaPref.end();
  }

  // Copy out while nvsValue is still alive. select_ota_password applies the
  // NVS-over-build-default-over-empty precedence in one host-tested place.
  const char *eff = select_ota_password(nvsValue.c_str(), OPENHALDEX_OTA_PASSWORD);
  strlcpy(effective, eff, sizeof(effective));
  return effective;
}

// ============================================================================
// SAFETY-CRITICAL: Fail-closed auth gate for every flash/control path
// ============================================================================
// Returns true only when the request is authorized. On refusal a response has
// already been sent (503 unprovisioned, 401 challenge) - the caller must return.
bool requireOtaAuth(AsyncWebServerRequest *request) {
  const char *effective = resolveOtaCredential();

  if (!ota_credential_configured(effective)) {
    // No credential set: refuse rather than accept a default password
    request->send(503, "text/plain", "BLOCKED: No password set - visit /setup first");
    return false;
  }

  if (!request->authenticate("admin", effective)) {
    request->requestAuthentication();
    return false;
  }

  return true;
}

// True when a password is set (NVS or build-time). Gates the first-run /setup
// page, which closes permanently once a credential exists.
bool isOtaCredentialProvisioned() {
  return ota_credential_configured(resolveOtaCredential());
}

// ============================================================================
// SAFETY-CRITICAL: Fail-closed analyzer-injection gate
// ============================================================================
// Returns true only when an OTA credential is provisioned. Composes the
// host-characterized analyzer_injection_allowed() predicate over the resolved
// credential so the fail-closed policy stays a single reviewable source of
// truth. The credential value never leaves this TU - only the boolean result is
// exposed to the analyzer task.
bool analyzerInjectionPermitted() {
  // Resolve the credential into a local buffer instead of calling
  // resolveOtaCredential(), which writes into a shared static that requireOtaAuth()
  // also writes concurrently from the web task. Same logic, task-local storage.
  char local[OTA_PASSWORD_MAX_LEN + 1];
  Preferences otaPref;
  String nvsValue = "";
  if (otaPref.begin("otaCred", /*readOnly=*/true)) {
    nvsValue = otaPref.getString("otaPass", "");
    otaPref.end();
  }
  const char *eff = select_ota_password(nvsValue.c_str(), OPENHALDEX_OTA_PASSWORD);
  strlcpy(local, eff, sizeof(local));
  return analyzer_injection_allowed(local);
}

// ============================================================================
// SAFETY-CRITICAL: Check if system is in safe state for OTA update
// ============================================================================
// Behavior:
// - If CAN is NOT detected (bench setting): OTA allowed immediately.
// - If CAN IS detected (vehicle): enforce safety AND auto-revert to STOCK.
//
// Vehicle safety conditions:
// 1. Vehicle speed == 0
// 2. CAN buses operational (no bus failure)
// 3. Outputs safe: controller disabled OR mode switched to STOCK automatically
// 4. No active Haldex temp protection
// ============================================================================
bool isSystemSafeForOTA() {
  // BENCH MODE: No CAN detected -> allow OTA
  bool canDetected = (hasCANChassis || hasCANHaldex);
  if (!canDetected) {
#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA SAFETY] CAN not detected - assuming BENCH mode: OTA allowed");
#endif
    return true;
  }

  // VEHICLE MODE: CAN detected -> enforce safety

  // SAFETY CHECK 1: Vehicle MUST be stationary
  if (received_vehicle_speed > 0) {
#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA SAFETY] Vehicle moving: %d kmh - OTA BLOCKED", received_vehicle_speed);
#endif
    return false;
  }

  // SAFETY CHECK 2: CAN bus health
  if (isBusFailure) {
#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA SAFETY] CAN bus failure detected - OTA BLOCKED");
#endif
    return false;
  }

  // SAFETY CHECK 3: Outputs safe
  // If controller is disabled, we're safe. Otherwise, force STOCK.
  if (!disableController) {
    xSemaphoreTake(stateMutex, portMAX_DELAY); // check-then-force must be one atomic step
    if (state.mode != MODE_STOCK) {
#if enableDebug || detailedDebugWiFi
      DEBUG("[OTA SAFETY] Controller active in non-stock mode - auto-switching to STOCK for OTA safety");
#endif
      state.mode = MODE_STOCK;
    }
    xSemaphoreGive(stateMutex);
  }

  // SAFETY CHECK 4: No active Haldex faults (temp protection)
  if (received_temp_protection) {
#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA SAFETY] Haldex temperature protection active - OTA BLOCKED");
#endif
    return false;
  }

  // All safety checks passed
#if enableDebug || detailedDebugWiFi
  DEBUG("[OTA SAFETY] System safe for OTA update");
#endif
  return true;
}

// ============================================================================
// SAFETY-CRITICAL: Confirm firmware validity after successful boot
// ============================================================================
// This function MUST be called in setup() AFTER:
// 1. CAN buses are initialized
// 2. Outputs are set to safe state
// 3. No faults are detected
//
// If this is not called, ESP-IDF will automatically rollback on next boot
// ============================================================================
void confirmFirmwareValidity() {
  // SAFETY CHECK: Only confirm if system is in safe state
  if (!isSystemSafeForOTA()) {
#if enableDebug
    DEBUG("[OTA SAFETY] System not safe - firmware confirmation BLOCKED");
    DEBUG("[OTA SAFETY] Rollback will occur on next boot");
#endif
    return;
  }

  // SAFETY CHECK: Verify CAN buses are initialized
  // Check if CAN buses have received messages (indicates initialization)
  // In standalone mode, chassis CAN may not be present, so we check accordingly
  if (!isStandalone && !hasCANChassis) {
#if enableDebug
    DEBUG("[OTA SAFETY] CAN buses not initialized - firmware confirmation BLOCKED");
#endif
    return;
  }

  // SAFETY CHECK: Verify no active faults
  if (isBusFailure) {
#if enableDebug
    DEBUG("[OTA SAFETY] CAN bus failure detected - firmware confirmation BLOCKED");
#endif
    return;
  }

  // All safety checks passed - mark firmware as valid
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    firmwareConfirmed = true;
#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA SAFETY] Firmware confirmed as valid");
#endif
  } else {
#if enableDebug
    DEBUG("[OTA SAFETY] Failed to confirm firmware: %s", esp_err_to_name(err));
#endif
  }
}

// ============================================================================
// SAFETY-CRITICAL: Check if firmware needs confirmation on boot
// ============================================================================
// Call this early in setup() to check if we booted from a new OTA partition
// ============================================================================
bool needsFirmwareConfirmation() {
  esp_ota_img_states_t ota_state;
  esp_err_t err = esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state);

  if (err != ESP_OK) {
    return false;
  }

  // If state is ESP_OTA_IMG_PENDING_VERIFY, firmware needs confirmation
  return (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
}

// ============================================================================
// OTA Update Handler - SAFETY-CRITICAL: Blocks unsafe updates
// ============================================================================
//        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)

void handleOTAUpdate(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  // SAFETY CHECK: auth and safety must be enforced HERE, not in the onRequest
  // callback - ESPAsyncWebServer delivers the upload body BEFORE onRequest runs,
  // so gating there would let an unauthenticated upload reach esp_ota_write().
  // Chunk 0 runs the full gate and sets _tempObject as an "authorized" marker;
  // later chunks no-op while it is null. malloc'd because the request destructor
  // releases _tempObject with free() if the upload aborts.
  if (index == 0) {
    // SAFETY CHECK: fail-closed auth before anything else.
    if (!requireOtaAuth(request)) return;

    // SAFETY CHECK: block update if system is not in a safe state.
    if (!isSystemSafeForOTA()) {
      request->send(403, "text/plain", "OTA BLOCKED: System not in safe state. Vehicle must be stationary, CAN initialized, outputs safe, no faults.");
#if enableDebug || detailedDebugWiFi
      DEBUG("[OTA SAFETY] Update rejected - system not safe");
#endif
      return;
    }

    otaUpdateInProgress = true;

    // Get next OTA partition
    otaPartition = esp_ota_get_next_update_partition(NULL);
    if (otaPartition == NULL) {
      request->send(500, "text/plain", "OTA ERROR: No OTA partition found. Check partition table.");
      otaUpdateInProgress = false;
      return;
    }

#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA] Starting update to partition: %s", otaPartition->label);
#endif

    // Begin OTA update
    esp_err_t beginErr = esp_ota_begin(otaPartition, OTA_SIZE_UNKNOWN, &otaHandle);
    if (beginErr != ESP_OK) {
      request->send(500, "text/plain", "OTA ERROR: Failed to begin update");
      otaUpdateInProgress = false;
      return;
    }

    // Index 0 fully passed: authorize the remaining chunks to write. If the
    // allocation fails, abort the already-begun OTA cleanly rather than leave
    // it half-open with every later chunk stranded.
    request->_tempObject = malloc(1);
    if (request->_tempObject == nullptr) {
      esp_ota_abort(otaHandle);
      otaUpdateInProgress = false;
      request->send(500, "text/plain", "OTA ERROR: Out of memory");
      return;
    }
  } else if (request->_tempObject == nullptr) {
    // First chunk was rejected (auth / safety / begin) -> ignore every later chunk.
    return;
  }

  // Write data chunk
  esp_err_t err = esp_ota_write(otaHandle, data, len);
  if (err != ESP_OK) {
    request->send(500, "text/plain", "OTA ERROR: Write failed");
    esp_ota_abort(otaHandle);
    otaUpdateInProgress = false;
    return;
  }

  // Final chunk - finish OTA
  if (final) {
    err = esp_ota_end(otaHandle);
    if (err != ESP_OK) {
      if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        request->send(400, "text/plain", "OTA ERROR: Image validation failed");
      } else {
        request->send(500, "text/plain", "OTA ERROR: End failed");
      }
      esp_ota_abort(otaHandle);
      otaUpdateInProgress = false;
      return;
    }

    // Set boot partition to new firmware
    err = esp_ota_set_boot_partition(otaPartition);
    if (err != ESP_OK) {
      request->send(500, "text/plain", "OTA ERROR: Failed to set boot partition");
      otaUpdateInProgress = false;
      return;
    }

#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA] Update complete. Rebooting...");
    DEBUG("[OTA SAFETY] New firmware will require confirmation on boot");
#endif

    request->send(200, "text/plain", "OTA update complete. Rebooting... Firmware will be confirmed after safety checks pass.");

    // Small delay to allow response to be sent
    delay(1000);

    for (int i = 0; i <= 8; i++) {
      strip.setLedColorData(led_channel, ledBrightness/2, ledBrightness/2, ledBrightness/2);  // red
      strip.show();
      delay(50);
      strip.setLedColorData(led_channel, 0, 0, 0);  // red
      strip.show();
      delay(50);
    }

    // Reboot
    ESP.restart();
  } else {
    // Progress update
    request->send(200, "text/plain", "OK");
  }
}

// ============================================================================
// Store a new OTA password from a JSON body {"password":"..."}
// ============================================================================
// Shared by POST /setup (first run, unauthenticated, refused once a password
// exists) and POST /ota/credential (rotation, auth-gated). Gates run on chunk 0,
// before any body is kept - the body handler fires before onRequest, so this is
// the only place they work. The body is buffered in a malloc block via
// _tempObject so an aborted POST is released cleanly by the request destructor's
// free(). The password is never logged or echoed back.
static void storeOtaPassword(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total, bool firstRun) {
  if (index == 0) {
    if (firstRun) {
      if (isOtaCredentialProvisioned()) {
        request->send(403, "application/json", "{\"error\":\"Password already set - use /ota/credential to change it\"}");
        return;
      }
    } else {
      if (!requireOtaAuth(request)) return;
    }
    // A password body is tiny - refuse anything oversized before buffering it
    if (total > OTA_PASSWORD_MAX_LEN + 64) {
      request->send(413, "application/json", "{\"error\":\"Request body too large\"}");
      return;
    }
    char *buf = (char *)malloc(total + 1);
    if (buf == nullptr) {
      request->send(500, "application/json", "{\"error\":\"Out of memory\"}");
      return;
    }
    memset(buf, 0, total + 1);
    request->_tempObject = buf;
  } else if (request->_tempObject == nullptr) {
    // Chunk 0 was rejected - ignore the rest of the body
    return;
  }

  char *buf = (char *)request->_tempObject;
  memcpy(buf + index, data, len);
  if (index + len != total) return; // wait for the full body

  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, buf);
  free(buf);
  request->_tempObject = nullptr;

  if (jsonErr != DeserializationError::Ok || !doc["password"].is<const char *>()) {
    request->send(422, "application/json", "{\"error\":\"Expected JSON with a password field\"}");
    return;
  }

  String newPass = doc["password"].as<String>();
  if (newPass.length() < 8) {
    request->send(422, "application/json", "{\"error\":\"Password must be at least 8 characters\"}");
    return;
  }
  if (newPass.length() > OTA_PASSWORD_MAX_LEN) {
    request->send(422, "application/json", "{\"error\":\"Password must be at most 63 characters\"}");
    return;
  }

  // Re-check at write time so two racing first-run POSTs can't both store
  if (firstRun && isOtaCredentialProvisioned()) {
    request->send(403, "application/json", "{\"error\":\"Password already set - use /ota/credential to change it\"}");
    return;
  }

  // Dedicated handle again - never the shared global `pref`
  Preferences otaPref;
  if (!otaPref.begin("otaCred", false)) {
    request->send(500, "application/json", "{\"error\":\"Could not open credential store\"}");
    return;
  }
  size_t written = otaPref.putString("otaPass", newPass);
  otaPref.end();

  if (written == 0) {
    request->send(500, "application/json", "{\"error\":\"Could not save password\"}");
    return;
  }

  request->send(200, "application/json", "{\"ok\":true}");
}

// ============================================================================
// Setup OTA Server
// ============================================================================
void setupOTA() {
#if detailedDebugWiFi
  DEBUG("[OTA] Setting up OTA update server...");
#endif

  // Check if firmware needs confirmation
  if (needsFirmwareConfirmation()) {
#if enableDebug
    DEBUG("[OTA SAFETY] New firmware detected - will confirm after safety checks");
#endif
    // Don't confirm yet - wait for confirmFirmwareValidity() to be called
  }

  // First-run password setup page. Unauthenticated by design: it exists to
  // bootstrap the credential on a device that has none, and closes permanently
  // (403/redirect) once one is stored. Rotation uses POST /ota/credential.
  static const char SETUP_PAGE[] =
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>OpenHaldex-C6 - Setup</title>"
    "<style>"
    "body{font-family:sans-serif;background:#111;color:#eee;display:flex;"
    "align-items:center;justify-content:center;min-height:100vh;margin:0}"
    ".card{background:#222;border-radius:8px;padding:2rem;max-width:400px;width:100%}"
    "h1{margin:0 0 .4rem;font-size:1.4rem}"
    "p{color:#aaa;font-size:.9rem;margin:0 0 1.5rem}"
    "label{display:block;margin-bottom:.3rem;font-size:.85rem}"
    "input[type=password]{width:100%;box-sizing:border-box;padding:.6rem;"
    "border-radius:4px;border:1px solid #444;background:#333;color:#eee;"
    "font-size:1rem;margin-bottom:1rem}"
    "button{width:100%;padding:.7rem;background:#0a84ff;border:none;"
    "border-radius:4px;color:#fff;font-size:1rem;cursor:pointer}"
    "button:disabled{opacity:.5;cursor:default}"
    ".msg{font-size:.85rem;margin-bottom:1rem;display:none}"
    ".err{color:#ff6b6b}.ok{color:#4caf50}"
    "</style></head><body>"
    "<div class=\"card\">"
    "<h1>OpenHaldex-C6</h1>"
    "<p>Set a password to protect firmware updates and settings. Minimum 8 characters.</p>"
    "<div class=\"msg err\" id=\"err\"></div>"
    "<div class=\"msg ok\" id=\"ok\"></div>"
    "<label>Password</label>"
    "<input type=\"password\" id=\"p\" autocomplete=\"new-password\" placeholder=\"Min. 8 characters\">"
    "<label>Confirm password</label>"
    "<input type=\"password\" id=\"p2\" autocomplete=\"new-password\" placeholder=\"Repeat password\">"
    "<button id=\"btn\" onclick=\"go()\">Set password</button>"
    "</div>"
    "<script>"
    "async function go(){"
    "var p=document.getElementById('p').value,"
    "p2=document.getElementById('p2').value,"
    "err=document.getElementById('err'),"
    "ok=document.getElementById('ok');"
    "err.style.display=ok.style.display='none';"
    "if(p.length<8){err.textContent='Password must be at least 8 characters.';err.style.display='block';return;}"
    "if(p!==p2){err.textContent='Passwords do not match.';err.style.display='block';return;}"
    "document.getElementById('btn').disabled=true;"
    "try{"
    "var r=await fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:p})});"
    "var j=await r.json();"
    "if(j.ok){ok.textContent='Password set. Loading...';ok.style.display='block';"
    "setTimeout(function(){location.href='/';},800);}"
    "else{err.textContent=j.error||'Setup failed.';err.style.display='block';"
    "document.getElementById('btn').disabled=false;}"
    "}catch(e){err.textContent='Network error. Try again.';err.style.display='block';"
    "document.getElementById('btn').disabled=false;}"
    "}"
    "</script></body></html>";

  webServer.on("/setup", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (isOtaCredentialProvisioned()) {
      request->redirect("/");
      return;
    }
    request->send(200, "text/html", SETUP_PAGE);
  });

  webServer.on(
    "/setup", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      // Only reached when the POST had no body
      if (isOtaCredentialProvisioned()) {
        request->send(403, "application/json", "{\"error\":\"Password already set - use /ota/credential to change it\"}");
        return;
      }
      request->send(422, "application/json", "{\"error\":\"JSON body required\"}");
    },
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      storeOtaPassword(request, data, len, index, total, true);
    });

  // Info endpoint
  webServer.on("/ota/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_info;

    if (running != NULL) {
      esp_ota_get_partition_description(running, &app_info);
    }

    String json = "{";
    json += "\"version\":\"" + String(FW_VERSION) + "\",";
    json += "\"hostname\":\"" + String(wifiHostName) + "\",";
    json += "\"chipModel\":\"" + String(ESP.getChipModel()) + "\",";
    json += "\"chipRevision\":\"" + String(ESP.getChipRevision()) + "\",";
    json += "\"freeHeap\":\"" + String(ESP.getFreeHeap()) + "\",";
    json += "\"flashSize\":\"" + String(ESP.getFlashChipSize() / 1024) + " KB\",";
    if (running != NULL) {
      json += "\"partition\":\"" + String(running->label) + "\",";
      json += "\"appVersion\":\"" + String(app_info.version) + "\",";
      json += "\"appDate\":\"" + String(app_info.date) + "\",";
      json += "\"appTime\":\"" + String(app_info.time) + "\"";
    }
    json += "}";
    request->send(200, "application/json", json);
  });

  // Health check endpoint
  webServer.on("/ota/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "OK");
  });

  // SAFETY-CRITICAL: Safety check endpoint
  webServer.on("/ota/check", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool safe = isSystemSafeForOTA();
    String json = "{";
    json += "\"allowed\":" + String(safe ? "true" : "false") + ",";
    json += "\"speed\":" + String(received_vehicle_speed) + ",";
    json += "\"canInitialized\":" + String((hasCANChassis || isStandalone) && (hasCANHaldex || isStandalone) ? "true" : "false") + ",";
    json += "\"busFailure\":" + String(isBusFailure ? "true" : "false") + ",";
    json += "\"controllerDisabled\":" + String(disableController ? "true" : "false") + ",";
    json += "\"mode\":\"" + String(get_openhaldex_mode_string(state.mode)) + "\"";

    if (!safe) {
      json += ",\"reason\":\"";
      if (received_vehicle_speed > 0) json += "Vehicle moving. ";
      if (!hasCANChassis && !isStandalone) json += "Chassis CAN not initialized. ";
      if (!hasCANHaldex) json += "Haldex CAN not initialized. ";
      if (isBusFailure) json += "CAN bus failure. ";
      if (!disableController && state.mode != MODE_STOCK) json += "Controller active. ";
      json += "\"";
    } else {
      json += ",\"reason\":\"System safe for OTA update\"";
    }

    json += "}";
    request->send(200, "application/json", json);
  });

  // SAFETY-CRITICAL: OTA update endpoint with authentication
  webServer.on(
    "/ota/update", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      // Fail-closed auth gate (503 unprovisioned / 401 challenge)
      if (!requireOtaAuth(request)) return;

      // SAFETY CHECK: Block if system not safe
      if (!isSystemSafeForOTA()) {
        request->send(403, "application/json", "{\"error\":\"OTA BLOCKED: System not in safe state\"}");
        return;
      }

      request->send(200, "text/plain", "Ready for upload");
    },
    handleOTAUpdate);

  // Authenticated password rotation - same store path as /setup, but gated by
  // requireOtaAuth so only the current credential holder can replace it
  webServer.on(
    "/ota/credential", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      // Only reached when the POST had no body
      if (!requireOtaAuth(request)) return;
      request->send(422, "application/json", "{\"error\":\"JSON body required\"}");
    },
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      storeOtaPassword(request, data, len, index, total, false);
    });

  // Legacy endpoint for AsyncElegantOTA compatibility (redirects to new endpoint)
  webServer.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Fail-closed auth gate (503 unprovisioned / 401 challenge)
    if (!requireOtaAuth(request)) return;

    // Redirect to info page with instructions
    String html = "<!DOCTYPE html><html><head><title>OTA Update</title></head><body>";
    html += "<h1>OTA Firmware Update</h1>";
    html += "<p>Use the /ota/update endpoint to upload firmware.</p>";
    html += "<p>Current version: " + String(FW_VERSION) + "</p>";

    bool safe = isSystemSafeForOTA();
    html += "<p>System status: " + String(safe ? "<span style='color:green'>SAFE</span>" : "<span style='color:red'>NOT SAFE</span>") + "</p>";

    if (!safe) {
      html += "<p style='color:red'><strong>OTA BLOCKED: System not in safe state</strong></p>";
    }

    html += "<form method='POST' action='/ota/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='firmware' accept='.bin'><br><br>";
    html += "<input type='submit' value='Upload Firmware' " + String(safe ? "" : "disabled") + ">";
    html += "</form>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });
  
  //otaServer->begin();

#if enableDebug || detailedDebugWiFi
  DEBUG("[OTA] OTA server started successfully!");
  DEBUG("[OTA] Update URL: http://192.168.1.1/ota/update");
  DEBUG("[OTA] Username: admin");
  DEBUG("[OTA] Version: %s", FW_VERSION);
  DEBUG("[OTA SAFETY] OTA updates require system to be in safe state");
#endif
}

// ============================================================================
// Check if OTA update is in progress
// ============================================================================
bool isOTAUpdateInProgress() {
  return otaUpdateInProgress;
}

// ============================================================================
// Get current firmware version
// ============================================================================
String getFirmwareVersion() {
  return String(FW_VERSION);
}



