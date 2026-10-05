#include <OpenHaldexC6_Calculations.h>
#include <OpenHaldexC6_tasks.h>
#include <math.h> // tanf/sqrtf/fabsf for the per-corner slip geometry
#include <cstdlib> // malloc/free for http_body_alloc
#include <cstring> // memset/memcpy for the request-body buffer helpers
#include <cstdint> // SIZE_MAX for the overflow guard in http_body_alloc

// Geometry-compensated per-corner slip. Adopted from OpenHaldex-Edge by Rekt
// (Kile Thomson) - https://github.com/Kile-Thomson/OpenHaldex-Edge - see
// THIRD_PARTY_NOTICES.md. At steering road-wheel angle delta,
// pure Ackermann puts the turn centre on the rear-axle line R = wheelbase /
// tan(delta) from the centreline. Each wheel traces its own radius, so with
// zero real slip the wheel speeds are proportional to those radii. We build the
// four geometric radii, turn them into expected speeds sharing the measured
// mean, and report each corner's fractional excess as slip. On a straight this
// reduces to "speed vs. the average of the others" with no special-casing.
bool compute_corner_slip(const uint16_t wheel_raw[4], int16_t steer_wheel_tenths,
                         float steering_ratio, uint16_t wheelbase_mm,
                         uint16_t track_front_mm, uint16_t track_rear_mm,
                         uint16_t min_speed_raw, int8_t slip_out[4])
{
  slip_out[0] = slip_out[1] = slip_out[2] = slip_out[3] = 0;

  if (wheelbase_mm == 0 || track_front_mm == 0 || track_rear_mm == 0 ||
      steering_ratio < 1.0f)
    return false;

  const uint32_t sum_raw =
      (uint32_t)wheel_raw[0] + wheel_raw[1] + wheel_raw[2] + wheel_raw[3];
  const float mean_actual = sum_raw * 0.25f;
  if (mean_actual < (float)min_speed_raw || mean_actual <= 0.0f)
    return false;

  const float L = (float)wheelbase_mm;
  const float half_tf = (float)track_front_mm * 0.5f;
  const float half_tr = (float)track_rear_mm * 0.5f;

  const float delta_deg = ((float)steer_wheel_tenths * 0.1f) / steering_ratio;
  const float delta = fabsf(delta_deg) * (float)M_PI / 180.0f;

  float rad[4]; // [FL, FR, RL, RR]
  const float kStraight = 0.0005f;
  if (delta < kStraight)
  {
    rad[0] = rad[1] = rad[2] = rad[3] = 1.0f;
  }
  else
  {
    float R = L / tanf(delta);
    const float min_R = (half_tf > half_tr ? half_tf : half_tr) + 1.0f;
    if (R < min_R)
      R = min_R;

    const float r_rear_inner = R - half_tr;
    const float r_rear_outer = R + half_tr;
    const float r_front_inner = sqrtf(L * L + (R - half_tf) * (R - half_tf));
    const float r_front_outer = sqrtf(L * L + (R + half_tf) * (R + half_tf));

    if (delta_deg > 0.0f)
    {
      // Turning right: right-side wheels are inner (shorter radius, slower).
      rad[0] = r_front_outer; // FL
      rad[1] = r_front_inner; // FR
      rad[2] = r_rear_outer;  // RL
      rad[3] = r_rear_inner;  // RR
    }
    else
    {
      rad[0] = r_front_inner; // FL
      rad[1] = r_front_outer; // FR
      rad[2] = r_rear_inner;  // RL
      rad[3] = r_rear_outer;  // RR
    }
  }

  const float mean_rad = (rad[0] + rad[1] + rad[2] + rad[3]) * 0.25f;
  if (mean_rad <= 0.0f)
    return false;

  for (int i = 0; i < 4; i++)
  {
    const float expected = mean_actual * (rad[i] / mean_rad);
    if (expected <= 0.0f)
    {
      slip_out[i] = 0;
      continue;
    }
    float slip_pct = ((float)wheel_raw[i] / expected - 1.0f) * 100.0f;
    if (slip_pct > 127.0f)
      slip_pct = 127.0f;
    else if (slip_pct < -100.0f)
      slip_pct = -100.0f;
    slip_out[i] = (int8_t)(slip_pct < 0.0f ? slip_pct - 0.5f : slip_pct + 0.5f);
  }
  return true;
}

// Global lock gate: the "Disengage Under/Above Speed" and "Minimum Throttle
// Before Lock" settings. Applies to EVERY lock-producing path - the selected
// mode (50:50 / 60:40 / 75:25 / Expert) AND any force-mode trigger (TC,
// hazards, external button) - so a forced 50:50 in a car park still obeys the
// under-speed cut-off. A bound of 0 means that side of the window is disabled.
// Expert mode used to bypass this entirely (its map has its own speed axis);
// it is now gated too, matching the UI hint "disable ANY lock below...".

// Speed-disengage gate. See include/OpenHaldexC6_Calculations.h for the full
// rationale. Lock is permitted only while speed is at or above the lower bound
// AND at or below the upper bound; a bound of 0 disables that side.
bool speed_disengage_ok(uint16_t speed, uint16_t disengage_under, uint16_t disengage_above)
{
  bool above_lower = (disengage_under == 0) || (speed >= disengage_under);
  bool below_upper = (disengage_above == 0) || (speed <= disengage_above);
  return above_lower && below_upper;
}

// Hysteretic speed-disengage gate. See include/OpenHaldexC6_Calculations.h for
// the full rationale. Sharp entry, deadband exit; hysteresis 0 reduces exactly to
// speed_disengage_ok. The `speed + hysteresis` form (rather than
// `speed >= disengage_under - hysteresis`) keeps the arithmetic in unsigned range
// so a small bound can never underflow, and the upper cast prevents u16 overflow.
bool disengage_gate_hysteresis(uint16_t speed, uint16_t disengage_under, uint16_t disengage_above,
                               uint16_t hysteresis, bool currently_enabled)
{
  // Not yet engaged: entry is sharp, exactly the nominal band.
  if (!currently_enabled)
  {
    return speed_disengage_ok(speed, disengage_under, disengage_above);
  }

  // Already engaged: hold until speed moves `hysteresis` past a bound.
  bool above_lower = (disengage_under == 0) ||
                     ((uint32_t)speed + hysteresis >= disengage_under);
  bool below_upper = (disengage_above == 0) ||
                     (speed <= (uint32_t)disengage_above + hysteresis);
  return above_lower && below_upper;
}

// Integrating debounce for a force-mode CAN flag. See
// include/OpenHaldexC6_Calculations.h for the full rationale. A raw edge must
// persist for `threshold` consecutive disagreeing frames before it is accepted;
// any agreeing frame resets the counter, so a blip must be sustained. threshold
// <= 1 accepts every edge immediately, reducing to today's undebounced bitRead.
bool debounce_force_flag(bool raw, bool debounced, uint8_t &counter, uint8_t threshold)
{
  // Raw agrees with the accepted state: nothing pending, drop the counter.
  if (raw == debounced)
  {
    counter = 0;
    return debounced;
  }

  // Raw disagrees. threshold <= 1 accepts the edge on the first frame (no-op).
  if (threshold <= 1)
  {
    counter = 0;
    return raw;
  }

  // Count consecutive disagreeing frames; flip once the edge has held long enough.
  if (++counter >= threshold)
  {
    counter = 0;
    return raw;
  }
  return debounced;
}

// Strictly-ascending axis check for the expert-map interpolation.
// See include/OpenHaldexC6_Calculations.h.
bool is_strictly_ascending_u16(const uint16_t* arr, uint8_t count)
{
  for (uint8_t i = 1; i < count; i++)
  {
    if (arr[i] <= arr[i - 1])
    {
      return false;
    }
  }
  return true;
}


static inline bool lock_enabled()
{
  const bool throttle_ok = (state.pedal_threshold == 0) || (int(received_pedal_value) >= state.pedal_threshold);
  // Lock is allowed only within the window [disengageUnderSpeed, disengageAboveSpeed]
  // (a 0 bound disables that side). Pure seam: speed_disengage_ok().
  const bool speed_ok = speed_disengage_ok(received_vehicle_speed, disengageUnderSpeed, disengageAboveSpeed);
  return throttle_ok && speed_ok;
}

static float get_expert_lock_target()
{
  // 2D throttle/speed map - interpolation between arrays.  Result is an (int) of float of requested lock percentage
  float throttle = received_pedal_value;
  throttle = constrain(throttle, 0, 100); // fix throttle from 0-100

  float speed = received_vehicle_speed; // fix speed from 0-300 kmh (should be plenty)
  speed = constrain(speed, 0, 300);

  uint8_t t0 = 0; // index for throttle array
  uint8_t t1 = 0; // index for throttle array
  float t_ratio = 0;

  if (throttle >= throttleArray[throttleArrayCount - 1])
  {
    t0 = throttleArrayCount - 1; // if throttle is above top value, use top 2 values for interpolation (will just return top value)
    t1 = t0;                     // if throttle is above top value, use top 2 values for interpolation (will just return top value)
  }
  else
  {
    for (uint8_t i = 0; i < throttleArrayCount - 1; i++) // find throttle indexes for interpolation
    {
      if (throttle <= throttleArray[i + 1])
      {
        t0 = i;
        t1 = i + 1;
        float denom = (float)throttleArray[t1] - (float)throttleArray[t0];    // calculate ratio for interpolation - handle divide by zero just in case
        t_ratio = (denom > 0) ? ((throttle - throttleArray[t0]) / denom) : 0; // if denom is zero, just use t0 value (ratio of 0), otherwise calculate ratio between t0 and t1
        break;
      }
    }
  }

  uint8_t s0 = 0; // index for speed array
  uint8_t s1 = 0; // index for speed array
  float s_ratio = 0;

  if (speed >= speedArray[speedArrayCount - 1])
  {
    s0 = speedArrayCount - 1; // if speed is above top value, use top 2 values for interpolation (will just return top value)
    s1 = s0;                  // if speed is above top value, use top 2 values for interpolation (will just return top value)
  }
  else
  {
    for (uint8_t i = 0; i < speedArrayCount - 1; i++)
    {
      if (speed <= speedArray[i + 1])
      {
        s0 = i;
        s1 = i + 1;
        float denom = (float)speedArray[s1] - (float)speedArray[s0];    // calculate ratio for interpolation - handle divide by zero just in case
        s_ratio = (denom > 0) ? ((speed - speedArray[s0]) / denom) : 0; // if denom is zero, just use s0 value (ratio of 0), otherwise calculate ratio between s0 and s1
        break;
      }
    }
  }

  float v00 = lockArray[t0][s0];
  float v01 = lockArray[t0][s1];
  float v10 = lockArray[t1][s0];
  float v11 = lockArray[t1][s1];

  float v0 = v00 + ((v01 - v00) * s_ratio);
  float v1 = v10 + ((v11 - v10) * s_ratio);
  float v = v0 + ((v1 - v0) * t_ratio);

  v = constrain(v, 0, 100); // ensure lock target is between 0 and 100
  return int(v);            // return lock target as an integer percentage (0-100)
}

// Inverse of steering_curve_from_taper for display: the lock stays 100% up to
// start (the breakpoint before the first value below 100), reaches the floor (the
// last value) at full (the first breakpoint holding the floor). Exact for curves
// built by steering_curve_from_taper, best effort for hand-edited ones. Pure.
void steering_taper_from_curve(const uint16_t *arr, const uint8_t *scale, uint8_t count,
                               uint16_t &start_deg, uint16_t &full_deg, uint8_t &floor_pct)
{
  floor_pct = scale[count - 1];
  start_deg = arr[count - 1];
  for (uint8_t i = 1; i < count; i++)
  {
    if (scale[i] < 100)
    {
      start_deg = arr[i - 1];
      break;
    }
  }
  full_deg = arr[count - 1];
  for (uint8_t i = 0; i < count; i++)
  {
    if (scale[i] == floor_pct && (i == 0 || scale[i - 1] > floor_pct))
    {
      full_deg = arr[i];
      break;
    }
  }
}

// Piecewise-linear steering curve: lock multiplier (0-100) for a steering-wheel
// angle magnitude (deg). Past the last breakpoint the last value holds. Pure.
float steering_curve_percent(float angle, const uint16_t *arr, const uint8_t *scale, uint8_t count)
{
  if (count < 2)
    return 100.0f;
  angle = constrain(angle, 0, (float)arr[count - 1]);
  if (angle >= arr[count - 1])
    return constrain((float)scale[count - 1], 0, 100);

  for (uint8_t i = 0; i < count - 1; i++)
  {
    if (angle <= arr[i + 1])
    {
      const float denom = (float)arr[i + 1] - (float)arr[i];
      const float ratio = (denom > 0) ? ((angle - arr[i]) / denom) : 0;
      const float v0 = scale[i];
      const float v1 = scale[i + 1];
      return constrain(v0 + ((v1 - v0) * ratio), 0, 100);
    }
  }
  return 100.0f;
}

// Build the 5-point curve for Edge's three-knob taper (start / full / floor):
// 100% up to start_deg, a straight line down to floor_pct at full_deg, floor
// beyond. Lets the Calibrate-tab keys steeringGainStartDeg / FullDeg / Floor and
// the upstream breakpoint curve be one implementation. Pure.
void steering_curve_from_taper(uint16_t start_deg, uint16_t full_deg, uint8_t floor_pct,
                               uint16_t arr[steeringArrayCount], uint8_t scale[steeringArrayCount])
{
  if (floor_pct > 100)
    floor_pct = 100;
  if (full_deg < start_deg)
    full_deg = start_deg;
  arr[0] = 0;
  arr[1] = start_deg;
  arr[2] = (uint16_t)(((uint32_t)start_deg + full_deg) / 2);
  arr[3] = full_deg;
  arr[4] = (full_deg < 360) ? 360 : (uint16_t)(full_deg + 1);
  scale[0] = 100;
  scale[1] = 100;
  scale[2] = (uint8_t)((100 + floor_pct) / 2);
  scale[3] = floor_pct;
  scale[4] = floor_pct;
}

