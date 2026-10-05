#include <OpenHaldexC6_EEP.h>          // include the header for EEPROM/preference functions
#include <OpenHaldexC6_Calculations.h> // for haldexLearnTable and learn globals

// Persisted settings: ONE NVS namespace ("openhaldex") with a "seeded" sentinel.
//
// History: the old code called pref.begin() once per setting NAME. Preferences
// ignores a begin() while a namespace is open, so only the FIRST call won and
// every key de-facto lived under that first name ("broadcastOpen"); earlier
// builds ended up under the LAST name ("udsMQBEn") or "learnTable". First-run
// detection also read a key that was never written, so the seed branch never ran.
// readEEP() therefore opens "openhaldex" and, when it is not seeded yet, migrates
// any of those legacy namespaces once. Key names are unchanged, so a v8 Edge unit
// (already on "openhaldex") and a stock upstream unit both keep their settings.

// Namespace holding the previous layout, probed read-only. "haldexGen" is written
// by every prior firmware, so its presence marks a real install.
static bool legacyNamespaceHasData(Preferences &legacy, bool &opened)
{
  static const char *const candidates[] = {"broadcastOpen", "udsMQBEn", "learnTable"};
  opened = false;
  for (const char *ns : candidates)
  {
    if (legacy.begin(ns, true))
    {
      if (legacy.isKey("haldexGen"))
      {
        opened = true;
        return true;
      }
      legacy.end();
    }
  }
  return false;
}

