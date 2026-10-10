#pragma once

#include <OpenHaldexC6_defs.h>

// Learn Haldex table
extern uint8_t haldexLearnTable[101];
extern bool haldexLearnTableValid;
// Pre-sweep snapshot, restored on learn cancel/abort (see globals.cpp).
extern uint8_t haldexLearnTableBackup[101];
extern bool haldexLearnTableBackupValid;
extern volatile bool haldexLearnActive;
extern volatile bool haldexLearnCancel;
extern volatile uint8_t haldexLearnStep;
extern volatile uint8_t haldexLearnCF;

float get_lock_target_adjustment();

// Which force-mode value applies right now: 0..5 (Stock/FWD/5050/6040/7525/
// Expert) when an enabled force trigger's flag is active (priority picked by
// forceModesPriority), or -1 when no force mode applies. Used by
// get_lock_target_adjustment and by the inline gateway to detect "effective
// mode is Stock", which must mean untouched passthrough - never frame edits
// built from mirrored engagement (the stuck-at-100% feedback loop). Pure logic
// over the force-mode globals, host-testable.
int get_forced_mode_value();

// Slew one step of the lock-target rate limiter. Ramp times are milliseconds for
// a full 0<->100 travel: rising transitions take engage_ms, falling transitions
// take release_ms. 0 ms = instant in that direction (rising 0 is the historical
// instant lock-up; falling 0 is an instant release, replacing the old %/s scheme
// where release rate 0 meant "never releases" - a stuck-locked footgun on a car).
// A ramp of N ms moves 100000 * dt_s / N percent per step. Pure, host-testable.
float lock_rate_limit_step(float current, float target, uint16_t engage_ms, uint16_t release_ms, float dt_s);

// One-time migration of a persisted lock-ramp rate (%/s, the pre-ms unit) to the
// new full-travel time in milliseconds. rate <= 0 maps to 0 ms (instant);
// otherwise ms = round(100000 / rate), clamped to the 1000 ms slider ceiling
// (so a very slow legacy rate becomes the 1 s maximum). Pure, host-testable.
uint16_t lock_ramp_ms_from_rate(float rate_per_sec);

// v9 %/s lock-release rate (web + BLE, 5..500) <-> the ms-for-full-travel the
// ramp stores. See Calculations.cpp.
uint16_t lock_ramp_ms_from_pct_rate(uint16_t rate_per_sec);
uint16_t lock_pct_rate_from_ramp_ms(uint16_t ramp_ms);
float steering_curve_percent(float angle, const uint16_t *arr, const uint8_t *scale, uint8_t count);
void steering_taper_from_curve(const uint16_t *arr, const uint8_t *scale, uint8_t count,
                               uint16_t &start_deg, uint16_t &full_deg, uint8_t &floor_pct);
void steering_curve_from_taper(uint16_t start_deg, uint16_t full_deg, uint8_t floor_pct,
                               uint16_t arr[steeringArrayCount], uint8_t scale[steeringArrayCount]);

// Geometry-compensated per-corner wheel slip. Given the four raw ABS wheel-speed
// counts (any consistent unit; MQB 0x0B2 is 0.0075 km/h/LSB) in [FL, FR, RL, RR]
// order, the SIGNED steering-WHEEL angle in 0.1-deg units (negative = left,
// positive = right), the rack ratio (steering-wheel deg per road-wheel deg, e.g.
// ~15.6 for MQB), and the car geometry (wheelbase and front/rear track in mm),
// fill slip_out[0..3] with each corner's slip as whole-percent int8 (1%/LSB,
// clamped -100..+127). Slip is actual_i / expected_i - 1, where expected_i is the
// speed pure Ackermann geometry predicts for that corner's turn radius at the
// current steering angle, normalised so the four expected speeds share the
// measured mean. Positive => spinning faster than geometry allows (losing grip);
// negative => the reference/dragging corner. On a straight the radii are equal so
// it reduces to speed-vs-average. Returns false and zeroes slip_out when the mean
// speed is below min_speed_raw (too slow to trust) or an input is degenerate.
// LIMITATION: a relative method - if all four corners break loose equally it reads
// ~0 slip. Pure float/trig math, no Arduino/TWAI symbols, host-testable.
bool compute_corner_slip(const uint16_t wheel_raw[4], int16_t steer_wheel_tenths,
                         float steering_ratio, uint16_t wheelbase_mm,
                         uint16_t track_front_mm, uint16_t track_rear_mm,
                         uint16_t min_speed_raw, int8_t slip_out[4]);
