// Characterization tests for the OpenHaldex expert-map interpolation and
// lock-target adjustment core.
//
// These pin the CURRENT computed outputs of:
//   * get_expert_lock_target()      (file-local `static`, exercised INDIRECTLY
//                                     through get_lock_target_adjustment with
//                                     MODE_EXPERT / force value 5)
//   * get_lock_target_adjustment()  (per-mode + force-mode lock target)
//   * get_lock_target_adjusted_value(value, invert)
//
// These functions reach the wire bytes the Haldex sees. Golden values are
// CHARACTERIZATION (the functions' present output recorded by running the real
// code), NOT a hand-derived spec. A red assertion here means a refactor changed
// what the Haldex receives - do NOT "fix" a golden to make a refactor pass.
//
// Every test deterministically sets ALL referenced extern globals before each
// assertion so the suite is order-independent.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h> // pulls OpenHaldexC6_defs.h (state, arrays, modes)

// Real functions under test (src/OpenHaldexC6_Calculations.cpp).
extern float get_lock_target_adjustment();
extern uint8_t get_lock_target_adjusted_value(uint8_t value, bool invert);

// ---------------------------------------------------------------------------
// Deterministic global reset. Drives every input referenced by the functions
// under test (directly or via the file-local lock_enabled()) into a known
// baseline where lock_enabled() == true and no force/learn path is active.
// ---------------------------------------------------------------------------
static void reset_globals(void)
{
  // Mode / force-mode selection
  state.mode            = MODE_FWD;
  state.pedal_threshold = 0; // -> throttle_ok always true
  state.mode_override   = false;

  extBtnForceMode        = false;
  tcForceMode            = false;
  hazardForceMode        = false;
  extButtonForceModeFlag = false;
  tcForceModeFlag        = false;
  hazardForceModeFlag    = false;
  tcForceModeValue       = 2;
  hazardForceModeValue   = 2;
  extBtnForceModeValue   = 2;
  forceModesPriority     = 0; // default: Hazard > TC > Ext

  // lock_enabled() speed gate (under==0 disables the gate -> speed_ok true)
  disengageUnderSpeed = 0;
  disengageAboveSpeed = 0;

  // Expert-map inputs
  received_pedal_value   = 0.0f;
  received_vehicle_speed = 0;

  // Stock / force-stock passthrough input
  received_haldex_engagement = 0;

  // Adjusted-value inputs
  haldexGeneration      = 1; // VAG default correction-factor formula
  lock_target           = 0.0f;
  haldexLearnActive     = false;
  haldexLearnCF         = 0;
  haldexLearnTableValid = false;
  for (int i = 0; i <= 100; i++)
  {
    haldexLearnTable[i] = 0;
  }
}

void setUp(void) { reset_globals(); }
void tearDown(void) {}

// Helper: drive an expert lock-target read through the public adjustment fn.
static float expert_target(float throttle, uint16_t speed)
{
  state.mode             = MODE_EXPERT;
  state.pedal_threshold  = 0;
  disengageUnderSpeed    = 0;
  received_pedal_value   = throttle;
  received_vehicle_speed = speed;
  return get_lock_target_adjustment();
}

// ===========================================================================
// get_expert_lock_target() - 2D throttle x speed interpolation.
// throttleArray = {0,15,30,45,60,75,90}; speedArray = {0,30,60,90,120,160,180}.
// lockArray rows by throttle index:
//   t0,t1 -> all 0; t2 -> {5,5,5,5,5,40,40}; t3 -> all 40; t4..t6 -> all 80.
// Grid lands on nodes AND between them; throttle/speed exceed the top node to
// pin the saturation branch. Result is int(constrain(v,0,100)).
// ===========================================================================

