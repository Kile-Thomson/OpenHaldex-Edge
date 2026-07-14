#include <OpenHaldexC6_OTA.h>
#include <OpenHaldexC6_Calculations.h> // wifi_password_provisioned
#include <OpenHaldexC6_OTARoute.h>     // upload classification + merged-image chunk routing (host-tested)
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
// /ota/update accepts three image types, classified from the first chunk (see
// OpenHaldexC6_OTARoute.h): a bare firmware.bin (dual-slot app OTA), a bare
// littlefs.bin (delegated to handleFSUpdate), or the release's
// firmware-merged.bin - the same single file used for USB flashing - whose app
// and filesystem segments are split out by flash offset and written to their
// partitions. The bootloader / partition-table / NVS regions of a merged image
// are never written, so settings and the learn table survive.
// ============================================================================

#define FS_OTA_SECTOR_SIZE 4096 // SPI flash erase granularity

static const esp_partition_t *fsPartition = nullptr;
static size_t fsErasedUpTo = 0; // erase high-water mark, always sector-aligned

// What the in-flight upload was classified as on chunk 0. One OTA upload at a
// time (matching the single otaHandle above); a failure mid-upload flips this
// back to OTA_KIND_INVALID so the dead upload's remaining chunks no-op.
static ota_upload_kind_t uploadKind = OTA_KIND_INVALID;

// Merged-image source windows: absolute offsets within the uploaded file where
// the app and filesystem segments sit (a merged image mirrors flash layout).
static size_t mergedAppSrcStart = 0, mergedAppSrcEnd = 0;
static size_t mergedFsSrcStart = 0, mergedFsSrcEnd = 0;
static bool mergedFsUnmounted = false;

// Best-effort web UI recovery after a failed filesystem update. LittleFS was
// unmounted before the partition write started and setupWebServer() only
// mounts at boot, so without this every failure path would leave the UI dead
// until a power cycle. The partition may be partially erased or written, in
// which case the mount fails - the HTTP API and the OTA endpoints themselves
// stay up regardless, so the user can retry the upload without touching the
// hardware (a retry unmounts again on its first chunk).
static void otaRestoreFS()
{
  if (LittleFS.begin(false)) {
#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA] LittleFS remounted after failed update");
#endif
  } else {
#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA] LittleFS remount failed - partition needs a successful re-upload before the UI returns");
#endif
  }
}

void handleFSUpdate(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);

// Erase the filesystem partition ahead of the write cursor in whole sectors -
// upload chunks arrive at arbitrary sizes, but flash erases only in
// FS_OTA_SECTOR_SIZE blocks.
static esp_err_t fsEraseAhead(size_t needed) {
  if (needed <= fsErasedUpTo) return ESP_OK;
  size_t eraseEnd = (needed + FS_OTA_SECTOR_SIZE - 1) & ~(size_t)(FS_OTA_SECTOR_SIZE - 1);
  if (eraseEnd > fsPartition->size) eraseEnd = fsPartition->size;
  esp_err_t err = esp_partition_erase_range(fsPartition, fsErasedUpTo, eraseEnd - fsErasedUpTo);
  if (err == ESP_OK) fsErasedUpTo = eraseEnd;
  return err;
}

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

    // Classify the upload so one endpoint (and one Settings-page file input)
    // takes firmware.bin, littlefs.bin, or firmware-merged.bin.
    const esp_partition_t *nextApp = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *fsPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (nextApp == NULL) {
      request->send(500, "text/plain", "OTA ERROR: No OTA partition found. Check partition table.");
      return;
    }
    uploadKind = ota_classify_upload(data, len, request->contentLength(),
                                     nextApp->size,
                                     fsPart ? fsPart->address : SIZE_MAX);

    if (uploadKind == OTA_KIND_FS) {
      // A bare littlefs.bin - the filesystem handler owns the whole upload.
      handleFSUpdate(request, filename, index, data, len, final);
      return;
    }
    if (uploadKind == OTA_KIND_INVALID) {
      request->send(400, "text/plain", "OTA ERROR: Unrecognized file. Upload firmware-merged.bin (full package), firmware.bin, or littlefs.bin.");
      return;
    }

    if (uploadKind == OTA_KIND_MERGED) {
      // A merged image mirrors absolute flash offsets: its app segment sits at
      // ota_0's address (a dual-slot app image runs from either slot), its
      // filesystem segment at the fs partition's address. classify() can only
      // return MERGED when fsPart resolved, but keep the write path honest.
      const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
      if (ota0 == NULL || fsPart == NULL) {
        request->send(500, "text/plain", "OTA ERROR: Partition table missing ota_0 or filesystem entry.");
        uploadKind = OTA_KIND_INVALID;
        return;
      }
      mergedAppSrcStart = ota0->address;
      mergedAppSrcEnd = ota0->address + nextApp->size;
      mergedFsSrcStart = fsPart->address;
      mergedFsSrcEnd = fsPart->address + fsPart->size;
      mergedFsUnmounted = false;
      fsPartition = fsPart;
      fsErasedUpTo = 0;
    }

    otaUpdateInProgress = true;
    otaPartition = nextApp;