// Steering-angle third axis (FWD bias): returns a 0-100% multiplier for the
// lock target based on |steering-wheel angle|. Max lock at low angle, reduced
// lock as angle grows. Only gens with a steering source (2/4/50/52) use it;
// unsupported gens or stale/absent steering data return 100 (no reduction).
static float get_steering_lock_scale()
{
  const bool supported = (haldexGeneration == 2 || haldexGeneration == 4 ||
                          haldexGeneration == 50 || haldexGeneration == 52);
  if (!supported)
    return 100.0f;
  if (!steeringScaleEnabled)
    return 100.0f; // feature disabled -> full lock, no reduction
  if (received_steering_ms == 0 || (millis() - received_steering_ms) > steeringStaleMs)
    return 100.0f; // no/stale steering -> full lock, bias off

  return steering_curve_percent(fabsf(received_steering_angle), steeringArray, steeringLockScaleArray, steeringArrayCount);
}

// Scale a lock target by the steering-angle curve. Applied to lock-producing
// modes only (not Stock passthrough or FWD).
// Telemetry captured for the UI engagement-split display: last requested
// (pre-scale) and applied (post-scale) lock, and whether scaling ran this cycle.
static float s_steer_requested = 0.0f;
static float s_steer_applied = 0.0f;
static bool s_steer_applied_flag = false;

static inline float apply_steering_scale(float lock)
{
  const float scaled = lock * get_steering_lock_scale() / 100.0f;
  s_steer_requested = lock;
  s_steer_applied = scaled;
  s_steer_applied_flag = true;
  return scaled;
}

// Steering-scale telemetry accessors (used by the API live-status endpoint).
bool steering_scale_is_active()
{
  return s_steer_applied_flag && (s_steer_applied + 0.5f < s_steer_requested);
}
uint8_t steering_scale_requested_pct()
{
  return (uint8_t)constrain((int)(s_steer_requested + 0.5f), 0, 100);
}
uint8_t steering_scale_result_pct()
{
  return (uint8_t)constrain((int)(s_steer_applied + 0.5f), 0, 100);
}

// Which force-mode value applies right now: 0..5 (Stock/FWD/5050/6040/7525/Expert)
// when an enabled trigger's flag is active, or -1 when no force mode applies.
// Priority between simultaneous triggers is user-configurable via
// forceModesPriority (0-5 covering all 6 orderings of TC/Hazards/External).
// Exposed so the inline gateway (parseCAN_chs) can detect "effective mode is
// Stock" and pass frames through untouched instead of editing them.
int get_forced_mode_value()
{
  if (!(extBtnForceMode || tcForceMode || hazardForceMode))
  {
    return -1;
  }

  struct TriggerCheck
  {
    bool enabled;
    bool flag;
    uint8_t value;
  };
  const TriggerCheck triggers[3] = {
      {tcForceMode, tcForceModeFlag, tcForceModeValue},                // index 0 = TC
      {hazardForceMode, hazardForceModeFlag, hazardForceModeValue},    // index 1 = Hazard
      {extBtnForceMode, extButtonForceModeFlag, extBtnForceModeValue}, // index 2 = Ext
  };

  // Each row is [first, second, third] trigger index in priority order. 0=TC, 1=Hazard, 2=Ext. 6 rows cover all 6 options.
  static const uint8_t priorityOrders[6][3] = {
      {1, 0, 2}, // 0: Hazard > TC > Ext (default)
      {0, 1, 2}, // 1: TC > Hazard > Ext
      {1, 2, 0}, // 2: Hazard > Ext > TC
      {0, 2, 1}, // 3: TC > Ext > Hazard
      {2, 0, 1}, // 4: Ext > TC > Hazard
      {2, 1, 0}, // 5: Ext > Hazard > TC
  };

  const uint8_t priority = (forceModesPriority < 6) ? forceModesPriority : 0;

  for (uint8_t i = 0; i < 3; i++)
  {
    const uint8_t idx = priorityOrders[priority][i];
    if (triggers[idx].enabled && triggers[idx].flag)
    {
      return triggers[idx].value;
    }
  }
  return -1;
}

float get_lock_target_adjustment()
{
  s_steer_applied_flag = false; // reset; apply_steering_scale sets it when used this cycle

  // The return value is a torque COMMAND: standalone packs it into the frames it
  // synthesizes, and the inline gateway packs it into edited chassis frames. It
  // must never mirror received_haldex_engagement back - engagement 100
  // commanding 100 closes a positive feedback loop that latches the clutch shut
  // until FWD or a power cycle. Stock as a command is zero forced lock; true
  // stock PASSTHROUGH is the caller's job (parseCAN_chs skips frame edits
  // entirely when the effective mode is Stock).
  // const MODE_NAMES = ['Stock', 'FWD', '50:50', '60:40', '75:25', 'Expert'];
  const int forced = get_forced_mode_value();
  if (forced >= 0)
  {
    // Forced lock modes go through the same speed/throttle gate as the
    // selected mode below, so lock_target (and the dashboard "Requested"
    // figure) never reads 100% below the under-speed cut-off.
    switch (forced)
    {
    case 0:
      return 0; // Stock -> no forced lock (passthrough is handled by the caller)
    case 1:
      return 0; // FWD
    case 2:
      return lock_enabled() ? apply_steering_scale(100) : 0; // 50:50
    case 3:
      return lock_enabled() ? apply_steering_scale(40) : 0; // 60:40
    case 4:
      return lock_enabled() ? apply_steering_scale(30) : 0; // 75:25
    case 5:
      return lock_enabled() ? apply_steering_scale(get_expert_lock_target()) : 0; // Expert
    default:
      return 0; // error - zero lock
    }
  }

  // getting here means no external influences - handle each mode and calculate lock
  switch (state.mode)
  {
  case MODE_STOCK:
    return 0; // Stock -> no forced lock. Standalone has nothing to pass through, so Stock is an open clutch; inline Stock never reaches here (parseCAN_chs forwards frames untouched).

  case MODE_FWD:
    return 0; // zero lock

  case MODE_5050:
    if (lock_enabled())
    {
      return apply_steering_scale(100); // 100% lock
    }
    return 0; // lock not enabled, zero lock

  case MODE_6040:
    if (lock_enabled())
    {
      return apply_steering_scale(40); // 40% lock
    }
    return 0; // lock not enabled, zero lock

  case MODE_7525:
    if (lock_enabled())
    {
      return apply_steering_scale(30); // 30% lock
    }
    return 0; // lock not enabled, zero lock

  case MODE_EXPERT:
    if (lock_enabled())
    {
      return apply_steering_scale(get_expert_lock_target());
    }
    return 0; // lock not enabled, zero lock

  default:
    return 0; // error - zero lock
  }
}

uint8_t get_lock_target_adjusted_value(uint8_t value, bool invert)
{
  // during learn, use the current learn correction factor
  if (haldexLearnActive)
  {
    uint8_t corrected_value = (uint16_t)value * haldexLearnCF / 100;
    return (invert ? (0xFE - corrected_value) : corrected_value);
  }

  // handle 5050 mode
  if (lock_target == 100)
  {
    if (lock_enabled())
    {
      return (invert ? (0xFE - value) : value); // if lock enabled, return full value (or inverted), otherwise return 0 (or inverted)
    }
    return (invert ? 0xFE : 0x00);
  }

  // handle FWD mode
  if (lock_target == 0)
  {
    return (invert ? 0xFE : 0x00);
  }

  uint8_t correction_factor = 0; // calculate correction factor based on learn table if valid, otherwise use a default formula to determine correction factor (which is not very accurate, but better than nothing)
  if (haldexLearnTableValid)
  {
    // smallest correction factor whose learned engagement meets lock_target;
    // clamps to the CF of the highest learned engagement (argmax) when more lock
    // is requested than was ever learned, instead of falling through to 0 or
    // over-claiming the full frame.
    correction_factor = lookup_learn_correction_factor(haldexLearnTable, (uint8_t)lock_target);
  }
  else if (haldexGeneration == 41)
  {
    // Gen41 (Vauxhall/Opel/Buick FDCM) defaults — observed mapping CF→engagement is
    // steeper than the VAG Haldex curve.
    // Linear fit: engagement = 2.2 * CF - 24  →  CF = (target + 24) / 2.2
    correction_factor = (uint8_t)constrain(((float)lock_target + 24.0f) / 2.2f, 0, 100);
  }
  else
  {
    // VAG Haldex default — linear fit: engagement = 2 * CF - 20  →  CF = (target + 20) / 2
    correction_factor = (uint8_t)constrain(((float)lock_target / 2) + 20, 0, 100);
  }

  uint8_t corrected_value = (uint16_t)value * correction_factor / 100;
  if (lock_enabled())
  {
    return (invert ? (0xFE - corrected_value) : corrected_value); // if lock enabled, return corrected value (or inverted), otherwise return 0 (or inverted)
  }
  return (invert ? 0xFE : 0x00); // if lock not enabled, return 0 (or inverted)
}

void fill_esp19_wheel_speeds(uint8_t data[8])
{
  // Wheel speed MUST keep changing or the Haldex slowly disengages - a
  // static value fades over time and eventually stops. This is the original,
  // proven keep-alive dither (all 4 corners share the same small swing).
  //
  // A lock_target-proportional front/rear delta (simulating real slip, like
  // the Gen42/Ford wheel-speed code does) was tried here and made things
  // worse on real hardware - reported lock still faded from 100% and then
  // collapsed to 0%, rather than holding. Likely reading to the Haldex as
  // excessive/implausible sustained slip and triggering a separate
  // protection cutoff. Reverted; do not reintroduce without confirming on
  // the car first.
  if (wsBaseRaw == 0)
  {
    // Legacy behaviour: all four corners driven from the free-running counters.
    const uint8_t hlLo = get_lock_target_adjusted_value(ESP_19_counter2, false);
    const uint8_t hlHi = get_lock_target_adjusted_value(ESP_19_counter, false);
    const uint8_t vlLo = get_lock_target_adjusted_value(ESP_19_counter2 + 0xBA, false);

    data[0] = hlLo; // HL (rear left) low
    data[1] = hlHi; // HL (rear left) high
    data[2] = hlLo; // HR (rear right) low
    data[3] = hlHi; // HR (rear right) high
    data[4] = vlLo; // VL (front left) low
    data[5] = hlHi; // VL (front left) high
    data[6] = vlLo; // VR (front right) low
    data[7] = hlHi; // VR (front right) high
    if (wsLeftRightDeltaRaw != 0)
    {
      // VAQ lever: split the front axle left vs right around the legacy value.
      const int32_t base = (int32_t)((uint16_t)hlHi << 8 | vlLo);
      int32_t vl = base + wsLeftRightDeltaRaw / 2;
      int32_t vr = base - wsLeftRightDeltaRaw / 2;
      if (vl < 0) vl = 0; if (vl > 0xFFFF) vl = 0xFFFF;
      if (vr < 0) vr = 0; if (vr > 0xFFFF) vr = 0xFFFF;
      data[4] = (uint8_t)(vl & 0xFF); data[5] = (uint8_t)(vl >> 8);
      data[6] = (uint8_t)(vr & 0xFF); data[7] = (uint8_t)(vr >> 8);
    }
  }
  else
  {
    // Explicit mode: a fixed base speed per corner, optionally dithered, with
    // an optional front-axle offset. Lets the "does the Haldex hunt because
    // the simulated wheel speed keeps moving?" question be tested directly -
    // set wsDitherRaw 0 for a genuinely static speed.
    static bool phase = false;
    phase = !phase;
    const int32_t dither = wsDitherRaw ? (phase ? (int32_t)wsDitherRaw : -(int32_t)wsDitherRaw) : 0;
    int32_t rear = (int32_t)wsBaseRaw + dither;
    int32_t front = rear + wsFrontDeltaRaw;
    // VAQ lever: front left-vs-right split (a transverse lock reacts to VL vs VR).
    int32_t vl = front + wsLeftRightDeltaRaw / 2;
    int32_t vr = front - wsLeftRightDeltaRaw / 2;
    if (rear < 0) rear = 0;
    if (vl < 0) vl = 0;
    if (vr < 0) vr = 0;
    const uint16_t r = (uint16_t)(rear > 0xFFFF ? 0xFFFF : rear);
    const uint16_t l16 = (uint16_t)(vl > 0xFFFF ? 0xFFFF : vl);
    const uint16_t r16 = (uint16_t)(vr > 0xFFFF ? 0xFFFF : vr);

    data[0] = (uint8_t)(r & 0xFF);   // HL low
    data[1] = (uint8_t)(r >> 8);     // HL high
    data[2] = (uint8_t)(r & 0xFF);   // HR low
    data[3] = (uint8_t)(r >> 8);     // HR high
    data[4] = (uint8_t)(l16 & 0xFF); // VL low
    data[5] = (uint8_t)(l16 >> 8);   // VL high
    data[6] = (uint8_t)(r16 & 0xFF); // VR low
    data[7] = (uint8_t)(r16 >> 8);   // VR high
  }

  if (!wsFreeze)
  {
    ESP_19_counter++;
    ESP_19_counter2++;
    if (ESP_19_counter > 0x10)
      ESP_19_counter = 0x0A;
    if (ESP_19_counter2 > 0x2F)
      ESP_19_counter2 = 0x2E;
  }
}

