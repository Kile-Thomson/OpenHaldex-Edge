#include <OpenHaldexC6_API.h>
#include <OpenHaldexC6_UDS.h>
#include <OpenHaldexC6_Calculations.h>
#include <OpenHaldexC6_WiFi.h>
#include <OpenHaldexC6_OTA.h> // requireOtaAuth

#include <cstring>

static int getCPUUsagePercent() // helper function to calculate CPU usage percentage based on FreeRTOS task run time stats
{
    static configRUN_TIME_COUNTER_TYPE previousTotalRuntime = 0;
    static configRUN_TIME_COUNTER_TYPE previousIdleRuntime = 0;

    const UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    if (taskCount == 0)
    {
        return -1;
    }

    TaskStatus_t *taskStats = static_cast<TaskStatus_t *>(malloc(taskCount * sizeof(TaskStatus_t)));
    if (taskStats == nullptr)
    {
        return -1;
    }

    configRUN_TIME_COUNTER_TYPE totalRuntime = 0;
    const UBaseType_t collectedTasks = uxTaskGetSystemState(taskStats, taskCount, &totalRuntime);

    if ((collectedTasks == 0) || (totalRuntime == 0))
    {
        free(taskStats);
        return -1;
    }

    configRUN_TIME_COUNTER_TYPE idleRuntime = 0;
    for (UBaseType_t i = 0; i < collectedTasks; i++)
    {
        if ((taskStats[i].pcTaskName != nullptr) && (strncmp(taskStats[i].pcTaskName, "IDLE", 4) == 0))
        {
            idleRuntime += taskStats[i].ulRunTimeCounter;
        }
    }

    free(taskStats);

    if ((previousTotalRuntime == 0) || (totalRuntime <= previousTotalRuntime) || (idleRuntime < previousIdleRuntime))
    {
        previousTotalRuntime = totalRuntime;
        previousIdleRuntime = idleRuntime;
        return -1;
    }

    const configRUN_TIME_COUNTER_TYPE totalRuntimeDelta = totalRuntime - previousTotalRuntime;
    const configRUN_TIME_COUNTER_TYPE idleRuntimeDelta = idleRuntime - previousIdleRuntime;

    previousTotalRuntime = totalRuntime;
    previousIdleRuntime = idleRuntime;

    if (totalRuntimeDelta == 0)
    {
        return -1;
    }

    float idlePercent = (static_cast<float>(idleRuntimeDelta) * 100.0f) / static_cast<float>(totalRuntimeDelta);
    idlePercent = constrain(idlePercent, 0.0f, 100.0f);

    const int cpuPercent = int(100.0f - idlePercent + 0.5f);
    return constrain(cpuPercent, 0, 100);
}

// helper function to send over JSON data to the ESP
static void sendJSON(AsyncWebServerRequest *request, int code, const JsonDocument &data)
{
    String out;
    serializeJson(data, out);
    request->send(code, "application/json", out);
}

// helper function to reserve space (and delete) for incoming data
static void parseJSON(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total, void (*done)(AsyncWebServerRequest *, const String &))
{
    if (index == 0)
    {
        if (total == 0) // empty body: nothing to buffer, let the handler send its 400
        {
            done(request, String());
            return;
        }
        // malloc'd (not new'd) because the request destructor releases _tempObject
        // with plain free() if the client aborts the POST mid-body
        request->_tempObject = malloc(total + 1);
        if (request->_tempObject == nullptr)
        {
            JsonDocument err;
            err["error"] = "Body buffer allocation failed";
            sendJSON(request, 500, err);
            return;
        }
        memset(request->_tempObject, 0, total + 1); // zero-fill so the terminator is always in place
    }
    else if (request->_tempObject == nullptr)
    {
        return; // allocation failed on chunk 0 (500 already sent) - skip the rest
    }

    if (index >= total) // never write past the buffer
    {
        return;
    }
    if (len > total - index)
    {
        len = total - index;
    }
    memcpy((char *)request->_tempObject + index, data, len); // append the incoming chunk

    if (index + len == total)
    {
        String bodyString((const char *)request->_tempObject); // copy out before freeing
        free(request->_tempObject);                            // plain free() matches the malloc above
        request->_tempObject = nullptr;                        // restore the sentinel for the body-lambda guards
        done(request, bodyString);                             // hand the complete body to the done callback
    }
}