static float get_expert_lock_target();
uint8_t get_lock_target_adjusted_value(uint8_t value, bool invert);
void getLockData(twai_message_t& rx_message_chs);
void startHaldexLearn();

// Blocking learn sweep (CF 0..100, 300 ms/step) - the body of the manual Learn
// task, shared with Long Learn. Must be called from a task. preHoldMs > 0 holds
// CF=0 first until the Haldex has released (or the hold times out) so residual
// engagement from a previous sweep does not lift the bottom of the table.
// Returns true when the sweep completed and recorded at least one non-zero
// engagement (i.e. haldexLearnTableValid was set).
bool runLearnSweep(uint32_t preHoldMs = 0);

// Gen5 (0CQ/VAQ) ESP_19 wheel-speed simulation, shared by standalone frame
// generation and normal-mode in-place editing (both call this so the two
// copies can't drift). Wheel speed MUST keep changing or the Haldex slowly
// disengages (found by trial and error on real hardware). A lock_target-
// proportional front/rear delta was tried here and made things worse on the
// car (lock faded then collapsed to 0%), so this stays the flat, proven
// dither. Fills data[0..7] and advances the shared counters.
void fill_esp19_wheel_speeds(uint8_t data[8]);

// Motor_11 (0x0A7) BPK packing, shared by standalone generation and normal-mode
// in-place editing so the two can't drift. Fills data[0..7] (byte 0 is left as
// a CRC placeholder for the caller) from the runtime BPK tunables in defs.h.
void fill_motor11_bpk(uint8_t data[8], uint8_t counter);

// Danger Zone is live this cycle: toggle on, full lock requested (> 99 %), not
// in a learn sweep. When true the Motor_11 packers use BPK packing with the
// ceiling raised to dangerZoneNm, whatever the Fix Hunting toggle says.
bool dangerZoneActive();

// Stores what the Motor_11 BPK packer computed this cycle, for the serial lab
// task to stream out. Called from both BPK code paths at the Motor_11 rate.
void bpkLogSample(uint16_t torqueNm, uint16_t istNm, uint16_t solfNm);

// ---- Serial lab (USB diagnostic harness) ------------------------------------
// Line-based control + telemetry over USB serial so a host script can drive
// ceiling/floor/lock/packing values and watch the Haldex respond in real time,
// instead of rebuilding firmware per experiment. See OpenHaldexC6_SerialLab.cpp.
void setupSerialLab();

// ---- Long Learn (automated frame-block bisection) --------------------------
// Scores a learn table for "smoothness": the Haldex may jump on its first
// engage step (e.g. 0 -> 30 %) but after that must climb without steps larger
// than LL_STEP_MAX and reach LL_REACH_MIN by CF 100.
struct LearnScore
{
    uint8_t reach;      // engagement at CF 100
    uint8_t maxStep;    // largest single-step rise AFTER the first engage step
    uint8_t engageCF;   // first CF with non-zero engagement (101 = never)
    uint8_t engageJump; // engagement recorded at engageCF
    uint8_t score;      // 0-100 composite used to rank configurations
    bool smooth;        // passes all three smoothness criteria
};
#define LL_REACH_MIN 90     // % engagement required at CF 100
#define LL_STEP_MAX 8       // largest tolerated single-step rise after engage
#define LL_ENGAGE_MAX_CF 60 // must have started engaging by this CF
#define LL_TOLERANCE 4      // minimum deviation (%) treated as "no change";
                            // raised to the measured reference-to-reference noise
#define LL_NOISE_MAX 10     // two all-on point reference reads further apart than
                            // this (worst point) are too noisy to judge blocks by