void fill_motor11_bpk(uint8_t data[8], uint8_t counter)
{
  // DBC-correct bit packing for Motor_11 (0x0A7). Every field below is a
  // runtime tunable (see defs.h) so the serial lab can massage the wire
  // format live; the defaults are the values that were hardcoded here.
  // Signals are 10-bit with offset -509, i.e. raw = Nm + 509.
  // The 10-bit field with offset -509 encodes at most +514 Nm: clamp the ceiling to the
  // documented 509 Nm signal maximum so a raised value can never wrap the 0x3FF mask.
  const uint16_t ceilNm = (bpkCeilingNm > 509) ? 509 : bpkCeilingNm;
  const uint16_t floorNm = (bpkFloorNm < ceilNm) ? bpkFloorNm : 0;

  uint16_t torqueNm = get_lock_target_adjusted_value(0xFE, false);
  torqueNm = (uint16_t)(floorNm + ((uint32_t)torqueNm * (ceilNm - floorNm)) / 0xFE);

  static uint16_t prevIstNm = 0, prevSolfNm = 0;
  auto slew = [](uint16_t cur, uint16_t target, uint16_t step) -> uint16_t
  {
    if (step == 0)
      return target; // 0 = no rate limit
    if (target > cur)
      return ((uint32_t)cur + step >= target) ? target : (uint16_t)(cur + step);
    if (target < cur)
      return (cur <= step || cur - step <= target) ? target : (uint16_t)(cur - step);
    return cur;
  };
  uint16_t istNm = slew(prevIstNm, torqueNm, bpkSlewIst);
  uint16_t solfNm = slew(prevSolfNm, torqueNm, bpkSlewSolf);
  prevIstNm = istNm;
  prevSolfNm = solfNm;

  // Optional overrides, to test which field the Haldex actually keys off.
  if (bpkForceIstNm >= 0)
    istNm = (uint16_t)bpkForceIstNm;
  if (bpkForceSolfNm >= 0)
    solfNm = (uint16_t)bpkForceSolfNm;

  bpkLogSample(torqueNm, istNm, solfNm);

  const uint16_t rawSollRoh = (uint16_t)(torqueNm + 509) & 0x3FF;
  const uint16_t rawIst = (uint16_t)(istNm + 509) & 0x3FF;
  const uint16_t rawSolf = (uint16_t)(solfNm + 509) & 0x3FF;
  const uint16_t rawTraeg = bpkTraegRaw & 0x3FF;
  const uint16_t rawSchub = bpkSchubRaw & 0x1FF;

  data[0] = 0x00; // CRC placeholder - caller fills it
  data[1] = (counter & 0x0F) | ((rawSollRoh & 0x000F) << 4);
  data[2] = ((rawSollRoh >> 4) & 0x3F) | ((rawIst & 0x0003) << 6);
  data[3] = (rawIst >> 2) & 0xFF;
  data[4] = rawTraeg & 0xFF;
  data[5] = ((rawTraeg >> 8) & 0x03) | ((rawSolf & 0x3F) << 2);
  data[6] = ((rawSolf >> 6) & 0x0F) | ((rawSchub & 0x0F) << 4);
  data[7] = ((rawSchub >> 4) & 0x1F) | bpkStatusFl;

  for (uint8_t i = 0; i < 8; i++)
    bpkLastFrame[i] = data[i];
}

void bpkLogSample(uint16_t torqueNm, uint16_t istNm, uint16_t solfNm)
{
  // Runs at the Motor_11 rate (~100 Hz) on both BPK paths, so it stays a
  // plain store - the serial lab task does its own timing when it streams.
  bpkLastTorqueNm = torqueNm;
  bpkLastIstNm = istNm;
  bpkLastSolfNm = solfNm;
}

// The v9 %/s lock-release setting (web lockReleaseRatePerSec and the BLE
// Settings characteristic, 5..500 %/s) mapped onto the one ramp mechanism, which
// stores milliseconds for a full 0..100 travel. 100000 / rate: 500 %/s = 200 ms,
// 5 %/s = 20000 ms. Out-of-range rates are clamped, never rejected.
uint16_t lock_ramp_ms_from_pct_rate(uint16_t rate_per_sec)
{
  if (rate_per_sec < 5) rate_per_sec = 5;
  if (rate_per_sec > 500) rate_per_sec = 500;
  return (uint16_t)((100000UL + rate_per_sec / 2) / rate_per_sec);
}

// Inverse for reporting. 0 ms (instant) reads as the 500 %/s ceiling.
uint16_t lock_pct_rate_from_ramp_ms(uint16_t ramp_ms)
{
  if (ramp_ms == 0) return 500;
  uint32_t r = (100000UL + ramp_ms / 2) / ramp_ms;
  if (r < 5) r = 5;
  if (r > 500) r = 500;
  return (uint16_t)r;
}

// Slew one step of the lock-target rate limiter. Ramp times are milliseconds for
// a full 0<->100 travel: rising transitions take engage_ms, falling transitions
// take release_ms. 0 ms = instant in that direction - rising 0 is the historical
// instant lock-up; falling 0 is an instant release (the old %/s scheme floored
// release at a nonzero rate because rate 0 meant "never releases", a stuck-locked
// footgun; in ms, 0 cleanly means "release immediately"). A ramp of N ms moves
// 100000 * dt_s / N percent per step.
float lock_rate_limit_step(float current, float target, uint16_t engage_ms, uint16_t release_ms, float dt_s)
{
  if (target > current)
  {
    if (engage_ms == 0)
    {
      return target; // instant lock-up
    }
    const float max_rise = 100000.0f * dt_s / (float)engage_ms;
    return (target < current + max_rise) ? target : current + max_rise;
  }

  if (target < current)
  {
    if (release_ms == 0)
    {
      return target; // instant release
    }
    const float max_drop = 100000.0f * dt_s / (float)release_ms;
    return (target > current - max_drop) ? target : current - max_drop;
  }

  return target;
}

// Convert a persisted lock-ramp rate (%/s) to the new full-travel time in ms.
// Used once when migrating a device that stored the pre-ms unit: rate <= 0 is
// instant (0 ms), otherwise ms = round(100000 / rate) clamped to the 1000 ms
// slider ceiling so a very slow legacy rate lands on the 1 s maximum.
uint16_t lock_ramp_ms_from_rate(float rate_per_sec)
{
  if (rate_per_sec <= 0.0f)
  {
    return 0; // instant
  }
  const float ms = 100000.0f / rate_per_sec;
  if (ms >= 1000.0f)
  {
    return 1000; // clamp a slow legacy rate to the 1 s ceiling
  }
  return (uint16_t)(ms + 0.5f); // round to nearest ms
}

// Learn interlock: the sweep commands up to full lock, which is only safe with
// the car stationary. Pure so the 5 km/h limit is pinned by a host test.
bool learn_speed_ok(uint16_t speed_kmh)
{
  return speed_kmh <= learnMaxSpeed;
}

void startHaldexLearn()
{
  if (!learn_speed_ok(received_vehicle_speed))
  {
    haldexLearnStep = 103; // refused: car is moving (same code a mid-sweep abort reports)
    return;
  }

  // Guard + reserve in one critical section so two concurrent callers can never
  // both pass the already-running check. runLearnSweep() snapshots and wipes
  // the table itself, under the same mutex.
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (haldexLearnActive || longLearnActive)
  {
    xSemaphoreGive(stateMutex);
    return; // already running (Long Learn drives its own sweeps)
  }
  haldexLearnCancel = false;
  haldexLearnStep = 0;
  haldexLearnCF = 0;
  haldexLearnActive = true;
  xSemaphoreGive(stateMutex);

  if (xTaskCreate(haldexLearnTask, "haldexLearn", 4096, nullptr, 1, nullptr) != pdPASS)
  {
    // Task never started, so nothing would ever clear the active flag.
    haldexLearnActive = false;
    DEBUG("startHaldexLearn: xTaskCreate failed - learn not started");
  }
}

bool runLearnSweep(uint32_t preHoldMs)
{
  // Bench-measured (Gen5 0CQ): engagement needs several hundred ms to settle
  // after a step, and a single instantaneous sample lands mid-transient - which
  // is what made learned mid-points wander. Settle first, then AVERAGE over a
  // short observation window, so each entry is the value actually held rather
  // than whatever the reading was passing through. Held steady, this hardware
  // tracks the request 1:1 with no jitter at all, so a clean sweep should come
  // out close to an identity table.
  // The window is reduced with learn_reduce_samples (lower median, then held
  // monotonic against the previous CF): near lock-up the Haldex ECU's own duty
  // loop briefly overshoots to a clean 100 for one frame, and a plain average
  // would write that spike into the table.
  const uint32_t settleMs = 400;
  const uint32_t sampleMs = 25;
  const uint8_t sampleCount = 8; // 8 * 25 ms = 200 ms observation window
  uint8_t prevRecorded = 0;
  bool speedAborted = false;

  // The ESP_14 launch floor pins BR_Vorg_*_Min to Max, leaving the Haldex no
  // room to modulate - it drives the pump to full duty and corrupts the top of
  // the sweep. Always learn with it at 0 and restore afterwards. NOTE: the
  // Motor_11 packing (fixHunting) is deliberately NOT forced here: which
  // packing a unit needs is a per-unit trait (554K needs BPK, this 0CQ does
  // not), and the table must describe how the car will actually be driven.
  const uint8_t floorBeforeLearn = esp14MinFloorPct;
  esp14MinFloorPct = 0;

  // Snapshot the current calibration, then wipe and invalidate it in one
  // critical section so the hot path never sees a valid flag over a half-wiped
  // table. A cancelled or speed-aborted sweep restores the snapshot (see
  // learn_finalize) instead of leaving the user with no table at all.
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  memcpy(haldexLearnTableBackup, haldexLearnTable, sizeof(haldexLearnTableBackup));
  haldexLearnTableBackupValid = haldexLearnTableValid;
  memset(haldexLearnTable, 0, sizeof(haldexLearnTable));
  haldexLearnTableValid = false;
  haldexLearnCancel = false;
  haldexLearnStep = 0;
  haldexLearnCF = 0;
  haldexLearnActive = true; // frames now carry haldexLearnCF regardless of mode
  xSemaphoreGive(stateMutex);

  // Optional pre-hold at CF 0: wait for the clutch to release from the previous
  // sweep (engagement <= 2 % for five consecutive 100 ms ticks) or time out.
  if (preHoldMs > 0)
  {
    uint8_t releasedTicks = 0;
    for (uint32_t held = 0; held < preHoldMs && !haldexLearnCancel; held += 100)
    {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      if (received_haldex_engagement <= 2)
      {
        if (++releasedTicks >= 5)
          break;
      }
      else
      {
        releasedTicks = 0;
      }
    }
  }

  for (uint16_t cf = 0; cf <= 100; cf++)
  {
    if (haldexLearnCancel)
    {
      break;
    }

    // Live speed interlock: abort without publishing the partial table if the
    // car moves off mid-sweep. Step 103 tells the UI why.
    if (!learn_speed_ok(received_vehicle_speed))
    {
      speedAborted = true;
      if (longLearnActive)
      {
        longLearnSpeedAborted = true; // Long Learn restores everything and reports failure
        longLearnCancel = true;
      }
      break;
    }

    haldexLearnStep = (uint8_t)cf;
    haldexLearnCF = (uint8_t)cf;

    vTaskDelay(settleMs / portTICK_PERIOD_MS);

    uint8_t samples[sampleCount];
    for (uint8_t i = 0; i < sampleCount; i++)
    {
      vTaskDelay(sampleMs / portTICK_PERIOD_MS);
      samples[i] = received_haldex_engagement;
    }

    // Engagement can never physically fall as the request climbs, so the
    // reducer holds the table monotonic against the previous CF as well.
    prevRecorded = learn_reduce_samples(samples, sampleCount, prevRecorded);
    haldexLearnTable[cf] = prevRecorded;
  }

  // Publish the outcome (restore on cancel/speed-abort, validate on complete)
  // together with the valid flag under the lock. The decision lives in
  // learn_finalize, a pure seam pinned by test_learn.
  bool tableValid = false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  haldexLearnStep = learn_finalize(haldexLearnTable, &tableValid,
                                   haldexLearnTableBackup, haldexLearnTableBackupValid,
                                   haldexLearnCancel, speedAborted, haldexLearnStep);
  haldexLearnTableValid = tableValid;
  xSemaphoreGive(stateMutex);

  const bool anyNonZero = (haldexLearnStep == 101);
  const bool ok = !haldexLearnCancel && anyNonZero;
  haldexLearnActive = false;
  haldexLearnCF = 0;
  esp14MinFloorPct = floorBeforeLearn; // restore whatever the user had set
  return ok;
}

void scoreLearnTable(const uint8_t *table, LearnScore &out)
{
  out.reach = table[100];
  out.engageCF = 101;
  for (uint8_t i = 0; i <= 100; i++)
  {
    if (table[i] > 0)
    {
      out.engageCF = i;
      break;
    }
  }
  out.engageJump = (out.engageCF <= 100) ? table[out.engageCF] : 0;

  // Largest rise between consecutive steps after the first engage step. The
  // table is monotonic (runLearnSweep holds the peak) so the delta is >= 0.
  out.maxStep = 0;
  for (uint16_t i = (uint16_t)out.engageCF + 1; i <= 100; i++)
  {
    const uint8_t d = table[i] - table[i - 1];
    if (d > out.maxStep)
      out.maxStep = d;
  }

  out.smooth = (out.engageCF <= LL_ENGAGE_MAX_CF) &&
               (out.reach >= LL_REACH_MIN) &&
               (out.maxStep <= LL_STEP_MAX);

  // Composite rank: start from reach, penalise discontinuities hard and late
  // engagement gently. A table that never engages scores 0.
  int s = out.reach;
  if (out.maxStep > 3)
    s -= 4 * (out.maxStep - 3);
  if (out.engageCF > 20)
    s -= (out.engageCF - 20) / 2;
  if (out.engageCF > 100)
    s = 0;
  out.score = (uint8_t)constrain(s, 0, 100);
}