void test_expert_throttle0_all_speeds_zero(void)
{
  // Throttle 0 -> lockArray rows t0/t1 are all 0 regardless of speed.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(0, 0),   "expert(thr=0,spd=0)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(0, 30),  "expert(thr=0,spd=30)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(0, 75),  "expert(thr=0,spd=75)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(0, 120), "expert(thr=0,spd=120)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(0, 180), "expert(thr=0,spd=180)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(0, 250), "expert(thr=0,spd=250)");
}

void test_expert_throttle15_node_zero(void)
{
  // Throttle 15 lands exactly on node t1 (all-zero row).
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(15, 0),   "expert(thr=15,spd=0)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(15, 30),  "expert(thr=15,spd=30)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(15, 75),  "expert(thr=15,spd=75)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(15, 120), "expert(thr=15,spd=120)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(15, 180), "expert(thr=15,spd=180)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, expert_target(15, 250), "expert(thr=15,spd=250)");
}

void test_expert_throttle22_interpolated(void)
{
  // Throttle 22 is between t1(15)=0 and t2(30)={5..40}; interpolates in throttle.
  // Speed 180+ pushes the t2 row into its 40-valued high-speed columns.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f,  expert_target(22, 0),   "expert(thr=22,spd=0)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f,  expert_target(22, 30),  "expert(thr=22,spd=30)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f,  expert_target(22, 75),  "expert(thr=22,spd=75)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f,  expert_target(22, 120), "expert(thr=22,spd=120)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(18.0f, expert_target(22, 180), "expert(thr=22,spd=180)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(18.0f, expert_target(22, 250), "expert(thr=22,spd=250)");
}

void test_expert_throttle45_node_forty(void)
{
  // Throttle 45 lands on node t3 (all-40 row), all speeds.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, expert_target(45, 0),   "expert(thr=45,spd=0)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, expert_target(45, 30),  "expert(thr=45,spd=30)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, expert_target(45, 75),  "expert(thr=45,spd=75)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, expert_target(45, 120), "expert(thr=45,spd=120)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, expert_target(45, 180), "expert(thr=45,spd=180)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, expert_target(45, 250), "expert(thr=45,spd=250)");
}

void test_expert_throttle90_node_eighty(void)
{
  // Throttle 90 lands on node t6 (all-80 row), all speeds.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(90, 0),   "expert(thr=90,spd=0)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(90, 30),  "expert(thr=90,spd=30)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(90, 75),  "expert(thr=90,spd=75)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(90, 120), "expert(thr=90,spd=120)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(90, 180), "expert(thr=90,spd=180)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(90, 250), "expert(thr=90,spd=250)");
}

void test_expert_throttle120_saturates(void)
{
  // Throttle 120 (>top node 90) saturates onto the top throttle row (all 80).
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(120, 0),   "expert(thr=120,spd=0)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(120, 30),  "expert(thr=120,spd=30)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(120, 75),  "expert(thr=120,spd=75)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(120, 120), "expert(thr=120,spd=120)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(120, 180), "expert(thr=120,spd=180)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, expert_target(120, 250), "expert(thr=120,spd=250)");
}

// ===========================================================================
// get_lock_target_adjustment() - per-mode lock target, lock_enabled() true.
// ===========================================================================

void test_adjustment_mode_fwd_is_zero(void)
{
  state.mode = MODE_FWD;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "adjustment MODE_FWD");
}

void test_adjustment_mode_5050_enabled_is_100(void)
{
  state.mode = MODE_5050;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, get_lock_target_adjustment(), "adjustment MODE_5050 enabled");
}

void test_adjustment_mode_6040_enabled_is_40(void)
{
  state.mode = MODE_6040;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, get_lock_target_adjustment(), "adjustment MODE_6040 enabled");
}

void test_adjustment_mode_7525_enabled_is_30(void)
{
  state.mode = MODE_7525;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(30.0f, get_lock_target_adjustment(), "adjustment MODE_7525 enabled");
}

void test_adjustment_mode_expert_enabled_uses_map(void)
{
  // MODE_EXPERT with throttle 45 / any speed -> expert map node t3 (40).
  state.mode             = MODE_EXPERT;
  received_pedal_value   = 45.0f;
  received_vehicle_speed = 60;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, get_lock_target_adjustment(), "adjustment MODE_EXPERT map");
}