// parse the current INCOMING request for status: WebServer asks, this delivers
static void statusOutgoing(AsyncWebServerRequest *request)
{
    JsonDocument data;
    const bool chassisOk = hasCANChassis;
    const bool haldexOk = hasCANHaldex;

    // snapshot the guarded values once so the JSON reflects one coherent moment
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const openhaldex_mode_t modeSnapshot = state.mode;
    const float lockTargetSnapshot = lock_target;
    const bool steerGainEnSnapshot = steeringGainEnabled;
    const uint16_t steerGainStartSnapshot = steeringGainStartDeg;
    const uint16_t steerGainFullSnapshot = steeringGainFullDeg;
    const uint8_t steerGainFloorSnapshot = steeringGainFloor;
    xSemaphoreGive(stateMutex);

    data["mode"] = modeSnapshot;

    // Ext-button force flag is driven by the external button
    data["extButtonActive"] = extButtonForceModeFlag;

    if (chassisOk) // if chassis CAN ok, set related values
    {
        data["speed"] = received_vehicle_speed;
        data["throttle"] = int(received_pedal_value);
        data["asrOn"] = !asrForceModeFlag;
        data["tcOn"] = !tcForceModeFlag;
        data["hazardActive"] = hazardForceModeFlag;
        data["rpm"] = received_vehicle_rpm;
        data["boost"] = received_vehicle_boost;
    }
    else // if chassis CAN not ok, set related values to null (displayed as "--" in the UI)
    {
        data["speed"] = nullptr;
        data["throttle"] = nullptr;
        data["asrOn"] = nullptr;
        data["tcOn"] = nullptr;
        data["hazardActive"] = nullptr;
        data["rpm"] = nullptr;
        data["boost"] = nullptr;
    }

    // Live steering angle plus the gain the taper is currently applying. Null
    // when the LWI_01 signal is stale or QBit-degraded - exactly the condition
    // under which getLockData leaves the gain at 100%.
    const bool steeringFresh = steeringAngleValid && ((millis() - lastSteeringResponse) < steeringTimeout);
    if (chassisOk && steeringFresh)
    {
        const float angleDeg = steeringAngleTenths / 10.0f;
        data["steeringAngle"] = steeringAngleNegative ? -angleDeg : angleDeg;
        data["steeringGainNow"] = steerGainEnSnapshot
                                      ? steering_gain_percent(steeringAngleTenths, steerGainStartSnapshot, steerGainFullSnapshot, steerGainFloorSnapshot)
                                      : 100;
    }
    else
    {
        data["steeringAngle"] = nullptr;
        data["steeringGainNow"] = nullptr;
    }

    data["brakeIn"] = brakeSignalActive;
    data["brakeOut"] = brakeActive;
    data["handbrakeIn"] = handbrakeSignalActive;
    data["handbrakeOut"] = handbrakeActive;

    // CAN-decoded brake / handbrake state (MQB has both; PQ has brake only -
    // PQ handbrake remains on the physical GPIO).
    if (chassisOk)
    {
        data["brakeFromCAN"] = brakeFromCAN;
        if (haldexGeneration == 50)
        {
            data["handbrakeFromCAN"] = handbrakeFromCAN;
        }
        else
        {
            data["handbrakeFromCAN"] = nullptr; // for non-MQB platforms, set to null (displayed as "--" in the UI) since we don't have this data from CAN
        }
    }
    else
    {
        data["brakeFromCAN"] = nullptr;     // if chassis CAN not ok, set to null (displayed as "--" in the UI)
        data["handbrakeFromCAN"] = nullptr; // if chassis CAN not ok, set to null (displayed as "--" in the UI)
    }

    data["lockTarget"] = int(lockTargetSnapshot);

    if (haldexOk) // if haldex CAN ok set related values
    {
        data["lockActual"] = received_haldex_engagement;
        data["haldexState"] = received_haldex_state;
        data["haldexEngagement"] = received_haldex_engagement;
        data["haldexEngagementRaw"] = received_haldex_engagement_raw;
        data["clutch1Report"] = received_report_clutch1;
        data["clutch2Report"] = received_report_clutch2;
        data["tempProtection"] = received_temp_protection;
        data["couplingOpen"] = received_coupling_open;
        data["speedLimit"] = received_speed_limit;

        if (haldexGeneration == 41)
        {
            JsonObject gen41 = data["gen41"].to<JsonObject>();
            JsonObject sec = gen41["secAxle"].to<JsonObject>();
            sec["statusRaw"] = received_sec_axle_status_raw;
            sec["torqueNm"] = received_sec_axle_torque_nm;
            sec["clutchState"] = received_sec_axle_clutch_state;
            sec["active"] = received_sec_axle_active;
            sec["fdcmHealthy"] = received_sec_axle_fdcm_healthy;
            sec["arc"] = received_sec_axle_arc;
            JsonObject rear = gen41["rearAxle"].to<JsonObject>();
            rear["statusFlags"] = received_rear_axle_status_flags;
            rear["metricA"] = received_rear_axle_metric_a;
            rear["metricB"] = received_rear_axle_metric_b;
            JsonObject hb = gen41["heartbeat"].to<JsonObject>();
            hb["aliveBus0"] = received_haldex_alive_bus0;
            hb["drivetrainOk"] = received_drivetrain_state_ok;
            hb["aliveBus0AgeMs"] = received_haldex_alive_bus0_ms ? (millis() - received_haldex_alive_bus0_ms) : 0;
            hb["drivetrainAgeMs"] = received_drivetrain_state_ms ? (millis() - received_drivetrain_state_ms) : 0;
        }
    }
    else // if haldex CAN not ok, set related values to null (displayed as "--" in the UI)
    {
        data["lockActual"] = nullptr;
        data["haldexState"] = nullptr;
        data["haldexEngagement"] = nullptr;
        data["haldexEngagementRaw"] = nullptr;
        data["clutch1Report"] = nullptr;
        data["clutch2Report"] = nullptr;
        data["tempProtection"] = nullptr;
        data["couplingOpen"] = nullptr;
        data["speedLimit"] = nullptr;
    }

    data["chassisCAN"] = chassisOk;
    data["haldexCAN"] = haldexOk;
    data["busFailure"] = isBusFailure;
    data["lastChassisMs"] = lastCANChassisTick > 0 ? (millis() - lastCANChassisTick) : 0;
    data["lastHaldexMs"] = lastCANHaldexTick > 0 ? (millis() - lastCANHaldexTick) : 0;

    if (haldexOk && udsMQBEnabled)
    {
        JsonObject uds = data["uds"].to<JsonObject>();
        uds["terminalVoltage"] = udsTerminalVoltage;
        uds["moduleTemp"] = udsModuleTemp;
        uds["clutchTemp"] = udsClutchTemp;
        uds["coolingFinTemp"] = udsCoolingFinTemp;
        uds["clutchCurrent"] = udsClutchCurrent;
        uds["clutchPWM"] = udsClutchPWM;
        uds["clutchVoltage"] = udsClutchVoltage;
        uds["blockagePct"] = udsBlockagePct;
    }

    data["uptimeMs"] = millis();
    data["freeHeap"] = ESP.getFreeHeap();
    data["lpChassisFrameCount"] = lpChassisFrameCount;
    data["lpHaldexFrameCount"] = lpHaldexFrameCount;

    const int cpuUsage = getCPUUsagePercent(); // calculate CPU usage percentage using FreeRTOS task run time stats
    if (cpuUsage >= 0)
    {
        data["cpuUsage"] = cpuUsage;
    }
    else // if CPU usage couldn't be calculated, set to "--"
    {
        data["cpuUsage"] = nullptr;
    }

    sendJSON(request, 200, data);
}