// Load every persisted setting from one open Preferences handle into the runtime
// globals. Shared by the normal-run LOAD path (source = the 'openhaldex'
// namespace) and the one-time legacy MIGRATE path (read-only legacy handle) so
// the two cannot drift apart. Missing keys keep the compiled default. Returns
// true when a frame-edit mask was missing/migrated and should be persisted.
// Does NOT touch the lastMode->state.mode mapping; the caller applies that once.
static bool loadSettingsFrom(Preferences &src)
{
  broadcastOpenHaldexOverCAN = src.getBool("broadcastOpen", false);      // load broadcast setting
  isStandalone = src.getBool("isStandalone", false);                     // load standalone setting
  useCANifAvailable = src.getBool("useCANifAvail", false);               // load use CAN if available setting
  disableController = src.getBool("disableControl", false);              // load controller disable
  followBrake = src.getBool("followBrake", false);                       // load follow brake
  followHandbrake = src.getBool("followHandbrake", false);               // load follow handbrake
  invertBrake = src.getBool("invertBrake", false);                       // load invert brake
  invertHandbrake = src.getBool("invertHandbrake", false);               // load invert handbrake
  tcForceMode = src.getBool("tcForceMode", false);                       // load tc force mode
  extBtnForceMode = src.getBool("extBtnForceMode", false);               // load ext button force mode
  hazardForceMode = src.getBool("hazardForceMode", false);               // load hazard force mode
  disableOnboardButton = src.getBool("dsbOnboardBtn", false);            // load disable onboard button
  disableExternalButton = src.getBool("dsbExtBtn", false);               // load disable external button
  fixHunting = src.getBool("fixHunting", false);                         // load Motor_11 BPK-mode toggle
  dangerZoneEnabled = src.getBool("dangerZone", false);                  // load Danger Zone (full-duty 50:50)
  bpkCeilingNm = src.getUShort("bpkCeilNm", 220);                        // load BPK per-car lock calibration (Nm)
  // Launch PWM floor: v8 key "esp14MinFl"; a stock v9 unit stored it as "esp14Floor".
  esp14MinFloorPct = src.isKey("esp14MinFl") ? src.getUChar("esp14MinFl", 0) : src.getUChar("esp14Floor", 0);
  src.getString("llNotes", longLearnNotes, sizeof(longLearnNotes));      // load Long Learn notes (empty if absent)
  canSleepEnabled = src.getBool("canSleepEn", true);                     // load CAN-wake light sleep enable
  canSleepAggressive = src.getBool("canSleepAggr", false);               // load aggressive CAN sleep enable
  lpWakeThresholdFps = src.getUShort("lpWakeFps", 1100);                 // load LP wake threshold (fps)
  benchMode = src.getBool("benchMode", false);                           // load bench mode
  bleEnabled = src.getBool("bleEn", true);                               // load BLE enable
  blePasskey = src.getUInt("blePasskey", 0);                             // load BLE pairing code (0 = generate)
  ledBrightness = src.getUChar("ledBrightness", led_brightness_default); // load LED brightness

  otaUpdate = src.getBool("otaUpdate", false);                          // load OTA update flag
  haldexGeneration = src.getUChar("haldexGen", 1);                      // load haldex generation with default
  if (haldexGeneration == 5) haldexGeneration = 50;                     // migrate legacy Gen5 -> Gen5 (0CQ)
  udsApplyDefaultIds(); // UDS pair follows the generation from boot (0x71E/0x788 for the VAQ)
  tcForceModeValue = src.getUChar("tcFMV", 2);                          // load TC force mode value (default 50:50)
  hazardForceModeValue = src.getUChar("hazFMV", 2);                     // load Hazard force mode value
  extBtnForceModeValue = src.getUChar("extFMV", 2);                     // load ExtBtn force mode value
  lastMode = src.getUChar("lastMode", 0);                               // load last mode with default
  disableThrottle = src.getUChar("disableThrottle", 0);                 // load throttle disable with default
  state.pedal_threshold = disableThrottle;                              // apply throttle disable to state
  disengageUnderSpeed = src.getUShort("disengageUSpeed", 0);            // load disengage under speed
  disengageAboveSpeed = src.getUShort("disengageASpeed", 0);            // load disengage above speed
  src.getBytes("speedArray", &speedArray, sizeof(speedArray));          // read speed array bytes
  src.getBytes("throttleArray", &throttleArray, sizeof(throttleArray)); // read throttle array bytes
  src.getBytes("lockArray", &lockArray, sizeof(lockArray));             // read lock array bytes
  haldexLearnTableValid = src.getBool("learnOK", false);                // read learn table valid flag
  if (haldexLearnTableValid)
  {
    src.getBytes("learnTbl", haldexLearnTable, sizeof(haldexLearnTable)); // read learn table bytes
  }
  src.getString("wifiPwd", wifiPassword, sizeof(wifiPassword)); // load WiFi AP password
  src.getString("wifiSsid", wifiSsid, sizeof(wifiSsid));        // load WiFi AP SSID
  if (wifiSsid[0] == '\0')                                      // legacy installs: missing key
  {
    strncpy(wifiSsid, wifiHostNameDefault, sizeof(wifiSsid) - 1); // restore factory default
  }
  src.getString("wifiStaSsid", wifiStaSsid, sizeof(wifiStaSsid));        // home-network SSID (missing = "" = bridge mode off)
  src.getString("wifiStaPwd", wifiStaPassword, sizeof(wifiStaPassword)); // home-network password
  liveDiagEnabled = src.getBool("udsMQBEn", false);                 // load live-diagnostics enable (legacy key name)
  forceModesPriority = src.getUChar("forceModesPrio", 0);           // load force-modes priority (0=TC>Haz>Ext)

  // Lock ramp times are milliseconds for a full travel. Prefer the ms keys; if
  // only the %/s key is present (a unit seeded on older firmware, or a legacy
  // namespace migration) convert it once. Re-persisted under the ms key.
  if (src.isKey("lockReleaseMs"))
    lockReleaseRampMs = src.getUShort("lockReleaseMs", 500);        // load lock release ramp (ms)
  else
    lockReleaseRampMs = lock_ramp_ms_from_rate(src.getFloat("lockReleaseRate", 120.0f)); // migrate %/s
  if (src.isKey("lockEngageMs"))
    lockEngageRampMs = src.getUShort("lockEngageMs", 0);            // load lock engage ramp (ms)
  else
    lockEngageRampMs = lock_ramp_ms_from_rate(src.getFloat("lockEngageRate", 0.0f));      // migrate %/s
  lockReleaseEnabled = src.getBool("lockReleaseEn", true);          // load lock release enable

  // Steering lock taper: the v9 breakpoint curve is the one implementation. A unit
  // that stored the v8 three-knob taper (steerGain*) and no curve yet gets the
  // equivalent curve built from it, so the setting carries over.
  steeringScaleEnabled = src.isKey("steerScaleEn") ? src.getBool("steerScaleEn", true)
                                                   : src.getBool("steerGainEn", true);
  if (src.getBytesLength("steerArray") == sizeof(steeringArray) &&
      src.getBytesLength("steerScale") == sizeof(steeringLockScaleArray))
  {
    src.getBytes("steerArray", &steeringArray, sizeof(steeringArray));             // breakpoints (deg)
    src.getBytes("steerScale", &steeringLockScaleArray, sizeof(steeringLockScaleArray)); // 0-100 % multipliers
  }
  else if (src.isKey("steerGainStart"))
  {
    steering_curve_from_taper(src.getUShort("steerGainStart", 45), src.getUShort("steerGainFull", 180),
                              src.getUChar("steerGainFloor", 50), steeringArray, steeringLockScaleArray);
  }

  // Per-car corner-slip geometry (Calibrate tab)
  slipWheelbaseMm = src.getUShort("slipWb", slipWheelbaseMm);
  slipTrackFrontMm = src.getUShort("slipTf", slipTrackFrontMm);
  slipTrackRearMm = src.getUShort("slipTr", slipTrackRearMm);
  slipSteeringRatio = src.getFloat("slipRatio", slipSteeringRatio);
  slipMinSpeedRaw = src.getUShort("slipMin", slipMinSpeedRaw);

  // Frame-edit masks (which CAN frame blocks are edited, per generation)
  bool masksChanged = false;
  if (src.getBytesLength("feMask") == sizeof(frameEditMask))
  {
    src.getBytes("feMask", frameEditMask, sizeof(frameEditMask));
    if (frameEditMask[FE_GEN_50] == 0x000003FFULL)
    {
      frameEditMask[FE_GEN_50] = frameEditMaskDefaults[FE_GEN_50]; // migrate old 0CQ normal default: Motor_14/ESP_07 passthrough
      masksChanged = true;
    }
  }
  else
  {
    for (uint8_t i = 0; i < FE_GEN_COUNT; i++)
      frameEditMask[i] = frameEditMaskDefaults[i]; // legacy install: use normal defaults
    masksChanged = true;
  }
  if (src.getBytesLength("feMaskSA") == sizeof(frameEditMaskSA))
    src.getBytes("feMaskSA", frameEditMaskSA, sizeof(frameEditMaskSA));
  else
  {
    for (uint8_t i = 0; i < FE_GEN_COUNT; i++)
      frameEditMaskSA[i] = frameEditMaskDefaultsSA[i]; // legacy install: use standalone all-on defaults
    masksChanged = true;
  }
  return masksChanged;
}