#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA] Starting %s update to partition: %s", uploadKind == OTA_KIND_MERGED ? "merged (firmware + web UI)" : "firmware", otaPartition->label);
#endif

    // Begin OTA update
    esp_err_t beginErr = esp_ota_begin(otaPartition, OTA_SIZE_UNKNOWN, &otaHandle);
    if (beginErr != ESP_OK) {
      request->send(500, "text/plain", "OTA ERROR: Failed to begin update");
      otaUpdateInProgress = false;
      uploadKind = OTA_KIND_INVALID;
      return;
    }

    // Index 0 fully passed: authorize the remaining chunks to write. If the
    // allocation fails, abort the already-begun OTA cleanly rather than leave
    // it half-open with every later chunk stranded.
    request->_tempObject = malloc(1);
    if (request->_tempObject == nullptr) {
      esp_ota_abort(otaHandle);
      otaUpdateInProgress = false;
      uploadKind = OTA_KIND_INVALID;
      request->send(500, "text/plain", "OTA ERROR: Out of memory");
      return;
    }
  } else {
    if (uploadKind == OTA_KIND_FS) {
      // Later chunks of a delegated littlefs.bin upload.
      handleFSUpdate(request, filename, index, data, len, final);
      return;
    }
    if (request->_tempObject == nullptr || uploadKind == OTA_KIND_INVALID) {
      // Chunk 0 was rejected, or the upload already failed -> ignore the rest.
      return;
    }
  }

  // Write data chunk
  if (uploadKind == OTA_KIND_MERGED) {
    size_t srcOff = 0, dstOff = 0, n;

    // App segment -> spare app slot via the OTA handle. Chunks arrive in file
    // order, so the app bytes reach esp_ota_write sequentially; the 0xFF pad
    // between the real image and the slot end writes harmlessly to erased
    // flash and esp_ota_end() validates only the image proper.
    n = ota_region_overlap(index, len, mergedAppSrcStart, mergedAppSrcEnd, &srcOff, &dstOff);
    if (n > 0) {
      esp_err_t werr = esp_ota_write(otaHandle, data + srcOff, n);
      if (werr != ESP_OK) {
        request->send(500, "text/plain", "OTA ERROR: Write failed");
        esp_ota_abort(otaHandle);
        if (mergedFsUnmounted) {
          otaRestoreFS();
          mergedFsUnmounted = false;
        }
        otaUpdateInProgress = false;
        uploadKind = OTA_KIND_INVALID;
        return;
      }
    }

    // Filesystem segment -> fs partition. Unmount on first touch: the web UI
    // is served from this partition and it is rewritten in place.
    n = ota_region_overlap(index, len, mergedFsSrcStart, mergedFsSrcEnd, &srcOff, &dstOff);
    if (n > 0) {
      if (!mergedFsUnmounted) {
        LittleFS.end();
        mergedFsUnmounted = true;
      }
      esp_err_t werr = fsEraseAhead(dstOff + n);
      if (werr == ESP_OK) werr = esp_partition_write(fsPartition, dstOff, data + srcOff, n);
      if (werr != ESP_OK) {
        request->send(500, "text/plain", "OTA ERROR: Filesystem write failed");
        esp_ota_abort(otaHandle);
        otaRestoreFS();
        mergedFsUnmounted = false;
        otaUpdateInProgress = false;
        uploadKind = OTA_KIND_INVALID;
        return;
      }
    }
  } else {
    esp_err_t werr = esp_ota_write(otaHandle, data, len);
    if (werr != ESP_OK) {
      request->send(500, "text/plain", "OTA ERROR: Write failed");
      esp_ota_abort(otaHandle);
      otaUpdateInProgress = false;
      uploadKind = OTA_KIND_INVALID;
      return;
    }
  }

  // Final chunk - finish OTA
  if (final) {
    esp_err_t err = esp_ota_end(otaHandle);
    if (err != ESP_OK) {
      if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        request->send(400, "text/plain", "OTA ERROR: Image validation failed");
      } else {
        request->send(500, "text/plain", "OTA ERROR: End failed");
      }
      esp_ota_abort(otaHandle);
      // Merged upload: the fs partition already holds the complete new image
      // at this point, so the remount brings the (new) UI back on the old app.
      if (mergedFsUnmounted) {
        otaRestoreFS();
        mergedFsUnmounted = false;
      }
      otaUpdateInProgress = false;
      uploadKind = OTA_KIND_INVALID;
      return;
    }

    // Set boot partition to new firmware
    err = esp_ota_set_boot_partition(otaPartition);
    if (err != ESP_OK) {
      request->send(500, "text/plain", "OTA ERROR: Failed to set boot partition");
      if (mergedFsUnmounted) {
        otaRestoreFS();
        mergedFsUnmounted = false;
      }
      otaUpdateInProgress = false;
      uploadKind = OTA_KIND_INVALID;
      return;
    }

