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