bool startLongLearn(bool testAll)
{
  if (longLearnActive || haldexLearnActive)
    return false;
  if (!learn_speed_ok(received_vehicle_speed))
  {
    haldexLearnStep = 103; // refused: car is moving
    return false;
  }
  const int gi = frameEditGenIdx(haldexGeneration);
  if (gi < 0)
    return false; // gen41/42 etc. have no gated blocks to bisect

  longLearnGenIdx = (uint8_t)gi;
  longLearnGeneration = haldexGeneration;
  longLearnTestAll = testAll;
  longLearnCancel = false;
  longLearnSpeedAborted = false;
  longLearnPhase = LL_SWEEP;
  longLearnSweepIdx = 0;
  longLearnSweepTotal = 0;
  longLearnSweepCount = 0;
  longLearnCurrentBit = -1;
  longLearnBaselineValid = false;
  longLearnFinalValid = false;
  longLearnBpkAdjusted = false;
  longLearnStartMs = millis();
  longLearnEndMs = 0;
  memset(longLearnBlockResult, 0, sizeof(longLearnBlockResult));
  longLearnActive = true;

  xTaskCreate(longLearnTask, "longLearn", 6144, nullptr, 1, nullptr);
  return true;
}

void getLockData(twai_message_t &rx_message_chs)
{
  // Hold stateMutex across the whole read + compute + frame edit so the Haldex
  // never sees a half-rewritten expert table, learn table or mode. The guard
  // releases on every return path below. No blocking calls inside, so the hold
  // is bounded; nothing called from here takes the mutex again.
  StateLock stateLock;

  // Calculate raw lock target then (optionally) apply rate-limited slewing.
  // When lockReleaseEnabled is false, all transitions to new lock % are instantaneous.
  // When enabled, falling transitions take `lockReleaseRampMs` ms for a full
  // release so the clutch opens gradually rather than snapping, and rising
  // transitions take `lockEngageRampMs` ms (0 = instantaneous, the default).
  static float smoothed_lock_target = 0.0f;
  static uint32_t last_lock_ms = 0;

  const float raw_target = get_lock_target_adjustment(); // calculate raw lock target based on mode, overrides, and learn table

  if (!lockReleaseEnabled)
  {
    smoothed_lock_target = raw_target; // instant: bypass rate limit
    last_lock_ms = millis();
  }
  else
  {
    const uint32_t now_ms = millis();
    const float dt_s = (last_lock_ms == 0) ? 0.0f : (float)(now_ms - last_lock_ms) / 1000.0f;
    last_lock_ms = now_ms;

    smoothed_lock_target = lock_rate_limit_step(smoothed_lock_target, raw_target,
                                                lockEngageRampMs, lockReleaseRampMs, dt_s);
  }
  lock_target = smoothed_lock_target;

  // If the incoming frame is a known frame for this generation and its bit is
  // cleared, skip all editing.  If its bit is set, allow editing
  {
    int _feGen = frameEditGenIdx(haldexGeneration);
    if (_feGen >= 0)
    {
      for (uint16_t _i = 0; _i < frameEditBlockCount; _i++)
      {
        if (frameEditBlocks[_i].genIdx == (uint8_t)_feGen &&
            frameEditBlocks[_i].canId == rx_message_chs.identifier)
        {
          if (!frameEditEnabled((uint8_t)_feGen, frameEditBlocks[_i].bit))
            return; // block disabled -> pass the car's real frame through untouched
          break;
        }
      }
    }
  }

  // begin frame parsing / editting
  // edit the frames if configured as Gen1...
  if (haldexGeneration == 1)
  {
    switch (rx_message_chs.identifier)
    {
    case MOTOR1_ID:
      rx_message_chs.data[0] = 0x00;
      rx_message_chs.data[1] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[2] = 0x21;
      rx_message_chs.data[3] = get_lock_target_adjusted_value(0x4E, false);
      rx_message_chs.data[4] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[5] = get_lock_target_adjusted_value(0xFE, false);
      appliedTorque = rx_message_chs.data[6];

      switch (state.mode)
      {
      case MODE_FWD:
        appliedTorque = get_lock_target_adjusted_value(0xFE, true);
        break;
      case MODE_5050:
        appliedTorque = get_lock_target_adjusted_value(0x16, false);
        break;
      case MODE_6040:
        appliedTorque = get_lock_target_adjusted_value(0x22, false);
        break;
      case MODE_7525:
        appliedTorque = get_lock_target_adjusted_value(0x50, false);
        break;
      default:
        break;
      }

      rx_message_chs.data[6] = appliedTorque;
      rx_message_chs.data[7] = 0x00;
      break;
    case MOTOR3_ID:
      rx_message_chs.data[2] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[7] = get_lock_target_adjusted_value(0xFE, false);
      break;
    case BRAKES1_ID:
      rx_message_chs.data[1] = get_lock_target_adjusted_value(0x00, false);
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = get_lock_target_adjusted_value(0x0A, false);
      break;
    case BRAKES3_ID:
      rx_message_chs.data[0] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[1] = 0x0A;
      rx_message_chs.data[2] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[3] = 0x0A;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x0A;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0x0A;
      break;
    }
  }

  // edit the frames if configured as Gen2...
  if (haldexGeneration == 2)
  {
    switch (rx_message_chs.identifier)
    {
    case MOTOR1_ID:
      rx_message_chs.data[1] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[2] = 0x21;
      rx_message_chs.data[3] = get_lock_target_adjusted_value(0x4E, false);
      rx_message_chs.data[6] = get_lock_target_adjusted_value(0xFE, false); // 0x20 in standalone - same as gen1?
      break;
    case MOTOR3_ID:
      rx_message_chs.data[2] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[7] = get_lock_target_adjusted_value(0x01, false);
      break;
    case BRAKES1_ID:
      rx_message_chs.data[0] = get_lock_target_adjusted_value(0x80, false);
      rx_message_chs.data[1] = get_lock_target_adjusted_value(0x41, false);
      rx_message_chs.data[2] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[3] = 0x0A;
      break;
    case BRAKES2_ID:
      rx_message_chs.data[4] = get_lock_target_adjusted_value(0x7F, false);
      rx_message_chs.data[5] = get_lock_target_adjusted_value(0xFE, false);
      break;
    case BRAKES3_ID:
      rx_message_chs.data[0] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[1] = 0x0A;
      rx_message_chs.data[2] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[3] = 0x0A;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x0A;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0x0A;
      break;

    // ---- Transferred from standalone (gated off by default) ----------------
    case BRAKES4_ID:
      rx_message_chs.data[0] = 0x00;
      rx_message_chs.data[1] = 0x00;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = BRAKES4_counter;
      rx_message_chs.data[7] = BRAKES4_counter;
      BRAKES4_counter = BRAKES4_counter + 10;
      if (BRAKES4_counter > 0xF0)
        BRAKES4_counter = 0;
      break;
    case BRAKES5_ID:
      rx_message_chs.data[0] = 0xFE;
      rx_message_chs.data[1] = 0x7F;
      rx_message_chs.data[2] = 0x03;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = BRAKES5_counter;
      rx_message_chs.data[7] = BRAKES5_counter2;
      BRAKES5_counter = BRAKES5_counter + 10;
      if (BRAKES5_counter > 0xF0)
        BRAKES5_counter = 0;
      BRAKES5_counter2 = BRAKES5_counter2 + 10;
      if (BRAKES5_counter2 > 0xF3)
        BRAKES5_counter2 = 3;
      break;
    case BRAKES9_ID:
      rx_message_chs.data[0] = BRAKES9_counter;
      rx_message_chs.data[1] = BRAKES9_counter2;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x02;
      rx_message_chs.data[7] = 0x00;
      BRAKES9_counter = BRAKES9_counter + 10;
      if (BRAKES9_counter > 0xF1)
        BRAKES9_counter = 0x11;
      BRAKES9_counter2 = BRAKES9_counter2 + 10;
      if (BRAKES9_counter2 > 0xF0)
        BRAKES9_counter2 = 0x00;
      break;
    case BRAKES10_ID:
      rx_message_chs.data[0] = 0xA6;
      rx_message_chs.data[1] = BRAKES10_counter;
      rx_message_chs.data[2] = 0x75;
      rx_message_chs.data[3] = 0xD4;
      rx_message_chs.data[4] = 0x51;
      rx_message_chs.data[5] = 0x47;
      rx_message_chs.data[6] = 0x1D;
      rx_message_chs.data[7] = 0x0F;
      BRAKES10_counter = BRAKES10_counter + 1;
      if (BRAKES10_counter > 0xF)
        BRAKES10_counter = 0;
      break;
    case MOTOR5_ID:
      rx_message_chs.data[0] = 0xFE;
      rx_message_chs.data[1] = 0x00;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = MOTOR5_counter;
      MOTOR5_counter++;
      break;
    case MOTOR2_ID:
      rx_message_chs.data[0] = 0x00;
      rx_message_chs.data[1] = 0x30;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x0A;
      rx_message_chs.data[4] = 0x0A;
      rx_message_chs.data[5] = 0x10;
      rx_message_chs.data[6] = 0xFE;
      rx_message_chs.data[7] = 0xFE;
      break;
    case mLW_1:
      rx_message_chs.data[0] = 0x20;
      rx_message_chs.data[1] = 0x00;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x80;
      rx_message_chs.data[5] = mLW_1_counter;
      rx_message_chs.data[6] = 0x00;
      mLW_1_crc = 255 - (rx_message_chs.data[0] + rx_message_chs.data[1] + rx_message_chs.data[2] + rx_message_chs.data[3] + rx_message_chs.data[5]);
      rx_message_chs.data[7] = mLW_1_crc;
      mLW_1_counter = mLW_1_counter + 16;
      if (mLW_1_counter >= 0xF0)
        mLW_1_counter = 0;
      break;
    case mKombi_1:
      rx_message_chs.data[0] = 0x00;
      rx_message_chs.data[1] = 0x02;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x36;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0x00;
      break;
    }
  }

  // edit the frames if configured as Gen4...
  if (haldexGeneration == 4)
  {
    switch (rx_message_chs.identifier)
    {
    case mLW_1:
      rx_message_chs.data[0] = lws_2[mLW_1_counter][0];
      rx_message_chs.data[1] = lws_2[mLW_1_counter][1];
      rx_message_chs.data[2] = lws_2[mLW_1_counter][2];
      rx_message_chs.data[3] = lws_2[mLW_1_counter][3];
      rx_message_chs.data[4] = lws_2[mLW_1_counter][4];
      rx_message_chs.data[5] = lws_2[mLW_1_counter][5];
      rx_message_chs.data[6] = lws_2[mLW_1_counter][6];
      rx_message_chs.data[7] = lws_2[mLW_1_counter][7];
      mLW_1_counter++;
      if (mLW_1_counter > 15)
      {
        mLW_1_counter = 0;
      }
      break;
    case MOTOR1_ID:
      rx_message_chs.data[1] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[2] = get_lock_target_adjusted_value(0x20, false);
      rx_message_chs.data[3] = get_lock_target_adjusted_value(0x4E, false);
      rx_message_chs.data[4] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[5] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[6] = get_lock_target_adjusted_value(0x16, false);
      rx_message_chs.data[7] = get_lock_target_adjusted_value(0xFE, false);
      break;
    case BRAKES1_ID:
      rx_message_chs.data[0] = 0x20;
      rx_message_chs.data[1] = 0x40;
      rx_message_chs.data[4] = get_lock_target_adjusted_value(0xFE, false);
      rx_message_chs.data[5] = get_lock_target_adjusted_value(0xFE, false);
      break;
    case BRAKES2_ID:
      rx_message_chs.data[4] = get_lock_target_adjusted_value(0x7F, false);
      break;
    case BRAKES3_ID:
      rx_message_chs.data[0] = get_lock_target_adjusted_value(0xB6, false);
      rx_message_chs.data[1] = 0x07;
      rx_message_chs.data[2] = get_lock_target_adjusted_value(0xCC, false);
      rx_message_chs.data[3] = 0x07;
      rx_message_chs.data[4] = get_lock_target_adjusted_value(0xD2, false);
      rx_message_chs.data[5] = 0x07;
      rx_message_chs.data[6] = get_lock_target_adjusted_value(0xD2, false);
      rx_message_chs.data[7] = 0x07;
      break;

    case BRAKES4_ID:
      if (haldexLearnActive || state.mode != MODE_5050)
      {
        appliedTorque = get_lock_target_adjusted_value(0x7F, false); // regulated clamp
      }
      else
      {
        appliedTorque = get_lock_target_adjusted_value(0xFE, false); // full clamp (27 bar)
      }

      rx_message_chs.data[0] = appliedTorque;
      rx_message_chs.data[1] = 0x00;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x64;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = BRAKES4_counter;
      BRAKES4_crc = 0;
      for (uint8_t i = 0; i < 7; i++)
      {
        BRAKES4_crc ^= rx_message_chs.data[i];
      }
      rx_message_chs.data[7] = BRAKES4_crc;

      BRAKES4_counter = BRAKES4_counter + 16;
      if (BRAKES4_counter > 0xF0)
      {
        BRAKES4_counter = 0x00;
      }
      break;

    // ---- Transferred from standalone (gated off by default) ----------------
    case mKombi_1:
      rx_message_chs.data[0] = 0x24;
      rx_message_chs.data[1] = 0x00;
      rx_message_chs.data[2] = 0x1D;
      rx_message_chs.data[3] = 0xB9;
      rx_message_chs.data[4] = 0x07;
      rx_message_chs.data[5] = 0x42;
      rx_message_chs.data[6] = 0x09;
      rx_message_chs.data[7] = 0x81;
      break;
    case mKombi_3:
      rx_message_chs.data[0] = 0x60;
      rx_message_chs.data[1] = 0x43;
      rx_message_chs.data[2] = 0x01;
      rx_message_chs.data[3] = 0x10;
      rx_message_chs.data[4] = 0x66;
      rx_message_chs.data[5] = 0xF1;
      rx_message_chs.data[6] = 0x03;
      rx_message_chs.data[7] = 0x02;
      break;
    case mGate_Komf_1:
      rx_message_chs.data[0] = 0x03;
      rx_message_chs.data[1] = 0x11;
      rx_message_chs.data[2] = 0x58;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x40;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x01;
      rx_message_chs.data[7] = 0x08;
      break;
    case BRAKES11_ID:
      rx_message_chs.data[0] = 0x00;
      rx_message_chs.data[1] = 0xC0;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0x00;
      break;
    case mKombi_2:
      rx_message_chs.data[0] = 0x4C;
      rx_message_chs.data[1] = 0x86;
      rx_message_chs.data[2] = 0x85;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x30;
      rx_message_chs.data[6] = 0xFF;
      rx_message_chs.data[7] = 0x04;
      break;
    case mDiagnose_1:
      rx_message_chs.data[0] = 0x26;
      rx_message_chs.data[1] = 0xF2;
      rx_message_chs.data[2] = 0x03;
      rx_message_chs.data[3] = 0x12;
      rx_message_chs.data[4] = 0x70;
      rx_message_chs.data[5] = 0x19;
      rx_message_chs.data[6] = 0x25;
      rx_message_chs.data[7] = mDiagnose_1_counter;
      mDiagnose_1_counter++;
      if (mDiagnose_1_counter > 0x1F)
        mDiagnose_1_counter = 0;
      break;
    }
  }

  // edit the frames if configured as Gen5 (0AY) - frames left
  // commented-out are so they can be re-enabled later if a required
  if (haldexGeneration == 51)
  {
    switch (rx_message_chs.identifier)
    {
    // ---- Active (lock-modulated) -------------------------------------------
    case MOTOR1_ID:
      // PQ Motor_1 (0x280) - engine ECU broadcast.  Bytes 1..7 lock-adjusted to
      // bias the haldex; byte 0 left at 0 (Fahrerwunschmoment).
      rx_message_chs.data[0] = 0x00;                                        // Fahrerwunschmoment
      rx_message_chs.data[1] = get_lock_target_adjusted_value(0xFE, false); // Fahrerwunschmoment
      rx_message_chs.data[2] = get_lock_target_adjusted_value(0x20, false); // Motordrehzahl low
      rx_message_chs.data[3] = get_lock_target_adjusted_value(0x4E, false); // Motordrehzahl high
      rx_message_chs.data[4] = get_lock_target_adjusted_value(0xFE, false); // Fahrpedalwert
      rx_message_chs.data[5] = get_lock_target_adjusted_value(0xFE, false); // inneres_Motor_Moment_ohne_extern
      rx_message_chs.data[6] = get_lock_target_adjusted_value(0x16, false); // mechanisches_Motor_Verlustmoment
      rx_message_chs.data[7] = get_lock_target_adjusted_value(0xFE, false); // inneres_Motor_Moment
      break;

    case BRAKES3_ID:
      // PQ Bremse_3 (0x4A0) - per-wheel speeds (Radgeschw_VL/VR/HL/HR).
      // Low bytes lock-adjusted, high bytes fixed at 0x07.
      rx_message_chs.data[0] = get_lock_target_adjusted_value(0xB6, false); // Radgeschw_VL low
      rx_message_chs.data[1] = 0x07;                                        // Radgeschw_VL high
      rx_message_chs.data[2] = get_lock_target_adjusted_value(0xCC, false); // Radgeschw_VR low
      rx_message_chs.data[3] = 0x07;                                        // Radgeschw_VR high
      rx_message_chs.data[4] = get_lock_target_adjusted_value(0xD2, false); // Radgeschw_HL low
      rx_message_chs.data[5] = 0x07;                                        // Radgeschw_HL high
      rx_message_chs.data[6] = get_lock_target_adjusted_value(0xD2, false); // Radgeschw_HR low
      rx_message_chs.data[7] = 0x07;                                        // Radgeschw_HR high
      break;

    case BRAKES4_ID:
      // PQ Bremse_4 (0x2A0) - ABS coupling moment.  Byte 0 is a signed offset
      // around 0x7F (matches standalone: get_lock_target_adjusted_value(0x7F) - 0x7F).
      // data[6] is rolling counter (16-step), data[7] is XOR-CRC over data[0..6].
      rx_message_chs.data[0] = get_lock_target_adjusted_value(0x7F, false) - 0x7F; // ABS_Vorgabewert_hinten_Kupplung
      rx_message_chs.data[1] = 0x00;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x64;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = BRAKES4_counter;
      BRAKES4_crc = 0;
      for (uint8_t i = 0; i < 7; i++)
      {
        BRAKES4_crc ^= rx_message_chs.data[i];
      }
      rx_message_chs.data[7] = BRAKES4_crc;

      BRAKES4_counter = BRAKES4_counter + 16;
      if (BRAKES4_counter > 0xF0)
      {
        BRAKES4_counter = 0x00;
      }
      break;

    case mLW_1:
      // PQ LW_1 (0x0C2) - steering-angle replay table; not lock-modulated.
      rx_message_chs.data[0] = lws_2[mLW_1_counter][0];
      rx_message_chs.data[1] = lws_2[mLW_1_counter][1];
      rx_message_chs.data[2] = lws_2[mLW_1_counter][2];
      rx_message_chs.data[3] = lws_2[mLW_1_counter][3];
      rx_message_chs.data[4] = lws_2[mLW_1_counter][4];
      rx_message_chs.data[5] = lws_2[mLW_1_counter][5];
      rx_message_chs.data[6] = lws_2[mLW_1_counter][6];
      rx_message_chs.data[7] = lws_2[mLW_1_counter][7];
      mLW_1_counter++;
      if (mLW_1_counter > 15)
        mLW_1_counter = 0;
      break;

    // ---- Inactive frames restored (gated off by default via frameEditMask) --
    case BRAKES1_ID:
      // PQ Bremse_1 (0x1A0) - ABS/ESP main broadcast.  Static in standalone.
      rx_message_chs.data[0] = 0x20;
      rx_message_chs.data[1] = 0x40;
      rx_message_chs.data[2] = 0xF0;
      rx_message_chs.data[3] = 0x07;
      rx_message_chs.data[4] = 0xFE;
      rx_message_chs.data[5] = 0xFE;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = BRAKES1_counter;
      if (++BRAKES1_counter > 0x1F)
        BRAKES1_counter = 10;
      break;

    case mGetriebe_2:
      // PQ Getriebe_2 (0x540) - transmission status; needed by DTC 17497 but static.
      rx_message_chs.data[0] = (mGetriebe_2_counter << 4) & 0xF0;
      rx_message_chs.data[1] = 0x50;
      rx_message_chs.data[2] = 0xFF;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0xFF;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0xFF;
      mGetriebe_2_counter = (mGetriebe_2_counter + 1) & 0x0F;
      break;

    case BRAKES5_ID:
      // PQ Bremse_5 (0x4A8) - ESP brake-event broadcast.  Static in standalone.
      rx_message_chs.data[0] = 0xFE;
      rx_message_chs.data[1] = 0x7F;
      rx_message_chs.data[2] = 0x03;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = BRAKES5_counter;
      rx_message_chs.data[7] = BRAKES5_counter2;
      BRAKES5_counter = BRAKES5_counter + 10;
      if (BRAKES5_counter > 0xF0)
        BRAKES5_counter = 0;
      BRAKES5_counter2 = BRAKES5_counter2 + 10;
      if (BRAKES5_counter2 > 0xF3)
        BRAKES5_counter2 = 3;
      break;

    case BRAKES8_ID:
      // PQ Bremse_8 (0x1AC) - ESP supplemental broadcast; dual rolling counters.
      rx_message_chs.data[0] = BRAKES8_counter;
      rx_message_chs.data[1] = BRAKES8_counter1;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x69;
      rx_message_chs.data[5] = 0x21;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0xC1;
      if (++BRAKES8_counter > 0x8F)
        BRAKES8_counter = 0x80;
      if (++BRAKES8_counter1 > 0x0F)
        BRAKES8_counter1 = 0x00;
      break;

    case BRAKES2_ID:
      // PQ Bremse_2 (0x5A0) - ESP/ABS sensor broadcast.  Now static in standalone
      // (Querbeschleunigung was lock-adjusted historically; now 0x7F literal).
      rx_message_chs.data[0] = 0x80;
      rx_message_chs.data[1] = 0x7A;
      rx_message_chs.data[2] = 0x05;
      rx_message_chs.data[3] = BRAKES2_counter;
      rx_message_chs.data[4] = 0x7F;
      rx_message_chs.data[5] = 0xCA;
      rx_message_chs.data[6] = 0x1B;
      rx_message_chs.data[7] = 0xAB;
      BRAKES2_counter = BRAKES2_counter + 16;
      if (BRAKES2_counter > 0xF0)
        BRAKES2_counter = 0;
      break;

    case MOTOR2_ID:
      // PQ Motor_2 (0x288) - secondary engine broadcast.  Static in standalone.
      rx_message_chs.data[0] = 0x00;
      rx_message_chs.data[1] = 0x30;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x0A;
      rx_message_chs.data[4] = 0x0A;
      rx_message_chs.data[5] = 0x10;
      rx_message_chs.data[6] = 0xFE;
      rx_message_chs.data[7] = 0xFE;
      break;

    case MOTOR5_ID:
      // PQ Motor_5 (0x480) - tertiary engine broadcast.  Static in standalone.
      rx_message_chs.data[0] = 0xFE;
      rx_message_chs.data[1] = 0x00;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = MOTOR5_counter;
      break;

    case mKombi_1:
      // PQ Kombi_1 (0x320) - instrument-cluster broadcast.  Static in standalone.
      rx_message_chs.data[0] = 0x24;
      rx_message_chs.data[1] = 0x00;
      rx_message_chs.data[2] = 0x1D;
      rx_message_chs.data[3] = 0xB9;
      rx_message_chs.data[4] = 0x07;
      rx_message_chs.data[5] = 0x42;
      rx_message_chs.data[6] = 0x09;
      rx_message_chs.data[7] = 0x81;
      break;

    case mKombi_3:
      // PQ Kombi_3 (0x520) - cluster odometer/keys.  Static in standalone.
      rx_message_chs.data[0] = 0x60;
      rx_message_chs.data[1] = 0x43;
      rx_message_chs.data[2] = 0x01;
      rx_message_chs.data[3] = 0x10;
      rx_message_chs.data[4] = 0x66;
      rx_message_chs.data[5] = 0xF1;
      rx_message_chs.data[6] = 0x03;
      rx_message_chs.data[7] = 0x02;
      break;

    case mGate_Komf_1:
      // PQ Gate_Komf_1 (0x390) - gateway-comfort broadcast.  Static in standalone.
      rx_message_chs.data[0] = 0x03;
      rx_message_chs.data[1] = 0x11;
      rx_message_chs.data[2] = 0x58;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x40;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x01;
      rx_message_chs.data[7] = 0x08;
      break;

    case BRAKES11_ID:
      // PQ Bremse_11 (0x5B7) - extended brake frame.  Static in standalone.
      rx_message_chs.data[0] = 0x00;
      rx_message_chs.data[1] = 0xC0;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0x00;
      break;

    case mSysteminfo_1:
      // PQ Systeminfo_1 (0x5D0) - gateway vehicle-identity broadcast.  Static.
      rx_message_chs.data[0] = 0x00;
      rx_message_chs.data[1] = 0x24;
      rx_message_chs.data[2] = 0x35;
      rx_message_chs.data[3] = 0x0F;
      rx_message_chs.data[4] = 0x39;
      rx_message_chs.data[5] = 0x59;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0x00;
      break;

    case mKombi_2:
      // PQ Kombi_2 (0x420) - cluster temps.  Static in standalone.
      rx_message_chs.data[0] = 0x4C;
      rx_message_chs.data[1] = 0x86;
      rx_message_chs.data[2] = 0x85;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x30;
      rx_message_chs.data[6] = 0xFF;
      rx_message_chs.data[7] = 0x04;
      break;

    case mDiagnose_1:
      // PQ Diagnose_1 (0x7D0) - diagnostic timestamp broadcast.  Static.
      rx_message_chs.data[0] = 0x26;
      rx_message_chs.data[1] = 0xF2;
      rx_message_chs.data[2] = 0x03;
      rx_message_chs.data[3] = 0x12;
      rx_message_chs.data[4] = 0x70;
      rx_message_chs.data[5] = 0x19;
      rx_message_chs.data[6] = 0x25;
      rx_message_chs.data[7] = mDiagnose_1_counter;
      mDiagnose_1_counter++;
      if (mDiagnose_1_counter > 0x1F)
        mDiagnose_1_counter = 0;
      break;
    }
  }

  // edit the frames if configured as Gen5 (0CQ) - frames left
  // commented-out are so they can be re-enabled later if a required
  if (haldexGeneration == 50 || haldexGeneration == 52) // 0CQ + VAQ (clone base)
  {
    switch (rx_message_chs.identifier)
    {
    case ESP_18: // 0x135 - fixed response, static (transferred; gated off by default)
      rx_message_chs.data[0] = 0x00;
      rx_message_chs.data[1] = 0xC0;
      rx_message_chs.data[2] = 0x00;
      rx_message_chs.data[3] = 0x00;
      rx_message_chs.data[4] = 0x00;
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0x00;
      break;
    case ESP_19:
      fill_esp19_wheel_speeds(rx_message_chs.data);
      break;

    case GETRIEBE_11:
      rx_message_chs.data[0] = 0x00;                // checksum placeholder none affect
      rx_message_chs.data[1] = GETRIEBE_11_counter; // rolling - 0x00>0x0F
      rx_message_chs.data[2] = 0x00;                // Was 0xFE Torque intervention at the engine. Requests a short-term reduction or increase in torque from the ECU. This signal is only valid in combination with GE_MMom_Status (for MQB) or GE_MMom_Status_02 (for MLBevo).
      rx_message_chs.data[3] = 0xFE;                // Pre-control torque (anticipatory torque request) (GE_MMom_Vorhalt_02)
      rx_message_chs.data[4] = 0x00;                // Actual gear/range selected (5=P, 6=R, 7=N, 8=D, 9=S, 10=E, 13/14=T)
      rx_message_chs.data[5] = 0x00;                // Shift sequence state (0=idle, 1=shift in progress, etc.) - does not affect (>0x00)
      rx_message_chs.data[6] = 0x00;                // Power transmission status / clutch lock-up state - does not affect (>0x00)
      rx_message_chs.data[7] = 0x00;                // Target gear of current shift

      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_0AD); // for 0x0AD

      GETRIEBE_11_counter++;
      if (GETRIEBE_11_counter > 0x0F)
      {
        GETRIEBE_11_counter = 0;
      }
      break;

    case MOTOR_12:
      rx_message_chs.data[0] = 0x00;                                                    // checksum placeholder
      rx_message_chs.data[1] = MOTOR_12_counter;                                        // rolling - 0x70>0x7F
      rx_message_chs.data[2] = 0x00;                                                    // doesn't affect Negative available torque (maximum engine braking) (MO_Mom_neg_verfuegbar) - does not affect
      rx_message_chs.data[3] = 0x00;                                                    // doesn't affect sometimes 0xC0, sometimes 0x00 Static torque limit
      rx_message_chs.data[4] = 0x00;                                                    // doesn't affect sometimes 0x3A, somtimes 0x39 Dynamic torque limit
      rx_message_chs.data[5] = 0x64;                                                    // doesn't affect Vehicle speed signal quality bit (0x64=good, 0x00=bad).  True?
      rx_message_chs.data[6] = 0x0F;                                                    // Engine speed signal quality bit. Was 0xD4 - does affect.  Bool
      rx_message_chs.data[7] = get_lock_target_adjusted_value(MOTOR_12_counter, false); // Engine speed / RPM was 0xAE affects.  >30 slows down.  Only adds 5%. Was 0x10 - does affect

      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_0A8); // for 0x0A8

      MOTOR_12_counter++;
      if (MOTOR_12_counter > 0x7F)
      {
        MOTOR_12_counter = 0x70;
      }
      break;

    case MOTOR_11:
      // MQB Motor_11 (0x0A7) - dual packing selected by user "Fix Hunting" toggle.
      //   fixHunting == false : V3 packing - works on 554C/D/H and 554K @ 100% lock.
      //   fixHunting == true  : DBC-correct BPK packing - needed on 554K at partial
      //                         lock (60/40, 70/30) where V3 packing causes hunting.
      if (!fixHunting)
      {
        rx_message_chs.data[0] = 0x00;                                        // checksum placeholder
        rx_message_chs.data[1] = MOTOR_11_counter;                            // rolling - 0x40>0x4F
        rx_message_chs.data[2] = 0xFA;                                        // Raw target torque (unfiltered driver demand) (MO_Mom_Soll_Roh)
        rx_message_chs.data[3] = 0xFA;                                        // Actual total torque output (Actual total torque output)
        rx_message_chs.data[4] = 0x00;                                        // doesn't affect Total inertia torque component (MO_Mom_Traegheit_Summe)
        rx_message_chs.data[5] = 0xFA;                                        // Filtered target torque (MO_Mom_Soll_gefiltert)
        rx_message_chs.data[6] = get_lock_target_adjusted_value(0xFA, false); // (MO_Mom_Soll_01?).  Massive effect.  Was 0x78
        rx_message_chs.data[7] = get_lock_target_adjusted_value(0xFA, false); // massive effect.  Was 0x3E
      }
      else
      {
        // ---- BPK packing (Fix Hunting on; needed for 554K @ partial lock) ----
        fill_motor11_bpk(rx_message_chs.data, MOTOR_11_counter);
      }

      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_0A7); // for 0x0A7

      MOTOR_11_counter++;
      if (MOTOR_11_counter > 0x4F)
      {
        MOTOR_11_counter = 0x40;
      }
      break;

    case ESP_14:                               // ESP_14 0x08A
      rx_message_chs.data[0] = 0x00;           // checksum placeholder
      rx_message_chs.data[1] = ESP_14_counter; // rolling - 0x10>0x1F
      rx_message_chs.data[2] = 0x00;           // doesn't affect
      rx_message_chs.data[3] = 0x00;           // doesn't affect sometimes 0xC0, sometimes 0x00

      appliedTorque = get_lock_target_adjusted_value(0xFE, false);

      // BR_Vorg_*_Max is the operating-RANGE ceiling (a permission envelope), not
      // a torque request. Feeding it the CF-attenuated appliedTorque collapsed the
      // declared ceiling to ~60% and capped PWM below stock's ~80%. esp14_range_max
      // declares the full range scaled only by the RAW commanded lock fraction
      // (lock_target), gated by the same lock-active signal appliedTorque already
      // encodes (appliedTorque > 0 -> lock commanded).
      {
        const uint8_t rangeMax = esp14_range_max((uint8_t)lock_target, appliedTorque > 0);
        rx_message_chs.data[5] = rangeMax; // BR_Vorg_Quer_Max   - full range at full command
        rx_message_chs.data[7] = rangeMax; // BR_Vorg_Allrad_Max - full range at full command

        // BR_Vorg_*_Min launch-PWM floor (esp14MinFloorPct, 0 = unchanged). Shared
        // helper: floor % of full command through the learn-corrected path, clamped
        // strictly below Max so the Haldex keeps room to modulate.
        uint8_t esp14Floor = esp14_min_floor(esp14MinFloorPct, rangeMax);
        // Danger Zone: at a full 50:50 request only, pin Min to Max so the Haldex has
        // no modulation room and goes to full pump duty.
        if (dangerZoneEnabled && lock_target >= 100 && rangeMax > 1)
          esp14Floor = (uint8_t)(rangeMax - 1);
        rx_message_chs.data[4] = esp14Floor; // BR_Vorg_Quer_Min
        rx_message_chs.data[6] = esp14Floor; // BR_Vorg_Allrad_Min
      }
      // massive effects (4>7)

      if (haldexGeneration == 52)
      {
        // VAQ (bench 2026-09-17, see Gen5_0CQ_VAQ_frames10): the front lock
        // follows BR_Vorg_Quer_Min 1:1 in plain percent (0.4 %/bit) and only
        // while BR_Status_Quer_ESP >= 3. The Allrad bytes (b6/b7) are not read.
        const uint8_t quer = get_lock_target_adjusted_value(250, false);
        rx_message_chs.data[3] = quer ? 0x20 : 0x00; // 4 = ESP requests cross lock / 0 deactivated
        rx_message_chs.data[4] = quer;               // BR_Vorg_Quer_Min
        rx_message_chs.data[5] = quer;               // BR_Vorg_Quer_Max = Min
      }

      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_08A); // for 0x08A

      ESP_14_counter++;
      if (ESP_14_counter > 0x1F)
      {
        ESP_14_counter = 0x10;
      }
      break;

    case LWI_01:
      rx_message_chs.data[0] = 0x00;           // checksum placeholder
      rx_message_chs.data[1] = LWI_01_counter; // rolling - 0x10>0x1F
      rx_message_chs.data[2] = 0x01;           // LWI_SensorStatus
      rx_message_chs.data[3] = 0x00;           // LWI_Qbit_sub_daten
      rx_message_chs.data[4] = 0x00;           // LWI_Qbit_Lendradwiken
      rx_message_chs.data[5] = 0x00;           // LWI_lendradwinken
      rx_message_chs.data[6] = 0x00;           // LWI_lendradw_geschw
      rx_message_chs.data[7] = 0x00;           // LWI_lendradw_geschw Unit Degress of Arc per Second

      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_086); // for 0x086

      LWI_01_counter++;
      if (LWI_01_counter > 0x1F)
      {
        LWI_01_counter = 0x10;
      }
      break;

    case MOTOR_20:
      rx_message_chs.data[0] = 0x00;             // checksum
      rx_message_chs.data[1] = MOTOR_20_counter; // rolling - 0x00>0x0F && MO_Accelerator_Raw_Value_01!
      rx_message_chs.data[2] = 0x40;             // no affect MO_Accelerator_Raw_Value_01 sss
      rx_message_chs.data[3] = 0x40;             // no affect sometimes 0xC0, sometimes 0x00
      rx_message_chs.data[4] = 0x19;             // no affect sometimes 0x3A, somtimes 0x39
      rx_message_chs.data[5] = 0x59;             // no affect
      rx_message_chs.data[6] = 0x7E;             // no affect
      rx_message_chs.data[7] = 0xFE;             // no affect

      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_121); // for 0x121

      MOTOR_20_counter++;
      if (MOTOR_20_counter > 0x0F)
      {
        MOTOR_20_counter = 0x00;
      }
      break;

    case ESP_10:
      rx_message_chs.data_length_code = 8;                                    // DLC 8
      rx_message_chs.data[0] = 0x00;                                          // checksum placeholder
      rx_message_chs.data[1] = ESP_10_counter;                                // rolling - 0x00>0x0F
      rx_message_chs.data[2] = 0x01;                                          // no affect all these affect, find which one
      rx_message_chs.data[3] = 0x04;                                          // no effect sometimes 0xC0, sometimes 0x00
      rx_message_chs.data[4] = 0x00;                                          // no effect sometimes 0x3A, somtimes 0x39
      rx_message_chs.data[5] = 0x40;                                          // no effect
      rx_message_chs.data[6] = 0x00;                                          // no effect
      rx_message_chs.data[7] = 0xFF;                                          // this affects(!) - a good 40%.  Was 0xFF
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_116); // for 0x116

      ESP_10_counter++;
      if (ESP_10_counter > 0x0F)
      {
        ESP_10_counter = 0x00;
      }
      break;

    case ESP_05:                                                              // ESP_05 0x106
      rx_message_chs.data_length_code = 8;                                    // DLC 8
      rx_message_chs.data[0] = 0x00;                                          // checksum
      rx_message_chs.data[1] = ESP_05_counter;                                // rolling - 0x80>0x8F
      rx_message_chs.data[2] = 0x64;                                          // no effect
      rx_message_chs.data[3] = 0xC0;                                          // this affects(!) sometimes 0xC0, sometimes 0x00
      rx_message_chs.data[4] = 0x00;                                          // no effect sometimes 0x3A, somtimes 0x39
      rx_message_chs.data[5] = 0x00;                                          // no effect
      rx_message_chs.data[6] = 0xFD;                                          // no effect
      rx_message_chs.data[7] = 0x00;                                          // this affects(!) - on/off.  Was 0x10.  0x00 doesn't hurt
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_106); // for 0x106

      ESP_05_counter++;
      if (ESP_05_counter > 0x8F)
      {
        ESP_05_counter = 0x80;
      }
      break;

    case EPB_01:                               // EPB_01 0x104
      rx_message_chs.data_length_code = 8;     // DLC 8
      rx_message_chs.data[0] = 0x00;           // checksum
      rx_message_chs.data[1] = EPB_01_counter; // rolling - 0x30>0x3F - none affect
      rx_message_chs.data[2] = 0xA6;
      rx_message_chs.data[3] = 0x00; // sometimes 0xC0, sometimes 0x00
      rx_message_chs.data[4] = 0xE6; // sometimes 0x3A, somtimes 0x39
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0x31;
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_104); // for 0x104

      EPB_01_counter++;
      if (EPB_01_counter > 0x3F)
      {
        EPB_01_counter = 0x30;
      }
      break;

    case ESP_02:                                                              // ESP_02 0x10B
      rx_message_chs.data[0] = 0x00;                                          // checksum
      rx_message_chs.data[1] = ESP_02_counter;                                // rolling - 0x00>0x1F
      rx_message_chs.data[2] = 0x7E;                                          // doesn't effect one of these affects, find which one - doesn't affect
      rx_message_chs.data[3] = 0x0F;                                          // doesn't effect sometimes 0xC0, sometimes 0x00
      rx_message_chs.data[4] = 0x82;                                          // doesn't effect sometimes 0x3A, somtimes 0x39
      rx_message_chs.data[5] = 0x0C;                                          // doesn't effect rolling?
      rx_message_chs.data[6] = 0x40;                                          // doesn't efffect
      rx_message_chs.data[7] = 0x00;                                          // doesn't effect
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_101); // for 0x101

      ESP_02_counter++;
      if (ESP_02_counter > 0x1F)
      {
        ESP_02_counter = 0x00;
      }
      break;

    case ESP_21:
      rx_message_chs.data[0] = 0x00;           // checksum
      rx_message_chs.data[1] = ESP_21_counter; // rolling - 0x00>0x1F
      rx_message_chs.data[2] = 0x1F;           // in diagnosis? none affect
      rx_message_chs.data[3] = 0x80;           // sometimes 0xC0, sometimes 0x00
      rx_message_chs.data[4] = 0x00;           // sometimes 0x3A, somtimes 0x39
      rx_message_chs.data[5] = 0x00;
      rx_message_chs.data[6] = 0x00;
      rx_message_chs.data[7] = 0x00;
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_0fd); // for 0x0fd

      ESP_21_counter++;
      if (ESP_21_counter > 0x1F)
      {
        ESP_21_counter = 0x00;
      }
      break;

    case KOMBI_01:
      rx_message_chs.data[0] = 0x10; // angle of turn (block 011) low byte
      rx_message_chs.data[1] = 0x20; // checksum (0x20>0x2F)
      rx_message_chs.data[2] = 0x02; //
      rx_message_chs.data[3] = 0x00; //
      rx_message_chs.data[4] = 0x0C; //
      rx_message_chs.data[5] = 0x00; //
      rx_message_chs.data[6] = 0x00; //
      rx_message_chs.data[7] = 0x24; //
      break;

    case ESP_23:
      rx_message_chs.data[0] = 0x00;                                          // checksum placeholder no effect
      rx_message_chs.data[1] = ESP_23_counter;                                // ESP_23_counter;           // no effect B high byte
      rx_message_chs.data[2] = 0xBF;                                          // no effect C
      rx_message_chs.data[3] = 0x7F;                                          // no effect D
      rx_message_chs.data[4] = 0x00;                                          // rate of change (block 010)
      rx_message_chs.data[5] = 0x00;                                          // rate of change (block 010)
      rx_message_chs.data[6] = 0x7C;                                          // rate of change (block 010)
      rx_message_chs.data[7] = 0x78;                                          // rate of change (block 010)
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_5be); // for 0x5be

      ESP_23_counter++;
      if (ESP_23_counter > 0x1F)
      {
        ESP_23_counter = 0x00;
      }
      break;

    case Parkhilfe_04:
      rx_message_chs.data[0] = 0x00; // angle of turn (block 011) low byte
      rx_message_chs.data[1] = 0x00; // no effect B high byte
      rx_message_chs.data[2] = 0x00; // no effect C
      rx_message_chs.data[3] = 0x00; // no effect D
      rx_message_chs.data[4] = 0x00; // rate of change (block 010)
      rx_message_chs.data[5] = 0x00; // rate of change (block 010)
      rx_message_chs.data[6] = 0x00; // rate of change (block 010)
      rx_message_chs.data[7] = 0x24; // rate of change (block 010)
      break;

    case GATEWAY_72:
      rx_message_chs.data[0] = 0x50; //
      rx_message_chs.data[1] = 0x80; //
      rx_message_chs.data[2] = 0x00; //
      rx_message_chs.data[3] = 0x00; //
      rx_message_chs.data[4] = 0x05; //
      rx_message_chs.data[5] = 0x10; //
      rx_message_chs.data[6] = 0x01; //
      rx_message_chs.data[7] = 0x78; //
      break;

    case GETRIEBE_14:
      rx_message_chs.data[0] = 0x00; // Maximum possible acceleration (limited by gear/clutch)
      rx_message_chs.data[1] = 0x00; // Charisma drive programme selected (affects shift mapping)
      rx_message_chs.data[2] = 0x54; // Charisma system status
      rx_message_chs.data[3] = 0x24; // Drag/friction loss torque in transmission
      rx_message_chs.data[4] = 0x00; // Launch control active
      rx_message_chs.data[5] = 0x60; //
      rx_message_chs.data[6] = 0x01; //
      rx_message_chs.data[7] = 0x51; //
      break;

    case MOTOR_14:
      rx_message_chs.data[0] = 0x00;                                          // checksum
      rx_message_chs.data[1] = MOTOR_14_counter;                              // 0x10 to 0x1F
      rx_message_chs.data[2] = 0xE6;                                          // doesn't effect Start/stop system state (0=inactive, 1=stopping, 2=stopped, 3=restarting)
      rx_message_chs.data[3] = 0x01;                                          // this affects(!) on/off Restart event flag
      rx_message_chs.data[4] = 0xC8;                                          // doesn't effect Engine stop event flag
      rx_message_chs.data[5] = 0x80;                                          // doesn't effect
      rx_message_chs.data[6] = 0x00;                                          // doesn't effect
      rx_message_chs.data[7] = 0x80;                                          // doesn't effect
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_3be); // for 0x3be

      MOTOR_14_counter++;
      if (MOTOR_14_counter > 0x1F)
      {
        MOTOR_14_counter = 0x10;
      }
      break;

    case ESP_07:
      rx_message_chs.data[0] = 0x00;                                          // checksum
      rx_message_chs.data[1] = ESP_07_counter;                                // 0x20>0x2F
      rx_message_chs.data[2] = 0x00;                                          // one of these affects, find which one
      rx_message_chs.data[3] = 0x00;                                          // no effect
      rx_message_chs.data[4] = 0x00;                                          // no effect
      rx_message_chs.data[5] = 0x00;                                          // no efefct
      rx_message_chs.data[6] = 0x00;                                          // no effect
      rx_message_chs.data[7] = 0x00;                                          // no effect
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_392); // for 0x392

      ESP_07_counter++;
      if (ESP_07_counter > 0x1F)
      {
        ESP_07_counter = 0x00;
      }
      break;

    case ESP_29:
      rx_message_chs.data[0] = 0x00; //
      rx_message_chs.data[1] = 0x20; // checksum (0x20>0x2F)?  Not in Savvy
      rx_message_chs.data[2] = 0x59; //
      rx_message_chs.data[3] = 0x00; //
      rx_message_chs.data[4] = 0x00; //
      rx_message_chs.data[5] = 0x00; //
      rx_message_chs.data[6] = 0x00; //
      rx_message_chs.data[7] = 0x00; //
      break;

    case MOTOR_07:
      rx_message_chs.data[0] = 0xA0; // no effect from any
      rx_message_chs.data[1] = 0x5A; //
      rx_message_chs.data[2] = 0x56; //
      rx_message_chs.data[3] = 0xA3; //
      rx_message_chs.data[4] = 0x80; //
      rx_message_chs.data[5] = 0xA0; //
      rx_message_chs.data[6] = 0x59; //
      rx_message_chs.data[7] = 0x01; //
      break;

    case CHARISMA_01:
      rx_message_chs.data[0] = 0x00; // CHA_Target_Driving_Program_AGA & CHA_Target_Driving_Prior_ESP
      rx_message_chs.data[1] = 0x00; // CHA_Target_Driving_Pri_Freewheel & void
      rx_message_chs.data[2] = 0x22; // CHA_Target_Driving_Program_MO & CHA_Target_Driving_Program_GE
      rx_message_chs.data[3] = 0x02; // CHA_Target_Driving_PR_ALR (inc. AWD) & CHA_Target_Driving_Program_MO_BZS
      rx_message_chs.data[4] = 0x02; // CHA_Target_Driving_Project_DR & CHA_Target_Driving_Prior_VAQ
      rx_message_chs.data[5] = 0x20; // CHA_Target_Driving_PR_AFS & CHA_Target_Driving_Program_RGS
      rx_message_chs.data[6] = 0x02; // CHA_Target_Driving_Price_EPS & CHA_Target_Driving_Principal_ACC
      rx_message_chs.data[7] = 0x02; // CHA_Target_Driving_Prior_SAK & CHA_Target_Driving_Program_MO_StSt
      break;

    case SYSTEMINFO_01:
      rx_message_chs.data[0] = 0x84; //
      rx_message_chs.data[1] = 0x3C; //
      rx_message_chs.data[2] = 0x00; //
      rx_message_chs.data[3] = 0x7F; //
      rx_message_chs.data[4] = 0x14; //
      rx_message_chs.data[5] = 0x00; //
      rx_message_chs.data[6] = 0x00; //
      rx_message_chs.data[7] = 0x00; //
      break;

    case MOTOR_CODE_01:
      rx_message_chs.data[0] = 0x00;                                          // checksum
      rx_message_chs.data[1] = MOTOR_CODE_01_counter;                         // rolling (10>1F)
      rx_message_chs.data[2] = 0x2B;                                          //
      rx_message_chs.data[3] = 0x53;                                          //
      rx_message_chs.data[4] = 0x14;                                          //
      rx_message_chs.data[5] = 0x14;                                          //
      rx_message_chs.data[6] = 0xD7;                                          //
      rx_message_chs.data[7] = 0x24;                                          //
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_641); // for 0x641

      MOTOR_CODE_01_counter++;
      if (MOTOR_CODE_01_counter > 0x1F)
      {
        MOTOR_CODE_01_counter = 0x10;
      }
      break;

    case ESP_20:
      rx_message_chs.data[0] = 0x00;                                          // checksum
      rx_message_chs.data[1] = ESP_20_counter;                                // rolling (30>3F)
      rx_message_chs.data[2] = 0x2B;                                          // no effect C
      rx_message_chs.data[3] = 0x10;                                          // no effect D
      rx_message_chs.data[4] = 0x00;                                          //
      rx_message_chs.data[5] = 0x00;                                          //
      rx_message_chs.data[6] = 0xE2;                                          //
      rx_message_chs.data[7] = 0x79;                                          // BR_Tire circumference
      rx_message_chs.data[0] = calcChecksum(rx_message_chs.data, ID_SEQ_65d); // for 0x65d

      ESP_20_counter++;
      if (ESP_20_counter > 0x3F)
      {
        ESP_20_counter = 0x30;
      }
      break;

    case DIAGNOSE_01:
      rx_message_chs.data[0] = 0x30; //
      rx_message_chs.data[1] = 0x4D; //
      rx_message_chs.data[2] = 0x58; //
      rx_message_chs.data[3] = 0xA2; //
      rx_message_chs.data[4] = 0x89; //
      rx_message_chs.data[5] = 0x85; //
      rx_message_chs.data[6] = 0x3F; // 0x3F OR 0xBF? (3F, then BF, then 3F, then BF...)
      rx_message_chs.data[7] = 0x30; // 2D, then 2D, then 2E, then 2E, then 2F, then 2F... roll over? When?
      break;

    case KOMBI_02:
      rx_message_chs.data[0] = 0x4D; // no effect from any
      rx_message_chs.data[1] = 0x58; //
      rx_message_chs.data[2] = 0xF2; //
      rx_message_chs.data[3] = 0xEE; //
      rx_message_chs.data[4] = 0x04; //
      rx_message_chs.data[5] = 0x2B; //
      rx_message_chs.data[6] = 0x00; //
      rx_message_chs.data[7] = 0x78; //
      break;
    }
  }
}