// parse the current INCOMING request for settings: WebServer asks, this delivers
static void settingsOutgoing(AsyncWebServerRequest *request)
{
    JsonDocument data;
    // values
    data["haldexGeneration"] = haldexGeneration;
    data["tcForceModeValue"] = tcForceModeValue;
    data["hazardForceModeValue"] = hazardForceModeValue;
    data["extBtnForceModeValue"] = extBtnForceModeValue;
    data["disengageUnderSpeed"] = disengageUnderSpeed;
    data["disengageAboveSpeed"] = disengageAboveSpeed;
    data["disableThrottle"] = disableThrottle;
    data["mode"] = lastMode;
    data["lockReleaseRatePerSec"] = lockReleaseRatePerSec;
    data["lockEngageRatePerSec"] = lockEngageRatePerSec;
    data["steeringGainStartDeg"] = steeringGainStartDeg;
    data["steeringGainFullDeg"] = steeringGainFullDeg;
    data["steeringGainFloor"] = steeringGainFloor;
    data["FW_VERSION"] = FW_VERSION;

    // bools
    data["lockReleaseEnabled"] = lockReleaseEnabled;
    data["steeringGainEnabled"] = steeringGainEnabled;
    data["disableController"] = disableController;
    data["isStandalone"] = isStandalone;
    data["useCANifAvailable"] = useCANifAvailable;
    data["tcForceMode"] = tcForceMode;
    data["extButtonForceMode"] = extBtnForceMode;
    data["hazardForceMode"] = hazardForceMode;

    data["disableOnboardButton"] = disableOnboardButton;
    data["disableExternalButton"] = disableExternalButton;
    data["fixHunting"] = fixHunting;
    data["canSleepEnabled"] = canSleepEnabled;
    data["canSleepAggressive"] = canSleepAggressive;
    data["lpWakeThresholdFps"] = lpWakeThresholdFps;

    data["analyzerMode"] = analyzerMode;
    data["analyzerSerial"] = analyzerSerial;
    data["udsMQBEnabled"] = udsMQBEnabled;

    data["followBrake"] = followBrake;
    data["invertBrake"] = invertBrake;
    data["followHandbrake"] = followHandbrake;
    data["invertHandbrake"] = invertHandbrake;

    data["broadcastOpenHaldexOverCAN"] = broadcastOpenHaldexOverCAN;

    data["ledBrightness"] = ledBrightness;

    // throttle/speed/lock array send - read under the lock so a tune being
    // published by /api/tune is never serialized half-old half-new
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    // row array
    JsonArray throttleArrayJSON = data["throttleArray"].to<JsonArray>();
    for (uint8_t i = 0; i < throttleArrayCount; i++)
    {
        throttleArrayJSON.add(throttleArray[i]);
    }

    // column array
    JsonArray speedArrayJSON = data["speedArray"].to<JsonArray>();
    for (uint8_t i = 0; i < speedArrayCount; i++)
    {
        speedArrayJSON.add(speedArray[i]);
    }

    // lock array
    JsonArray lockArrayJSON = data["lockArray"].to<JsonArray>();
    for (uint8_t throttlePos = 0; throttlePos < throttleArrayCount; throttlePos++)
    {
        JsonArray throttleRow = lockArrayJSON.add<JsonArray>();
        for (uint8_t speedPos = 0; speedPos < speedArrayCount; speedPos++)
        {
            throttleRow.add(lockArray[throttlePos][speedPos]);
        }
    }
    xSemaphoreGive(stateMutex);

    sendJSON(request, 200, data);
}