// CF points each block is read at - spread over the ramp so a block that only
// matters at part lock (a step at 20%, say) isn't missed on a clean 100%.
#define LL_NPTS 4
static const uint8_t LL_POINT_CF[LL_NPTS] = {20, 40, 70, 100};
#define LL_BPK_ACCEPT 90    // return% Long Learn treats as good enough before it
                            // starts changing BPK settings. 100% is the aim, but
                            // 90+ is accepted rather than chasing the last few
                            // points into the pressure-relief regime.
void scoreLearnTable(const uint8_t *table, LearnScore &out);

// Long Learn comparison seams (pure; used by longLearnTask in tasks.cpp).
// Worst |t - ref| over CF 0-100 and the mean signed difference.
void ll_compare_curves(const uint8_t *t, const uint8_t *ref, uint8_t &maxDev, int8_t &meanDelta);
// Same over the LL_NPTS point reads.
void ll_compare_points(const uint8_t *a, const uint8_t *ref, uint8_t &maxDev, int8_t &meanDelta);
// Verdict (LLB_*) for a read with block(s) off against the proven reference:
// within tol and smooth -> LLB_REMOVED; lower or not smooth -> LLB_NEEDED;
// higher -> LLB_HARMFUL (an effect, so still kept on).
uint8_t ll_judge(bool smooth, uint8_t maxDev, int8_t meanDelta, uint8_t tol);
// Effective Motor_11 BPK ceiling: the user's calibration, raised to the Danger
// Zone value while it is live, never above the 509 Nm the 10-bit field holds.
uint16_t bpk_effective_ceiling_nm(uint16_t ceilingNm, bool dangerActive, uint16_t dangerNm);

// Phase numbers are also sent over ESP-NOW (ohx status longPhase) - append only.
enum
{
    LL_IDLE = 0,
    LL_SWEEP,     // "Reference": all-on sweeps that must prove a smooth 100% first
    LL_BPK,       // "BPK Adjust" (Gen5 only): hunting/short -> BPK packing + torque ceiling
    LL_BLOCKS,    // "Testing Blocks": each candidate off on its own, full sweep, back on
    LL_FINAL,     // confirmation sweep on the final set
    LL_DONE,
    LL_CANCELLED,
    LL_FAILED     // see longLearnFailReason - previous state restored
};
enum
{
    LLF_NONE = 0,
    LLF_NO_DATA,    // a reference sweep got no Haldex feedback at all
    LLF_NOT_SMOOTH, // all blocks on did not give a smooth 100% - nothing to compare against
    LLF_NOISY       // the two point reference reads disagree by more than LL_NOISE_MAX
};
enum
{
    LLB_UNTESTED = 0, // candidate, not yet tested
    LLB_CORE,         // lock-driven block - always sent, never tested (unless Test All)
    LLB_NEEDED,       // off on its own read lower at the test points -> kept on
    LLB_REMOVED,      // off on its own made no difference -> left off
    LLB_HARMFUL       // off on its own read higher -> still kept on, flagged
};
enum
{
    LLS_BASELINE = 0, // all-on full sweep (first sweep / reference curve)
    LLS_POINTS,       // all-on LL_POINT_CF reference read (was LLS_FLOOR, unused)
    LLS_BLOCK,        // one candidate block off, LL_POINT_CF read
    LLS_FINAL,        // confirmation sweep
    LLS_BPK           // BPK packing / torque-ceiling candidate (Gen5 only)
};
struct LongLearnSweep
{
    uint8_t kind;     // LLS_*
    uint8_t bit;      // block bit under test (0xFF = n/a)
    uint8_t floorPct; // esp14MinFloorPct during the sweep
    uint16_t bpkNm;   // bpkCeilingNm during the sweep (Gen5 only; 0 elsewhere)
    uint8_t verdict;  // LLB_* for block sweeps, 1/0 smooth/reached-100 for the rest
    uint8_t maxDev;   // block: worst |read - ref| over the points; final: over CF 0-100
    int8_t meanDelta; // block/final: mean (read - reference), sign = direction
    uint8_t pts[LL_NPTS]; // point reads (LLS_POINTS / LLS_BLOCK), else 0
    LearnScore s;     // point reads: reach = 100% point, engage = first non-zero point
};
#define LL_MAX_SWEEPS 80
#define LL_NOTES_LEN 200