// NVS init policy. Pure decision over two booleans, no Arduino/NVS symbols, so
// the readEEP first-run/migrate/seed branch is host-tested under env:native.
// See include/OpenHaldexC6_Calculations.h.
EepInitAction eeprom_init_action(bool new_ns_seeded, bool legacy_ns_has_data)
{
  // Already seeded on the canonical namespace -> nothing to migrate or write,
  // just load. Checked first so a seeded device never re-reads legacy data.
  if (new_ns_seeded)
  {
    return EEP_LOAD_EXISTING;
  }
  // Not seeded but the de-facto legacy namespace holds a prior install's
  // settings -> migrate them forward so existing devices keep their config.
  if (legacy_ns_has_data)
  {
    return EEP_MIGRATE_LEGACY;
  }
  // First ever run on a blank device -> write the compiled defaults.
  return EEP_SEED_DEFAULTS;
}

// Boot-time mapping from the persisted lastMode byte to the runtime drive mode.
// Valid stored values are 0..5; anything else (e.g. a haldexGeneration number
// like 41/50/51 that a prior bug wrote into lastMode) is not a real mode and
// falls back to MODE_FWD, matching the original boot switch's default arm.
openhaldex_mode_t mode_from_last_mode(uint8_t last_mode)
{
  switch (last_mode)
  {
  case 0:
    return MODE_STOCK;
  case 1:
    return MODE_FWD;
  case 2:
    return MODE_5050;
  case 3:
    return MODE_6040;
  case 4:
    return MODE_7525;
  case 5:
    return MODE_EXPERT;
  default:
    return MODE_FWD;
  }
}