// manage settings (saved from Web, handled here): ESP sends, this handles
static void settingsIncoming(AsyncWebServerRequest *request, const String &body)
{
    JsonDocument data;
    if (deserializeJson(data, body) != DeserializationError::Ok)
    {
        DEBUG("Invalid JSON");
        return;
    }

    if (data["haldexGeneration"].is<uint8_t>())
    {
        int generation = data["haldexGeneration"];
        if (generation == 1 || generation == 2 || generation == 4 || generation == 50 || generation == 51 || generation == 41)
        {
            haldexGeneration = (uint8_t)generation;
            lastMode = generation;
        }
    }

    if (data["tcForceModeValue"].is<uint8_t>())
    {
        uint8_t v = data["tcForceModeValue"];
        if (v < 6)
            tcForceModeValue = v;
    }

    if (data["hazardForceModeValue"].is<uint8_t>())
    {
        uint8_t v = data["hazardForceModeValue"];
        if (v < 6)
            hazardForceModeValue = v;
    }

    if (data["extBtnForceModeValue"].is<uint8_t>())
    {
        uint8_t v = data["extBtnForceModeValue"];
        if (v < 6)
            extBtnForceModeValue = v;
    }

    if (data["disengageUnderSpeed"].is<uint16_t>())
    {
        uint16_t value = data["disengageUnderSpeed"];
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        disengageUnderSpeed = constrain(value, 0, 300);
        xSemaphoreGive(stateMutex);
    }

    if (data["disengageAboveSpeed"].is<uint16_t>())
    {
        uint16_t value = data["disengageAboveSpeed"];
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        disengageAboveSpeed = constrain(value, 0, 300);
        xSemaphoreGive(stateMutex);
    }

    if (data["disableThrottle"].is<uint8_t>())
    {
        uint8_t value = data["disableThrottle"];
        xSemaphoreTake(stateMutex, portMAX_DELAY); // both fields land together
        disableThrottle = constrain(value, 0, 100);
        state.pedal_threshold = disableThrottle;
        xSemaphoreGive(stateMutex);
    }

    // Lock-response rates: the UI has always POSTed these on save, but nothing
    // accepted them, so the sliders silently did nothing. Read on the hot path
    // inside getLockData's critical section, so write under the same lock.
    if (data["lockReleaseRatePerSec"].is<float>())
    {
        float value = data["lockReleaseRatePerSec"];
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        lockReleaseRatePerSec = constrain(value, 5.0f, 500.0f); // 0 would leave the lock unable to release
        xSemaphoreGive(stateMutex);
    }

    if (data["lockEngageRatePerSec"].is<float>())
    {
        float value = data["lockEngageRatePerSec"];
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        lockEngageRatePerSec = constrain(value, 0.0f, 500.0f); // 0 = instant lock-up
        xSemaphoreGive(stateMutex);
    }

    if (data["lockReleaseEnabled"].is<bool>())
    {
        lockReleaseEnabled = data["lockReleaseEnabled"];
    }

    // Steering-gain taper. Start/full are accepted independently (the UI saves
    // one key at a time); if a client leaves full <= start the taper degrades
    // to a hard step at the start angle (see steering_gain_percent), never a
    // divide-by-zero. Read on the hot path inside getLockData's critical
    // section, so write under the same lock.
    if (data["steeringGainStartDeg"].is<uint16_t>())
    {
        uint16_t value = data["steeringGainStartDeg"];
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        steeringGainStartDeg = constrain(value, 0, 720);
        xSemaphoreGive(stateMutex);
    }

    if (data["steeringGainFullDeg"].is<uint16_t>())
    {
        uint16_t value = data["steeringGainFullDeg"];
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        steeringGainFullDeg = constrain(value, 0, 720);
        xSemaphoreGive(stateMutex);
    }

    if (data["steeringGainFloor"].is<uint8_t>())
    {
        uint8_t value = data["steeringGainFloor"];
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        steeringGainFloor = constrain(value, 0, 100);
        xSemaphoreGive(stateMutex);
    }

    if (data["steeringGainEnabled"].is<bool>())
    {
        steeringGainEnabled = data["steeringGainEnabled"];
    }

    if (data["disableController"].is<bool>())
    {
        disableController = data["disableController"];
        if (disableController)
        {
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            state.mode = MODE_STOCK;
            lastMode = 0;
            xSemaphoreGive(stateMutex);
        }
        if (!disableController && analyzerMode)
        {
            setAnalyzerMode(false);
        }
    }

    if (data["isStandalone"].is<bool>())
    {
        isStandalone = data["isStandalone"];

        if (!isStandalone)
        {
            vTaskSuspend(handle_frames1000);
            vTaskSuspend(handle_frames200);
            vTaskSuspend(handle_frames100);
            vTaskSuspend(handle_frames25);
            vTaskSuspend(handle_frames20);
            vTaskSuspend(handle_frames10);
            vTaskSuspend(handle_frames13);
            vTaskSuspend(handle_frames50);
            vTaskSuspend(handle_frames250);
            vTaskSuspend(handle_gen41_dual_bus_rates);
        }
        else
        {
            vTaskResume(handle_frames1000);
            vTaskResume(handle_frames200);
            vTaskResume(handle_frames100);
            vTaskResume(handle_frames25);
            vTaskResume(handle_frames20);
            vTaskResume(handle_frames10);
            vTaskResume(handle_frames13);
            vTaskResume(handle_frames50);
            vTaskResume(handle_frames250);
            vTaskResume(handle_gen41_dual_bus_rates);
        }
    }

    if (data["analyzerMode"].is<bool>())
    {
        setAnalyzerMode(data["analyzerMode"]);
    }

    if (data["analyzerSerial"].is<bool>())
    {
        setAnalyzerSerialMode(data["analyzerSerial"]);
    }

    if (data["udsMQBEnabled"].is<bool>())
    {
        udsMQBEnabled = data["udsMQBEnabled"];
    }

    if (data["useCANifAvailable"].is<bool>())
    {
        useCANifAvailable = data["useCANifAvailable"];
    }

    if (data["tcForceMode"].is<bool>())
    {
        tcForceMode = data["tcForceMode"];
    }

    if (data["extButtonForceMode"].is<bool>())
    {
        extBtnForceMode = data["extButtonForceMode"];
    }

    if (data["hazardForceMode"].is<bool>())
    {
        hazardForceMode = data["hazardForceMode"];
        if (!hazardForceMode)
        {

            hazardForceModeFlag = false; // clear active flag when feature is disabled
        }
    }

    if (data["disableOnboardButton"].is<bool>())
    {
        disableOnboardButton = data["disableOnboardButton"];
    }

    if (data["disableExternalButton"].is<bool>())
    {
        disableExternalButton = data["disableExternalButton"];
    }

    if (data["fixHunting"].is<bool>())
    {
        fixHunting = data["fixHunting"];
    }

    if (data["canSleepEnabled"].is<bool>())
    {
        canSleepEnabled = data["canSleepEnabled"];
        // Aggressive depends on the sleep path (esp_pm_configure / light sleep).
        // Disabling the base feature should also disable aggressive mode
        if (!canSleepEnabled)
            canSleepAggressive = false;
    }
    if (data["canSleepAggressive"].is<bool>())
    {
        canSleepAggressive = data["canSleepAggressive"];
        // Enabling aggressive implies the base sleep path is on.
        if (canSleepAggressive)
            canSleepEnabled = true;
    }
    if (data["lpWakeThresholdFps"].is<uint16_t>())
    {
        lpWakeThresholdFps = constrain((uint16_t)data["lpWakeThresholdFps"], 0, 2000);
    }
    if (data["followBrake"].is<bool>())
    {
        followBrake = data["followBrake"];
    }

    if (data["invertBrake"].is<bool>())
    {
        invertBrake = data["invertBrake"];
    }

    if (data["followHandbrake"].is<bool>())
    {
        followHandbrake = data["followHandbrake"];
    }

    if (data["invertHandbrake"].is<bool>())
    {
        invertHandbrake = data["invertHandbrake"];
    }

    if (data["broadcastOpenHaldexOverCAN"].is<bool>())
    {
        broadcastOpenHaldexOverCAN = data["broadcastOpenHaldexOverCAN"];
    }

    if (data["ledBrightness"].is<int>())
    {
        ledBrightness = (uint8_t)constrain((int)data["ledBrightness"], 0, 255);
    }

    JsonDocument resp;
    resp["ok"] = true;
    sendJSON(request, 200, resp);
}

