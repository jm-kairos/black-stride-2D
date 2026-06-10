#pragma once

#include <defines.h>
#include <game_types.h>
#include <renderer/renderer_types.h>
#include "ship.h"
#include "nav.h"
#include "job.h"
#include "hull_contour.h"   // Stage-4 render of the cosmetic marching-squares hull silhouette
#include <containers/vector.h>

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
    // Which hull this crew is bound to: 0 = player ship, 1 = enemy ship. ALL of this crew's
    // local-space fields (position, velocity, path) are expressed in crew_ship(s,this)'s frame —
    // the deck it currently stands on. A crew BOARDS the other hull by walking to the mated
    // airlock and handing off (game.cpp update_crew_handoff): ship_id flips and position is
    // re-rooted into the new hull's frame at the SAME world point (no teleport). The binding is
    // durable across undock — a crew that boarded the enemy rides it away when the hulls part.
    i32           ship_id;    // 0 = player (s->ship), 1 = enemy (s->enemy_ship)

    bs_math::Vec2 position;   // SHIP-LOCAL center (axis-aligned; rides ship_id's full pose)
    bs_math::Vec2 velocity;   // SHIP-LOCAL velocity (steered along the path)
    f32           radius;     // half-extent for AABB tile collision (ship-local)

    // Active move order: a list of SHIP-LOCAL waypoint centers from A* (start -> goal). The
    // crew steers toward path[path_idx], advancing as it arrives. path_len == 0 means idle
    // (no order). Because the waypoints are ship-local, the path stays valid for free while
    // the ship translates and rotates — the crew keeps walking it in every mode.
    bs_math::Vec2 path[NAV_MAX_PATH];
    i32           path_len;   // number of valid waypoints (0 = idle / no order)
    i32           path_idx;   // index of the waypoint currently steered toward

    // ---- Multi-agent avoidance (crew_avoid.*) ----
    // Destination TILE this crew is heading to (its reserved goal). Other crew route around it,
    // and Tier-3 deadlock resolution replans toward it. Valid only while has_dest.
    i32           dest_col, dest_row;
    b8            has_dest;
    // Avoidance clocks (seconds): block_timer accrues while the per-frame peer gate is vetoing
    // motion (maintained in simulate_crew); wait_timer > 0 means this agent is politely YIELDING
    // (holding position) so a higher-priority peer can pass; stuck_timer is total time spent
    // unable to make progress, used to abort a hopeless order instead of freezing forever.
    f32           block_timer;
    f32           wait_timer;
    f32           stuck_timer;

    // ---- Cross-ship boarding (Phase 3) ----
    // A board order is a TWO-LEG move across the docked seam: the crew walks leg A to its OWN hull's
    // airlock-interior tile, HOPS the mated airlock onto the other hull (ship_id flips, position
    // re-roots into the new hull's frame at the landfall tile — see update_crew_handoff), then walks
    // leg B to board_goal on that hull. `boarding` is TRUE only while leg A is in flight (it clears
    // the instant the hop fires or the order aborts). board_target_ship is the destination hull id;
    // board_goal_(col,row) is the final goal tile IN THE DESTINATION HULL's grid. A plain same-hull
    // move leaves boarding FALSE. If the hulls undock mid-transit the order aborts and the crew stays
    // put (you can't cross a broken seam). Zero-init (Crew{}) => boarding FALSE, so non-board crew are
    // unaffected.
    b8            boarding;          // leg A of a cross-ship board order is in flight
    i32           board_target_ship; // destination hull id (the hull being boarded)
    i32           board_goal_col;    // final goal tile (destination hull grid)
    i32           board_goal_row;

    // ---- Seam GLIDE (the smooth airlock crossing) ----
    // Leg A ends at the crew's OWN airlock-interior tile; the destination landfall tile is ~3 tiles
    // (one door + the mated gap + one door) away in world space. Rather than snap ship_id+position
    // across that gap in a single frame (a visible TELEPORT — the old bug: the crew "jumped" between
    // ships), the crew GLIDES through both open doorways at walking speed. While `transiting` it stays
    // bound to its origin hull (ship_id unchanged) and its local position is rewritten each frame to
    // track a world-space point lerped from its own interior tile to the destination interior tile;
    // the two endpoints are recomputed from the live poses so the glide rides the rigid mated pair.
    // At t>=1 it binds to the destination hull (ship_id flips, position = the landfall tile center in
    // that hull's frame) and plans leg B. The exit/landfall interior tiles are collinear through both
    // mated door centers (doors parallel + TWO tiles apart, the connector bridge tile spanning the gap
    // at their midpoint), so the straight glide threads the airlocks across the connector.
    // Zero-init (Crew{}) => transiting FALSE, so non-boarding crew are unaffected (regression guard).
    b8            transiting;        // a smooth seam crossing is in flight (leg A done, leg B pending)
    f32           transit_t;         // glide progress 0..1 along the own-interior -> dest-interior line
    bs_math::Vec2 transit_from_local; // glide start: own airlock-interior tile center, in ORIGIN frame
    bs_math::Vec2 transit_to_local;   // glide end: landfall tile center, in DESTINATION (board) frame

    // ---- Job system (Phase 3) ----
    SkillSet      skills;     // per-skill levels (stub; no gameplay effect yet)
    Job           queue[CREW_MAX_JOBS]; // pending assigned jobs (dispatch: priority then FIFO)
    i32           job_count;  // number of valid entries in `queue`
    Job           current;    // the in-flight job (valid when has_current); drives the runner
    b8            has_current; // is `current` an active job (vs. idle)?
    b8            is_active_pilot; // TRUE only while a PILOTING job is EXECUTING at the helm

    b8        crew_selected; 
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
    Ship      enemy_ship;      // hostile hull (assets/enemy_ship.tmap); combat prototype, no crew yet

    // ---- Cosmetic smoothed hull silhouettes (hull_contour.{h,cpp}, marching squares) -----
    // The de-blocked outline of each hull, in SHIP-LOCAL space. Because the contour is
    // pose-INDEPENDENT (origin/angle not baked in) it is extracted ONCE here in game_init — the
    // tilemaps never change at runtime — and merely transformed to world at draw time (the
    // draw_hull_outline helper applies ship_local_to_world per vertex, exactly like draw_tile_span).
    // PURELY COSMETIC: collision/docking/nav still run on the discrete tile grid; nothing reads these
    // loops back into the simulation. Drawn as line loops on LAYER_HULL_OUTLINE (above the roof).
    HullContour hull_outline;       // player ship smoothed contour
    HullContour enemy_hull_outline; // enemy ship smoothed contour

    // Docking state. TWO distinct concepts, deliberately separated:
    //
    //  * enemy_docked  — the LATCHED join. FALSE until the player presses T while dock-eligible;
    //    then TRUE (the hulls are mechanically mated at the airlock) until they press T again to
    //    release. This is a piece of GAME STATE, not a geometric readout: once docked the ships move
    //    as one and the player can let go of the controls. It GATES (a) enemy-interior visibility
    //    (see game_render — undocked shows only the enemy roof silhouette; docked reveals the
    //    interior to board it) and (b) ship-ship collision (a mated joint must not self-repel).
    //
    //  * dock_eligible — a per-frame GEOMETRIC readout: TRUE this frame iff the player's and enemy's
    //    HULL_DOOR airlocks are currently aligned and within docking tolerance (ships_docked). It is
    //    NOT latched — it tracks live geometry every frame. It drives the "Press T to dock" HUD
    //    prompt and gates whether a T press is allowed to FIRE the dock latch. (While already docked
    //    it is irrelevant; undocking only needs the latch.)
    //
    // Splitting them is what turns docking from a flickery per-frame proximity flag into a real
    // mechanical state machine: you fly into alignment (dock_eligible goes TRUE, prompt appears),
    // press T (enemy_docked latches TRUE, hulls snap mated + lock), and they STAY joined even as you
    // release the stick or drift — until you press T again.
    b8        enemy_docked;   // LATCHED: are the hulls mechanically mated? (toggled by T)
    b8        dock_eligible;  // per-frame: are the airlocks aligned+close enough to dock right now?

    // ---- Docking CONNECTOR bridge -------------------------------------------------------
    // When the hulls latch (enemy_docked goes TRUE) a single walkable CONNECTOR tile is spawned in the
    // two-tile gap between the mated airlock doors — an airlock tube bridging the two ships so the
    // crews walk freely across the seam (the merged navmesh routes door -> connector -> door, and the
    // connector renders as one tile through the same interior/roof cross-fade as the hulls). It is
    // torn down on undock. Its placement is computed by dock_connector_tile() from the live mated pose
    // at dock time and stored here (the hulls are rigidly joined while docked, so it never moves).
    // connector_active mirrors enemy_docked but is kept as its own flag so render/nav read cleanly and
    // a future "tube still extending" animation can gate on it independently of the latch.
    b8            connector_active; // is the seam connector tile currently spawned?
    bs_math::Vec2 connector_world;  // world-space center of the connector tile (valid while active)
    f32           connector_angle;  // world angle of the connector tile (aligned to the mated doors)

    GameMode  mode;            // current view/control mode (hysteresis-latched)
    f32       roof_alpha;      // 0 = fully interior, 1 = fully roof; cross-fade state

    Vector(Crew) crew;         // multiple crew members.

    // Free-roam local camera. The view target is a SHIP-LOCAL focus point (so it rides the
    // ship's pose); WASD pans it in local mode. On zoom-out the camera eases from this focus
    // toward the ship origin (global view), so the mode hand-off still glides.
    bs_math::Vec2 cam_focus_local;

    ShipFlight flight;         // global-mode inertial flight dynamics
};

// ---- Cross-ship crew binding (Phase 3) ------------------------------------------------
// Resolve the hull a crew member is bound to from its ship_id. This is THE seam that makes the
// crew/nav/render code multi-hull: every place that used to hardcode &s->ship now asks the crew
// which deck it stands on. 0 = player, anything else = enemy (only two hulls in the prototype).
// Const and mutable overloads so callers that mutate the pose (none yet) and read-only callers
// (sim, avoidance, render) both compile against the right Ship*.
inline const Ship* crew_ship(const game_state* s, const Crew* c) {
    return (c->ship_id == 0) ? &s->ship : &s->enemy_ship;
}
inline Ship* crew_ship(game_state* s, const Crew* c) {
    return (c->ship_id == 0) ? &s->ship : &s->enemy_ship;
}

b8 game_init(Game* game_inst);
b8 game_update(Game* game_inst, f32 dt);
b8 game_render(Game* game_inst, f32 dt);
void game_on_resize(Game* game_inst, u32 width, u32 height);
