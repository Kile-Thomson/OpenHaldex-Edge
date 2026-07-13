#include <OpenHaldexC6_OTA.h>
#include <OpenHaldexC6_Calculations.h> // wifi_password_provisioned
#include <LittleFS.h>                  // unmount before rewriting the FS partition
#include <esp_partition.h>             // raw partition writes for the web-UI image

//static AsyncWebServer *otaServer = nullptr;

// ============================================================================
// SAFETY-CRITICAL: Configuration
// ============================================================================

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

// ============================================================================
// Auth boundary: the WiFi AP password
// ============================================================================
// The AP password is the single auth boundary. Anyone who can reach the web
// server or the analyzer port has already joined the WPA2-protected AP, so there
// is no separate HTTP login. Both predicates below delegate to the one
// host-tested wifi_password_provisioned() decision over the live AP password.

// True once a WiFi AP password (>= 8 chars) is set. Gates the first-run /setup
// page and the dashboard redirect while the AP is still open.
bool isDeviceProvisioned() {
  return wifi_password_provisioned(wifiPassword);
}

// Fail-closed analyzer-injection gate, evaluated once per analyzer TCP
// connection. host->device CAN transmit is dropped while the AP is open; passive
// sniffing is unaffected.
bool analyzerInjectionPermitted() {
  return wifi_password_provisioned(wifiPassword);
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
  // SAFETY CHECK: the safe-state gate must be enforced HERE, not in the onRequest
  // callback - ESPAsyncWebServer delivers the upload body BEFORE onRequest runs,
  // so gating there would let an unsafe upload reach esp_ota_write(). Chunk 0 runs
  // the gate and sets _tempObject as an "authorized" marker; later chunks no-op
  // while it is null. malloc'd because the request destructor releases
  // _tempObject with free() if the upload aborts. Network access is already gated
  // by the WiFi AP password (the single auth boundary).
  if (index == 0) {
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
// Filesystem (web UI) Update Handler - SAFETY-CRITICAL: Blocks unsafe updates
// ============================================================================
// Writes a LittleFS image straight to the filesystem partition so the web UI
// can be updated over the air. Same chunk-0 authorization pattern as
// handleOTAUpdate above: the safe-state gate runs on chunk 0 and sets
// _tempObject as the "authorized" marker; later chunks no-op while it is null.
// Unlike the app path there is no esp_ota_end() image validation, so chunk 0
// also rejects anything carrying the ESP firmware image magic (0xE9) - the one
// realistic wrong-file mistake that would otherwise blank the UI silently.
// ============================================================================

#define FS_OTA_SECTOR_SIZE 4096 // SPI flash erase granularity

static const esp_partition_t *fsPartition = nullptr;
static size_t fsErasedUpTo = 0; // erase high-water mark, always sector-aligned

void handleFSUpdate(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    // SAFETY CHECK: block update if system is not in a safe state.
    if (!isSystemSafeForOTA()) {
      request->send(403, "text/plain", "FS OTA BLOCKED: System not in safe state. Vehicle must be stationary, CAN initialized, outputs safe, no faults.");
      return;
    }

    // Wrong-file guard: a firmware image starts with the ESP image magic byte
    // 0xE9; a LittleFS image never does.
    if (len > 0 && data[0] == 0xE9) {
      request->send(400, "text/plain", "FS OTA ERROR: This looks like a firmware image. Upload littlefs.bin here; firmware goes in the firmware slot.");
      return;
    }

    fsPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (fsPartition == NULL) {
      request->send(500, "text/plain", "FS OTA ERROR: No filesystem partition found. Check partition table.");
      return;
    }

    otaUpdateInProgress = true;
    fsErasedUpTo = 0;

    // The web UI is served from this partition - unmount before overwriting.
    // From here until the post-update reboot, static file requests will fail;
    // the uploading client is told to stay on the page.
    LittleFS.end();

#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA] Starting filesystem update to partition: %s (%u bytes)", fsPartition->label, (unsigned)fsPartition->size);
#endif

    request->_tempObject = malloc(1);
    if (request->_tempObject == nullptr) {
      otaUpdateInProgress = false;
      request->send(500, "text/plain", "FS OTA ERROR: Out of memory");
      return;
    }
  } else if (request->_tempObject == nullptr) {
    // First chunk was rejected (auth / safety / partition) -> ignore every later chunk.
    return;
  }

  if (index + len > fsPartition->size) {
    request->send(400, "text/plain", "FS OTA ERROR: Image larger than filesystem partition");
    otaUpdateInProgress = false;
    return;
  }

  // Erase ahead of the write cursor in whole sectors - upload chunks arrive at
  // arbitrary sizes, but flash erases only in FS_OTA_SECTOR_SIZE blocks.
  size_t needed = index + len;
  if (needed > fsErasedUpTo) {
    size_t eraseEnd = (needed + FS_OTA_SECTOR_SIZE - 1) & ~(size_t)(FS_OTA_SECTOR_SIZE - 1);
    if (eraseEnd > fsPartition->size) eraseEnd = fsPartition->size;
    esp_err_t eraseErr = esp_partition_erase_range(fsPartition, fsErasedUpTo, eraseEnd - fsErasedUpTo);
    if (eraseErr != ESP_OK) {
      request->send(500, "text/plain", "FS OTA ERROR: Erase failed");
      otaUpdateInProgress = false;
      return;
    }
    fsErasedUpTo = eraseEnd;
  }

  esp_err_t err = esp_partition_write(fsPartition, index, data, len);
  if (err != ESP_OK) {
    request->send(500, "text/plain", "FS OTA ERROR: Write failed");
    otaUpdateInProgress = false;
    return;
  }

  if (final) {
#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA] Filesystem update complete (%u bytes). Rebooting...", (unsigned)(index + len));
#endif
    request->send(200, "text/plain", "Filesystem update complete. Rebooting...");

    // Small delay to allow response to be sent
    delay(1000);
    ESP.restart();
  }
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

  // First-run WiFi-password setup page. The AP password is the single auth
  // boundary, so a fresh device comes up as an open AP and the dashboard forces
  // this page until a WPA2-length (>= 8 char) password is set. The page POSTs to
  // the existing /api/wifi endpoint, which persists the password and restarts the
  // AP protected; the client then reconnects with the new password. Closes
  // (redirects to /) once the AP is provisioned.
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
    "<p>Set a WiFi password to secure this device. Minimum 8 characters. The AP "
    "will restart protected - reconnect with your new password.</p>"
    "<div class=\"msg err\" id=\"err\"></div>"
    "<div class=\"msg ok\" id=\"ok\"></div>"
    "<label>WiFi password</label>"
    "<input type=\"password\" id=\"p\" autocomplete=\"new-password\" placeholder=\"Min. 8 characters\">"
    "<label>Confirm password</label>"
    "<input type=\"password\" id=\"p2\" autocomplete=\"new-password\" placeholder=\"Repeat password\">"
    "<button id=\"btn\" onclick=\"go()\">Set WiFi password</button>"
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
    "var r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:p})});"
    "var j=await r.json();"
    "if(j.ok&&j.passwordSet){ok.textContent='WiFi password set. The AP is restarting - reconnect with your new password, then reopen this page.';ok.style.display='block';}"
    "else{err.textContent=j.error||'Setup failed.';err.style.display='block';"
    "document.getElementById('btn').disabled=false;}"
    "}catch(e){"
    // The AP restart tears down this connection, so a network error after a
    // submit usually means the password was accepted. Tell the user to reconnect.
    "ok.textContent='WiFi password set. The AP is restarting - reconnect with your new password, then reopen this page.';ok.style.display='block';"
    "}"
    "}"
    "</script></body></html>";

  webServer.on("/setup", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (isDeviceProvisioned()) {
      request->redirect("/");
      return;
    }
    request->send(200, "text/html", SETUP_PAGE);
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

  // SAFETY-CRITICAL: OTA update endpoint. Network access is gated by the WiFi AP
  // password (the single auth boundary); this handler only enforces the safe-state
  // gate. The upload body handler (handleOTAUpdate) re-checks safe-state on chunk 0.
  webServer.on(
    "/ota/update", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      // SAFETY CHECK: Block if system not safe
      if (!isSystemSafeForOTA()) {
        request->send(403, "application/json", "{\"error\":\"OTA BLOCKED: System not in safe state\"}");
        return;
      }

      request->send(200, "text/plain", "Ready for upload");
    },
    handleOTAUpdate);

  // SAFETY-CRITICAL: Filesystem (web UI) update endpoint. Same auth and
  // safe-state model as /ota/update; handleFSUpdate re-checks safe-state on
  // chunk 0 and sends the definitive response from the body handler.
  webServer.on(
    "/ota/updatefs", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      // SAFETY CHECK: Block if system not safe
      if (!isSystemSafeForOTA()) {
        request->send(403, "application/json", "{\"error\":\"OTA BLOCKED: System not in safe state\"}");
        return;
      }

      request->send(200, "text/plain", "Ready for upload");
    },
    handleFSUpdate);

  // Legacy endpoint for AsyncElegantOTA compatibility (redirects to new endpoint)
  webServer.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
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
  DEBUG("[OTA] Auth: WiFi AP password (no separate HTTP login)");
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