// manage mode (saved from Web, handled here): ESP sends, this handles
static void modeIncoming(AsyncWebServerRequest *request, const String &body)
{
    JsonDocument data;
    if (deserializeJson(data, body) != DeserializationError::Ok)
    {
        DEBUG("Invalid JSON");
        return;
    }

    if (data["mode"].is<uint8_t>())
    {
        if (!disableController)
        {
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            if (isStandalone && (openhaldex_mode_t)data["mode"] == 0)
            {
                state.mode = (openhaldex_mode_t)lastMode;
            }
            else
            {
                state.mode = (openhaldex_mode_t)data["mode"];
            }
            lastMode = state.mode;
            xSemaphoreGive(stateMutex);
        }
    }
}

// manage tune (saved from Web, handled here): ESP sends, this handles
static void tuneIncoming(AsyncWebServerRequest *request, const String &body)
{
    JsonDocument data;
    if (deserializeJson(data, body) != DeserializationError::Ok)
    {
        DEBUG("Invalid JSON");
        return;
    }

    JsonArray speedArrayJSON = data["speedArray"].as<JsonArray>();
    JsonArray throttleArrayJSON = data["throttleArray"].as<JsonArray>();
    JsonArray lockArrayJSON = data["lockArray"].as<JsonArray>();

    if (speedArrayJSON.size() != speedArrayCount || throttleArrayJSON.size() != throttleArrayCount)
    {
        DEBUG("Invalid Array Length");
        return;
    }

    // Parse and validate the whole tune into staging copies first, then publish
    // all three tables under one short hold - so getLockData's interpolation
    // never reads a half-updated map, and an invalid row leaves the live tables
    // untouched instead of partially overwritten
    uint8_t stagedThrottle[throttleArrayCount];
    uint16_t stagedSpeed[speedArrayCount];
    uint8_t stagedLock[throttleArrayCount][speedArrayCount];

    // fill throttle array
    for (uint8_t i = 0; i < throttleArrayCount; i++)
    {
        stagedThrottle[i] = (uint8_t)(throttleArrayJSON[i] | 0);
    }

    // fill speed array
    for (uint8_t i = 0; i < speedArrayCount; i++)
    {
        stagedSpeed[i] = (uint16_t)(speedArrayJSON[i] | 0);
    }

    // fill lock array
    for (uint8_t throttle = 0; throttle < throttleArrayCount; throttle++)
    {
        JsonArray throttleRow = lockArrayJSON[throttle].as<JsonArray>();
        if (throttleRow.size() != speedArrayCount)
        {
            DEBUG("Invalid lock array");
            return;
        }
        for (uint8_t speed = 0; speed < speedArrayCount; speed++)
        {
            stagedLock[throttle][speed] = (uint8_t)throttleRow[speed];
        }
    }

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    memcpy(throttleArray, stagedThrottle, sizeof(throttleArray));
    memcpy(speedArray, stagedSpeed, sizeof(speedArray));
    memcpy(lockArray, stagedLock, sizeof(lockArray));
    xSemaphoreGive(stateMutex);

    JsonDocument resp;
    resp["ok"] = true;
    sendJSON(request, 200, resp);
}