#if enableDebug || detailedDebugWiFi
    DEBUG("[OTA] Update complete. Rebooting...");
    DEBUG("[OTA SAFETY] New firmware will require confirmation on boot");
#endif

    request->send(200, "text/plain", uploadKind == OTA_KIND_MERGED
      ? "Update complete (firmware + web UI). Rebooting... Firmware will be confirmed after safety checks pass."
      : "OTA update complete. Rebooting... Firmware will be confirmed after safety checks pass.");

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
// requires the littlefs superblock magic - any other file (a firmware image,
// a random download) is rejected before it can blank the UI silently.
// ============================================================================

void handleFSUpdate(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    // SAFETY CHECK: block update if system is not in a safe state.
    if (!isSystemSafeForOTA()) {
      request->send(403, "text/plain", "FS OTA BLOCKED: System not in safe state. Vehicle must be stationary, CAN initialized, outputs safe, no faults.");
      return;
    }

    // Wrong-file guard: a littlefs image carries the "littlefs" superblock
    // magic at byte 8. Anything else - a firmware image, a random file - gets
    // rejected before it can overwrite the web UI.
    if (!ota_image_is_littlefs(data, len)) {
      request->send(400, "text/plain", "FS OTA ERROR: Not a littlefs image. Upload littlefs.bin here, or use firmware-merged.bin for a full update.");
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
      otaRestoreFS(); // nothing written yet - the old filesystem mounts fine
      otaUpdateInProgress = false;
      request->send(500, "text/plain", "FS OTA ERROR: Out of memory");
      return;
    }
  } else if (request->_tempObject == nullptr) {
    // First chunk was rejected (auth / safety / partition) -> ignore every later chunk.
    return;
  }

  // Shared failure exit for the write phase: revoke the chunk-0 authorization
  // so every later chunk of this failed upload no-ops, then try to bring the
  // web UI back (the partition may be partially erased, so the mount can fail;
  // the OTA endpoint still accepts a retry either way).
  auto failFSUpdate = [&](int code, const char *msg) {
    request->send(code, "text/plain", msg);
    free(request->_tempObject);
    request->_tempObject = nullptr;
    otaRestoreFS();
    otaUpdateInProgress = false;
  };

  if (index + len > fsPartition->size) {
    failFSUpdate(400, "FS OTA ERROR: Image larger than filesystem partition");
    return;
  }

  esp_err_t eraseErr = fsEraseAhead(index + len);
  if (eraseErr != ESP_OK) {
    failFSUpdate(500, "FS OTA ERROR: Erase failed");
    return;
  }

  esp_err_t err = esp_partition_write(fsPartition, index, data, len);
  if (err != ESP_OK) {
    failFSUpdate(500, "FS OTA ERROR: Write failed");
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

  // SAFETY-CRITICAL: OTA update endpoint. Accepts firmware.bin, littlefs.bin,
  // or firmware-merged.bin - the body handler classifies from the first chunk.
  // Network access is gated by the WiFi AP password (the single auth boundary);
  // this handler only enforces the safe-state gate. The upload body handler
  // (handleOTAUpdate) re-checks safe-state on chunk 0.
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