void test_adjustment_mode_stock_commands_zero(void)
{
  // MODE_STOCK must command zero forced lock and must NEVER mirror
  // received_haldex_engagement back: the return value is packed into
  // transmitted frames (standalone always, inline when editing), so mirroring
  // engagement closes a positive feedback loop that latches the clutch at 100%
  // (the stuck-at-100% field bug). Inline stock passthrough is handled by the
  // caller skipping frame edits, not by this function.
  state.mode                 = MODE_STOCK;
  received_haldex_engagement = 100;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "adjustment MODE_STOCK never mirrors engagement");
  received_haldex_engagement = 37;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "adjustment MODE_STOCK commands zero");
}

// --- lock_enabled() == false branches (throttle gate fails) ---------------

void test_adjustment_5050_lock_disabled_is_zero(void)
{
  state.mode            = MODE_5050;
  state.pedal_threshold = 50;      // throttle_ok requires pedal >= 50
  received_pedal_value  = 10.0f;   // below threshold -> lock_enabled() false
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "adjustment MODE_5050 lock-disabled");
}

void test_adjustment_6040_lock_disabled_is_zero(void)
{
  state.mode            = MODE_6040;
  state.pedal_threshold = 50;
  received_pedal_value  = 10.0f;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "adjustment MODE_6040 lock-disabled");
}

void test_adjustment_7525_lock_disabled_is_zero(void)
{
  state.mode            = MODE_7525;
  state.pedal_threshold = 50;
  received_pedal_value  = 10.0f;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "adjustment MODE_7525 lock-disabled");
}

// Note: MODE_EXPERT's lock_enabled() ignores the throttle gate (always true),
// so there is no expert lock-disabled branch to pin here.

// --- force-mode override branches ------------------------------------------
// Any enabled trigger (TC / Hazard / Ext) with its flag active wins over the
// mode switch; which trigger wins is picked by forceModesPriority (default 0 =
// Hazard > TC > Ext). Each trigger carries its own force value 0..5.

void test_force_ext_value0_stock_commands_zero(void)
{
  // Force value 0 (Stock) commands zero lock - same never-mirror rule as
  // MODE_STOCK above. The inline gateway detects force-Stock via
  // get_forced_mode_value() and passes frames through untouched instead.
  extBtnForceMode = true; extButtonForceModeFlag = true; extBtnForceModeValue = 0;
  received_haldex_engagement = 42;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "force ext value=0 (stock) never mirrors engagement");
}

// --- get_forced_mode_value(): the effective-mode seam used by parseCAN_chs ---

void test_forced_mode_value_none_active(void)
{
  // No trigger enabled -> -1 (no force mode applies).
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, get_forced_mode_value(), "forced mode: none enabled");

  // Enabled but flag not asserted -> still -1.
  tcForceMode = true; tcForceModeFlag = false;
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, get_forced_mode_value(), "forced mode: enabled, flag off");

  // Flag asserted but feature disabled (stray ESP 'passiv' bit) -> still -1.
  tcForceMode = false; tcForceModeFlag = true;
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, get_forced_mode_value(), "forced mode: flag on, feature off");
}

void test_forced_mode_value_active_and_priority(void)
{
  extBtnForceMode = true; extButtonForceModeFlag = true; extBtnForceModeValue = 3;
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, get_forced_mode_value(), "forced mode: ext value 3");

  // Hazard (value 0 = Stock) beats Ext under default priority 0.
  hazardForceMode = true; hazardForceModeFlag = true; hazardForceModeValue = 0;
  forceModesPriority = 0;
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, get_forced_mode_value(), "forced mode: hazard-stock wins priority 0");
}

