#include <OpenHaldexC6_tasks.h>
#include <OpenHaldexC6_IO.h>
#include <OpenHaldexC6_can.h>
#include <OpenHaldexC6_EEP.h>
#include <OpenHaldexC6_Analyzer.h>
#include <OpenHaldexC6_StandaloneCAN.h>
#include <OpenHaldexC6_Calculations.h>
#include <OpenHaldexC6_UDS.h>

void haldexLearnTask(void *arg)
{
  const uint32_t settleMs = 300;
  uint8_t lastValid = 0;

  for (uint16_t cf = 0; cf <= 100; cf++)
  {
    if (haldexLearnCancel)
    {
      break;
    }

    haldexLearnStep = (uint8_t)cf;
    haldexLearnCF   = (uint8_t)cf;

    vTaskDelay(settleMs / portTICK_PERIOD_MS);

    uint8_t eng = received_haldex_engagement;

    if (eng == 0 && cf > 0)
    {
      eng = lastValid; // glaze over zero - keep previous valid reading
    }
    else
    {
      lastValid = eng;
    }

    haldexLearnTable[cf] = eng;
  }

  if (!haldexLearnCancel)
  {
    // only mark valid if at least one non-zero engagement was recorded
    bool anyNonZero = false;
    for (uint8_t i = 0; i <= 100; i++)
    {
      if (haldexLearnTable[i] > 0) { anyNonZero = true; break; }
    }
    haldexLearnTableValid = anyNonZero;
    haldexLearnStep = anyNonZero ? 101 : 102; // 101 = complete OK, 102 = complete but no data
  }

  haldexLearnActive = false;
  haldexLearnCF     = 0;
  vTaskDelete(NULL);
}

void setupTasks()
{
  // max task priority = 24
  xTaskCreate(showHaldexState, "showHaldexState", 5000, NULL, 1, &handle_showHaldexState);
  xTaskCreate(writeEEP, "writeEEP", 2000, NULL, 3, NULL);
  xTaskCreate(updateTriggers, "updateTriggers", 2000, NULL, 4, &handle_updateTriggers);

  // Analyzer task stays idle unless analyzerMode is enabled.
  setupAnalyzer();

  // Create tasks for frame generation at various intervals. These will run in the background and can be suspended when not in use (e.g., when not in standalone mode).
  xTaskCreate(frames1000, "frames1000", 8000, NULL, 5, &handle_frames1000);
  xTaskCreate(frames200, "frames200", 8000, NULL, 6, &handle_frames200);
  xTaskCreate(frames100, "frames100", 8000, NULL, 7, &handle_frames100);
  xTaskCreate(frames25, "frames25", 8000, NULL, 8, &handle_frames25);
  xTaskCreate(frames20, "frames20", 8000, NULL, 9, &handle_frames20);
  xTaskCreate(frames10, "frames10", 8000, NULL, 10, &handle_frames10);
  xTaskCreate(frames13, "frames13", 4000, NULL, 10, &handle_frames13);
  xTaskCreate(frames50, "frames50", 4000, NULL, 8, &handle_frames50);
  xTaskCreate(frames250, "frames250", 4000, NULL, 5, &handle_frames250);
  xTaskCreate(gen41DualBusRatesTask, "gen41DualBusRates", 4000, NULL, 10, &handle_gen41_dual_bus_rates);

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

  xTaskCreate(broadcastOpenHaldex, "broadcastOpenHaldex", 1000, NULL, 10, &handle_broadcastOpenHaldex); // create a task for FreeRTOS for broadcasting the haldex state
  xTaskCreate(parseCAN_hdx, "parseHaldex", 2048, NULL, 11, NULL);                // create a task for FreeRTOS for incoming haldex CAN - in '_can.ino'
  xTaskCreate(parseCAN_chs, "parseChassis", 2048, NULL, 12, NULL);               // create a task for FreeRTOS for incoming chassis CAN - in '_can.ino'
  xTaskCreate(udsMQBTask, "udsMQBTask", 2048, NULL, 5, NULL);                    // UDS MQB diagnostic polling task (Gen 5 only)
}