// Persist every runtime-global setting into the canonical 'openhaldex' handle
// (`pref`). Shared by the first-run SEED path (globals hold the compiled
// defaults), the legacy MIGRATE path (globals were just loaded from the legacy
// namespace) and the periodic writeEEP task, so the namespace ends up identical
// whichever way it was filled. Multi-field structures are snapshotted under
// stateMutex first, so a persisted table is never one caught mid-update.
static void persistSettingsToPref()
{
  static uint8_t snapSpeed[sizeof(speedArray)];
  static uint8_t snapThrottle[sizeof(throttleArray)];
  static uint8_t snapLock[sizeof(lockArray)];
  static uint8_t snapLearn[sizeof(haldexLearnTable)];
  static uint8_t snapSteer[sizeof(steeringArray)];
  static uint8_t snapSteerScale[sizeof(steeringLockScaleArray)];
  static uint64_t snapMask[FE_GEN_COUNT];
  static uint64_t snapMaskSA[FE_GEN_COUNT];
  bool snapLearnValid;
  uint8_t snapDisableThrottle;
  uint16_t snapDisengageUnder;
  uint16_t snapDisengageAbove;
  uint16_t snapReleaseMs, snapEngageMs;
  if (stateMutex != nullptr)
    xSemaphoreTake(stateMutex, portMAX_DELAY);
  memcpy(snapSpeed, speedArray, sizeof(speedArray));
  memcpy(snapThrottle, throttleArray, sizeof(throttleArray));
  memcpy(snapLock, lockArray, sizeof(lockArray));
  memcpy(snapLearn, haldexLearnTable, sizeof(haldexLearnTable));
  memcpy(snapSteer, steeringArray, sizeof(steeringArray));
  memcpy(snapSteerScale, steeringLockScaleArray, sizeof(steeringLockScaleArray));
  memcpy(snapMask, frameEditMask, sizeof(snapMask));
  memcpy(snapMaskSA, frameEditMaskSA, sizeof(snapMaskSA));
  snapLearnValid = haldexLearnTableValid;
  snapDisableThrottle = disableThrottle;
  snapDisengageUnder = disengageUnderSpeed;
  snapDisengageAbove = disengageAboveSpeed;
  snapReleaseMs = lockReleaseRampMs;
  snapEngageMs = lockEngageRampMs;
  if (stateMutex != nullptr)
    xSemaphoreGive(stateMutex);

  pref.putBool("broadcastOpen", broadcastOpenHaldexOverCAN); // broadcast setting
  pref.putBool("isStandalone", isStandalone);                // standalone setting
  pref.putBool("useCANifAvail", useCANifAvailable);          // use CAN if available setting
  pref.putBool("disableControl", disableController);         // controller disable
  pref.putBool("followBrake", followBrake);                  // follow brake
  pref.putBool("followHandbrake", followHandbrake);          // follow handbrake
  pref.putBool("invertBrake", invertBrake);                  // invert brake
  pref.putBool("invertHandbrake", invertHandbrake);          // invert handbrake
  pref.putBool("tcForceMode", tcForceMode);                  // tc force mode
  pref.putBool("extBtnForceMode", extBtnForceMode);          // ext button force mode
  pref.putBool("hazardForceMode", hazardForceMode);          // hazard force mode
  pref.putBool("dsbOnboardBtn", disableOnboardButton);       // disable onboard button
  pref.putBool("dsbExtBtn", disableExternalButton);          // disable external button
  pref.putBool("fixHunting", fixHunting);                    // Motor_11 BPK-mode toggle
  pref.putBool("dangerZone", dangerZoneEnabled);             // Danger Zone (full-duty 50:50)
  pref.putUShort("bpkCeilNm", bpkCeilingNm);                 // BPK per-car lock calibration (Nm)
  pref.putUChar("esp14MinFl", esp14MinFloorPct);             // ESP_14 Min-band floor (% of full command)
  pref.putString("llNotes", longLearnNotes);                 // Long Learn chassis/car notes
  pref.putBool("canSleepEn", canSleepEnabled);               // CAN-wake light sleep enable
  pref.putBool("canSleepAggr", canSleepAggressive);          // aggressive CAN sleep enable
  pref.putUShort("lpWakeFps", lpWakeThresholdFps);           // LP wake threshold (fps)
  pref.putBool("benchMode", benchMode);                      // bench mode
  pref.putBool("bleEn", bleEnabled);                         // BLE enable
  pref.putUInt("blePasskey", blePasskey);                    // BLE pairing code
  pref.putUChar("ledBrightness", ledBrightness);             // LED brightness

  pref.putBool("otaUpdate", otaUpdate);                      // OTA update flag
  pref.putUChar("haldexGen", haldexGeneration);              // haldex generation
  pref.putUChar("tcFMV", tcForceModeValue);                  // TC force mode value
  pref.putUChar("hazFMV", hazardForceModeValue);             // Hazard force mode value
  pref.putUChar("extFMV", extBtnForceModeValue);             // ExtBtn force mode value
  pref.putUChar("lastMode", lastMode);                       // last used mode
  pref.putUChar("disableThrottle", snapDisableThrottle);     // throttle disable
  pref.putUShort("disengageUSpeed", snapDisengageUnder);     // disengage under speed
  pref.putUShort("disengageASpeed", snapDisengageAbove);     // disengage above speed
  pref.putBytes("speedArray", snapSpeed, sizeof(snapSpeed));          // speed array
  pref.putBytes("throttleArray", snapThrottle, sizeof(snapThrottle)); // throttle array
  pref.putBytes("lockArray", snapLock, sizeof(snapLock));             // lock array
  pref.putBool("learnOK", snapLearnValid);                            // learn table valid flag
  if (snapLearnValid)
  {
    pref.putBytes("learnTbl", snapLearn, sizeof(snapLearn)); // learn table bytes
  }
  pref.putString("wifiPwd", wifiPassword);        // WiFi AP password
  pref.putString("wifiSsid", wifiSsid);           // WiFi AP SSID
  pref.putString("wifiStaSsid", wifiStaSsid);     // home-network (bridge mode) SSID
  pref.putString("wifiStaPwd", wifiStaPassword);  // home-network password
  pref.putBool("udsMQBEn", liveDiagEnabled);      // live-diagnostics enable (legacy key name)
  pref.putUChar("forceModesPrio", forceModesPriority); // force-modes priority order
  pref.putUShort("lockReleaseMs", snapReleaseMs); // lock release ramp (ms)
  pref.putUShort("lockEngageMs", snapEngageMs);   // lock engage ramp (ms)
  pref.putBool("lockReleaseEn", lockReleaseEnabled); // lock release enable
  pref.putBool("steerScaleEn", steeringScaleEnabled); // steering-scale enable
  pref.putBytes("steerArray", snapSteer, sizeof(snapSteer));           // steering breakpoints
  pref.putBytes("steerScale", snapSteerScale, sizeof(snapSteerScale)); // steering lock-scale
  pref.putUShort("slipWb", slipWheelbaseMm);      // slip geometry: wheelbase
  pref.putUShort("slipTf", slipTrackFrontMm);     // slip geometry: front track
  pref.putUShort("slipTr", slipTrackRearMm);      // slip geometry: rear track
  pref.putFloat("slipRatio", slipSteeringRatio);  // slip geometry: steering ratio
  pref.putUShort("slipMin", slipMinSpeedRaw);     // slip geometry: min speed
  pref.putBytes("feMask", snapMask, sizeof(snapMask));       // frame-edit masks
  pref.putBytes("feMaskSA", snapMaskSA, sizeof(snapMaskSA)); // standalone frame-edit masks
}