void test_force_ext_value1_fwd_is_zero(void)
{
  extBtnForceMode = true; extButtonForceModeFlag = true; extBtnForceModeValue = 1;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "force ext value=1 (FWD)");
}

void test_force_ext_value2_5050_is_100(void)
{
  extBtnForceMode = true; extButtonForceModeFlag = true; extBtnForceModeValue = 2;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, get_lock_target_adjustment(), "force ext value=2 (50:50)");
}

void test_force_ext_value3_6040_is_40(void)
{
  extBtnForceMode = true; extButtonForceModeFlag = true; extBtnForceModeValue = 3;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, get_lock_target_adjustment(), "force ext value=3 (60:40)");
}

void test_force_ext_value4_7525_is_30(void)
{
  extBtnForceMode = true; extButtonForceModeFlag = true; extBtnForceModeValue = 4;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(30.0f, get_lock_target_adjustment(), "force ext value=4 (75:25)");
}

void test_force_ext_value5_expert_uses_map(void)
{
  // value==5 -> get_expert_lock_target(); throttle 90 -> 80.
  extBtnForceMode = true; extButtonForceModeFlag = true; extBtnForceModeValue = 5;
  received_pedal_value   = 90.0f;
  received_vehicle_speed = 120;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(80.0f, get_lock_target_adjustment(), "force ext value=5 (expert)");
}

void test_force_tc_branch_value3(void)
{
  // TC trigger path with tcForceModeFlag (instead of the ext-button pair).
  tcForceMode = true; tcForceModeFlag = true; tcForceModeValue = 3;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, get_lock_target_adjustment(), "force tc branch value=3");
}

void test_force_hazard_branch_value1(void)
{
  // Hazard trigger forcing FWD (value 1) overrides the active mode.
  hazardForceMode = true; hazardForceModeFlag = true; hazardForceModeValue = 1;
  state.mode = MODE_5050; // would be 100 without the hazard override
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "force hazard value=1 (FWD)");
}

void test_priority0_hazard_beats_tc(void)
{
  // Default priority 0 = Hazard > TC > Ext: hazard's value wins when both fire.
  hazardForceMode = true; hazardForceModeFlag = true; hazardForceModeValue = 1; // FWD -> 0
  tcForceMode = true; tcForceModeFlag = true; tcForceModeValue = 2;             // 50:50 -> 100
  forceModesPriority = 0;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "priority 0: hazard beats tc");
}

void test_priority1_tc_beats_hazard(void)
{
  // Priority 1 = TC > Hazard > Ext: same triggers, TC's value now wins.
  hazardForceMode = true; hazardForceModeFlag = true; hazardForceModeValue = 1; // FWD -> 0
  tcForceMode = true; tcForceModeFlag = true; tcForceModeValue = 2;             // 50:50 -> 100
  forceModesPriority = 1;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, get_lock_target_adjustment(), "priority 1: tc beats hazard");
}

void test_hazard_enabled_but_hazards_off_falls_through(void)
{
  // Toggle on but hazards not flashing -> normal mode behaviour.
  hazardForceMode = true; hazardForceModeFlag = false;
  state.mode = MODE_6040;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, get_lock_target_adjustment(), "hazard enabled, off -> mode value");
}

void test_hazard_flag_without_toggle_is_ignored(void)
{
  // Hazards flashing but the setting is off -> no override.
  hazardForceMode = false; hazardForceModeFlag = true;
  state.mode = MODE_5050;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, get_lock_target_adjustment(), "hazard flag without toggle ignored");
}

void test_force_mode_enabled_but_no_flag_falls_through(void)
{
  // Force mode enabled but NO flag active -> force block skipped, falls to the
  // mode switch. state.mode == MODE_6040 (enabled) -> 40.
  extBtnForceMode = true; extButtonForceModeFlag = false; tcForceModeFlag = false;
  extBtnForceModeValue = 2;       // would be 100 IF the flag path were taken
  state.mode           = MODE_6040; // fall-through target
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, get_lock_target_adjustment(),
                                  "force enabled, no flag -> falls through to mode");
}