extern volatile bool longLearnActive;
extern volatile bool longLearnCancel;
extern volatile bool longLearnSpeedAborted; // run stopped because the car moved (learn interlock)
extern volatile uint8_t longLearnPhase;      // LL_*
extern volatile uint8_t longLearnSweepIdx;   // sweeps completed so far
extern volatile uint8_t longLearnSweepTotal; // estimated total (exact after the floor phase)
extern volatile int16_t longLearnCurrentBit; // block being tested (-1 = none)
extern uint8_t longLearnGenIdx;              // FE_GEN_* the run belongs to
extern uint8_t longLearnGeneration;          // haldexGeneration the run belongs to
extern bool longLearnTestAll;                // also bisect the default (core) blocks
extern uint8_t longLearnBlockResult[64];     // LLB_* per bit
extern LearnScore longLearnBaseline;
extern LearnScore longLearnFinal;
extern bool longLearnBaselineValid;
extern bool longLearnFinalValid;
extern uint8_t longLearnFloorStart;  // esp14MinFloorPct before the run
extern uint8_t longLearnFloorResult; // esp14MinFloorPct chosen by the run
extern uint64_t longLearnMaskStart;  // active mask before the run (restored on cancel)
extern uint16_t longLearnBpkStart;   // bpkCeilingNm before the run (Gen5; restored on cancel/failure)
extern bool longLearnBpkAdjusted;    // true if the BPK-adjust phase changed packing or ceiling
extern uint8_t longLearnFailReason;  // LLF_* when longLearnPhase == LL_FAILED
extern uint8_t longLearnNoise;       // worst |refA - refB| over CF 0-100
extern uint8_t longLearnTol;         // deviation threshold actually used (max(LL_TOLERANCE, noise))
extern bool longLearnInteraction;    // the "no effect" blocks off TOGETHER changed the curve -> all left on
extern LongLearnSweep longLearnSweeps[LL_MAX_SWEEPS];
extern uint8_t longLearnSweepCount;
extern uint32_t longLearnStartMs;
extern uint32_t longLearnEndMs;
extern char longLearnNotes[LL_NOTES_LEN + 1]; // user chassis/car notes (exported with the report)

bool startLongLearn(bool testAll); // false if already running / not a gated generation

// Steering-angle lock-scale telemetry (for the engagement-split display).
bool steering_scale_is_active();
uint8_t steering_scale_requested_pct();
uint8_t steering_scale_result_pct();

// Scale a received Haldex engagement byte to a 0..100 percentage.
// Replaces a raw Arduino map(raw, in_min, in_max, 0, 100) at the CAN parse site:
// for valid in-window frames the result is identical, but raw bytes outside
// [in_min,in_max] are clamped (below -> 0, above -> 100) instead of extrapolated,
// so the uint8_t result can never wrap or exceed 100. Returns 0 if in_max<=in_min.
uint8_t scale_haldex_engagement(uint8_t raw, uint8_t in_min, uint8_t in_max);

// Learn-table lookup. Returns the smallest index i in 0..100 with
// table[i] >= target - the lowest correction factor whose learned engagement
// meets the requested lock target. When NO entry meets target (more lock
// requested than was ever learned), returns the index of the MAXIMUM learned
// engagement (argmax) - the CF that produced the most lock the sweep actually
// measured. NOT 100: a hardcoded 100 commands the full frame value for a target
// the car never learned (the stuck-at-100% symptom on a light-load or partial
// learn), the inverse of the old loop's fall-through 0. argmax never
// extrapolates past learned data; an all-zero table yields 0. Pure array math,
// host-testable.
uint8_t lookup_learn_correction_factor(const uint8_t* table, uint8_t target);