void readEEP() // function to read stored preferences into runtime variables
{              // start readEEP
#if detailedDebugEEP
  DEBUG("EEPROM initialising!"); // debug: EEPROM init start
#endif

  if (!pref.begin("openhaldex", false)) // open the one canonical namespace (read/write)
  {
    // NVS partition inaccessible (corrupt or full): boot on in-memory defaults.
    DEBUG("[EEP] pref.begin failed - booting on defaults");
    return;
  }
  const bool seeded = pref.isKey("seeded"); // first-run sentinel (a missing key => not seeded)

  // Read-only peek at the previous layout through a SEPARATE handle so the old
  // namespace is never disturbed. Only looked at when we are not seeded yet.
  Preferences legacy;
  bool legacyOpen = false;
  const bool legacyHas = !seeded && legacyNamespaceHasData(legacy, legacyOpen);

  switch (eeprom_init_action(seeded, legacyHas)) // single LOAD/MIGRATE/SEED decision point
  {
  case EEP_LOAD_EXISTING: // normal run: load stored values from the canonical namespace
    if (loadSettingsFrom(pref))
    {
      persistSettingsToPref(); // a frame-edit mask was missing/migrated: write the fix back
    }
    break;
  case EEP_MIGRATE_LEGACY: // one-time: carry a prior install's settings forward, then mark seeded
#if detailedDebugEEPF
    DEBUG("Migrating legacy NVS namespace..."); // debug: legacy migration
#endif
    loadSettingsFrom(legacy);     // pull the old device's settings into the runtime globals
    persistSettingsToPref();      // write them into "openhaldex" so the legacy namespace is no longer needed
    pref.putBool("seeded", true); // sentinel: this device is now seeded; never migrate again
    break;
  case EEP_SEED_DEFAULTS: // first ever run on a blank device: write the compiled defaults
#if detailedDebugEEPF
    DEBUG("First run..."); // debug: first run detected
#endif
    persistSettingsToPref();      // globals still hold the compiled defaults
    pref.putBool("seeded", true); // sentinel: subsequent boots take the LOAD path
    break;
  }

  if (legacyOpen)
  {
    legacy.end(); // done with the read-only legacy handle; `pref` stays open
  }

  // Map the (now-populated) lastMode to the runtime enum for every path. The
  // mapping is a pure seam (mode_from_last_mode), host-tested; an out-of-range
  // stored byte falls back to MODE_FWD.
  state.mode = mode_from_last_mode(lastMode);

  // Write the normalized value back into lastMode so the raw stored byte can
  // never leak downstream (the settings API reports it; the standalone mode-0
  // path casts it directly). Also self-heals a device already carrying a corrupt
  // byte (a generation number 41/50/51 written by an old bug): the next
  // writeEEP persists the sane 0-5 value.
  lastMode = (uint8_t)state.mode;

#if detailedDebugEEP
  DEBUG("EEPROM initialised with...");                                                           // debug: print loaded prefs
  DEBUG("    Broadcast OpenHaldex over CAN: %s", broadcastOpenHaldexOverCAN ? "true" : "false"); // debug broadcast
  DEBUG("    Standalone mode: %s", isStandalone ? "true" : "false");                             // debug standalone
  DEBUG("    Haldex Generation: %d", haldexGeneration);                                          // debug haldex gen
  DEBUG("    Force Mode TC/Haz/Ext: %d/%d/%d", tcForceModeValue, hazardForceModeValue, extBtnForceModeValue);
  DEBUG("    Last Mode: %d", lastMode);                      // debug last mode
  DEBUG("    Disable Under Speed: %d", disengageUnderSpeed); // debug disengage under speed
  DEBUG("    Disable Above Speed: %d", disengageAboveSpeed); // debug disengage above speed
  DEBUG("    System Update on Reboot: %d", otaUpdate);       // debug ota update flag
#endif
}