// ===========================================================================
// get_lock_target_adjusted_value(value, invert)
// ===========================================================================

// --- default-formula path: cf = constrain((lock_target + 20) / 2, 0, 100) --
// lock_enabled() true (baseline), haldexGeneration == 1 (VAG formula).
// lock_target in {0,40,100}.

void test_adjval_default_lt0_returns_floor(void)
{
  // lock_target==0 short-circuits: non-invert 0x00, invert 0xFE.
  lock_target = 0.0f;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, get_lock_target_adjusted_value(0x00, false), "adjval lt=0 val=0x00 inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFE, get_lock_target_adjusted_value(0x00, true),  "adjval lt=0 val=0x00 inv=1");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, get_lock_target_adjusted_value(0xFE, false), "adjval lt=0 val=0xFE inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFE, get_lock_target_adjusted_value(0xFE, true),  "adjval lt=0 val=0xFE inv=1");
}

void test_adjval_default_lt40(void)
{
  // cf = (40 + 20) / 2 = 30. corrected = value*30/100.
  lock_target = 40.0f;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, get_lock_target_adjusted_value(0x00, false), "adjval lt=40 val=0x00 inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFE, get_lock_target_adjusted_value(0x00, true),  "adjval lt=40 val=0x00 inv=1");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x17, get_lock_target_adjusted_value(0x4E, false), "adjval lt=40 val=0x4E inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xE7, get_lock_target_adjusted_value(0x4E, true),  "adjval lt=40 val=0x4E inv=1");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x26, get_lock_target_adjusted_value(0x7F, false), "adjval lt=40 val=0x7F inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xD8, get_lock_target_adjusted_value(0x7F, true),  "adjval lt=40 val=0x7F inv=1");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x4C, get_lock_target_adjusted_value(0xFE, false), "adjval lt=40 val=0xFE inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xB2, get_lock_target_adjusted_value(0xFE, true),  "adjval lt=40 val=0xFE inv=1");
}

void test_adjval_lt100_full_passthrough(void)
{
  // lock_target==100 short-circuits to full value (lock_enabled() true).
  lock_target = 100.0f;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x4E, get_lock_target_adjusted_value(0x4E, false), "adjval lt=100 val=0x4E inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xB0, get_lock_target_adjusted_value(0x4E, true),  "adjval lt=100 val=0x4E inv=1");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFE, get_lock_target_adjusted_value(0xFE, false), "adjval lt=100 val=0xFE inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, get_lock_target_adjusted_value(0xFE, true),  "adjval lt=100 val=0xFE inv=1");
}

// --- Gen41 default-formula path: cf = constrain((lt + 24) / 2.2, 0, 100) ---

void test_adjval_gen41_default_lt40(void)
{
  // Gen41 formula: cf = (40 + 24) / 2.2 = 29 (truncated). corrected = value*29/100.
  haldexGeneration = 41;
  lock_target      = 40.0f;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x24, get_lock_target_adjusted_value(0x7F, false), "adjval gen41 lt=40 val=0x7F inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x49, get_lock_target_adjusted_value(0xFE, false), "adjval gen41 lt=40 val=0xFE inv=0");
}

// --- default-formula path with lock_enabled() == false --------------------

void test_adjval_default_lock_disabled_returns_floor(void)
{
  // throttle gate fails -> after correction, returns floor (0x00 / 0xFE).
  state.pedal_threshold = 50;
  received_pedal_value  = 10.0f; // below threshold -> lock_enabled() false
  lock_target           = 40.0f;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, get_lock_target_adjusted_value(0xFE, false), "adjval lt=40 le=F inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFE, get_lock_target_adjusted_value(0xFE, true),  "adjval lt=40 le=F inv=1");
}