// Reduce one CF settle window's burst of engagement samples into the value
// recorded in the learn table. Robust against the two artifacts seen on a live
// car during a learn scan:
//   * pump overshoot - near lock-up the Haldex ECU's own duty loop briefly
//     drives full PWM, so a single feedback frame decodes to ~100 before it
//     backs off (audible pump oscillation above ~60% engagement);
//   * CAN dropout - a momentary 0 frame.
// The old loop took ONE raw sample at the end of the settle and only glazed a 0,
// so a lone overshoot frame was written verbatim; scale_haldex_engagement clamps
// anything at/above the window top to a clean 100, and lookup_learn_correction_factor
// (smallest CF whose learned engagement meets target) then collapses the whole
// upper map onto that CF - one spike poisons the calibration.
// This takes the median of the window so a lone spike or dropout cannot move the
// recorded value, then clamps monotonic non-decreasing against prev_recorded:
// learned engagement can only rise with CF, so this both keeps the lookup
// coherent and subsumes the dropout-glaze (an all-zero window can't pull the
// recorded value below the previous CF's). n == 0 returns prev_recorded. Uses a
// lower median so an even spike/clean split still rejects the spike. Pure,
// host-testable (no Arduino/TWAI symbols).
uint8_t learn_reduce_samples(const uint8_t* samples, uint8_t n, uint8_t prev_recorded);

// Learn-sweep finalization seam, called by haldexLearnTask under stateMutex.
// Interrupted sweep (cancel or speed abort): restores the pre-sweep snapshot
// into table/valid and returns 103 for a speed abort or current_step for a
// plain cancel. Completed sweep: publishes valid = any non-zero entry and
// returns 101 (complete) or 102 (complete but no data). Pure apart from the
// caller-supplied buffers, so the native tests pin the shipped restore logic
// rather than a mirrored copy (same seam pattern as lpCanActive).
// True when the learn interlock allows a sweep: speed (km/h) at or below
// learnMaxSpeed. The sweep commands up to full lock, so it is refused in a
// moving car at start and aborted if the car moves mid-sweep.
bool learn_speed_ok(uint16_t speed_kmh);

uint8_t learn_finalize(uint8_t* table, bool* valid,
                       const uint8_t* backup, bool backup_valid,
                       bool cancelled, bool speed_aborted, uint8_t current_step);

// ESP_14 (0x08A) BR_Vorg_*_Min launch-PWM floor. Shared between the standalone
// frame generator and the CAN-passthrough edit path so the two never drift.
// floor_pct is esp14MinFloorPct (0..100); applied_torque is the ESP_14 Max byte
// already computed for the same frame. Returns 0 when floor_pct is 0 (inherited
// Min=0). Otherwise the floor is floor_pct% of full command, routed through
// get_lock_target_adjusted_value (gates to 0 off-throttle/FWD/coast) and clamped
// strictly below applied_torque so the Haldex keeps modulation headroom.
uint8_t esp14_min_floor(uint8_t floor_pct, uint8_t applied_torque);

// ESP_14 (0x08A) BR_Vorg_*_Max operating-range ceiling. This byte declares how
// much of the clutch's operating range the ESP permits the Haldex to use - a
// PERMISSION envelope, not a torque request. Upstream fed it the same
// CF-attenuated byte as the MOTOR_11 torque request (get_lock_target_adjusted_value),
// which collapsed the declared ceiling to ~60% (the correction_factor tops out
// near 60 at full lock), so the Haldex never opened its pump duty past ~60% PWM
// while stock reached ~80%. This helper decouples the ceiling from the CF
// translation: it declares the full 0xFE range scaled by the RAW commanded lock
// fraction (frac_pct = lock_target 0..100), so 30% lock still declares ~30% range
// (keeping the sane part of the original partial-lock intent) but 100% command
// declares the FULL range. Gating is caller-supplied via lock_active: pass the
// same lock gate the rest of the frame uses (lock_enabled() / not FWD / not
// stock-passthrough) so the ceiling collapses to 0 whenever lock isn't commanded.
// Returns 0 when lock_active is false. Pure arithmetic given its inputs, so it is
// host-testable in isolation.
uint8_t esp14_range_max(uint8_t frac_pct, bool lock_active);

