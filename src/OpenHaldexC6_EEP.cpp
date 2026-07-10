#include <OpenHaldexC6_EEP.h>          // include the header for EEPROM/preference functions
#include <OpenHaldexC6_Calculations.h> // for haldexLearnTable and learn globals

// Load every persisted setting from one open Preferences handle into the runtime
// globals, using the canonical keys. Shared by the normal-run LOAD path (source =
// the 'openhaldex' namespace) and the one-time legacy MIGRATE path (source = a
// read-only handle on the old de-facto namespace) so the two cannot drift apart.
// Does NOT touch the lastMode->state.mode mapping; the caller applies that once
// after the branch.
static void loadSettingsFrom(Preferences &src)
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
  bpkCeilingNm = src.getUShort("bpkCeilNm", 220);                        // load BPK full-lock torque ceiling (Nm)
  canSleepEnabled = src.getBool("canSleepEn", true);                     // load CAN-wake light sleep enable
  canSleepAggressive = src.getBool("canSleepAggr", false);               // load aggressive CAN sleep enable
  lpWakeThresholdFps = src.getUShort("lpWakeFps", 1100);                 // load LP wake threshold (fps)
  ledBrightness = src.getUChar("ledBrightness", led_brightness_default); // load LED brightness

  otaUpdate = src.getBool("otaUpdate", false);                          // load OTA update flag
  haldexGeneration = src.getUChar("haldexGen", 1);                      // load haldex generation with default
  if (haldexGeneration == 5) haldexGeneration = 50;                     // migrate legacy Gen5 -> Gen5 (0CQ)
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
  udsMQBEnabled = src.getBool("udsMQBEn", false);                   // load UDS MQB polling enable
  forceModesPriority = src.getUChar("forceModesPrio", 0);           // load force-modes priority (0=TC>Haz>Ext)
  // Lock ramp times moved from %/s to ms-for-full-travel. Prefer the new key;
  // if only the legacy %/s key is present (a device seeded on older firmware, or
  // a legacy-namespace migration), convert it once via lock_ramp_ms_from_rate.
  // The converted value is re-persisted on the next writeEEP under the ms key.
  if (src.isKey("lockReleaseMs"))
    lockReleaseRampMs = src.getUShort("lockReleaseMs", 500);        // load lock release ramp (ms)
  else
    lockReleaseRampMs = lock_ramp_ms_from_rate(src.getFloat("lockReleaseRate", 120.0f)); // migrate legacy %/s
  if (src.isKey("lockEngageMs"))
    lockEngageRampMs = src.getUShort("lockEngageMs", 0);            // load lock engage ramp (ms)
  else
    lockEngageRampMs = lock_ramp_ms_from_rate(src.getFloat("lockEngageRate", 0.0f));      // migrate legacy %/s
  lockReleaseEnabled = src.getBool("lockReleaseEn", true);          // load lock release enable
  steeringGainEnabled = src.getBool("steerGainEn", false);          // load steering-gain toggle
  steeringGainStartDeg = src.getUShort("steerGainStart", 45);       // load steering-gain start angle
  steeringGainFullDeg = src.getUShort("steerGainFull", 180);        // load steering-gain full angle
  steeringGainFloor = src.getUChar("steerGainFloor", 50);           // load steering-gain floor percent
}