void test_adjval_lt100_lock_disabled_returns_floor(void)
{
  // lock_target==100 with lock_enabled() false -> floor (0x00 / 0xFE).
  state.pedal_threshold = 50;
  received_pedal_value  = 10.0f;
  lock_target           = 100.0f;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, get_lock_target_adjusted_value(0xFE, false), "adjval lt=100 le=F inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFE, get_lock_target_adjusted_value(0xFE, true),  "adjval lt=100 le=F inv=1");
}

// --- valid learn-table path: cf = smallest i with table[i] >= lock_target ---

void test_adjval_valid_table_lt40(void)
{
  // Seed table[i] = min(2*i,100) so smallest i with table[i]>=40 is i=20 (cf=20),
  // DISTINCT from the default formula's cf=40 for lock_target=40.
  haldexLearnTableValid = true;
  for (int i = 0; i <= 100; i++)
  {
    int v = 2 * i;
    haldexLearnTable[i] = (uint8_t)(v > 100 ? 100 : v);
  }
  lock_target = 40.0f;
  // corrected = value * 20 / 100
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x19, get_lock_target_adjusted_value(0x7F, false), "adjval valid-tbl lt=40 val=0x7F inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xE5, get_lock_target_adjusted_value(0x7F, true),  "adjval valid-tbl lt=40 val=0x7F inv=1");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x32, get_lock_target_adjusted_value(0xFE, false), "adjval valid-tbl lt=40 val=0xFE inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xCC, get_lock_target_adjusted_value(0xFE, true),  "adjval valid-tbl lt=40 val=0xFE inv=1");
}

void test_adjval_valid_table_lock_disabled(void)
{
  haldexLearnTableValid = true;
  for (int i = 0; i <= 100; i++)
  {
    int v = 2 * i;
    haldexLearnTable[i] = (uint8_t)(v > 100 ? 100 : v);
  }
  lock_target           = 40.0f;
  state.pedal_threshold = 50;
  received_pedal_value  = 10.0f; // lock_enabled() false
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, get_lock_target_adjusted_value(0xFE, false), "adjval valid-tbl lt=40 le=F inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFE, get_lock_target_adjusted_value(0xFE, true),  "adjval valid-tbl lt=40 le=F inv=1");
}

// --- haldexLearnActive path: corrected = value * haldexLearnCF / 100 -------
// This path is checked FIRST and ignores lock_target / lock_enabled().

void test_adjval_learn_active_cf50(void)
{
  haldexLearnActive = true;
  haldexLearnCF     = 50;
  lock_target       = 0.0f; // ignored on this path
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x3F, get_lock_target_adjusted_value(0x7F, false), "adjval learn cf=50 val=0x7F inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xBF, get_lock_target_adjusted_value(0x7F, true),  "adjval learn cf=50 val=0x7F inv=1");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x7F, get_lock_target_adjusted_value(0xFE, false), "adjval learn cf=50 val=0xFE inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x7F, get_lock_target_adjusted_value(0xFE, true),  "adjval learn cf=50 val=0xFE inv=1");
}

void test_adjval_learn_active_cf100(void)
{
  haldexLearnActive = true;
  haldexLearnCF     = 100;
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x7F, get_lock_target_adjusted_value(0x7F, false), "adjval learn cf=100 val=0x7F inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x7F, get_lock_target_adjusted_value(0x7F, true),  "adjval learn cf=100 val=0x7F inv=1");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFE, get_lock_target_adjusted_value(0xFE, false), "adjval learn cf=100 val=0xFE inv=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, get_lock_target_adjusted_value(0xFE, true),  "adjval learn cf=100 val=0xFE inv=1");
}

// ===========================================================================
// bpk_pack_motor11(out, command, counter, ceil_nm, ist_nm, solf_nm)
// ===========================================================================
// The BPK Motor_11 torque spoof. These pin the DBC-verified semantics: the three
// 10-bit torque fields (Soll_Roh at bit 12, Ist_Summe at bit 22, Soll_gefiltert
// at bit 42) are 1 Nm/LSB with offset -509. We decode the fields back out of the
// packed bytes and assert the Nm, so a refactor that moves a bit is caught.