// Speed-disengage gate. Returns true when lock is permitted at the
// given speed: the vehicle must be at or ABOVE disengage_under AND at or BELOW
// disengage_above. A bound of 0 disables that side (0 = "no lower/upper cut").
// This replaces the previous inverted, default-defeated expression:
//   (under==0) || (speed<=under) || (speed>=above)
// which was always true because disengage_above defaults to 0 (speed>=0), and
// which disengaged lock in the wrong band. In v9 every lock path consults it (passive modes, Expert and the
// force-mode triggers, via lock_enabled()), so a disengage cut-off holds however
// lock was requested. (Edge v8 let Expert and force modes bypass it.) Pure integer logic, no Arduino
// symbols, host-testable.
bool speed_disengage_ok(uint16_t speed, uint16_t disengage_under, uint16_t disengage_above);

// Hysteretic (Schmitt-trigger) form of the speed-disengage gate. speed_disengage_ok
// is a bare comparison with no memory, so a speed resting on disengage_under or
// disengage_above and dithering by one unit flips the gate every frame - the lock
// target then hunts 0 <-> target frame-to-frame (see getLockData). This adds a
// deadband: entry is sharp (identical to speed_disengage_ok), but once enabled the
// gate only drops out after speed moves `hysteresis` units PAST the bound
// (speed < disengage_under - hysteresis, or speed > disengage_above + hysteresis).
// Asymmetric on purpose: engage promptly, be reluctant to disengage, so sensor
// noise at a steady cruise cannot chatter the clutch. hysteresis 0 reduces exactly
// to speed_disengage_ok regardless of currently_enabled, so a zero band is a
// behaviour-preserving no-op. currently_enabled is the gate's previous output.
// A bound of 0 disables that side (no cut), matching speed_disengage_ok. The band
// width is a bench-tuned calibration against real speed-signal noise, so this seam
// is proven here but left unwired into getLockData until it can be tuned on the car.
// Pure integer logic, no Arduino symbols, host-testable.
bool disengage_gate_hysteresis(uint16_t speed, uint16_t disengage_under, uint16_t disengage_above,
                               uint16_t hysteresis, bool currently_enabled);

// Integrating debounce for a force-mode CAN flag (TC/ESP "passiv", hazard lights,
// external button). Today these are read with a bare bitRead every chassis frame
// (OpenHaldexC6_can.cpp:335/355/385/443) with no memory, so a single-frame edge -
// a one-frame ESP "passiv" blip, a stray hazard bit - flips force mode for exactly
// as long as the source bit is set, and (with the release ramp off, the default)
// snaps the lock target for that cycle unfiltered. This requires a raw edge to
// persist for `threshold` consecutive frames before it is accepted; a shorter blip
// is rejected and the debounced state holds. `counter` is caller-owned scratch
// state (one per flag) counting consecutive frames the raw bit has disagreed with
// the debounced state; it is reset to 0 whenever raw agrees, so a blip must be
// sustained, not merely cumulative. threshold <= 1 accepts every edge immediately,
// reducing exactly to today's undebounced bitRead, so wiring this in with a default
// threshold of 0 is a behaviour-preserving no-op. The frame count needed to reject
// real-world bus noise without adding felt latency to a genuine ESP/hazard event is
// a bench-tuned calibration, so this seam is proven here but left unwired until it
// can be tuned on the car. Pure boolean/counter logic, no Arduino/TWAI symbols,
// host-testable.
bool debounce_force_flag(bool raw, bool debounced, uint8_t &counter, uint8_t threshold);

// True when arr[0..count-1] is strictly ascending (each element greater than the
// previous). The expert 2D map (get_expert_lock_target) assumes ascending speed
// and throttle axes; a non-monotonic axis makes the interpolation bracket search
// pick the wrong pair and silently mis-interpolate. tuneIncoming rejects a tune
// whose axes fail this. count 0 or 1 is vacuously ascending. Pure, host-testable.
bool is_strictly_ascending_u16(const uint16_t* arr, uint8_t count);