// setup webserver function
void setupWebServer()
{
    if (!LittleFS.begin(false))
    {
        DEBUG("LittleFS mount failed!"); // littleFS didn't mount
        // add a warning visual - flashing LED?
        return;
    }
    DEBUG("LittleFS mounted successfully");

    // When "/" or "/index.html" is requested and no credential is provisioned yet,
    // redirect to the first-run setup page. Both routes need the same guard because
    // serveStatic("/", ...) serves /index.html directly, bypassing the "/" handler.
    auto rootHandler = [](AsyncWebServerRequest *request) {
        if (!isOtaCredentialProvisioned()) {
            request->redirect("/setup");
            return;
        }
        request->send(LittleFS, "/index.html", "text/html");
    };
    webServer.on("/", HTTP_GET, rootHandler);
    webServer.on("/index.html", HTTP_GET, rootHandler);

    // Static assets only once provisioned; otherwise the filter drops the request
    // through to onNotFound (end of setupAPI), which redirects to /setup
    webServer.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setFilter([](AsyncWebServerRequest *request) -> bool {
            return isOtaCredentialProvisioned();
        });

    webServer.begin(); // begin the webServer
    DEBUG("Web server started");

    if (MDNS.begin("openhaldex"))
    {
        MDNS.addService("http", "tcp", 80);
        DEBUG("mDNS responder started: openhaldex.local");
    }
}