// Persist every runtime-global setting into the canonical 'openhaldex' handle
// (`pref`), using the same keys/putters as writeEEP. Shared by the first-run SEED
// path (globals hold the compiled defaults) and the legacy MIGRATE path (globals
// were just populated from the legacy namespace) so the new namespace ends up
// equivalent to a steady-state writeEEP cycle.
static void persistSettingsToPref()
{
  pref.putBool("broadcastOpen", broadcastOpenHaldexOverCAN); // save broadcast setting
  pref.putBool("isStandalone", isStandalone);                // save standalone setting
  pref.putBool("useCANifAvail", useCANifAvailable);          // save use CAN if available setting
  pref.putBool("disableControl", disableController);         // save controller disable
  pref.putBool("followBrake", followBrake);                  // save follow brake
  pref.putBool("followHandbrake", followHandbrake);          // save follow handbrake
  pref.putBool("invertBrake", invertBrake);                  // save invert brake
  pref.putBool("invertHandbrake", invertHandbrake);          // save invert handbrake
  pref.putBool("tcForceMode", tcForceMode);                  // save tc force mode
  pref.putBool("extBtnForceMode", extBtnForceMode);          // save ext button force mode
  pref.putBool("hazardForceMode", hazardForceMode);          // save hazard force mode
  pref.putBool("dsbOnboardBtn", disableOnboardButton);       // save disable onboard button
  pref.putBool("dsbExtBtn", disableExternalButton);          // save disable external button
  pref.putBool("fixHunting", fixHunting);                    // save Motor_11 BPK-mode toggle
  pref.putUShort("bpkCeilNm", bpkCeilingNm);                 // save BPK full-lock torque ceiling (Nm)
  pref.putBool("canSleepEn", canSleepEnabled);               // save CAN-wake light sleep enable
  pref.putBool("canSleepAggr", canSleepAggressive);          // save aggressive CAN sleep enable
  pref.putUShort("lpWakeFps", lpWakeThresholdFps);           // save LP wake threshold (fps)
  pref.putUChar("ledBrightness", ledBrightness);             // save LED brightness

  pref.putBool("otaUpdate", otaUpdate);                                            // save OTA update flag
  pref.putUChar("haldexGen", haldexGeneration);                                    // save haldex generation
  pref.putUChar("tcFMV", tcForceModeValue);                                        // save TC force mode value
  pref.putUChar("hazFMV", hazardForceModeValue);                                   // save Hazard force mode value
  pref.putUChar("extFMV", extBtnForceModeValue);                                   // save ExtBtn force mode value
  pref.putUChar("lastMode", lastMode);                                             // save last used mode
  pref.putUChar("disableThrottle", disableThrottle);                               // save throttle disable
  pref.putUShort("disengageUSpeed", disengageUnderSpeed);                          // save disengage under speed
  pref.putUShort("disengageASpeed", disengageAboveSpeed);                          // save disengage above speed
  pref.putBytes("speedArray", (byte *)(&speedArray), sizeof(speedArray));          // save speed array bytes
  pref.putBytes("throttleArray", (byte *)(&throttleArray), sizeof(throttleArray)); // save throttle array bytes
  pref.putBytes("lockArray", (byte *)(&lockArray), sizeof(lockArray));             // save lock array bytes
  pref.putBool("learnOK", haldexLearnTableValid);                                  // save learn table valid flag
  if (haldexLearnTableValid)
  {
    pref.putBytes("learnTbl", haldexLearnTable, sizeof(haldexLearnTable)); // save learn table bytes
  }
  pref.putString("wifiPwd", wifiPassword);                 // save WiFi password (empty = open network)
  pref.putString("wifiSsid", wifiSsid);                    // save WiFi SSID
  pref.putBool("udsMQBEn", udsMQBEnabled);                 // save UDS MQB polling enable
  pref.putUChar("forceModesPrio", forceModesPriority);     // save force-modes priority order
  pref.putUShort("lockReleaseMs", lockReleaseRampMs); // save lock release ramp (ms)
  pref.putUShort("lockEngageMs", lockEngageRampMs);   // save lock engage ramp (ms)
  pref.putBool("lockReleaseEn", lockReleaseEnabled);       // save lock release enable
  pref.putBool("steerGainEn", steeringGainEnabled);        // save steering-gain toggle
  pref.putUShort("steerGainStart", steeringGainStartDeg);  // save steering-gain start angle
  pref.putUShort("steerGainFull", steeringGainFullDeg);    // save steering-gain full angle
  pref.putUChar("steerGainFloor", steeringGainFloor);      // save steering-gain floor percent
}