// Auth policy. The WiFi AP password is the single auth boundary: anyone who can
// reach the web server or analyzer port has already joined the WPA2-protected AP.
//
// wifi_password_provisioned: true only when a WPA2-length AP password (>= 8
// chars) is set. Until then the AP runs open and the dashboard forces the
// first-run password page. This same predicate gates the fail-closed
// analyzer-injection policy: host->device CAN transmit on the analyzer port is
// authorized only when the AP is protected; passive sniffing is unaffected.
// Pure pointer logic, no Arduino/NVS/TWAI symbols, so the decision lives in one
// host-tested place.
bool wifi_password_provisioned(const char* ap_pw);

// is_captive_probe: true when a request path is one of the well-known phone-OS
// "is there internet on this WiFi?" connectivity checks (Android generate_204,
// Apple hotspot-detect, Windows NCSI/connecttest, Firefox canonical). The device
// is an offline car AP with no uplink, so the web server answers these with a
// bare 404 (no 204, no redirect) to signal "no internet, not a captive portal",
// which lets the phone keep its own cellular data alive for messages/calls/OTA
// downloads instead of routing everything through us and going dark. Case- and
// query-string-insensitive path match; pure const char* logic so the URL set is
// host-tested in one place. Null path returns false.
bool is_captive_probe(const char* path);

// Pure CAN bus-health predicates. Plain arithmetic ((alerts & mask) != 0), no
// TWAI driver symbols, so the always-on failure/recovery decision that drives
// isBusFailure is host-testable. can_alerts_indicate_failure: true when any
// failure bit is set. can_alerts_indicate_recovered: true when a bus that went
// off has finished recovery and can be restarted with twai_start_v2.
bool can_alerts_indicate_failure(uint32_t alerts, uint32_t failure_mask);
bool can_alerts_indicate_recovered(uint32_t alerts, uint32_t recovered_mask);

// External-diagnostic-tool detection. Pure integer logic, no Arduino/TWAI
// symbols, so the auto-pause decision that keeps our UDS polling off a busy
// diagnostic bus is host-testable.
// is_external_diag_request_id: true when a chassis-bus (Bus 0) CAN id is an
// ISO/VAG diagnostic tester request - 0x7DF (OBD-II functional request) or the
// 0x700-0x71F physical-tester range. Our own UDS polling transmits on Bus 1, so
// any of these seen inbound on Bus 0 means a real scan tool (VCDS/ODIS/OBD) is
// on the bus. TP2.0 setup id 0x200 is deliberately excluded: this fork does not
// run TP2.0, and 0x200 sits in the normal MQB broadcast-data id region where it
// would false-trigger.
bool is_external_diag_request_id(uint32_t can_id);
// external_diag_stamp: normalize a millis() reading for storage so 0 stays
// reserved as the "never seen" sentinel. millis() legitimately returns 0 at boot
// and at every 32-bit rollover; a raw 0 stored as last_seen would be misread as
// "never seen". Maps only the single 0 tick to 1 (<=1 ms error); all other
// values pass through. The stamp site must use this before writing last_seen_ms.
uint32_t external_diag_stamp(uint32_t now_ms);
// external_diag_active: true while a tester was seen within timeout_ms.
// last_seen_ms == 0 means never seen (never a real stamp - see external_diag_stamp).
// Uses wrap-safe unsigned subtraction so it survives the millis() rollover.
bool external_diag_active(uint32_t last_seen_ms, uint32_t now_ms, uint32_t timeout_ms);

// NVS init policy. Pure decision over two booleans - no Arduino/NVS symbols -
// so the readEEP first-run/migrate/seed branch lives in one host-testable place.
// Given whether the canonical namespace is already seeded and whether the
// de-facto legacy namespace still holds a previous device's data:
//   * new_ns_seeded            -> EEP_LOAD_EXISTING (normal run, just load)
//   * legacy data, not seeded  -> EEP_MIGRATE_LEGACY (copy legacy -> new, mark seeded)
//   * neither                  -> EEP_SEED_DEFAULTS  (first ever run, write defaults)
enum EepInitAction { EEP_LOAD_EXISTING, EEP_MIGRATE_LEGACY, EEP_SEED_DEFAULTS };
EepInitAction eeprom_init_action(bool new_ns_seeded, bool legacy_ns_has_data);

