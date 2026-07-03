// Host-native link-support translation unit.
//
// Lives at the test-root so PlatformIO compiles it into EVERY test suite
// (shared test code). The natively-compiled core (OpenHaldexC6_globals.cpp +
// _Calculations.cpp) references a handful of symbols that are *defined* in
// firmware TUs we deliberately exclude from the native build (see
// build_src_filter in [env:native]). We supply minimal no-op definitions here
// so the native test binary links without dragging in hardware/RTOS/IO source.
//
// Keep this list tight: add a definition only when the linker reports an
// undefined reference for a genuinely-excluded firmware symbol.

#include <OpenHaldexC6_defs.h>

// Defined on-target in OpenHaldexC6_tasks.cpp; invoked by startHaldexLearn()
// in _Calculations.cpp via xTaskCreate. No-op off-target.
void haldexLearnTask(void * /*arg*/) {}