// Decode a 10-bit little-endian field starting at bit_pos from the 8 frame bytes.
static uint16_t bpk_decode_raw(const uint8_t *d, int bit_pos)
{
  uint32_t bits = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) |
                  ((uint32_t)d[3] << 24);
  // Fields we read (12, 22) live within the low 4 bytes; 42 needs bytes 4..7.
  uint32_t hi = (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) |
                ((uint32_t)d[7] << 24);
  if (bit_pos >= 32)
  {
    return (uint16_t)((hi >> (bit_pos - 32)) & 0x3FF);
  }
  // Field may straddle the 32-bit boundary (Soll_gefiltert at 42 does not, but be
  // safe): combine low and high words.
  uint64_t full = (uint64_t)bits | ((uint64_t)hi << 32);
  return (uint16_t)((full >> bit_pos) & 0x3FF);
}

static int bpk_soll_roh_nm(const uint8_t *d) { return (int)bpk_decode_raw(d, 12) - 509; }
static int bpk_ist_nm(const uint8_t *d)      { return (int)bpk_decode_raw(d, 22) - 509; }
static int bpk_solf_nm(const uint8_t *d)     { return (int)bpk_decode_raw(d, 42) - 509; }

void test_bpk_ceiling_maps_full_command_to_ceiling_nm(void)
{
  uint8_t out[8];
  uint16_t ist = 500, solf = 500; // pre-settled so Soll_Roh (un-slewed) shows ceiling
  bpk_pack_motor11(out, 0xFE, 0x40, 220, &ist, &solf);
  TEST_ASSERT_EQUAL_INT_MESSAGE(220, bpk_soll_roh_nm(out), "ceil=220 cmd=0xFE Soll_Roh");

  ist = 500; solf = 500;
  bpk_pack_motor11(out, 0xFE, 0x40, 500, &ist, &solf);
  TEST_ASSERT_EQUAL_INT_MESSAGE(500, bpk_soll_roh_nm(out), "ceil=500 cmd=0xFE Soll_Roh");
}

void test_bpk_zero_command_is_floor(void)
{
  uint8_t out[8];
  uint16_t ist = 0, solf = 0;
  bpk_pack_motor11(out, 0x00, 0x40, 220, &ist, &solf);
  TEST_ASSERT_EQUAL_INT_MESSAGE(10, bpk_soll_roh_nm(out), "cmd=0 Soll_Roh is floor 10Nm");
}

void test_bpk_ceiling_clamps_to_509(void)
{
  uint8_t out[8];
  uint16_t ist = 999, solf = 999;
  bpk_pack_motor11(out, 0xFE, 0x40, 1000, &ist, &solf); // absurd ceiling
  TEST_ASSERT_EQUAL_INT_MESSAGE(509, bpk_soll_roh_nm(out), "ceil clamps to 509 Nm max");
}

void test_bpk_slew_limits_ist_and_solf(void)
{
  uint8_t out[8];
  uint16_t ist = 0, solf = 0;
  // First cycle from rest: Ist steps 8 Nm, Solf steps 32 Nm toward the 220 target.
  bpk_pack_motor11(out, 0xFE, 0x40, 220, &ist, &solf);
  TEST_ASSERT_EQUAL_INT_MESSAGE(8,  bpk_ist_nm(out),  "cycle1 Ist slew +8");
  TEST_ASSERT_EQUAL_INT_MESSAGE(32, bpk_solf_nm(out), "cycle1 Solf slew +32");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(8,  ist,  "cycle1 ist state out");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(32, solf, "cycle1 solf state out");
  // Second cycle continues ramping from the caller-owned state.
  bpk_pack_motor11(out, 0xFE, 0x40, 220, &ist, &solf);
  TEST_ASSERT_EQUAL_INT_MESSAGE(16, bpk_ist_nm(out),  "cycle2 Ist slew +8");
  TEST_ASSERT_EQUAL_INT_MESSAGE(64, bpk_solf_nm(out), "cycle2 Solf slew +32");
}