// mode_from_last_mode: the boot-time mapping from the persisted lastMode byte to
// the runtime drive-mode enum. Valid stored values are 0..5 (Stock/FWD/5050/
// 6040/7525/Expert); anything else (notably a haldexGeneration number like 41/50/
// 51 that a bug once wrote into lastMode) falls back to MODE_FWD. Extracted as a
// pure seam so a host test proves generation-namespace values can never be
// mistaken for a valid drive mode. Pure logic, no Arduino/NVS symbols.
openhaldex_mode_t mode_from_last_mode(uint8_t last_mode);

// last_mode_after_generation_change: the drive mode that must remain stored when
// the haldex generation is changed in Settings. Generation (1/2/4/41/50/51) and
// drive mode (0..5) are separate namespaces; setting the generation must NOT
// touch the stored mode. This seam returns current_last_mode unchanged, and
// settingsIncoming() assigns lastMode through it - so re-introducing the old
// `lastMode = generation` corruption breaks a host test rather than shipping
// silently. Pure logic, no Arduino/NVS symbols.
uint8_t last_mode_after_generation_change(uint8_t current_last_mode, int generation);

// HTTP request-body buffer ownership, extracted from parseJSON so the
// malloc-owned single-block contract that ESPAsyncWebServer frees with plain
// free() can be host-tested. Both reference only <cstdlib>/<cstring>, no
// Arduino/Async symbols, so they compile and test under env:native.
//
// http_body_alloc: returns a malloc(total + 1) block, fully zero-filled with a
// NUL guaranteed at index `total`. Returns nullptr when total == 0 or malloc
// fails, so the caller can fall through to its empty-body path. One allocation,
// released by a single plain free().
char* http_body_alloc(size_t total);

// http_body_write_chunk: bounds-checked copy of `len` bytes from `data` into
// `buf` at offset `index`, never writing past `total` (the NUL guard at `total`
// is preserved). No-ops when buf is null, data is null, index >= total, or len
// is zero; a chunk that would overrun `total` is truncated to fit.
void http_body_write_chunk(char* buf, const uint8_t* data, size_t len, size_t index, size_t total);

// ---- Gen5 (MQB) Haldex UDS live data ---------------------------------------
// Wire format and per-DID scaling recovered from the upstream V8.00.2 binary and
// confirmed against the author's V8.00.2 source. Both functions are pure
// byte/float arithmetic (no TWAI/Arduino symbols), so the decode feeding the
// dashboard live-data card is host-testable.
//
// uds_parse_sf_rdbi: parse an ISO-TP single-frame positive ReadDataByIdentifier
// response for `did` out of a raw CAN payload. Returns the number of data bytes
// (those after the 62 <DID_hi> <DID_lo> header) copied into `out`, or -1 when the
// frame is anything else: not a single frame, a declared length the frame does
// not actually carry, a negative response, a different service or DID, or more
// data than `out_cap` can hold. All poller responses are single frames.
int uds_parse_sf_rdbi(const uint8_t *data, uint8_t dlc, uint16_t did, uint8_t *out, uint8_t out_cap);

// uds_scale_mqb_did: apply the per-DID raw->engineering-value scaling. The u16
// byte order is mixed per DID, matching the upstream poller: temperatures
// (0x2BF1, 0x2BE4) and clutch voltage (0x2BE9, x 0.1 V) are little-endian, clutch
// current (0x2BE6, x 0.001 A) is big-endian. Returns false for a short payload or an unknown DID, leaving `out`
// untouched.
bool uds_scale_mqb_did(uint16_t did, const uint8_t *payload, uint8_t len, float &out);

// uds_temp_plausible: true when a decoded Haldex temperature is within a
// physically possible band (-40..150 degC). The 0x2BE4/0x2BF1 scale is an
// unvalidated disassembled guess that reads an impossible ~160 degC on the fin
// under load; callers null the display when this returns false rather than
// showing a value the hardware cannot produce.
bool uds_temp_plausible(float degC);