void writeEEP(void *arg) // task function to periodically write preferences
{                        // start writeEEP
  while (1)              // loop forever in task
  {
    stackwriteEEP = uxTaskGetStackHighWaterMark(NULL); // record stack high watermark

#if detailedDebugEEP
    DEBUG("Writing EEPROM..."); // debug: writing prefs
#endif

    persistSettingsToPref(); // update EEP (the NVS layer skips writes of unchanged values)

    vTaskDelay(eepRefresh / portTICK_PERIOD_MS); // wait before next write
  } // end while
} // end writeEEP

// ---- On-device map slot library --------------------------------------------
// Slots persist in the "ohmaps" NVS namespace, one blob + one name string per
// slot (keys "b0".."b4" / "n0".."n4"). A slot is free when its name key is
// absent or empty. Each op opens its own short-lived handle so this code never
// shares the settings `pref` handle used by writeEEP.

#define MAP_NS "ohmaps"

// One slot's tune, packed for a single NVS blob write/read.
struct MapSlotBlob
{
  uint16_t speed[speedArrayCount];
  uint8_t throttle[throttleArrayCount];
  uint8_t lock[throttleArrayCount][speedArrayCount];
};

static void mapSlotKeys(uint8_t idx, char blobKey[4], char nameKey[4])
{
  snprintf(blobKey, 4, "b%u", (unsigned)idx);
  snprintf(nameKey, 4, "n%u", (unsigned)idx);
}