void readEEP() // function to read stored preferences into runtime variables
{              // start readEEP
#if detailedDebugEEP
  DEBUG("EEPROM initialising!"); // debug: EEPROM init start
#endif

  // Single NVS namespace + seeded sentinel + legacy migration.
  // One namespace for everything: the old code called pref.begin() once per
  // setting NAME, so only the last begin() won and every key de-facto lived in
  // that namespace. Worse, first-run detection read a key ("haldexGeneration")
  // that was never written, so the seed branch never ran. `pref` stays open on
  // "openhaldex" for the lifetime of the program; writeEEP reuses it without
  // re-opening.
  if (!pref.begin("openhaldex", false)) // open the one canonical namespace (read/write)
  {
    // NVS partition inaccessible (corrupt or full): boot on in-memory defaults.
    DEBUG("[EEP] pref.begin failed - booting on defaults");
    return;
  }
  bool seeded = pref.isKey("seeded"); // first-run sentinel (a missing key => not seeded)

  // Read-only peek at the de-facto legacy namespace through a SEPARATE handle so
  // we never disturb (or hold open) the old namespace. With this file's original
  // begin() list the last call won, so prior installs of this firmware stored
  // everything under "udsMQBEn"; still-older builds ended the list differently
  // (e.g. "learnTable"), so both candidates are checked. "haldexGen" is written
  // by every prior firmware, so its presence marks a real install.
  Preferences legacy;
  bool legacyOpen = legacy.begin("udsMQBEn", true); // read-only
  bool legacyHas = legacyOpen && legacy.isKey("haldexGen");
  if (!legacyHas)
  {
    if (legacyOpen)
    {
      legacy.end();
    }
    legacyOpen = legacy.begin("learnTable", true); // read-only, older builds
    legacyHas = legacyOpen && legacy.isKey("haldexGen");
  }

  switch (eeprom_init_action(seeded, legacyHas)) // single LOAD/MIGRATE/SEED decision point
  {
  case EEP_LOAD_EXISTING: // normal run: load stored values from the canonical namespace
    loadSettingsFrom(pref);
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

  // Map the (now-populated) lastMode to the runtime enum for every path, so a
  // freshly seeded or migrated device boots into a coherent mode just like a load.
  switch (lastMode) // map stored lastMode to runtime enum
  {
  case 0:
    state.mode = MODE_STOCK;  // set MODE_STOCK
    break;
  case 1:
    state.mode = MODE_FWD;    // set MODE_FWD
    break;
  case 2:
    state.mode = MODE_5050;   // set MODE_5050
    break;
  case 3:
    state.mode = MODE_6040;   // set MODE_6040
    break;
  case 4:
    state.mode = MODE_7525;   // set MODE_7525
    break;
  case 5:
    state.mode = MODE_EXPERT; // set MODE_EXPERT
    break;
  default:                    // unrecognized value
    state.mode = MODE_FWD;    // default to MODE_FWD
    break;
  }

#if detailedDebugEEP
  DEBUG("EEPROM initialised with...");                                                           // debug: print loaded prefs
  DEBUG("    Broadcast OpenHaldex over CAN: %s", broadcastOpenHaldexOverCAN ? "true" : "false"); // debug broadcast
  DEBUG("    Standalone mode: %s", isStandalone ? "true" : "false");                             // debug standalone
  DEBUG("    Follow handbrake: %s", followHandbrake ? "true" : "false");                         // debug handbrake
  DEBUG("    Follow brake: %s", followBrake ? "true" : "false");                                 // debug brake
  DEBUG("    Invert handbrake: %s", invertHandbrake ? "true" : "false");                         // debug invert handbrake
  DEBUG("    Invert brake: %s", invertBrake ? "true" : "false");                                 // debug invert brake
  DEBUG("    tcForceMode: %s", tcForceMode ? "true" : "false");                                  // debug tc force mode
  DEBUG("    extBtnForceMode: %s", extBtnForceMode ? "true" : "false");                          // debug ext btn force

  DEBUG("    Haldex Generation: %d", haldexGeneration); // debug haldex gen
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

    // Snapshot the multi-field shared structures under a short hold, then run
    // the slow NVS writes from the copies - so a persisted config is never an
    // expert/learn table caught mid-update. Single-word settings are written
    // directly below.
    static uint8_t snapSpeed[sizeof(speedArray)];
    static uint8_t snapThrottle[sizeof(throttleArray)];
    static uint8_t snapLock[sizeof(lockArray)];
    static uint8_t snapLearn[sizeof(haldexLearnTable)];
    bool snapLearnValid;
    uint8_t snapDisableThrottle;
    uint16_t snapDisengageUnder;
    uint16_t snapDisengageAbove;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    memcpy(snapSpeed, speedArray, sizeof(speedArray));
    memcpy(snapThrottle, throttleArray, sizeof(throttleArray));
    memcpy(snapLock, lockArray, sizeof(lockArray));
    memcpy(snapLearn, haldexLearnTable, sizeof(haldexLearnTable));
    snapLearnValid = haldexLearnTableValid;
    snapDisableThrottle = disableThrottle;
    snapDisengageUnder = disengageUnderSpeed;
    snapDisengageAbove = disengageAboveSpeed;
    xSemaphoreGive(stateMutex);

    // update EEP only if changes have been made
    pref.putBool("broadcastOpen", broadcastOpenHaldexOverCAN); // write broadcast setting
    pref.putBool("isStandalone", isStandalone);                // write standalone setting
    pref.putBool("useCANifAvail", useCANifAvailable);          // write use CAN if available setting
    pref.putBool("disableControl", disableController);         // write controller disable
    pref.putBool("followBrake", followBrake);                  // write follow brake
    pref.putBool("followHandbrake", followHandbrake);          // write follow handbrake
    pref.putBool("invertBrake", invertBrake);                  // write invert brake
    pref.putBool("invertHandbrake", invertHandbrake);          // write invert handbrake
    pref.putBool("tcForceMode", tcForceMode);                  // write tc force mode
    pref.putBool("extBtnForceMode", extBtnForceMode);          // write ext button force mode
    pref.putBool("hazardForceMode", hazardForceMode);          // write hazard force mode
    pref.putBool("dsbOnboardBtn", disableOnboardButton);       // write disable onboard button
    pref.putBool("dsbExtBtn", disableExternalButton);          // write disable external button
    pref.putBool("fixHunting", fixHunting);                    // write Motor_11 BPK-mode toggle
    pref.putUShort("bpkCeilNm", bpkCeilingNm);                 // write BPK full-lock torque ceiling (Nm)
    pref.putBool("canSleepEn", canSleepEnabled);               // write CAN-wake light sleep enable
    pref.putBool("canSleepAggr", canSleepAggressive);          // write aggressive CAN sleep enable
    pref.putUShort("lpWakeFps", lpWakeThresholdFps);           // write LP wake threshold (fps)
    pref.putUChar("ledBrightness", ledBrightness);             // write LED brightness

    pref.putUChar("haldexGen", haldexGeneration);                                    // write haldex generation
    pref.putUChar("tcFMV", tcForceModeValue);                                        // write TC force mode value
    pref.putUChar("hazFMV", hazardForceModeValue);                                   // write Hazard force mode value
    pref.putUChar("extFMV", extBtnForceModeValue);                                   // write ExtBtn force mode value
    pref.putUChar("lastMode", lastMode);                                             // write last mode
    pref.putUChar("disableThrottle", snapDisableThrottle);                     // write throttle disable
    pref.putUShort("disengageUSpeed", snapDisengageUnder);                     // write disengage under speed
    pref.putUShort("disengageASpeed", snapDisengageAbove);                     // write disengage above speed
    pref.putBytes("speedArray", snapSpeed, sizeof(snapSpeed));                 // write speed array
    pref.putBytes("throttleArray", snapThrottle, sizeof(snapThrottle));        // write throttle array
    pref.putBytes("lockArray", snapLock, sizeof(snapLock));                    // write lock array
    pref.putBool("learnOK", snapLearnValid);                                   // write learn valid flag
    if (snapLearnValid)
    {
      pref.putBytes("learnTbl", snapLearn, sizeof(snapLearn)); // write learn table bytes
    }
    pref.putString("wifiSsid", wifiSsid);    // write WiFi AP SSID
    pref.putString("wifiPwd", wifiPassword); // write WiFi AP password
    pref.putBool("udsMQBEn", udsMQBEnabled); // write UDS MQB polling enable
    pref.putUChar("forceModesPrio", forceModesPriority);      // write force-modes priority order
    pref.putUShort("lockReleaseMs", lockReleaseRampMs);  // write lock release ramp (ms)
    pref.putUShort("lockEngageMs", lockEngageRampMs);    // write lock engage ramp (ms)
    pref.putBool("lockReleaseEn", lockReleaseEnabled);        // write lock release enable
    pref.putBool("steerGainEn", steeringGainEnabled);         // write steering-gain toggle
    pref.putUShort("steerGainStart", steeringGainStartDeg);   // write steering-gain start angle
    pref.putUShort("steerGainFull", steeringGainFullDeg);     // write steering-gain full angle
    pref.putUChar("steerGainFloor", steeringGainFloor);       // write steering-gain floor percent

#if detailedDebugEEP
    DEBUG("Written EEPROM with data:");                                                            // debug: print written prefs
    DEBUG("    Broadcast OpenHaldex over CAN: %s", broadcastOpenHaldexOverCAN ? "true" : "false"); // debug broadcast
    DEBUG("    Standalone mode: %s", isStandalone ? "true" : "false");                             // debug standalone
    DEBUG("    Follow handbrake: %s", followHandbrake ? "true" : "false");                         // debug handbrake
    DEBUG("    Follow brake: %s", followBrake ? "true" : "false");                                 // debug brake
    DEBUG("    Invert handbrake: %s", invertHandbrake ? "true" : "false");                         // debug invert handbrake
    DEBUG("    Invert brake: %s", invertBrake ? "true" : "false");                                 // debug invert brake
    DEBUG("    Haldex Generation: %d", haldexGeneration);                                          // debug haldex gen
    DEBUG("    tcForceMode: %s", tcForceMode ? "true" : "false");                                  // debug tc force mode
    DEBUG("    extBtnForceMode: %s", extBtnForceMode ? "true" : "false");                          // debug ext btn force
    DEBUG("    Force Mode TC/Haz/Ext: %d/%d/%d", tcForceModeValue, hazardForceModeValue, extBtnForceModeValue);
    DEBUG("    Last Mode: %d", lastMode);                      // debug last mode
    DEBUG("    Disable Below Throttle: %d", disableThrottle);  // debug disable throttle
    DEBUG("    Disable Under Speed: %d", disengageUnderSpeed); // debug disengage under speed
    DEBUG("    Disable Above Speed: %d", disengageAboveSpeed); // debug disengage above speed
#endif

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