// Setting the haldex generation must leave the stored drive mode untouched -
// they are separate namespaces (generation 1/2/4/41/50/51 vs mode 0..5). The
// generation argument is intentionally unused: it exists so the seam documents
// exactly which write path this guards, and so a test can pass generation values
// and assert the returned mode is unchanged. Reintroducing the old
// `lastMode = generation` bug means editing this return, which reddens the test.
uint8_t last_mode_after_generation_change(uint8_t current_last_mode, int generation)
{
  (void)generation;
  return current_last_mode;
}

// Learn-table lookup. Returns the smallest index i in 0..100 with table[i] >=
// target - the lowest correction factor whose learned engagement meets the
// requested lock target, matching the previous inline loop. When NO learned
// entry meets target (more lock requested than was ever learned) we clamp to the
// index of the MAXIMUM learned engagement, i.e. the CF that produced the most
// lock the sweep ever actually measured - not a hardcoded 100.
//
// Why not 100: the table is only required to have one nonzero entry to be
// "valid", so a light-load or interrupted sweep can top out well below 100 (e.g.
// 25% engagement, or entries only up to CF 30 with the rest still 0). Returning
// 100 then commands CF=100 -> value * 100 / 100 = the full frame value (full
// bpkCeilingNm) for a target the car never learned - the stuck-at-100% field
// symptom, and the exact inverse of the old fall-through-to-0 bug. Returning the
// argmax commands the largest lock the sweep proved reachable and never
// extrapolates past learned data. An all-zero table yields index 0 (zero lock,
// safe) - table validity is enforced upstream. See the header.
uint8_t lookup_learn_correction_factor(const uint8_t* table, uint8_t target)
{
  uint8_t max_idx = 0;
  for (uint8_t i = 0; i <= 100; i++)
  {
    if (table[i] >= target)
    {
      return i;
    }
    if (table[i] > table[max_idx])
    {
      max_idx = i; // track highest learned engagement while scanning
    }
  }
  return max_idx;
}

