#pragma once

#include <defines.h>
#include <game_types.h>
#include <renderer/renderer_types.h>
#include "ship.h"
#include "nav.h"
#include "job.h"

// =====================================================================================
// Black Stride prototype: tile-based spaceship with two zoom-driven view modes.
//
//   mode::local  — zoomed in. Interior tiles (floor/walls/doors/hull) are drawn and one
//                  crew member is commanded RTS/Kenshi-style: LEFT-CLICK selects them,
//                  RIGHT-CLICK orders a move and the crew walks there autonomously along an
//                  A* path through the ship interior. WASD pans the (free-roam) camera.
//   mode::global — zoomed out. The ship's roof is drawn as a solid silhouette hiding the
//                  interior; the crew is not shown. WASD flies the whole ship.
//
// Crew navigation is SIMULATION, not control: an ordered crew keeps walking its path even
// after you zoom out to pilot, so it continues across the decks of the moving, rotating
// hull while you fly (crew lives in ship-local space and rides the rigid-body pose).
//
// The mode is derived from camera zoom with a hysteresis band so it does not flicker at
// the boundary, plus a short cross-fade between interior and roof for a seamless feel.
//
// Controls:
//   Mouse wheel  — zoom in/out (drives the mode switch).
//   Local mode (crew command):
//     Left-click   — select the crew member (click on its tile; click elsewhere deselects).
//     Right-click  — order the selected crew to walk to the clicked walkable tile (A* path).
//     WASD         — pan the camera (free-roam look-around; screen-aligned).
//   Global mode (ship flight, Starsector-style inertial):
//     W / S      — thrust forward / reverse along heading (accel / decel to max speed).
//     C          — brake: decelerate current velocity to zero.
//     Q / E      — strafe left / right (accel fraction by hull class: 100/75/50/25%).
//     A / D      — turn left / right (turn-accel ramps to max turn rate; auto-stabilizes).
//   Esc          — quit (handled by the engine).
// =====================================================================================

enum GameMode {
    MODE_LOCAL = 0,
    MODE_GLOBAL
};

// Max jobs a single crew member can have queued (beyond the one in-flight `current` job).
#define CREW_MAX_JOBS 16

struct Crew {
    bs_math::Vec2 position;   // SHIP-LOCAL center (axis-aligned; rides the ship's full pose)
    bs_math::Vec2 velocity;   // SHIP-LOCAL velocity (steered along the path)
    f32           radius;     // half-extent for AABB tile collision (ship-local)

    // Active move order: a list of SHIP-LOCAL waypoint centers from A* (start -> goal). The
    // crew steers toward path[path_idx], advancing as it arrives. path_len == 0 means idle
    // (no order). Because the waypoints are ship-local, the path stays valid for free while
    // the ship translates and rotates — the crew keeps walking it in every mode.
    bs_math::Vec2 path[NAV_MAX_PATH];
    i32           path_len;   // number of valid waypoints (0 = idle / no order)
    i32           path_idx;   // index of the waypoint currently steered toward

    // ---- Job system (Phase 3) ----
    SkillSet      skills;     // per-skill levels (stub; no gameplay effect yet)
    Job           queue[CREW_MAX_JOBS]; // pending assigned jobs (dispatch: priority then FIFO)
    i32           job_count;  // number of valid entries in `queue`
    Job           current;    // the in-flight job (valid when has_current); drives the runner
    b8            has_current; // is `current` an active job (vs. idle)?
    b8            is_active_pilot; // TRUE only while a PILOTING job is EXECUTING at the helm
};

// Hull size class — scales how much of the acceleration value strafing (Q/E) may use:
// frigate 100%, destroyer 75%, cruiser 50%, capital 25%.
enum HullClass {
    HULL_FRIGATE = 0,
    HULL_DESTROYER,
    HULL_CRUISER,
    HULL_CAPITAL
};

// Global-mode inertial flight dynamics (Starsector-style). The ship's POSE (origin, angle)
// lives in Ship; these are the integrators that drive it. The ship coasts — there is no
// passive linear drag, only the active brake (C) / reverse (S).
struct ShipFlight {
    bs_math::Vec2 velocity;          // world-space linear velocity
    f32           angular_velocity;  // rad/s, CCW positive; auto-stabilizes when A/D released
    HullClass     hull;              // determines strafe thrust fraction
};

struct game_state {
    u16 fb_width;
    u16 fb_height;

    Camera2D  camera;          // persistent; zoom mutated by the wheel
    Ship      ship;            // loaded from assets/ship.tmap

    GameMode  mode;            // current view/control mode (hysteresis-latched)
    f32       roof_alpha;      // 0 = fully interior, 1 = fully roof; cross-fade state

    Crew      crew;            // the single commandable crew member
    b8        crew_selected;   // is the crew member currently selected? (multi-crew seam)

    // Free-roam local camera. The view target is a SHIP-LOCAL focus point (so it rides the
    // ship's pose); WASD pans it in local mode. On zoom-out the camera eases from this focus
    // toward the ship origin (global view), so the mode hand-off still glides.
    bs_math::Vec2 cam_focus_local;

    ShipFlight flight;         // global-mode inertial flight dynamics
};

b8 game_init(Game* game_inst);
b8 game_update(Game* game_inst, f32 dt);
b8 game_render(Game* game_inst, f32 dt);
void game_on_resize(Game* game_inst, u32 width, u32 height);
