#pragma once

#include <stdint.h>

// Pure, host-testable seam for the low-power wake decision used by the
// LP_WATCHING/LP_SLEEPING state machine in src/OpenHaldexC6_IO.cpp.
//
// Returns true when CAN traffic indicates the bus is active enough to keep
// the unit awake (or to wake it back up):
//   - Standalone: our own Haldex frame generator runs at a fixed 50 fps, so
//     the wake threshold is the fixed constant 50 fps on the Haldex fps count.
//   - OEM: the vehicle drives the chassis bus, so wake is gated on the
//     configurable chassis fps threshold (lpWakeThresholdFps, UI slider,
//     default 50).
//
// This is the single source of truth shared by updateTriggers() and the native
// Unity characterization test (test/test_lowpower) so the shipped wake logic
// cannot silently drift from a mirrored copy.
static inline bool lpCanActive(bool isStandalone,
                               uint32_t lpHaldexFps,
                               uint32_t lpChassisFps,
                               uint32_t lpWakeThresholdFps)
{
  return isStandalone ? (lpHaldexFps >= 50U)
                      : (lpChassisFps >= lpWakeThresholdFps);
}

// Pure, host-testable seams for the USB-host bench override.
//
// A USB host connected to the USB Serial/JTAG port means the box is sitting on
// the bench plugged into a PC (SOF packets present). In that case keep the WiFi
// AP up even with no CAN traffic and no WiFi clients, so the dashboard can be
// reached without dragging the box back out to the car. In the car the box is
// powered from the vehicle, not a USB host, so usbHostConnected is false and the
// normal low-power logic is unchanged.

// True when the box should drop into low-power WiFi shutdown: no WiFi clients,
// CAN idle, AND no USB host connected. A USB host holds the AP up.
static inline bool lpShouldSleep(bool noClients, bool canActive, bool usbHostConnected)
{
  return noClients && !canActive && !usbHostConnected;
}

// True when a sleeping box should wake and restore WiFi: CAN traffic returned,
// or a USB host appeared (someone plugged it into a PC to test on the bench).
static inline bool lpShouldWake(bool canActive, bool usbHostConnected)
{
  return canActive || usbHostConnected;
}

// Sleep auto-setup seams (see sleepCalTick in src/OpenHaldexC6_IO.cpp).
//
// "Parked" for the 15 min measurement: not moving, engine off (or no chassis
// bus to read it from) and the ignition (KL15) not on.
static inline bool sleepCalParked(uint16_t speed, uint16_t rpm, bool hasChassis, bool ignitionIsOn)
{
  return speed == 0 && (rpm == 0 || !hasChassis) && !ignitionIsOn;
}

// A second only counts toward the window while nobody is connected, the car is
// parked and real CAN has been seen this session (not a bench unit). Anything
// else restarts the window.
static inline bool sleepCalCounts(bool noClients, bool parked, bool everSawCAN)
{
  return noClients && parked && everSawCAN;
}

// Wake threshold from the parked-bus average: average + margin, rounded up to
// the 10 fps slider step, kept inside the slider range 100..2000.
static inline uint16_t sleepCalThreshold(uint16_t avgFps, uint16_t marginFps)
{
  uint32_t t = (uint32_t)avgFps + marginFps;
  t = ((t + 9U) / 10U) * 10U;
  if (t < 100U) t = 100U;
  if (t > 2000U) t = 2000U;
  return (uint16_t)t;
}