void mapSlotNames(char names[MAP_SLOT_COUNT][MAP_NAME_MAX])
{
  for (uint8_t i = 0; i < MAP_SLOT_COUNT; i++)
    names[i][0] = '\0';

  Preferences mp;
  if (!mp.begin(MAP_NS, true)) // read-only; absent namespace = all slots free
    return;

  for (uint8_t i = 0; i < MAP_SLOT_COUNT; i++)
  {
    char blobKey[4], nameKey[4];
    mapSlotKeys(i, blobKey, nameKey);
    // A name is only real if a matching, correctly-sized blob can actually be
    // read back. This MUST use the exact same operation mapSlotLoad() uses to
    // decide a slot is loadable - a real getBytes() into a full-size buffer -
    // not getBytesLength(). getBytesLength() is a different NVS call and can
    // disagree with getBytes() (e.g. it returns 0 for entries it doesn't
    // classify as a plain blob), which would list a slot as used that load()
    // then rejects ("Empty slot" on Load) or, worse, hide a slot that loads
    // fine. Reading the blob here guarantees list and load never disagree. A
    // wrong-size blob (stale write from an older struct layout, or a partial
    // write) fails the == sizeof check and reads as free, so a fresh Save can
    // cleanly reclaim the slot.
    MapSlotBlob probe;
    if (mp.isKey(nameKey) && mp.getBytes(blobKey, &probe, sizeof(probe)) == sizeof(probe))
    {
      String n = mp.getString(nameKey, "");
      strncpy(names[i], n.c_str(), MAP_NAME_MAX - 1);
      names[i][MAP_NAME_MAX - 1] = '\0';
    }
  }
  mp.end();
}

