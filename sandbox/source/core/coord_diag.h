#pragma once
#include <defines.h>

// =====================================================================================
// Coordinate diagnostics (big_space / HierPos2 refactor verification).
//
// Dumps the precision-safe HierPos2 coordinates of every gameplay object each frame
// (throttled) and checks a set of runtime invariants that prove the simulation is
// running in hierarchical cell space rather than lossy absolute f32. Output is appended
// to bin/coord_diag.txt. Compiled out entirely in non-debug builds.
// =====================================================================================

struct game_state;

// Runtime-toggled per-frame dump + invariant check. Cheap no-op when disabled.
void coord_diag_update(const game_state* s, f32 dt);

// Toggle the diagnostics dump on/off (default off). Safe to call in any build.
void coord_diag_set_enabled(b8 enabled);
b8   coord_diag_is_enabled(void);

// Last-frame invariant violation count (0 == all invariants held). For an ImGui readout.
i32  coord_diag_last_violations(void);