// setup main section for handling requests
void setupAPI()
{
    // ===== GET ENDPOINTS =====

    // GET /api/settings - retrieve all current settings
    webServer.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request)
                 { settingsOutgoing(request); });

    // GET /api/dashboard - retrieve live status data (polled regularly by JS)
    webServer.on("/api/dashboard", HTTP_GET, [](AsyncWebServerRequest *request)
                 { statusOutgoing(request); });

    // GET /api/uds/read - UDS read-by-identifier helper
    webServer.on("/api/uds/read", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
                     if (!requireOtaAuth(request)) return; // injects a UDS request frame onto CAN

                     auto reqP = request->getParam("req", false);
                     auto resP = request->getParam("res", false);
                     auto didP = request->getParam("did", false);

                     if (!reqP || !resP || !didP)
                     {
                         JsonDocument response;
                         response["error"] = "Parameters required: req, res, did";
                         sendJSON(request, 400, response);
                         return;
                     }

                     uint32_t requestId = strtoul(reqP->value().c_str(), nullptr, 16);
                     uint32_t responseId = strtoul(resP->value().c_str(), nullptr, 16);
                     uint16_t did = (uint16_t)strtoul(didP->value().c_str(), nullptr, 16);

                     OpenHaldexC6::UDS uds;
                     uint8_t buffer[256];
                     size_t bufferLen = sizeof(buffer);

                     if (!uds.readDataByIdentifier(requestId, responseId, did, buffer, bufferLen))
                     {
                         JsonDocument response;
                         response["success"] = false;
                         response["error"] = "UDS read failed";
                         sendJSON(request, 500, response);
                         return;
                     }

                     JsonDocument response;
                     response["success"] = true;
                     response["did"] = did;
                     JsonArray dataArray = response["data"].to<JsonArray>();
                     for (size_t i = 0; i < bufferLen; i++)
                         dataArray.add(buffer[i]);

                     sendJSON(request, 200, response); });

    // GET /api/learn/status - returns current learn progress and table (when valid)
    webServer.on("/api/learn/status", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
                     JsonDocument data;
                     data["active"]     = (bool)haldexLearnActive;
                     data["progress"]   = (uint8_t)haldexLearnStep;
                     data["currentCF"]  = (uint8_t)haldexLearnCF;
                     data["currentEng"] = received_haldex_engagement;
                     // read flag + table under the lock so a table mid-publish
                     // is never serialized half-old half-new
                     xSemaphoreTake(stateMutex, portMAX_DELAY);
                     data["tableValid"] = haldexLearnTableValid;
                     if (haldexLearnTableValid)
                     {
                         JsonArray table = data["table"].to<JsonArray>();
                         for (uint8_t i = 0; i <= 100; i++)
                             table.add(haldexLearnTable[i]);
                     }
                     xSemaphoreGive(stateMutex);
                     sendJSON(request, 200, data); });

    // ===== POST ENDPOINTS =====

    // POST /api/settings - save settings from web UI
    webServer.on(
        "/api/settings", HTTP_POST, [](AsyncWebServerRequest *request)
        { (void)request; }, nullptr,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
        {
            // The auth gate must live HERE in the body handler, not in onRequest:
            // ESPAsyncWebServer delivers the body first, so gating onRequest is too
            // late to stop the mutation. On refusal _tempObject stays null and the
            // trailing chunks of the rejected body no-op. Same pattern below.
            if (index == 0)
            {
                if (!requireOtaAuth(request)) return;
            }
            else if (request->_tempObject == nullptr)
            {
                return;
            }
            parseJSON(request, data, len, index, total, settingsIncoming);
        });

    // POST /api/mode - change operating mode
    webServer.on(
        "/api/mode", HTTP_POST, [](AsyncWebServerRequest *request)
        { (void)request; }, nullptr,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
        {
            if (index == 0) // auth in the body handler - see /api/settings above
            {
                if (!requireOtaAuth(request)) return;
            }
            else if (request->_tempObject == nullptr)
            {
                return;
            }
            parseJSON(request, data, len, index, total, modeIncoming);
        });

    // POST /api/tune - update throttle/speed/lock arrays
    webServer.on(
        "/api/tune", HTTP_POST, [](AsyncWebServerRequest *request)
        { (void)request; }, nullptr,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
        {
            if (index == 0) // auth in the body handler - see /api/settings above
            {
                if (!requireOtaAuth(request)) return;
            }
            else if (request->_tempObject == nullptr)
            {
                return;
            }
            parseJSON(request, data, len, index, total, tuneIncoming);
        });

    // POST /api/learn/start - begin the learn sweep
    webServer.on("/api/learn/start", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
                     if (!requireOtaAuth(request)) return; // the sweep drives live lock on the car

                     if (!hasCANHaldex)
                     {
                         JsonDocument resp;
                         resp["ok"]    = false;
                         resp["error"] = "No Haldex CAN data available";
                         sendJSON(request, 200, resp);
                         return;
                     }
                     startHaldexLearn();
                     JsonDocument resp;
                     resp["ok"] = true;
                     sendJSON(request, 200, resp); });

    // POST /api/learn/cancel - abort an in-progress learn sweep
    webServer.on("/api/learn/cancel", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
                     if (!requireOtaAuth(request)) return;
                     haldexLearnCancel = true;
                     JsonDocument resp;
                     resp["ok"] = true;
                     sendJSON(request, 200, resp); });

    // POST /api/learn/clear - discard the stored learn table and revert to formula
    webServer.on("/api/learn/clear", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
                     if (!requireOtaAuth(request)) return;
                     xSemaphoreTake(stateMutex, portMAX_DELAY);
                     haldexLearnTableValid = false;
                     memset(haldexLearnTable, 0, sizeof(haldexLearnTable));
                     xSemaphoreGive(stateMutex);
                     JsonDocument resp;
                     resp["ok"] = true;
                     sendJSON(request, 200, resp); });

    // NOTE: route registration order matters. ESPAsyncWebServer's URL matcher
    // accepts a registered "/api/wifi" handler for any URL that starts with
    // "/api/wifi/" (see WebHandlerImpl.h canHandle). The more-specific routes 
    // must be registered BEFORE "/api/wifi"
    // or POSTs to "/api/wifi/ssid" get missed 

    // POST /api/wifi/ssid/reset - restore factory SSID and restart AP
    webServer.on("/api/wifi/ssid/reset", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
                     if (!requireOtaAuth(request)) return;
                     resetWifiSsid();
                     JsonDocument resp;
                     resp["ok"] = true;
                     resp["ssid"] = wifiSsid;
                     sendJSON(request, 200, resp); });

    // GET /api/wifi/ssid - return current AP SSID
    webServer.on("/api/wifi/ssid", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
                     JsonDocument resp;
                     resp["ssid"] = wifiSsid;
                     resp["default"] = wifiHostNameDefault;
                     sendJSON(request, 200, resp); });

    // POST /api/wifi/ssid - change AP SSID; AP restarts immediately
    webServer.on(
        "/api/wifi/ssid", HTTP_POST, [](AsyncWebServerRequest *request)
        { (void)request; }, nullptr,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
        {
            if (index == 0) // auth in the body handler - see /api/settings above
            {
                if (!requireOtaAuth(request)) return;
            }
            else if (request->_tempObject == nullptr)
            {
                return;
            }
            parseJSON(request, data, len, index, total, [](AsyncWebServerRequest *req, const String &body)
                      {
                JsonDocument d;
                if (deserializeJson(d, body) != DeserializationError::Ok)
                {
                    JsonDocument resp; resp["ok"] = false; resp["error"] = "Invalid JSON";
                    sendJSON(req, 400, resp); return;
                }
                if (!d["ssid"].is<const char *>())
                {
                    JsonDocument resp; resp["ok"] = false; resp["error"] = "Missing 'ssid' field";
                    sendJSON(req, 400, resp); return;
                }
                const char *newSsid = d["ssid"];
                const size_t ssidLen = strlen(newSsid);
                if (ssidLen < 1)
                {
                    JsonDocument resp; resp["ok"] = false; resp["error"] = "SSID must be at least 1 character";
                    sendJSON(req, 400, resp); return;
                }
                if (ssidLen > 32)
                {
                    JsonDocument resp; resp["ok"] = false; resp["error"] = "SSID too long (max 32)";
                    sendJSON(req, 400, resp); return;
                }
                // basic printable-ASCII guard (reject control chars / non-ASCII to keep AP discoverable)
                for (size_t i = 0; i < ssidLen; ++i)
                {
                    unsigned char c = (unsigned char)newSsid[i];
                    if (c < 0x20 || c > 0x7E)
                    {
                        JsonDocument resp; resp["ok"] = false; resp["error"] = "SSID must be printable ASCII";
                        sendJSON(req, 400, resp); return;
                    }
                }
                memset(wifiSsid, 0, sizeof(wifiSsid));
                strncpy(wifiSsid, newSsid, sizeof(wifiSsid) - 1);
                rebootWiFi = true; // restart AP with new SSID
                JsonDocument resp;
                resp["ok"] = true;
                resp["ssid"] = wifiSsid;
                sendJSON(req, 200, resp); });
        });

    // POST /api/wifi/reset - clear password and restart AP as open network
    webServer.on("/api/wifi/reset", HTTP_POST, [](AsyncWebServerRequest *request)
                 {
                     if (!requireOtaAuth(request)) return; // clears the AP password, reopens the network
                     resetWifiPassword();
                     JsonDocument resp;
                     resp["ok"] = true;
                     sendJSON(request, 200, resp); });

    // GET /api/wifi - return whether a WiFi password is currently set
    webServer.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest *request)
                 {
                     JsonDocument resp;
                     resp["passwordSet"] = (strlen(wifiPassword) >= 8);
                     sendJSON(request, 200, resp); });

    // POST /api/wifi - set or clear WiFi password; AP restarts immediately
    webServer.on(
        "/api/wifi", HTTP_POST, [](AsyncWebServerRequest *request)
        { (void)request; }, nullptr,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
        {
            if (index == 0) // auth in the body handler - see /api/settings above
            {
                if (!requireOtaAuth(request)) return;
            }
            else if (request->_tempObject == nullptr)
            {
                return;
            }
            parseJSON(request, data, len, index, total, [](AsyncWebServerRequest *req, const String &body)
                      {
                JsonDocument d;
                if (deserializeJson(d, body) != DeserializationError::Ok)
                {
                    JsonDocument resp; resp["ok"] = false; resp["error"] = "Invalid JSON";
                    sendJSON(req, 400, resp); return;
                }
                if (!d["password"].is<const char *>())
                {
                    JsonDocument resp; resp["ok"] = false; resp["error"] = "Missing 'password' field";
                    sendJSON(req, 400, resp); return;
                }
                const char *newPwd = d["password"];
                const size_t pwdLen = strlen(newPwd);
                if (pwdLen > 0 && pwdLen < 8)
                {
                    JsonDocument resp; resp["ok"] = false; resp["error"] = "Password must be at least 8 characters or empty";
                    sendJSON(req, 400, resp); return;
                }
                if (pwdLen >= 65)
                {
                    JsonDocument resp; resp["ok"] = false; resp["error"] = "Password too long (max 64)";
                    sendJSON(req, 400, resp); return;
                }
                memset(wifiPassword, 0, sizeof(wifiPassword));
                if (pwdLen > 0) strncpy(wifiPassword, newPwd, sizeof(wifiPassword) - 1);
                rebootWiFi = true; // restart AP with new credentials
                JsonDocument resp;
                resp["ok"] = true;
                resp["passwordSet"] = (pwdLen >= 8);
                sendJSON(req, 200, resp); });
        });

    // While unprovisioned, every unmatched URL (including static assets blocked by
    // the serveStatic filter) redirects to the first-run setup page.
    webServer.onNotFound([](AsyncWebServerRequest *request) {
        if (!isOtaCredentialProvisioned()) {
            request->redirect("/setup");
            return;
        }
        request->send(404, "text/plain", "Not found");
    });
}