bool mapSlotLoad(uint8_t idx,
                 uint16_t outSpeed[speedArrayCount],
                 uint8_t outThrottle[throttleArrayCount],
                 uint8_t outLock[throttleArrayCount][speedArrayCount],
                 char outName[MAP_NAME_MAX])
{
  if (idx >= MAP_SLOT_COUNT)
    return false;

  Preferences mp;
  if (!mp.begin(MAP_NS, true))
    return false;

  char blobKey[4], nameKey[4];
  mapSlotKeys(idx, blobKey, nameKey);

  MapSlotBlob blob;
  bool ok = false;
  if (mp.isKey(blobKey) && mp.getBytes(blobKey, &blob, sizeof(blob)) == sizeof(blob))
  {
    memcpy(outSpeed, blob.speed, sizeof(blob.speed));
    memcpy(outThrottle, blob.throttle, sizeof(blob.throttle));
    memcpy(outLock, blob.lock, sizeof(blob.lock));
    String n = mp.getString(nameKey, "");
    strncpy(outName, n.c_str(), MAP_NAME_MAX - 1);
    outName[MAP_NAME_MAX - 1] = '\0';
    ok = outName[0] != '\0'; // an empty name means the slot was deleted
  }
  mp.end();
  return ok;
}

bool mapSlotSave(uint8_t idx, const char *name,
                 const uint16_t inSpeed[speedArrayCount],
                 const uint8_t inThrottle[throttleArrayCount],
                 const uint8_t inLock[throttleArrayCount][speedArrayCount])
{
  if (idx >= MAP_SLOT_COUNT || name == nullptr || name[0] == '\0')
    return false;

  MapSlotBlob blob;
  memcpy(blob.speed, inSpeed, sizeof(blob.speed));
  memcpy(blob.throttle, inThrottle, sizeof(blob.throttle));
  memcpy(blob.lock, inLock, sizeof(blob.lock));

  Preferences mp;
  if (!mp.begin(MAP_NS, false)) // read/write
    return false;

  char blobKey[4], nameKey[4];
  mapSlotKeys(idx, blobKey, nameKey);

  char safeName[MAP_NAME_MAX];
  strncpy(safeName, name, MAP_NAME_MAX - 1);
  safeName[MAP_NAME_MAX - 1] = '\0';

  bool ok = mp.putBytes(blobKey, &blob, sizeof(blob)) == sizeof(blob);
  if (ok)
    ok = mp.putString(nameKey, safeName) > 0;
  mp.end();
  return ok;
}

bool mapSlotDelete(uint8_t idx)
{
  if (idx >= MAP_SLOT_COUNT)
    return false;

  Preferences mp;
  if (!mp.begin(MAP_NS, false))
    return true; // namespace never existed - nothing to delete

  char blobKey[4], nameKey[4];
  mapSlotKeys(idx, blobKey, nameKey);
  if (mp.isKey(blobKey))
    mp.remove(blobKey);
  if (mp.isKey(nameKey))
    mp.remove(nameKey);
  mp.end();
  return true;
}