// See OpenHaldexC6_Calculations.h for the rationale. Median-of-window + monotonic
// clamp so a lone pump-overshoot or dropout frame cannot poison the learn table.
uint8_t learn_reduce_samples(const uint8_t* samples, uint8_t n, uint8_t prev_recorded)
{
  if (n == 0 || samples == nullptr)
  {
    return prev_recorded; // nothing sampled - hold the previous value
  }

  // Insertion sort a bounded local copy (learn windows are small, <= 32 samples).
  // No dynamic allocation, no Arduino symbols, so this stays host-testable.
  if (n > 32)
  {
    n = 32; // defensive cap - callers use ~8; never truncates a real learn window
  }
  uint8_t sorted[32];
  for (uint8_t i = 0; i < n; i++)
  {
    uint8_t v = samples[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && sorted[j] > v)
    {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = v;
  }

  // Lower median: for an even count this picks the lower of the two middles, so a
  // clean 50/50 split between a spike cluster and the true value still rejects the
  // spike (only a strict majority of spiked frames - i.e. sustained, real - wins).
  const uint8_t median = sorted[(n - 1) / 2];

  // Engagement can only rise with CF, so never record below the previous CF. This
  // keeps lookup_learn_correction_factor coherent and glazes an all-zero (dropout)
  // window back to the last good reading in one place.
  return (median < prev_recorded) ? prev_recorded : median;
}

// Learn-sweep finalization. See the header for the contract; kept free of
// Arduino/FreeRTOS symbols so the native tests exercise this exact code.
uint8_t learn_finalize(uint8_t* table, bool* valid,
                       const uint8_t* backup, bool backup_valid,
                       bool cancelled, bool speed_aborted, uint8_t current_step)
{
  if (cancelled || speed_aborted)
  {
    // Interrupted sweep: restore the pre-learn calibration snapshotted by
    // startHaldexLearn, so a cancel at CF=5 doesn't destroy a good table and
    // silently revert the user to the default CF formula (and V3 packing).
    memcpy(table, backup, 101);
    *valid = backup_valid;
    return speed_aborted ? 103 : current_step; // 103 = aborted: vehicle moving
  }

  // Completed sweep: only mark valid if at least one non-zero engagement was
  // recorded.
  bool anyNonZero = false;
  for (uint8_t i = 0; i <= 100; i++)
  {
    if (table[i] > 0) { anyNonZero = true; break; }
  }
  *valid = anyNonZero;
  return anyNonZero ? 101 : 102; // 101 = complete OK, 102 = complete but no data
}

// See OpenHaldexC6_Calculations.h for the full rationale. Single shared
// implementation of the ESP_14 BR_Vorg_*_Min launch-PWM floor so the standalone
// frame generator (OpenHaldexC6_StandaloneCAN.cpp) and the CAN-passthrough edit
// path (getLockData below) can never drift apart - both wrote the same block by
// hand. floor_pct 0 yields 0 (inherited Min=0). Otherwise the floor byte is a %
// of full command routed through get_lock_target_adjusted_value, so it gates to
// 0 whenever lock isn't commanded (off-throttle/FWD/coast), then clamped strictly
// below applied_torque (Max) so the Haldex keeps modulation headroom.
uint8_t esp14_min_floor(uint8_t floor_pct, uint8_t applied_torque)
{
  if (floor_pct == 0)
  {
    return 0;
  }
  const uint8_t floorByte = (uint8_t)((uint16_t)0xFE * floor_pct / 100);
  uint8_t minFloor = get_lock_target_adjusted_value(floorByte, false);
  if (minFloor >= applied_torque)
  {
    minFloor = (applied_torque > 0) ? (uint8_t)(applied_torque - 1) : 0;
  }
  return minFloor;
}

// See OpenHaldexC6_Calculations.h for the full rationale. The ESP_14 Max byte is
// a PERMISSION envelope (how much operating range the Haldex may use), NOT a
// torque request, so it must not be routed through the correction_factor that
// translates lock_target into an engagement byte - that collapsed the ceiling to
// ~60% and capped PWM. Declare the full 0xFE range scaled only by the RAW
// commanded lock fraction: full command -> full range, partial command -> partial
// range. Gated to 0 by the caller-supplied lock_active. (uint16 product 0xFE*100
// = 25400 stays clear of overflow.)
uint8_t esp14_range_max(uint8_t frac_pct, bool lock_active)
{
  if (!lock_active)
  {
    return 0;
  }
  if (frac_pct >= 100)
  {
    return 0xFE; // full command -> full declared range (the launch-authority lever)
  }
  return (uint8_t)((uint16_t)0xFE * frac_pct / 100);
}

// Scale a received Haldex engagement byte into a 0..100 percentage.
// Pure integer math, no Arduino symbols, so it compiles and tests on host and
// the wire result for valid in-window frames is byte-identical to the previous
// Arduino map(raw, in_min, in_max, 0, 100) call. Unlike map(), this never
// extrapolates: a raw byte below in_min or above in_max is clamped first, so the
// uint8_t result can never wrap (e.g. -1 -> 255) or exceed 100.
uint8_t scale_haldex_engagement(uint8_t raw, uint8_t in_min, uint8_t in_max)
{
  // Defensive: a non-positive input span has no meaningful scale; fail to 0
  // rather than divide by zero or by a negative span.
  if (in_max <= in_min)
  {
    return 0;
  }

  // Clamp into [in_min, in_max] BEFORE scaling so we interpolate, never
  // extrapolate. Below-window -> in_min (0%), above-window -> in_max (100%).
  int value = raw;
  if (value < in_min)
  {
    value = in_min;
  }
  else if (value > in_max)
  {
    value = in_max;
  }

  // Same arithmetic as Arduino map() with out_min=0, out_max=100. int math keeps
  // the (value - in_min) * 100 product (max 25000) well clear of any overflow.
  int scaled = (value - (int)in_min) * 100 / ((int)in_max - (int)in_min);

  // The clamp above already bounds scaled to 0..100; constrain again so the
  // post-condition (0..100, no wrap) holds by construction.
  if (scaled < 0)
  {
    scaled = 0;
  }
  else if (scaled > 100)
  {
    scaled = 100;
  }
  return (uint8_t)scaled;
}

// Auth policy. The WiFi AP password is the single auth boundary. Pure pointer
// logic, no Arduino/NVS symbols, so it compiles under env:native and the
// provisioning/injection decision lives in one reviewable, host-tested place.
// See include/OpenHaldexC6_Calculations.h.
bool wifi_password_provisioned(const char* ap_pw)
{
  // A WPA2 AP password must be >= 8 chars. Anything shorter (including "" / NULL)
  // leaves the AP open, so the device is treated as unprovisioned and the
  // dashboard forces the first-run password page. Count without <cstring> so the
  // predicate stays dependency-free under env:native.
  if (ap_pw == nullptr)
  {
    return false;
  }
  size_t n = 0;
  while (ap_pw[n] != '\0')
  {
    if (++n >= 8)
    {
      return true;
    }
  }
  return false;
}

// See include/OpenHaldexC6_Calculations.h for the full rationale. Matches the
// request path (up to any '?' query string, case-insensitively) against the
// well-known phone-OS connectivity-probe URLs. Kept dependency-free of Arduino
// so the URL set is host-tested; std::strncasecmp-free by hand-rolling the
// compare so it builds identically under env:native.
bool is_captive_probe(const char* path)
{
  if (path == nullptr)
  {
    return false;
  }

  // The exact request paths each OS hits to decide "is there internet here?".
  // Android: /generate_204 and /gen_204 (various Google/vendor probe hosts).
  // Apple:   /hotspot-detect.html and the /library/test/success.html variant.
  // Windows: /ncsi.txt and /connecttest.txt (NCSI).
  // Firefox: /canonical.html; some builds also request /success.txt.
  static const char *const kProbes[] = {
      "/generate_204",
      "/gen_204",
      "/hotspot-detect.html",
      "/library/test/success.html",
      "/ncsi.txt",
      "/connecttest.txt",
      "/success.txt",
      "/canonical.html",
  };

  for (const char *probe : kProbes)
  {
    size_t i = 0;
    bool match = true;
    for (; probe[i] != '\0'; ++i)
    {
      char c = path[i];
      // Lower-case the path char (ASCII); probe entries are already lower-case.
      if (c >= 'A' && c <= 'Z')
      {
        c = (char)(c + ('a' - 'A'));
      }
      if (c != probe[i])
      {
        match = false;
        break;
      }
    }
    // A match requires the path to end here or continue only with a query string,
    // so "/generate_204extra" does not match but "/generate_204?foo" does.
    if (match && (path[i] == '\0' || path[i] == '?'))
    {
      return true;
    }
  }
  return false;
}

// Pure bus-health predicate: any failure bit set means a fault.
// Plain arithmetic, no TWAI symbols, so it runs in the native test suite.
bool can_alerts_indicate_failure(uint32_t alerts, uint32_t failure_mask)
{
  return (alerts & failure_mask) != 0;
}

// Pure bus-recovery predicate: true when a recovered bit is set, meaning a bus
// that went off has finished recovery and can be restarted with twai_start_v2.
// Plain arithmetic, no TWAI symbols, so it runs on host.
bool can_alerts_indicate_recovered(uint32_t alerts, uint32_t recovered_mask)
{
  return (alerts & recovered_mask) != 0;
}

// Pure external-diagnostic-tool predicates - see the header for the rationale.
// is_external_diag_request_id matches the reserved ISO/VAG tester request ids;
// our own polling transmits on Bus 1, so any of these inbound on Bus 0 is a
// foreign scan tool. No TWAI symbols, so both run in the native suite.
bool is_external_diag_request_id(uint32_t can_id)
{
  return can_id == 0x7DFu || (can_id >= 0x700u && can_id <= 0x71Fu);
}

// Reserve 0 as the "never seen" sentinel: millis() returns 0 at boot and every
// rollover, so map only that single tick to 1 (<=1 ms error) before storing.
uint32_t external_diag_stamp(uint32_t now_ms)
{
  return now_ms == 0u ? 1u : now_ms;
}

// True while a tester was seen within timeout_ms. last_seen_ms == 0 means never
// seen (never written as a real stamp - see external_diag_stamp). (uint32_t)(now
// - last) is wrap-safe, so a millis() rollover during the window still reports
// the correct elapsed time.
bool external_diag_active(uint32_t last_seen_ms, uint32_t now_ms, uint32_t timeout_ms)
{
  return last_seen_ms != 0 && (uint32_t)(now_ms - last_seen_ms) < timeout_ms;
}

// HTTP request-body buffer ownership. Pure <cstdlib>/<cstring> logic,
// no Arduino/Async symbols, so the malloc-owned single-block contract is pinned
// by the env:native suite. See include/OpenHaldexC6_Calculations.h.
char* http_body_alloc(size_t total)
{
  // A zero-length body has no buffer to own; return nullptr so the caller falls
  // through to its empty-body path instead of holding a 1-byte allocation.
  if (total == 0)
  {
    return nullptr;
  }

  // Guard against total + 1 wrapping to zero (SIZE_MAX edge case from a
  // request-controlled Content-Length). malloc(0) returns a valid pointer on ESP32
  // but the NUL-write at index `total` would write past the allocation.
  if (total == SIZE_MAX)
  {
    return nullptr;
  }

  // One block of total + 1 bytes: the body plus a trailing NUL slot at index
  // `total`. A single malloc means ESPAsyncWebServer's free(_tempObject) matches
  // exactly and an aborted POST releases the whole thing in one call.
  char* buf = (char*)malloc(total + 1);
  if (buf == nullptr)
  {
    return nullptr; // allocation failed; caller treats as empty body
  }

  // Zero-fill so the NUL terminator at `total` (and any not-yet-written gap) is
  // already in place before chunks arrive.
  memset(buf, 0, total + 1);
  return buf;
}

void http_body_write_chunk(char* buf, const uint8_t* data, size_t len, size_t index, size_t total)
{
  // Refuse every malformed call: no buffer, no source, a zero-length chunk, or
  // an offset already at/after the terminator. Each is a no-op, never a write.
  if (buf == nullptr || data == nullptr || len == 0 || index >= total)
  {
    return;
  }

  // Truncate a chunk that would run past `total` so the NUL guard at index
  // `total` is never overwritten and we never write outside the allocation.
  size_t writable = total - index;
  if (len > writable)
  {
    len = writable;
  }

  memcpy(buf + index, data, len);
}

// ---- Gen5 (MQB) Haldex UDS live data ----------------------------------------
// Wire format and scaling recovered from the upstream V8.00.2 binary and
// confirmed against the author's V8.00.2 source. Pure byte/float arithmetic, no
// TWAI/Arduino symbols, so the decode that feeds the dashboard values is pinned
// on the host (test/test_uds). See include/OpenHaldexC6_Calculations.h.
int uds_parse_sf_rdbi(const uint8_t *data, uint8_t dlc, uint16_t did, uint8_t *out, uint8_t out_cap)
{
  if (data == nullptr || out == nullptr)
  {
    return -1;
  }
  if (dlc < 1 || (data[0] & 0xF0) != 0x00)
  {
    return -1; // no PCI byte, or not an ISO-TP single frame
  }
  const uint8_t sfLen = data[0] & 0x0F; // declared single-frame payload length
  if (sfLen < 3 || sfLen > 7)
  {
    return -1; // must at least carry 62 <DID_hi> <DID_lo>
  }
  if (dlc < (uint8_t)(sfLen + 1))
  {
    return -1; // frame shorter than its own declared length
  }
  if (data[1] != 0x62)
  {
    return -1; // not a positive ReadDataByIdentifier response (covers 0x7F NRC)
  }
  if (data[2] != (uint8_t)(did >> 8) || data[3] != (uint8_t)(did & 0xFF))
  {
    return -1; // response to a different DID
  }
  const uint8_t payloadLen = sfLen - 3;
  if (payloadLen > out_cap)
  {
    return -1; // caller's buffer too small - no partial copy
  }
  memcpy(out, &data[4], payloadLen);
  return payloadLen;
}

bool uds_temp_plausible(float degC)
{
  return degC >= -40.0f && degC <= 150.0f;
}

bool uds_scale_mqb_did(uint16_t did, const uint8_t *payload, uint8_t len, float &out)
{
  if (payload == nullptr)
  {
    return false;
  }

  switch (did)
  {
  case 0x0286: // terminal voltage, V
    if (len < 1) return false;
    out = payload[0] * 0.1f;
    return true;

  case 0x028D: // module temperature, degC
    if (len < 1) return false;
    out = (float)payload[0] - 55.0f;
    return true;

  case 0x2BF1: // clutch temperature, degC
  case 0x2BE4: // cooling fin temperature, degC
  {
    if (len < 2) return false;
    const uint16_t raw = (uint16_t)(((uint16_t)payload[1] << 8) | payload[0]);
    out = ((float)raw - 22767.0f) / 100.0f;
    return true;
  }

  case 0x2BE6: // clutch pump current, A
  case 0x2BE9: // clutch pump voltage, V
  {
    // Big-endian on the wire, unlike the little-endian temperature DIDs -
    // confirmed against the upstream V8.00.2 source (its poller reads
    // data[4]<<8 | data[5] for these two DIDs only).
    if (len < 2) return false;
    const uint16_t raw = (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]);
    out = raw * 0.001f;
    return true;
  }

  case 0x2BE7: // clutch PWM duty, %, raw byte
    if (len < 1) return false;
    out = payload[0];
    return true;

  default:
    return false; // unknown DID - caller ignores the frame
  }
}