void test_bpk_counter_in_low_nibble(void)
{
  uint8_t out[8];
  uint16_t ist = 0, solf = 0;
  bpk_pack_motor11(out, 0x80, 0x4A, 220, &ist, &solf);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x0A, out[1] & 0x0F, "counter low nibble preserved in byte1");
}

// ===========================================================================

int main(int, char **)
{
  UNITY_BEGIN();

  // expert-map interpolation
  RUN_TEST(test_expert_throttle0_all_speeds_zero);
  RUN_TEST(test_expert_throttle15_node_zero);
  RUN_TEST(test_expert_throttle22_interpolated);
  RUN_TEST(test_expert_throttle45_node_forty);
  RUN_TEST(test_expert_throttle90_node_eighty);
  RUN_TEST(test_expert_throttle120_saturates);

  // per-mode lock-target adjustment
  RUN_TEST(test_adjustment_mode_fwd_is_zero);
  RUN_TEST(test_adjustment_mode_5050_enabled_is_100);
  RUN_TEST(test_adjustment_mode_6040_enabled_is_40);
  RUN_TEST(test_adjustment_mode_7525_enabled_is_30);
  RUN_TEST(test_adjustment_mode_expert_enabled_uses_map);
  RUN_TEST(test_adjustment_mode_stock_commands_zero);
  RUN_TEST(test_adjustment_5050_lock_disabled_is_zero);
  RUN_TEST(test_adjustment_6040_lock_disabled_is_zero);
  RUN_TEST(test_adjustment_7525_lock_disabled_is_zero);

  // force-mode override branches
  RUN_TEST(test_force_ext_value0_stock_commands_zero);
  RUN_TEST(test_forced_mode_value_none_active);
  RUN_TEST(test_forced_mode_value_active_and_priority);
  RUN_TEST(test_force_ext_value1_fwd_is_zero);
  RUN_TEST(test_force_ext_value2_5050_is_100);
  RUN_TEST(test_force_ext_value3_6040_is_40);
  RUN_TEST(test_force_ext_value4_7525_is_30);
  RUN_TEST(test_force_ext_value5_expert_uses_map);
  RUN_TEST(test_force_tc_branch_value3);
  RUN_TEST(test_force_hazard_branch_value1);
  RUN_TEST(test_priority0_hazard_beats_tc);
  RUN_TEST(test_priority1_tc_beats_hazard);
  RUN_TEST(test_hazard_enabled_but_hazards_off_falls_through);
  RUN_TEST(test_hazard_flag_without_toggle_is_ignored);
  RUN_TEST(test_force_mode_enabled_but_no_flag_falls_through);

  // adjusted-value
  RUN_TEST(test_adjval_default_lt0_returns_floor);
  RUN_TEST(test_adjval_default_lt40);
  RUN_TEST(test_adjval_lt100_full_passthrough);
  RUN_TEST(test_adjval_gen41_default_lt40);
  RUN_TEST(test_adjval_default_lock_disabled_returns_floor);
  RUN_TEST(test_adjval_lt100_lock_disabled_returns_floor);
  RUN_TEST(test_adjval_valid_table_lt40);
  RUN_TEST(test_adjval_valid_table_lock_disabled);
  RUN_TEST(test_adjval_learn_active_cf50);
  RUN_TEST(test_adjval_learn_active_cf100);

  // BPK Motor_11 torque-spoof packer
  RUN_TEST(test_bpk_ceiling_maps_full_command_to_ceiling_nm);
  RUN_TEST(test_bpk_zero_command_is_floor);
  RUN_TEST(test_bpk_ceiling_clamps_to_509);
  RUN_TEST(test_bpk_slew_limits_ist_and_solf);
  RUN_TEST(test_bpk_counter_in_low_nibble);

  return UNITY_END();
}