void showHaldexState(void *arg)
{
  while (1)
  {
    stackshowHaldexState = uxTaskGetStackHighWaterMark(NULL);

    DEBUG("Mode: %s", get_openhaldex_mode_string(state.mode));
    DEBUG("    Req:Act: %d:%d", int(lock_target), received_haldex_engagement); // this is the lock %

    if (detailedDebug)
    {
      DEBUG("    Raw haldexState: " BYTE_TO_BINARY_PATTERN, BYTE_TO_BINARY(received_haldex_state));
      DEBUG("    reportClutch1: %d", received_report_clutch1); // this means it has a clutch issue
      DEBUG("    reportClutch2: %d", received_report_clutch2); // this means it also has a clutch issue
      DEBUG("    couplingOpen: %d", received_coupling_open);   // clutch fully disengaged
      DEBUG("    speedLimit: %d", received_speed_limit);       // hit a speed limit...
      DEBUG("    tempCounter2: %d", tempCounter2);    // incrememting value for checking the response to vars...

      DEBUG("    hasChassisCAN: %d", hasCANChassis);         // incrememting value for checking the response to vars...
      DEBUG("    hasHaldexCAN: %d", hasCANHaldex);           // incrememting value for checking the response to vars...
      DEBUG("    lastCANChassis: %ldms", lastCANChassisTick > 0 ? (long)(millis() - lastCANChassisTick) : -1);   // incrememting value for checking the response to vars...
      DEBUG("    lastHaldex: %ldms", lastCANHaldexTick > 0 ? (long)(millis() - lastCANHaldexTick) : -1); // incrememting value for checking the response to vars...

      DEBUG("    currentMode: %d", lastMode);       // incrememting value for checking the response to vars...
      DEBUG("    isStandalone: %d", isStandalone);  // incrememting value for checking the response to vars...
      DEBUG("    haldexGen: %d", haldexGeneration); // incrememting value for checking the response to vars...
      DEBUG("    Analyser Mode: %d", analyzerMode);
      DEBUG("    Force Mode Value TC/Haz/Ext: %d/%d/%d", tcForceModeValue, hazardForceModeValue, extBtnForceModeValue);

      DEBUG("Free heap: %d bytes", ESP.getFreeHeap());        // for debug - checking ESP available space
      DEBUG("Min free heap: %d bytes", ESP.getMinFreeHeap()); // for debug - checking ESP available space

      if (isBusFailure)
      {
        DEBUG("    Bus Failure: True");
      }
    }

    if (detailedDebugIO)
    {
      DEBUG("    Brake signal: %s", brakeSignalActive ? "true" : "false");
      DEBUG("    Handbrake signal: %s", handbrakeSignalActive ? "true" : "false");

      DEBUG("    Follow brake: %s", followBrake ? "true" : "false");
      DEBUG("    Invert handbrake: %s", invertHandbrake ? "true" : "false");
      DEBUG("    Invert brake: %s", invertBrake ? "true" : "false");
    }

    if (detailedDebugArray)
    {
      DEBUG("%d%d%d%d%d%d", throttleArray[0], throttleArray[1], throttleArray[2], throttleArray[3], throttleArray[4], throttleArray[5], throttleArray[6]);
      DEBUG("%d%d%d%d%d%d", speedArray[0], speedArray[1], speedArray[2], speedArray[3], speedArray[4], speedArray[5], speedArray[6]);
      // DEBUG("%d", speedArray[j]);
      // DEBUG("%d%d%d%d%d%d", lockArray[i][0], lockArray[i][1], lockArray[i][2], lockArray[i][3], lockArray[i][4], lockArray[i][5], lockArray[i][6]);
    }

    if (detailedDebugStack)
    {
      DEBUG("Stack Sizes:");

      DEBUG("    stackCHS: %d", stackCHS); // incrememting value for checking the response to vars...
      DEBUG("    stackHDX: %d", stackHDX); // incrememting value for checking the response to vars...

      DEBUG("    stackframes10: %d", stackframes10);     // incrememting value for checking the response to vars...
      DEBUG("    stackframes20: %d", stackframes20);     // incrememting value for checking the response to vars...
      DEBUG("    stackframes25: %d", stackframes25);     // incrememting value for checking the response to vars...
      DEBUG("    stackframes100: %d", stackframes100);   // incrememting value for checking the response to vars...
      DEBUG("    stackframes200: %d", stackframes200);   // incrememting value for checking the response to vars...
      DEBUG("    stackframes1000: %d", stackframes1000); // incrememting value for checking the response to vars...
      DEBUG("    stackframes13: %d", stackframes13);     // incrememting value for checking the response to vars...
      DEBUG("    stackframes50: %d", stackframes50);     // incrememting value for checking the response to vars...
      DEBUG("    stackframes250: %d", stackframes250);   // incrememting value for checking the response to vars...

      DEBUG("    stackbroadcastOpenHaldex: %d", stackbroadcastOpenHaldex); // incrememting value for checking the response to vars...
      DEBUG("    stackupdateLabels: %d", stackupdateLabels);               // incrememting value for checking the response to vars...
      DEBUG("    stackshowHaldexState: %d", stackshowHaldexState);         // incrememting value for checking the response to vars...
      DEBUG("    stackwriteEEP: %d", stackwriteEEP);                       // incrememting value for checking the response to vars...
    }

    if (detailedDebugRuntimeStats)
    {
      char buffer2[2048] = {0};
      vTaskGetRunTimeStats(buffer2);
      Serial.println(buffer2);
    }

    // Bus-health detection on BOTH buses. This used to live inside
    // if(detailedDebugCAN) (off by default) with the legacy single-bus alert read,
    // so it was effectively dead. A failure latches per bus; IO.cpp keeps issuing
    // twai_initiate_recovery_v2 while isBusFailure is set. When BUS_RECOVERED
    // fires the bus is restarted with twai_start_v2 and its latch clears - but
    // only if the same read shows no fresh failure bit, so a new fault is never
    // masked by a stale recovery event.
    uint32_t alerts_bus0 = 0;
    uint32_t alerts_bus1 = 0;
    twai_read_alerts_v2(twai_bus_0, &alerts_bus0, pdMS_TO_TICKS(0));
    twai_read_alerts_v2(twai_bus_1, &alerts_bus1, pdMS_TO_TICKS(0));
    bool failure0 = (alerts_bus0 & CAN_ALERTS_FAILURE_MASK) != 0;
    bool failure1 = (alerts_bus1 & CAN_ALERTS_FAILURE_MASK) != 0;
    static bool busFailure0 = false; // per-bus latches: a bus stays failed until it recovers,
    static bool busFailure1 = false; // so one bus recovering can't clear the other's failure
    if (failure0) { busFailure0 = true; }
    if (failure1) { busFailure1 = true; }
    if (busFailure0 || busFailure1)
    {
      isBusFailure = true;
    }

    bool recovered0 = (alerts_bus0 & TWAI_ALERT_BUS_RECOVERED) != 0;
    bool recovered1 = (alerts_bus1 & TWAI_ALERT_BUS_RECOVERED) != 0;
    bool restarted0 = recovered0 && !failure0 && (twai_start_v2(twai_bus_0) == ESP_OK);
    bool restarted1 = recovered1 && !failure1 && (twai_start_v2(twai_bus_1) == ESP_OK);
    if (restarted0) { busFailure0 = false; }
    if (restarted1) { busFailure1 = false; }
    isBusFailure = busFailure0 || busFailure1;

    if (detailedDebugCAN)
    {
      twai_status_info_t twaistatus;
      twai_get_status_info_v2(twai_bus_0, &twaistatus);
      DEBUG("");
      DEBUG("CAN-BUS Details:");
      DEBUG("    RX buffered: %lu\t", twaistatus.msgs_to_rx);
      DEBUG("    RX missed: %lu\t", twaistatus.rx_missed_count);
      DEBUG("    RX overrun %lu\n", twaistatus.rx_overrun_count);
    }

    vTaskDelay(serialMonitorRefresh / portTICK_PERIOD_MS);
  }
}
