#include "game.h"
#include "nav.h"
#include "crew_jobs.h"
#include "crew_avoid.h"
#include "text.h"

#include <core/logger.h>
#include <core/input.h>
#include <math/math_utils.h>
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
#include <renderer/bs_imgui.h> // bs_imgui_wants_mouse: gate world input while ImGui owns the cursor
#include <renderer/bs_ui.h>    // bs_ui_* immediate-mode panel facade (Crew Job Panel)

#include <math.h>  // powf
#include <stdio.h> // snprintf (Crew Job Panel label formatting)

using namespace bs_math;

// =====================================================================================
// Tuning constants.
// =====================================================================================

// ---- Camera / zoom ----
static const f32 ZOOM_MIN          = 0.04f;  // most zoomed-out (global)
static const f32 ZOOM_MAX          = 3.00f;  // most zoomed-in (local)
static const f32 ZOOM_START        = 1.40f;  // begins in local mode
static const f32 ZOOM_STEP         = 1.12f;  // multiplicative per wheel notch

// Hysteresis band: once in local we only flip to global when zoom drops BELOW
// ZOOM_TO_GLOBAL; once in global we only return to local when zoom rises ABOVE
// ZOOM_TO_LOCAL. The gap between them prevents flicker at the boundary.
static const f32 ZOOM_TO_GLOBAL    = 0.80f;
static const f32 ZOOM_TO_LOCAL     = 1.00f;

static const f32 ROOF_FADE_SPEED   = 8.0f;   // cross-fade rate (1/seconds)

// ---- Crew navigation (local mode command; simulated in BOTH modes) ----
// The crew is commanded RTS-style (select + order); it then STEERS along an A* path of
// ship-local waypoints. Steering reuses an accel/friction/clamp feel: constant thrust
// toward the active waypoint, exponential friction, speed cap. Friction gives a terminal
// speed ~= CREW_ACCEL/CREW_FRICTION, capped by CREW_MAX_SPEED.
static const f32 CREW_ACCEL         = 2600.0f;
static const f32 CREW_FRICTION      = 9.0f;
static const f32 CREW_MAX_SPEED     = 260.0f;
static const f32 CREW_RADIUS        = 10.0f;
static const f32 CREW_ARRIVE_RADIUS = 6.0f;   // within this of a waypoint => arrived (advance)
static const f32 CREW_SLOW_RADIUS   = 30.0f;  // ease-in distance for the FINAL waypoint (arrival)
static const f32 CREW_PICK_RADIUS   = 22.0f;  // click within this (world units) of crew => select
// Seam-crossing glide speed (world units/s). Matched to CREW_MAX_SPEED so the airlock traversal
// reads as a continuation of the crew's walk — it strolls through the open doors instead of
// snapping across the ~96px (3-tile) seam gap in one frame. The crossing takes ~gap/speed ≈ 1s.
static const f32 CREW_SEAM_GLIDE_SPEED = 100.0f;

// ---- Free-roam local camera ----
static const f32 CAM_PAN_SPEED      = 420.0f; // WASD pan speed in local mode (units/s)

// ---- Ship movement (global mode): Starsector-style inertial flight ----
// Linear: W accelerates forward (along heading) toward SHIP_MAX_SPEED using SHIP_ACCEL.
// S accelerates in reverse toward SHIP_MAX_SPEED using SHIP_DECEL. C brakes the current
// velocity toward zero using SHIP_DECEL. Strafe (Q/E) thrusts sideways using a hull-class
// fraction of SHIP_ACCEL. The ship coasts: no passive drag, only active thrust/brake.
static const f32 SHIP_ACCEL        = 220.0f;  // forward / strafe thrust (units/s^2)
static const f32 SHIP_DECEL        = 160.0f;  // reverse + brake thrust (units/s^2)
static const f32 SHIP_MAX_SPEED    = 360.0f;  // linear speed cap (units/s)
// Angular: A/D ramp angular velocity using SHIP_TURN_ACCEL toward +/- SHIP_MAX_TURN.
// Releasing both lets SHIP_TURN_ACCEL bleed the spin back to zero (auto-stabilize).
static const f32 SHIP_TURN_ACCEL   = 6.0f;    // rad/s^2
static const f32 SHIP_MAX_TURN     = 1.8f;    // rad/s

// Undock kick: on release (T while docked) the player is shoved off the enemy along the airlock
// normal so the hulls cleanly part instead of resting in contact (which would immediately re-trip
// the collision resolver). One firm nudge; the ship then coasts (no passive drag) until piloted.
static const f32 UNDOCK_IMPULSE    = 90.0f;   // separation speed imparted to the player on undock (units/s)

// Strafe thrust fraction of SHIP_ACCEL by hull class: frigate/destroyer/cruiser/capital.
static const f32 STRAFE_FRACTION[4] = { 1.00f, 0.75f, 0.50f, 0.25f };

// ---- Render layers (lower draws first) ----
static const u32 LAYER_FLOOR  = 1;
static const u32 LAYER_WALL   = 2;
static const u32 LAYER_CREW   = 5;
static const u32 LAYER_PATH   = 6;   // move-order path line + destination marker (over crew)
static const u32 LAYER_SELECT = 7;   // selection ring (over the path)
static const u32 LAYER_ROOF   = 10;
static const u32 LAYER_HULL_OUTLINE = 11; // cosmetic smoothed silhouette stroke, over the roof fill
static const u32 LAYER_HUD_TEXT = 100; // screen-space HUD/UI text — always on top

static const u32 LAYER_DEBUG  = 0;

// Hard roster cap. The crew vector is reserve()d to exactly this in game_init, so as long as the
// crew count never exceeds it, push_back NEVER reallocates the backing store at runtime — which is
// what keeps any cached Crew* / `auto& c` (held by a peer scan mid-update) from dangling. MUST stay
// >= the design target (10-20 agents); 32 leaves headroom. Tie reserve() to THIS symbol so the
// capacity and the cap can never drift apart.
static const u32 MAX_CREW_MEMBERS = 32;

// Single choke-point for crew creation. Enforces the no-reallocation invariant: refuses to grow the
// roster past MAX_CREW_MEMBERS (the reserved capacity), so push_back here can never trigger a
// reallocation that would invalidate pointers other crew may be holding this frame. Returns the new
// crew's index, or -1 when the roster is full. ALL spawns must route through here.
static i32 crew_add(game_state* s) {
    if (s->crew.size() >= MAX_CREW_MEMBERS) {
        BS_LOG_WARN("crew_add: roster full (%u) - spawn ignored", MAX_CREW_MEMBERS);
        return -1;
    }
    s->crew.push_back(Crew{});
    return (i32)(s->crew.size() - 1);
}

// ---- Tile colors (flat quads; no textures this phase) ----
static bs_color color_for_tile(TileType t) {
    switch (t) {
        case TILE_FLOOR: return bs_color{ 0.42f, 0.28f, 0.20f, 1.0f }; // brown deck
        case TILE_DOOR:  return bs_color{ 0.68f, 0.46f, 0.18f, 1.0f }; // lit doorway
        case TILE_WALL:  return bs_color{ 0.38f, 0.40f, 0.45f, 1.0f }; // interior wall (grey)
        case TILE_HULL:  return bs_color{ 0.56f, 0.61f, 0.70f, 1.0f }; // hull (lighter blue-grey)
        case TILE_HULL_WINDOW: return bs_color{ 0.50f, 0.75f, 0.85f, 0.5f }; // glass (transparent)
        case TILE_FLOOR_WINDOW: return bs_color{ 0.50f, 0.75f, 0.85f, 0.5f }; // transparent floor
        case TILE_HELM:  return bs_color{ 0.18f, 0.62f, 0.66f, 1.0f }; // helm console (teal)
        case TILE_HULL_DOOR: return bs_color{ 0.85f, 0.72f, 0.20f, 1.0f }; // airlock (caution yellow)
        default:         return bs_color{ 0.0f, 0.0f, 0.0f, 0.0f };    // empty: nothing
    }
}
static const bs_color ROOF_COLOR   = bs_color{ 0.30f, 0.33f, 0.39f, 1.0f };
// Enemy roof silhouette: a hostile red-grey, distinct from the player's neutral blue-grey ROOF_COLOR
// at a glance in global mode. (Interior tiles still use the shared color_for_tile palette for now.)
static const bs_color ENEMY_ROOF_COLOR = bs_color{ 0.46f, 0.22f, 0.24f, 1.0f };
static const bs_color CREW_COLOR   = bs_color{ 0.35f, 0.92f, 1.00f, 1.0f };
static const bs_color SELECT_COLOR = bs_color{ 1.00f, 0.95f, 0.40f, 1.0f }; // yellow ring
static const bs_color PATH_COLOR   = bs_color{ 0.40f, 0.95f, 0.55f, 0.9f }; // green path + marker
// Docking CONNECTOR bridge tile: a man-made airlock-tube colour (brass/steel) distinct from either
// ship's deck, drawn in the two-tile gap between the mated doors. Reads as the walkable tube the crew
// crosses between hulls. Cross-fades to ROOF_COLOR like any other tile when you zoom out.
static const bs_color CONNECTOR_COLOR = bs_color{ 0.74f, 0.70f, 0.42f, 1.0f };
// Debug collider outline: a hot magenta loop traced over the exact tight OBB ships_collide tests, so
// the player can SEE the hitbox sitting flush on each hull. Deliberately a non-deck, non-faction hue
// (nothing else on screen is magenta) so it never reads as part of a ship.
static const bs_color COLLIDER_COLOR = bs_color{ 1.00f, 0.18f, 0.85f, 1.0f };
// Cosmetic smoothed hull silhouette stroke. A bright rim line tracing the de-blocked (marching-squares)
// outline of each hull, drawn just over the roof fill so the ship reads as a solid shape instead of a
// staircase of tiles. Per-faction tints, brightened siblings of each roof colour so the rim still reads
// as "this ship's edge" at a glance. Purely visual — see game_state::hull_outline.
static const bs_color HULL_OUTLINE_COLOR       = bs_color{ 0.62f, 0.74f, 0.92f, 1.0f }; // player: cool steel-blue
static const bs_color ENEMY_HULL_OUTLINE_COLOR = bs_color{ 0.92f, 0.50f, 0.52f, 1.0f }; // enemy: hot red-grey
// Human-readable job state for the periodic stats log.
static const char* job_state_name(JobState st) {
    switch (st) {
        case JOB_QUEUED:           return "queued";
        case JOB_MOVING_TO_TARGET: return "moving";
        case JOB_EXECUTING:        return "executing";
        case JOB_COMPLETED:        return "completed";
        case JOB_FAILED:           return "failed";
        case JOB_INTERRUPTED:      return "interrupted";
        default:                   return "?";
    }
}

// =====================================================================================
// Collision: AABB crew vs solid tiles.
// =====================================================================================
static b8 crew_blocked(const Ship* ship, Vec2 center, f32 r) {
    // `center` is SHIP-LOCAL; the crew AABB is axis-aligned in this frame (y-up: top = larger Y).
    i32 c0, r0, c1, r1;
    ship_local_to_tile(ship, Vec2{ center.x - r, center.y + r }, &c0, &r0); // top-left
    ship_local_to_tile(ship, Vec2{ center.x + r, center.y - r }, &c1, &r1); // bottom-right
    for (i32 row = r0; row <= r1; ++row)
        for (i32 col = c0; col <= c1; ++col)
            if (ship_tile_is_solid(ship, col, row)) return TRUE;
    return FALSE;
}

// =====================================================================================
// Lifecycle.
// =====================================================================================
b8 game_init(Game* game_inst) {
    BS_LOG_DEBUG("game_init: tile-ship prototype starting.");
    game_state* s = (game_state*)game_inst->state;
    if (!s) return FALSE;

    *s = game_state{};
    s->fb_width  = 1280;
    s->fb_height = 720;

    if (!ship_load(&s->ship, "assets/ship.tmap")) {
        BS_LOG_FATAL("game_init: failed to load ship tilemap.");
        return FALSE;
    }

    // Enemy hull: load the same way, then place it NEAR the player so both are on screen together.
    // ship_load sets origin={0,0}, so without this offset the two hulls would overlap at the world
    // origin. Park it off the player's starboard bow (player spans ~416x544 world units centered on
    // origin) and yaw it ~135 deg (CCW) so it reads as a distinct, separately-oriented contact, not
    // a copy of the player ship. No crew/flight/AI yet — this is just geometry for the combat slice.
    if (!ship_load(&s->enemy_ship, "assets/enemy_ship.tmap")) {
        BS_LOG_FATAL("game_init: failed to load enemy ship tilemap.");
        return FALSE;
    }
    // Park the derelict off the player's starboard bow (player spans ~416x544 world units centered on
    // origin), yawed ~135 deg (CCW) so it reads as a distinct, separately-oriented contact. It starts
    // UNDOCKED and at a distance, so by default its interior is hidden (you only see its roof
    // silhouette) — the player must fly over and mate airlocks (HULL_DOOR) to dock and board it.
    s->enemy_ship.origin = Vec2{ 1e4, 0 };
    s->enemy_ship.angle  = 2.36f; // ~135 deg CCW; nose (angle 0 => +Y) swings toward the player

    // Extract each hull's cosmetic smoothed silhouette ONCE (pose-independent ship-local loops;
    // the tilemaps are immutable at runtime). game_render transforms them to world per-vertex and
    // strokes them as line loops. Purely visual — failure (empty hull) just leaves an empty contour
    // that draws nothing, so it's non-fatal.
    ship_hull_contour(&s->ship,       &s->hull_outline);
    ship_hull_contour(&s->enemy_ship, &s->enemy_hull_outline);

    // Camera starts zoomed in (local mode), centered on the ship.
    s->camera          = camera2d_default();
    s->camera.zoom     = ZOOM_START;
    s->camera.position = s->ship.origin;
    s->mode            = MODE_LOCAL;
    s->roof_alpha      = 0.0f;

    // Drop the crew on a known floor tile near the ship's center, in SHIP-LOCAL space.
    s->crew = {};
    s->crew.reserve(MAX_CREW_MEMBERS); // reserve to the cap => push_back never reallocates (invariant)
    crew_add(s);                       // crew[0] (guarded spawn; can't exceed the reserved capacity)
    s->crew[0].position = ship_tile_center_local(&s->ship, s->ship.cols / 2, s->ship.rows / 2);
    s->crew[0].velocity = Vec2{ 0.0f, 0.0f };
    s->crew[0].radius   = CREW_RADIUS;
    s->crew[0].path_len = 0;   // idle: no move order
    s->crew[0].path_idx = 0;
    s->crew[0].crew_selected = FALSE;
    crew_add(s);                       // crew[1]
    s->crew[1].position = ship_tile_center_local(&s->ship, s->ship.cols / 2 + 1, s->ship.rows / 2 - 1);
    s->crew[1].velocity = Vec2{ 0.0f, 0.0f };
    s->crew[1].radius   = CREW_RADIUS;
    s->crew[1].path_len = 0;   // idle: no move order
    s->crew[1].path_idx = 0;
    s->crew[1].crew_selected = FALSE;

    // Crew job system: start IDLE. The player assigns work at runtime — select the crew, hover a
    // station tile (e.g. the helm), and Shift+Right-Click to queue that tile's job; the runner then
    // walks the crew there to perform it. (Phase 3 used to auto-enqueue a PILOTING job here as a
    // runner demo; Phase 5 makes assignment player-driven, so the crew begins with an empty queue.)
    for (auto& c : s->crew)
    {
        c.job_count      = 0;
        c.has_current    = FALSE;
        c.is_active_pilot = FALSE;
        c.current        = Job{};
        c.current.type   = JOB_NONE;
        c.skills         = SkillSet{};

        // Multi-agent avoidance state: no destination reserved, all avoidance clocks clear.
        c.has_dest       = FALSE;
        c.dest_col       = 0;
        c.dest_row       = 0;
        c.block_timer    = 0.0f;
        c.wait_timer     = 0.0f;
        c.stuck_timer    = 0.0f;
    }

    // Free-roam local camera starts focused on the crew (ship-local), so the opening view is
    // centered on it; WASD pans this focus from here.
    s->cam_focus_local = s->crew[0].position;

    // Global-mode flight starts at rest. Prototype hull is a frigate (full strafe thrust).
    s->flight.velocity         = Vec2{ 0.0f, 0.0f };
    s->flight.angular_velocity = 0.0f;
    s->flight.hull             = HULL_FRIGATE;

    renderer_set_clear_color(bs_color{ 0.03f, 0.03f, 0.06f, 1.0f });

    // Bake the bitmap-font atlas now that the renderer is live (it backs the HUD/UI text).
    if (!text_init()) {
        BS_LOG_ERROR("game_init: text_init failed; HUD text will be disabled.");
    }

    s->ship.curr_wall_texture = renderer_load_texture("assets/textures/wall_texture.png");
    s->ship.curr_hull_texture = renderer_load_texture("assets/textures/hull_texture.png");
    s->ship.curr_window_texture = renderer_load_texture("assets/textures/window_texture.png");
    s->ship.curr_door_texture = renderer_load_texture("assets/textures/door_texture.png");

    if (!s->ship.curr_wall_texture.id || 
        !s->ship.curr_hull_texture.id || 
        !s->ship.curr_window_texture.id || 
        !s->ship.curr_door_texture.id) {
        BS_LOG_ERROR("game_init: failed to load one or more ship textures; demo texture will be missing.");
    }

    return TRUE;
}

// =====================================================================================
// Update.
// =====================================================================================
static void update_zoom_and_mode(game_state* s, f32 dt) {
    // Mouse wheel -> multiplicative zoom.
    i32 wheel = input_get_mouse_wheel();
    if (wheel != 0) {
        s->camera.zoom *= powf(ZOOM_STEP, (f32)wheel);
        s->camera.zoom = clampf(s->camera.zoom, ZOOM_MIN, ZOOM_MAX);
    }

    // Hysteresis-latched mode switch.
    if (s->mode == MODE_LOCAL && s->camera.zoom < ZOOM_TO_GLOBAL)
        s->mode = MODE_GLOBAL;
    else if (s->mode == MODE_GLOBAL && s->camera.zoom > ZOOM_TO_LOCAL)
        s->mode = MODE_LOCAL;

    // Cross-fade roof_alpha toward the target for the current mode.
    f32 target = (s->mode == MODE_GLOBAL) ? 1.0f : 0.0f;
    f32 k = ROOF_FADE_SPEED * dt;
    if (k > 1.0f) k = 1.0f;
    s->roof_alpha += (target - s->roof_alpha) * k;
}

static Vec2 read_wasd_dir() {
    Vec2 d{ 0.0f, 0.0f };
    if (input_is_key_down(KEY_W)) d.y += 1.0f;
    if (input_is_key_down(KEY_S)) d.y -= 1.0f;
    if (input_is_key_down(KEY_D)) d.x += 1.0f;
    if (input_is_key_down(KEY_A)) d.x -= 1.0f;
    return d;
}

// Map the current mouse position to the ship tile under the cursor. Returns the world-space
// click point via out_world, and the tile via out_col/out_row (may be out of range -> empty).
// Uses the SAME transform chain the renderer uses (camera2d_screen_to_world accounts for the
// camera rotation, which in local mode cancels the ship heading), so the pick lands on the
// tile actually drawn under the cursor even after the ship has translated/rotated.
static void pick_tile_under_mouse(const game_state* s, Vec2* out_world, i32* out_col, i32* out_row) {
    i32 mx = 0, my = 0;
    input_get_mouse_position(&mx, &my);
    Vec2 screen{ (f32)mx, (f32)my };
    Vec2 world = camera2d_screen_to_world(&s->camera, s->fb_width, s->fb_height, screen);
    if (out_world) *out_world = world;
    ship_world_to_tile(&s->ship, world, out_col, out_row);
}

// ---- Cross-ship boarding command (Phase 3) -------------------------------------------------
// Issue a move order from crew `idx` to a world-space click, choosing same-hull vs cross-ship board.
// When the hulls are DOCKED and the click lands on a walkable tile of the OTHER hull, this is a
// BOARD order: leg A is planned across the crew's CURRENT hull to that hull's airlock-interior tile,
// and the destination (other hull + goal tile) is recorded so the handoff pass can fire leg B after
// the seam hop. Otherwise it's a plain same-hull move on whichever hull the crew currently stands on
// (and the click must be walkable there). Returns TRUE if any order was issued. The crew's existing
// path is left untouched on failure (bad click = no-op), matching crew_plan_path's contract.
static b8 command_crew_to_world(game_state* s, i32 idx, Vec2 world) {
    Crew* c = &s->crew[idx];
    const Ship* cur = crew_ship(s, c);              // hull the crew stands on right now
    const i32   other_id = (c->ship_id == 0) ? 1 : 0;
    Ship*       other = (other_id == 0) ? &s->ship : &s->enemy_ship;

    // Cross-ship board: only when mechanically docked, and only if the click hit walkable DECK on the
    // OTHER hull. (When docked the player sees the enemy interior, so they can click a tile there.)
    if (s->enemy_docked) {
        i32 oc, orow;
        ship_world_to_tile(other, world, &oc, &orow);
        if (ship_tile_is_walkable(other, oc, orow)) {
            // Plan leg A: route across the CURRENT hull to ITS airlock-interior tile (the seam exit).
            // ship_seam_landfall(from=other, to=cur) returns cur's own interior landfall — pass the
            // hulls so `to` is the crew's current hull (where leg A must end).
            i32 ec, er;
            if (ship_seam_landfall(other, cur, ship_dock_tolerance(&s->ship), &ec, &er, nullptr)) {
                if (crew_plan_path(s, idx, ec, er)) {
                    c->boarding          = TRUE;
                    c->board_target_ship = other_id;
                    c->board_goal_col    = oc;
                    c->board_goal_row    = orow;
                    BS_LOG_INFO("board: crew %d -> hull %d tile (%d,%d) via airlock-interior (%d,%d)",
                                idx, other_id, oc, orow, ec, er);
                    return TRUE;
                }
            }
            return FALSE; // couldn't stage the board (no seam / unreachable airlock): leave path as-is
        }
    }

    // Plain same-hull move on the crew's current deck.
    i32 gc, gr;
    ship_world_to_tile(cur, world, &gc, &gr);
    if (ship_tile_is_walkable(cur, gc, gr)) {
        if (crew_plan_path(s, idx, gc, gr)) {
            c->boarding = FALSE; // a fresh same-hull order cancels any in-flight board intent
            return TRUE;
        }
    }
    return FALSE;
}

// ---- Crew COMMAND: RTS-style select + move-order + camera pan. LOCAL-mode only (this is
// control, not simulation). Left-click selects/deselects the crew; right-click orders the
// selected crew to walk to the clicked walkable tile (runs A*); WASD pans the free camera.
static void update_crew_command(game_state* s, f32 dt) {
    // ---- Left-click: select the crew if the click landed on/near it, else deselect ----
    // Skip when the cursor is over the UI: a click on a panel/button must not also
    // select/deselect the crew (the ImGui panel takes mouse precedence over the world).
    // Selection is MUTUALLY EXCLUSIVE: a click selects the SINGLE nearest crew within the pick
    // radius and clears everyone else (or clears all if the click missed). The old per-crew loop
    // tested each crew's radius INDEPENDENTLY, so a click landing within CREW_PICK_RADIUS of two
    // neighbours (tiles are only 32 apart; the radius is 22) selected BOTH — which then built the
    // fixed-title "CREW - JOB CONTROL" panel twice (ImGui id clash) and dispatched a button action
    // to both crew. One nearest-wins pass guarantees at most one selected.
    if (!bs_imgui_wants_mouse() &&
        input_is_button_down(BUTTON_LEFT) &&
        !input_was_button_down(BUTTON_LEFT)) {
        Vec2 world;
        i32 col, row;
        pick_tile_under_mouse(s, &world, &col, &row);

        // Find the nearest crew whose center is within the pick radius (strict `<` so an exact
        // distance tie deterministically keeps the LOWER index instead of flickering).
        i32 best   = -1;
        f32 best_d = (f32)CREW_PICK_RADIUS;
        for (size_t i = 0; i < s->crew.size(); ++i) {
            Vec2 crew_world = ship_local_to_world(crew_ship(s, &s->crew[i]), s->crew[i].position);
            f32  d = vec2_length(vec2_sub(world, crew_world));
            if (d <= best_d && (best < 0 || d < best_d)) { best_d = d; best = (i32)i; }
        }

        // Apply: exactly the nearest crew (if any) is selected; all others cleared.
        for (size_t i = 0; i < s->crew.size(); ++i)
            s->crew[i].crew_selected = ((i32)i == best) ? TRUE : FALSE;
    }

    // ---- Right-click: assign a job (Shift held) or order a move (plain). UI yields first ----
    // Shift + Right-Click on a tile that offers a job (e.g. TILE_HELM -> Piloting) ASSIGNS that
    // job to the selected crew, targeting that exact tile: the job is queued and the runner walks
    // the crew there to perform it. Plain Right-Click is the existing move order. Both yield to the
    // UI so a click on a panel never reaches the world.
    for (auto& c : s->crew) {
        if (c.crew_selected && 
            !bs_imgui_wants_mouse() &&
            input_is_button_down(BUTTON_RIGHT) && 
            !input_was_button_down(BUTTON_RIGHT)){
                Vec2 world;
                i32 goal_col, goal_row;
                pick_tile_under_mouse(s, &world, &goal_col, &goal_row);

                b8 shift = input_is_key_down(KEY_LSHIFT) || input_is_key_down(KEY_RSHIFT);
                TileType hovered = ship_tile_at(&s->ship, goal_col, goal_row);
                JobType  offered = job_for_tile(hovered);

                if (shift && offered != JOB_NONE) {
                    // ---- ASSIGN: enqueue the tile's job, targeted at the hovered station tile ----
                    // The runner (crew_update_jobs) pops it, A*-paths the crew to (goal_col,goal_row), and
                    // performs it there. If the crew is idle the dispatch happens the SAME frame, so it
                    // starts moving immediately; otherwise it queues in execution order behind current work.
                    Job job = job_make_for_tile(hovered, goal_col, goal_row);
                    if (crew_enqueue_job(&c, job)) {
                        BS_LOG_INFO("assign: %s job at tile (%d,%d) -> crew queue (%d queued)",
                                    job_type_name(job.type), goal_col, goal_row, c.job_count);
                    } else {
                        BS_LOG_WARN("assign: crew job queue full (%d); ignored", CREW_MAX_JOBS);
                    }
                } else if (!shift) {
                    // ---- MOVE / BOARD ORDER: walk the selected crew to the clicked world point.
                    // command_crew_to_world decides same-hull move vs cross-ship BOARD: when the
                    // hulls are docked and the click landed on the OTHER hull's walkable deck, it
                    // stages leg A (A* to this hull's airlock-interior tile) and records the board
                    // destination so the handoff pass fires leg B after the seam hop; otherwise it's
                    // a plain same-hull move. Either way it routes avoidance-aware and leaves the
                    // path untouched on a bad click (the per-frame peer gate guarantees no overlap).
                    i32 idx = (i32)(&c - s->crew.data());
                    command_crew_to_world(s, idx, world);
                }
            }
        }
    

    // ---- WASD: pan the free-roam camera (screen-aligned; local view is heading-cancelled) ----
    Vec2 pan = read_wasd_dir();
    if (pan.x != 0.0f || pan.y != 0.0f) {
        s->cam_focus_local = vec2_add(s->cam_focus_local, vec2_scale(pan, CAM_PAN_SPEED * dt));
    }
}

// ---- Crew SIMULATION: steer along the active path. Runs EVERY frame in BOTH modes, so a
// crew that was ordered to move keeps walking its route even after you zoom out to pilot —
// across the decks of the moving, rotating hull (the crew lives in ship-local space and rides
// the rigid-body pose, so the ship-local path stays valid for free). Mirrors simulate_ship.
static void simulate_crew(game_state* s, f32 dt) {
    for (size_t i = 0; i < s->crew.size(); ++i)
    {   
        Crew* c = &s->crew[i];

        // Mid-seam glide: a board crossing is scripting this crew's position frame-by-frame
        // (update_crew_handoff). Don't let steering/friction/collision touch it — just hold velocity
        // at zero and skip. The handoff pass owns position until the crew lands on the boarded hull.
        if (c->transiting) {
            c->velocity = Vec2{ 0.0f, 0.0f };
            continue;
        }

        // Tier-3 YIELD hold: a crew told to wait (crew_resolve_deadlocks) freezes in place this
        // frame so a higher-priority peer can clear the pinch. Zero velocity and skip steering
        // entirely; the resolver counts the timer down. This is what breaks a head-to-head standoff.
        if (c->wait_timer > 0.0f) {
            c->velocity = Vec2{ 0.0f, 0.0f };
            continue;
        }

        // Desired acceleration toward the current waypoint (zero when idle / between waypoints).
        Vec2 desired{ 0.0f, 0.0f };
        if (c->path_len > 0 && c->path_idx < c->path_len) {
            Vec2 to = vec2_sub(c->path[c->path_idx], c->position);
            f32  d  = vec2_length(to);
        
            if (d <= CREW_ARRIVE_RADIUS) {
                // Arrived at this waypoint; advance. Reaching the last one clears the order.
                c->path_idx++;
                if (c->path_idx >= c->path_len) {
                    c->path_len = 0;
                    c->path_idx = 0;
                }
            } else {
                Vec2 dir = vec2_scale(to, 1.0f / d);
                // Ease in on the FINAL waypoint so the crew settles instead of overshooting.
                f32 scale = 1.0f;
                b8  is_final = (c->path_idx == c->path_len - 1);
                if (is_final && d < CREW_SLOW_RADIUS) scale = d / CREW_SLOW_RADIUS;
                desired = vec2_scale(dir, CREW_ACCEL * scale);
            }
        }

        // Accelerate toward the waypoint, apply exponential friction (also brakes an idle crew),
        // clamp to top speed.
        c->velocity = vec2_add(c->velocity, vec2_scale(desired, dt));
        f32 damp = 1.0f - CREW_FRICTION * dt;
        if (damp < 0.0f) damp = 0.0f;
        c->velocity = vec2_scale(c->velocity, damp);
        f32 spd = vec2_length(c->velocity);
        if (spd > CREW_MAX_SPEED) c->velocity = vec2_scale(c->velocity, CREW_MAX_SPEED / spd);

        // Integrate with per-axis collision (move X, then Y; cancel the blocked axis). Each axis
        // is gated by BOTH a solid-tile test (walls/hull/glass — the original safety net) AND the
        // peer gate (crew_peer_blocks — Tier 2), so a step is taken only if it hits neither a wall
        // nor another crew member. The peer gate is per-axis and only vetoes motion that REDUCES
        // separation, so crew slide along each other in open space instead of jamming. This is the
        // hard guarantee that two crew can never overlap or pass through each other, for ANY count.
        Vec2 p = c->position;
        b8   peer_vetoed = FALSE; // did a CREWMATE (not a wall) stop us this frame? -> feeds Tier 3

        // Collision tiles come from the hull THIS crew is bound to (crew_ship), not a hardcoded
        // player ship — a crew that boarded the enemy walks the enemy's decks/walls.
        const Ship* hull = crew_ship(s, c);

        Vec2 nx = Vec2{ p.x + c->velocity.x * dt, p.y };
        if (crew_blocked(hull, nx, c->radius)) {
            c->velocity.x = 0.0f;                          // wall on X
        } else if (crew_peer_blocks(s, (i32)i, p, nx, c->radius)) {
            c->velocity.x = 0.0f;                          // crewmate on X
            peer_vetoed = TRUE;
        } else {
            p.x = nx.x;
        }

        Vec2 ny = Vec2{ p.x, p.y + c->velocity.y * dt };
        if (crew_blocked(hull, ny, c->radius)) {
            c->velocity.y = 0.0f;                          // wall on Y
        } else if (crew_peer_blocks(s, (i32)i, p, ny, c->radius)) {
            c->velocity.y = 0.0f;                          // crewmate on Y
            peer_vetoed = TRUE;
        } else {
            p.y = ny.y;
        }

        c->position = p;

        // Maintain the Tier-3 block clock: accrue while a PEER is actively stopping a crew that
        // still wants to move; clear the instant it's moving freely (or isn't trying to). Only a
        // peer veto counts — being stopped by a wall is normal arrival/steering, not a deadlock.
        b8 wants_to_move = (c->path_len > 0 && c->path_idx < c->path_len) ? TRUE : FALSE;
        if (wants_to_move && peer_vetoed) {
            c->block_timer += dt;
        } else {
            c->block_timer = 0.0f;
        }
    }
}

// ---- Ship CONTROL: pilot input -> forces. The CALLER gates this to global mode AND an
// actively-manned helm (crew.is_active_pilot), so it runs ONLY while a pilot is at the helm;
// an unmanned helm has no flight authority at all (see the gate in game_update).
// WASD here are NOT screen-relative; thrust is applied along the ship's heading
// (Starsector-style). The ship coasts (no passive drag); speed only changes via thrust
// (W/S/Q/E) or the brake (C). This function mutates only flight velocities, never the pose —
// integration is simulate_ship's job, so the ship keeps moving even when nobody is piloting.
// Returns TRUE if a turn is actively commanded this frame (A/D held) so simulate_ship knows to
// skip auto-stabilizing the spin; FALSE otherwise. In local mode this fn isn't called at all,
// so the simulator always stabilizes — that is what finally settles spin carried over from a turn.
static b8 control_ship_global(game_state* s, f32 dt) {
    Ship*       ship = &s->ship;
    ShipFlight* fl   = &s->flight;

    // Heading basis: angle 0 => nose points +Y (up), matching the tilemap's nose-at-top.
    Vec2 fwd   = vec2_rotate(Vec2{ 0.0f, 1.0f }, ship->angle); // forward (nose)
    Vec2 right = vec2_rotate(Vec2{ 1.0f, 0.0f }, ship->angle); // starboard

    f32 strafe = SHIP_ACCEL * STRAFE_FRACTION[(i32)fl->hull];

    // ---- Linear thrust (accumulate this frame's acceleration along the heading) ----
    Vec2 acc{ 0.0f, 0.0f };
    if (input_is_key_down(KEY_W)) acc = vec2_add(acc, vec2_scale(fwd,   SHIP_ACCEL)); // forward
    if (input_is_key_down(KEY_S)) acc = vec2_add(acc, vec2_scale(fwd,  -SHIP_DECEL)); // reverse
    if (input_is_key_down(KEY_E)) acc = vec2_add(acc, vec2_scale(right, strafe));     // strafe right
    if (input_is_key_down(KEY_Q)) acc = vec2_add(acc, vec2_scale(right,-strafe));     // strafe left
    fl->velocity = vec2_add(fl->velocity, vec2_scale(acc, dt));

    // ---- Brake (C): bleed the current velocity toward zero at the decel rate ----
    if (input_is_key_down(KEY_C)) {
        f32 spd = vec2_length(fl->velocity);
        if (spd > 0.0001f) {
            f32 ns = spd - SHIP_DECEL * dt;
            if (ns < 0.0f) ns = 0.0f;
            fl->velocity = vec2_scale(fl->velocity, ns / spd);
        }
    }

    // ---- Angular: A/D ramp angular velocity toward +/- max. The COMPLEMENTARY auto-stabilize
    // (no turn input -> bleed spin to zero) is deliberately NOT done here; it lives in
    // simulate_ship so it runs in BOTH modes. Reason: simulate_ship integrates angular_velocity
    // into ship->angle every frame regardless of mode, but this control fn only runs while
    // piloting (global). If the stabilizer lived here, spin built up in global mode and carried
    // into local mode would never bleed off and the integrator would rotate the ship forever.
    // Here we only ADD the commanded turn and report whether one was issued so the simulator
    // knows to skip stabilizing the spin this frame.
    f32 turn_in = 0.0f;
    if (input_is_key_down(KEY_A)) turn_in += 1.0f; // turn left (CCW)
    if (input_is_key_down(KEY_D)) turn_in -= 1.0f; // turn right (CW)
    if (turn_in != 0.0f) {
        fl->angular_velocity += turn_in * SHIP_TURN_ACCEL * dt;
        return TRUE;  // a turn is actively commanded this frame
    }
    return FALSE;     // no turn commanded -> simulate_ship will auto-stabilize the spin
}

// ---- Ship SIMULATION: momentum -> motion. Runs EVERY frame in BOTH modes. The ship is a
// physical body coasting through space; zooming the camera inside (local mode) must not
// freeze it. Integrates the pose from the flight velocities; the crew lives in ship-local
// space, so it rides this translation AND rotation automatically — no manual carry.
//
// `turn_commanded` is TRUE only while a pilot is actively turning (global mode, A/D held). When
// FALSE — every frame in local mode, and in global mode whenever A/D is released — the flight
// computer auto-stabilizes residual spin back to zero. This stabilization MUST live here (the
// simulator), not in the control fn, because angular_velocity is integrated into ship->angle
// every frame in BOTH modes: if it were only damped while piloting, spin built up in global mode
// and carried into local mode would integrate forever and the ship would rotate without end.
// (Linear velocity is intentionally NOT damped — the ship coasts by design, identically in both
// modes; only the commanded turn auto-settles.)
static void simulate_ship(game_state* s, f32 dt, b8 turn_commanded) {
    Ship*       ship = &s->ship;
    ShipFlight* fl   = &s->flight;

    // Auto-stabilize spin toward zero whenever no turn is commanded (always so in local mode).
    // Bleeds at the turn-accel rate, mirroring the A/D ramp, so a released turn coasts down the
    // same way it spun up — and a turn interrupted by a zoom-to-local still settles to rest
    // instead of integrating forever.
    if (!turn_commanded) {
        f32 drop = SHIP_TURN_ACCEL * dt;
        if (fl->angular_velocity > 0.0f)      fl->angular_velocity = (fl->angular_velocity > drop) ? fl->angular_velocity - drop : 0.0f;
        else if (fl->angular_velocity < 0.0f) fl->angular_velocity = (fl->angular_velocity < -drop) ? fl->angular_velocity + drop : 0.0f;
    }

    // Clamp linear + angular speed to their caps (guards against runaway thrust input).
    f32 spd = vec2_length(fl->velocity);
    if (spd > SHIP_MAX_SPEED) fl->velocity = vec2_scale(fl->velocity, SHIP_MAX_SPEED / spd);
    fl->angular_velocity = clampf(fl->angular_velocity, -SHIP_MAX_TURN, SHIP_MAX_TURN);

    // Integrate the rigid-body pose.
    ship->origin = vec2_add(ship->origin, vec2_scale(fl->velocity, dt));
    ship->angle += fl->angular_velocity * dt;
}

// =====================================================================================
// Ship-ship collision response (combat). The enemy hull is an inoperative derelict — an
// IMMOVABLE obstacle — so the player can never shove it; the player is pushed OUT of any
// penetration instead. Called AFTER simulate_ship has integrated the player's pose: test the two
// hulls' oriented bounding boxes (ships_collide -> SAT minimum-translation vector). On overlap,
// translate the player clear by the FULL MTV (the shortest separating displacement) and cancel
// the INWARD component of linear velocity, so the ship slides along the hull instead of sticking
// to it or phasing through. The tangential component survives, so a grazing impact still glides.
//
// Skipped while docked: a mated ship is rigidly joined at the airlock (Phase 2), so that
// deliberate hull contact must NOT read as a collision to repel — and docking takes no collision
// damage. At the clean mated pose the tight OBBs touch at ~0 penetration anyway (verified
// headless), so even the frame before the dock latches, there is no spurious shove.
static void resolve_ship_collision(game_state* s) {
    if (s->enemy_docked) return; // joined at the airlock: no repulsion, no docking damage

    Vec2 mtv{ 0.0f, 0.0f };
    if (!ships_collide(&s->ship, &s->enemy_ship, &mtv)) return; // hulls clear -> nothing to resolve

    // Push the player fully out of the enemy hull (enemy immovable: it absorbs none of the move).
    s->ship.origin = vec2_add(s->ship.origin, mtv);

    // Cancel only the velocity INTO the surface. The MTV points from the enemy toward the player,
    // i.e. along the outward contact normal n. Velocity with a negative projection on n is heading
    // into the hull; remove exactly that component so the ship stops penetrating but keeps its
    // sideways glide (slide, don't stick or phase). Velocity already pointing away is left alone.
    f32 mlen = vec2_length(mtv);
    if (mlen > 1.0e-6f) {
        Vec2 n  = vec2_scale(mtv, 1.0f / mlen);     // unit outward normal (enemy -> player)
        f32  vn = vec2_dot(s->flight.velocity, n);  // signed speed along that normal
        if (vn < 0.0f)                               // moving INTO the hull
            s->flight.velocity = vec2_sub(s->flight.velocity, vec2_scale(n, vn)); // kill inward part only
    }
}

// =====================================================================================
// Cross-ship crew handoff (Phase 3). Drives the SMOOTH airlock crossing for any crew mid-board.
// A board order is three stages: leg A (A* across the crew's CURRENT hull to that hull's airlock-
// interior tile), the SEAM GLIDE (this pass), then leg B (A* across the boarded hull to the goal).
//
// The glide replaces the old single-frame teleport. Leg A leaves the crew on its own interior tile,
// ~3 tiles (one door + the mated gap + one door) from the destination landfall tile in world space.
// Snapping ship_id+position across that gap in one frame rendered as a visible JUMP between ships.
// Instead, once leg A arrives we start a constant-speed glide of the crew's WORLD position straight
// from its own interior tile to the destination interior tile. Those two tiles are collinear through
// both mated door centers (doors parallel, TWO tiles apart with the connector bridge tile spanning
// the gap at their midpoint), so the straight glide threads both open doorways AND the connector —
// the crew strolls through the airlocks across the bridge. ship_id stays on the ORIGIN hull during the
// glide (local position rewritten each frame to track the lerped world point) and flips to the
// destination hull only at arrival, where leg B is planned. Endpoints are recomputed from the live
// poses every frame so the glide rides the rigid mated pair if it drifts.
//
// Runs AFTER simulate_crew + crew_resolve_deadlocks (so it reads settled end-of-frame arrival state)
// and BEFORE simulate_ship (so the crossing lands before the pose integrates). simulate_crew skips
// any `transiting` crew, so the scripted glide isn't fought by steering/collision.
//
// SAFETY GATES:
//   * START the glide only on a CLEAN leg-A arrival: boarding && path_len==0 && the crew is AT its
//     airlock-interior tile (its current tile == the seam-exit tile). A crew whose order was aborted
//     mid-deck (path cleared by the deadlock resolver) is NOT on that tile -> it drops the board
//     intent without gliding (no teleport).
//   * The hulls must STILL be docked (re-query ship_seam_landfall) both to START and DURING the
//     glide. If they undock mid-transit the seam is gone: end the crossing, leave the crew on the
//     hull it started from at its interior tile, drop the order (you can't cross a broken seam).
static void update_crew_handoff(game_state* s, f32 dt) {
    for (size_t i = 0; i < s->crew.size(); ++i) {
        Crew* c = &s->crew[i];

        // ---- PHASE B: a seam glide is already in flight -> advance it ---------------------------
        if (c->transiting) {
            const Ship* cur   = crew_ship(s, c); // still bound to the ORIGIN hull until arrival
            Ship*       board = (c->board_target_ship == 0) ? &s->ship : &s->enemy_ship;

            // The hulls must stay mated for the seam to exist. If they parted mid-glide, abort: drop
            // the crew back onto its origin hull at the interior tile it launched from, clear transit.
            i32 echk, rchk;
            if (!ship_seam_landfall(board, cur, ship_dock_tolerance(&s->ship), &echk, &rchk, nullptr)) {
                c->position   = c->transit_from_local; // back on the origin interior tile (its frame)
                c->velocity   = Vec2{ 0.0f, 0.0f };
                c->transiting = FALSE;
                BS_LOG_WARN("board: crew %zu seam glide aborted (hulls undocked mid-transit)", i);
                continue;
            }

            // Live world endpoints (recomputed each frame so the glide rides the mated pair's pose):
            // from = own interior tile in the origin frame; to = landfall tile in the boarded frame.
            Vec2 w_from = ship_local_to_world(cur,   c->transit_from_local);
            Vec2 w_to   = ship_local_to_world(board, c->transit_to_local);
            f32  span   = vec2_length(vec2_sub(w_to, w_from));

            // Advance the normalized progress at a constant world speed (units/s). A near-zero span
            // (degenerate geometry) just completes immediately rather than dividing by ~0.
            if (span > 1.0e-3f) c->transit_t += (CREW_SEAM_GLIDE_SPEED * dt) / span;
            else                c->transit_t  = 1.0f;

            if (c->transit_t < 1.0f) {
                // Mid-glide: place the crew at the lerped world point, expressed in the ORIGIN frame
                // (ship_id unchanged) so the renderer draws it exactly there. Velocity stays zero —
                // this is a scripted slide, not steered motion.
                Vec2 wp       = vec2_add(w_from, vec2_scale(vec2_sub(w_to, w_from), c->transit_t));
                c->position   = ship_world_to_local(cur, wp);
                c->velocity   = Vec2{ 0.0f, 0.0f };
                continue;
            }

            // ---- ARRIVAL: bind to the boarded hull and plan leg B ---------------------------------
            c->ship_id    = c->board_target_ship;  // now a crew of the boarded hull
            c->position   = c->transit_to_local;   // landfall tile center, already in the board frame
            c->velocity   = Vec2{ 0.0f, 0.0f };
            c->transiting = FALSE;
            c->transit_t  = 0.0f;

            i32 idx = (i32)i;
            if (!crew_plan_path(s, idx, c->board_goal_col, c->board_goal_row)) {
                BS_LOG_WARN("board: crew %zu crossed seam onto hull %d but leg-B path to (%d,%d) failed; idle",
                            i, c->ship_id, c->board_goal_col, c->board_goal_row);
            } else {
                BS_LOG_INFO("board: crew %zu crossed seam onto hull %d; leg B -> (%d,%d)",
                            i, c->ship_id, c->board_goal_col, c->board_goal_row);
            }
            continue;
        }

        // ---- PHASE A: leg A in flight -> detect a clean arrival and KICK OFF the glide -----------
        if (!c->boarding) continue;
        if (c->path_len != 0) continue; // leg A still walking

        const Ship* cur   = crew_ship(s, c);
        Ship*       board = (c->board_target_ship == 0) ? &s->ship : &s->enemy_ship;

        // Where leg A was supposed to END: this hull's airlock-interior tile (seam exit). Recompute
        // from live geometry; FALSE => the hulls undocked mid-transit, so abort the board cleanly.
        i32 exit_col, exit_row;
        if (!ship_seam_landfall(board, cur, ship_dock_tolerance(&s->ship), &exit_col, &exit_row, nullptr)) {
            c->boarding = FALSE;                 // seam gone (undocked) -> stay put, drop the order
            continue;
        }

        // Did leg A actually ARRIVE at that tile (vs. a deadlock-abort mid-deck)? Compare the crew's
        // current tile to the seam-exit tile; only an on-tile crew crosses.
        i32 here_col, here_row;
        ship_local_to_tile(cur, c->position, &here_col, &here_row);
        if (here_col != exit_col || here_row != exit_row) {
            c->boarding = FALSE;                 // order ended somewhere else -> not a board arrival
            continue;
        }

        // Landfall tile + local center ON THE BOARDED HULL (from=cur leaving, to=board boarding).
        i32  land_col, land_row;
        Vec2 land_local;
        if (!ship_seam_landfall(cur, board, ship_dock_tolerance(&s->ship), &land_col, &land_row, &land_local)) {
            c->boarding = FALSE;                 // shouldn't happen (symmetric with the exit query)
            continue;
        }

        // ---- BEGIN THE GLIDE -------------------------------------------------------------------
        // Stay on the ORIGIN hull (don't flip ship_id yet); record both interior tiles (each in its
        // own hull's frame) and start the constant-speed world-space slide. The crew is currently
        // sitting ON exit tile (exit_col,exit_row); use that tile's center as the glide start so the
        // slide begins exactly where the crew stands (no snap into the glide).
        c->boarding           = FALSE;                 // leg A done; the glide owns the crossing now
        c->transiting         = TRUE;
        c->transit_t          = 0.0f;
        c->transit_from_local = ship_tile_center_local(cur,   exit_col, exit_row);
        c->transit_to_local   = land_local;            // already board-frame (from ship_seam_landfall)
        c->velocity           = Vec2{ 0.0f, 0.0f };
        BS_LOG_INFO("board: crew %zu reached airlock; gliding seam -> hull %d landfall (%d,%d)",
                    i, c->board_target_ship, land_col, land_row);
    }
}

// =====================================================================================
// Docking state machine (Phase 2). Turns the per-frame proximity readout into a latched mechanical
// join toggled by the T key. Runs once per frame BEFORE simulate_ship so a dock latched this frame
// zeroes velocity before the integrator can carry the ship off the mating pose.
//
//   dock_eligible (per frame) = airlocks aligned + within tolerance right now (ships_docked).
//   enemy_docked  (latched)   = the hulls are mechanically mated; flips only on a T edge.
//
// T is edge-triggered (down this frame, up last) so a held key toggles exactly once:
//   * UNDOCKED + eligible + T  -> DOCK:   snap the player so its airlock sits one tile off the
//       enemy's (dock_snap_delta — verified geometry), zero linear+angular velocity, latch docked.
//       The snap guarantees the clean ~0-penetration mated pose; the velocity zero + the
//       collision skip (resolve_ship_collision early-outs while docked) hold it there rigidly.
//   * DOCKED + T              -> UNDOCK:  unlatch, then kick the player off along the outward
//       airlock normal (UNDOCK_IMPULSE) so the hulls part cleanly instead of resting in contact and
//       immediately re-tripping the collision push. Normal collision resumes next frame.
// A dock takes no collision damage (we never call the collision resolver while docked).
static void update_docking(game_state* s) {
    // Live geometry: are the airlocks mated right now? Tolerance from ship_dock_tolerance (sized to
    // the two-tile mated gap; same constant the snap, the render gate, and the harness use). This
    // drives the HUD prompt AND gates a fresh dock.
    const f32 dock_tol = ship_dock_tolerance(&s->ship);
    s->dock_eligible = ships_docked(&s->ship, &s->enemy_ship, dock_tol,
                                    nullptr, nullptr, nullptr, nullptr);

    // Edge-trigger T: TRUE only on the frame the key transitions up -> down (matches the click idiom
    // used for crew orders), so holding T can't oscillate the latch.
    b8 toggle = input_is_key_down(KEY_T) && !input_was_key_down(KEY_T);
    if (!toggle) return;

    if (!s->enemy_docked) {
        // ---- DOCK ----: only if the airlocks are actually aligned this frame.
        if (!s->dock_eligible) return; // not lined up -> T does nothing (prompt isn't showing either)

        Vec2 delta{ 0.0f, 0.0f };
        f32  dtheta = 0.0f;
        if (dock_snap_delta(&s->ship, &s->enemy_ship, dock_tol, &delta, &dtheta)) {
            // Mate rigidly: rotate the player so its airlock door ends PARALLEL to (and facing) the
            // enemy's, THEN translate so the doors sit TWO tiles apart (room for the connector) with no
            // hull overlap. Order matters — dock_snap_delta solved the translation AT the post-rotation
            // pose, so apply the rotation first. (Rotating about origin then translating == the rigid
            // motion it computed.)
            s->ship.angle  = s->ship.angle + dtheta;
            s->ship.origin = vec2_add(s->ship.origin, delta);
        }
        // Rigidly mate: kill all motion so the integrator can't drift the joint, then latch. The
        // collision resolver early-outs on enemy_docked, so the touching hulls won't self-repel.
        s->flight.velocity         = Vec2{ 0.0f, 0.0f };
        s->flight.angular_velocity = 0.0f;
        s->enemy_docked            = TRUE;

        // Spawn the CONNECTOR bridge in the now-two-tile gap between the mated doors. Computed from the
        // settled post-snap pose (dock_connector_tile = midpoint of the mated door centers), so it sits
        // exactly on the seam the crew glide threads and the merged navmesh bridges. The hulls are
        // rigidly joined while docked, so this pose stays valid until undock without recompute.
        Vec2 cworld{ 0.0f, 0.0f };
        f32  cangle = s->ship.angle;
        if (dock_connector_tile(&s->ship, &s->enemy_ship, dock_tol, &cworld, &cangle)) {
            s->connector_world  = cworld;
            s->connector_angle  = cangle;
            s->connector_active = TRUE;
            BS_LOG_INFO("Docked to enemy hull (airlocks mated; connector bridge spawned).");
        } else {
            // Shouldn't happen (we just confirmed dock-eligibility), but never leave a stale bridge.
            s->connector_active = FALSE;
            BS_LOG_INFO("Docked to enemy hull (airlocks mated).");
        }
    } else {
        // ---- UNDOCK ----: unlatch, tear down the connector, and shove the player off along the
        // outward airlock normal so the hulls separate cleanly. Normal is enemy-door -> player-door
        // (points away from the enemy).
        s->enemy_docked     = FALSE;
        s->connector_active = FALSE; // remove the bridge tile: the seam is broken, no cross-ship route

        i32 ca, ra, cb, rb;
        Vec2 kick{ 0.0f, 0.0f };
        if (ships_docked(&s->enemy_ship, &s->ship, dock_tol, &ca, &ra, &cb, &rb)) {
            Vec2 enemy_door  = ship_tile_center_world(&s->enemy_ship, ca, ra);
            Vec2 player_door = ship_tile_center_world(&s->ship,       cb, rb);
            Vec2 out         = vec2_sub(player_door, enemy_door);
            f32  ol          = vec2_length(out);
            if (ol > 1.0e-3f) kick = vec2_scale(out, UNDOCK_IMPULSE / ol);
        }
        if (vec2_length(kick) < 1.0e-3f) kick = Vec2{ 0.0f, -UNDOCK_IMPULSE }; // fallback: shove -Y
        s->flight.velocity = kick;
        BS_LOG_INFO("Undocked from enemy hull (connector removed, separation impulse applied).");
    }
}

// =====================================================================================
// Crew Job Panel HUD. A Dear ImGui panel (via the bs_ui_* facade) surfacing the SELECTED crew's
// job management: the current job + its state + target station + a progress bar, then the queued
// jobs in execution order each with reorder (^/v) and remove (X) controls, plus Assign/Cancel
// actions. Immediate-mode: this BUILDS AND PAINTS in one pass, so it runs from game_render
// (between renderer_begin_frame/end_frame). Shown only while a crew member is selected. Button
// clicks are collected and applied AFTER the panel closes so a remove/reorder never mutates the
// queue mid-build. Input gating uses bs_imgui_wants_mouse() — an ImGui window grabs the cursor.
// =====================================================================================
// Forward declaration: the panel collects a clicked action and dispatches it through this, which
// is defined just below the panel (it applies the intent to the crew's job queue).
static void apply_crew_job_action(game_state* s, UiAction action, i32 param);
static void build_crew_job_panel(game_state* s) {
    for (size_t i = 0; i < s->crew.size(); ++i)
    {
        const Crew* c = &s->crew[i];
        // Build the panel for WHICHEVER crew is selected. Use `continue`, not `return`: with the
        // crew now a vector, an unselected crew earlier in the list (e.g. crew[0]) must be SKIPPED,
        // not treated as a reason to abort — `return` here meant selecting crew[1+] showed no panel
        // because crew[0] (unselected) bailed the whole function. At most one crew is selected at a
        // time (the left-click selector deselects all others), so this builds exactly one panel.
        if (!c->crew_selected)
        {
            continue;
        }

        // ---- palette (RGBA, matches the retired ui.cpp panel) ----
        const f32 TITLE[4] = { 0.55f, 0.85f, 1.00f, 1.00f };
        const f32 TEXT [4] = { 0.86f, 0.90f, 0.96f, 1.00f };
        const f32 DIM  [4] = { 0.60f, 0.64f, 0.72f, 1.00f };

        // Collect at most one clicked action; dispatch AFTER the panel closes so a remove/reorder
        // never mutates c->queue while we are still iterating it to build rows.
        UiAction fired = UI_ACTION_NONE;
        i32      fired_param = 0;

        // Pinned top-right (the helm HUD owns top-left), auto-sized, 12px margin.
        if (bs_ui_begin_panel("CREW - JOB CONTROL", BS_UI_ANCHOR_TOP_RIGHT, 12.0f, BsUiType::BS_UI_TYPE_GAME)) {
            char buf[64];

            // ---- Title ----
            bs_ui_text_colored(TITLE[0], TITLE[1], TITLE[2], TITLE[3], "CREW - JOB CONTROL");
            bs_ui_separator();

            // ---- Current active job (the state rides on the progress bar below) ----
            if (c->has_current)
                snprintf(buf, sizeof(buf), "Job: %s", job_type_name(c->current.type));
            else
                snprintf(buf, sizeof(buf), "Job: Idle");
            bs_ui_text_colored(TEXT[0], TEXT[1], TEXT[2], TEXT[3], buf);

            // ---- Progress bar for the current job (state + percent centered on the fill) ----
            f32  prog = crew_job_progress(c);
            char pbuf[32];
            if (c->has_current)
                snprintf(pbuf, sizeof(pbuf), "%s %.0f%%", job_state_label(c->current.state), prog * 100.0f);
            else
                snprintf(pbuf, sizeof(pbuf), "--");
            bs_ui_progress(prog, pbuf);

            // ---- Target station for the current job ----
            if (c->has_current && c->current.has_target)
                snprintf(buf, sizeof(buf), "Target: %s (%d,%d)",
                         job_station_name(c->current.type), c->current.target_col, c->current.target_row);
            else
                snprintf(buf, sizeof(buf), "Target: -");
            bs_ui_text_colored(DIM[0], DIM[1], DIM[2], DIM[3], buf);

            bs_ui_separator();

            // ---- Queue header ----
            const i32 qn = c->job_count;
            snprintf(buf, sizeof(buf), "Queue (%d):", qn);
            bs_ui_text_colored(TITLE[0], TITLE[1], TITLE[2], TITLE[3], buf);

            // ---- Queue rows in EXECUTION order: "i. Job"  [^][v][X] ----
            // ^ moves the job earlier (disabled on the first), v later (disabled on the last), X removes.
            // param carries the slot index. Button ids are suffixed "##<i>" so the repeated ^/v/X glyphs
            // get unique ImGui ids per row (the suffix is hidden from the visible label).
            const f32 bw = 26.0f;
            for (i32 i = 0; i < qn; ++i) {
                char qbuf[48];
                snprintf(qbuf, sizeof(qbuf), "%d. %s", i + 1, job_type_name(c->queue[i].type));
                bs_ui_text_colored(TEXT[0], TEXT[1], TEXT[2], TEXT[3], qbuf);
                bs_ui_same_line();

                char id[8];
                snprintf(id, sizeof(id), "^##%d", i);
                if (bs_ui_button_sized(id, bw, (i > 0) ? TRUE : FALSE))      { fired = UI_ACTION_REORDER_UP;   fired_param = i; }
                bs_ui_same_line();
                snprintf(id, sizeof(id), "v##%d", i);
                if (bs_ui_button_sized(id, bw, (i < qn - 1) ? TRUE : FALSE)) { fired = UI_ACTION_REORDER_DOWN; fired_param = i; }
                bs_ui_same_line();
                snprintf(id, sizeof(id), "X##%d", i);
                if (bs_ui_button_sized(id, bw, TRUE))                       { fired = UI_ACTION_REMOVE_JOB;   fired_param = i; }
            }

            bs_ui_separator();

            // ---- Action buttons (full width, stacked): assign-to-first-helm + cancel current ----
            if (bs_ui_button("Assign Piloting", TRUE))                       { fired = UI_ACTION_ASSIGN_PILOTING; fired_param = 0; }
            if (bs_ui_button("Cancel Current Job", c->has_current ? TRUE : FALSE)) { fired = UI_ACTION_CANCEL_CURRENT; fired_param = 0; }
        }

        bs_ui_end_panel(); // ALWAYS paired with begin_panel, even when it returned FALSE

        // Dispatch the collected click now that the panel is closed and queue iteration is done.
        if (fired != UI_ACTION_NONE)
            apply_crew_job_action(s, fired, fired_param);
        
    }
}

// Apply a Crew Job Panel button click to the selected crew. `param` is the queue slot (for the
// per-row reorder/remove buttons) or 0 (for the global Assign/Cancel actions). This is the bridge
// from UI intents to crew_jobs.* queue operations — the mirror of the Shift+Right-Click assign path.
static void apply_crew_job_action(game_state* s, UiAction action, i32 param) {

    for (size_t i = 0; i < s->crew.size(); ++i)
    {
        Crew* c = &s->crew[i];
        // Apply the panel action ONLY to the selected crew. The panel is built for the selected
        // crew, so its button clicks must target that same crew — without this guard the action
        // (assign/cancel/reorder/remove) would be applied to EVERY crew in the vector.
        if (!c->crew_selected)
            continue;

        switch (action) {
            case UI_ACTION_ASSIGN_PILOTING: {
                // Convenience: enqueue a TARGET-LESS Piloting job; the runner resolves it to the ship's
                // first helm (job_station_tile). The primary flow is Shift+Right-Click on a helm tile.
                Job job{};
                job.type     = JOB_PILOTING;
                job.priority = 0;
                // Brace both branches: BS_LOG_* macros carry a trailing ';', so a bare if/else here
                // would expand to a double-semicolon empty statement and orphan the else.
                if (crew_enqueue_job(c, job)) {
                    BS_LOG_INFO("panel: assigned Piloting (first helm) -> %d queued", c->job_count);
                } else {
                    BS_LOG_WARN("panel: crew job queue full; assign ignored");
                }
            } break;
            case UI_ACTION_REMOVE_JOB:     crew_remove_job(c, param);       break;
            case UI_ACTION_REORDER_UP:     crew_reorder_job(c, param, -1);  break;
            case UI_ACTION_REORDER_DOWN:   crew_reorder_job(c, param, +1);  break;
            case UI_ACTION_CANCEL_CURRENT: {
                // Interrupt the in-flight job: drop it, halt movement, and clear the pilot flag. The
                // runner then dispatches the next queued job (or stays idle if the queue is empty).
                if (c->has_current) {
                    c->has_current     = FALSE;
                    c->current         = Job{};
                    c->current.type    = JOB_NONE;
                    c->is_active_pilot = FALSE;
                    c->path_len        = 0; // stop walking toward the cancelled job's station
                    c->path_idx        = 0;
                    BS_LOG_INFO("panel: cancelled current job");
                }
            } break;
            default: break;
        }
    }
}

static b8 crew_is_piloting(const game_state* s){
    for (const auto& c : s->crew)
        if (c.is_active_pilot) return TRUE;
    return FALSE;
}

// Wrap an angle (radians) into (-PI, PI] — the shortest signed rotation equivalent to `a`.
// ship->angle is integrated unbounded (simulate_ship never reduces it mod 2*PI), so after the
// pilot spins the hull a few revolutions it can be many multiples of 2*PI. Rotations are
// 2*PI-periodic, so R(wrap(a)) == R(a) — wrapping changes nothing the renderer or mouse-pick
// sees in a SETTLED frame. It only matters when the value is INTERPOLATED: the local<->global
// camera lerps rotation from 0 toward this heading across the zoom cross-fade, and an unwrapped
// multi-turn angle makes that lerp unwind every accumulated revolution (a ~360*N spin) instead
// of righting the view by the shortest arc. Wrap at the interpolation seam to kill that.
static f32 wrap_angle(f32 a) {
    a = fmodf(a + BS_PI, 2.0f * BS_PI);   // shift origin to PI so fmod lands in (-2PI, 2PI)
    if (a < 0.0f) a += 2.0f * BS_PI;      // fmodf keeps the sign of the dividend -> fold negatives up
    return a - BS_PI;                     // shift back: result in (-PI, PI]
}

b8 game_update(Game* game_inst, f32 dt) {
    game_state* s = (game_state*)game_inst->state;
    if (!s) return TRUE;

    if (dt > 0.05f) dt = 0.05f; // clamp hitches

    update_zoom_and_mode(s, dt);

    // Docking state machine (Phase 2): refresh the per-frame eligibility readout (airlocks aligned?)
    // and process a T-key edge to latch/unlatch the mechanical join. Runs BEFORE simulate_ship so a
    // dock latched this frame zeroes the player's velocity before the integrator moves the hull off
    // the mating pose. enemy_docked (the latch) then gates enemy-interior visibility (game_render)
    // and the collision skip (resolve_ship_collision); dock_eligible drives the HUD prompt.
    update_docking(s);

    // The Crew Job Panel is now immediate-mode ImGui: it is BUILT, resolved, and dispatched in one
    // pass from game_render (build_crew_job_panel). Input gating reads bs_imgui_wants_mouse()
    // directly (live ImGui focus state), so no UI pre-pass is needed here in update.

    // CONTROL is mode-gated AND, in global mode, PILOT-gated. Local mode commands the crew. Global
    // mode pilots the ship's thrusters — but ONLY when a crew member is actively manning the helm
    // (crew.is_active_pilot). With the helm UNMANNED — no pilot assigned, or the pilot was ordered
    // away / demoted off the helm — the flight controls are dead: WASD/Q/E/C have no authority.
    // This is the Phase 4 flight gate. Note SIMULATION still runs unconditionally below
    // (simulate_ship), so an unmanned ship keeps COASTING on its existing momentum and any residual
    // spin auto-stabilizes — losing the pilot removes thrust authority, it does not slam the brakes.
    // Piloting reports whether a turn is actively commanded this frame; whenever control_ship_global
    // is skipped (local mode, or global with no pilot) turn_commanded stays FALSE and the simulator
    // auto-stabilizes any carried-over spin.
    b8 turn_commanded = FALSE;
    if (s->mode == MODE_LOCAL) {
        update_crew_command(s, dt);
    } else if (crew_is_piloting(s) && !s->enemy_docked) {
        // Flight authority requires a manned helm AND an unmated ship. While DOCKED the hulls are
        // rigidly joined at the airlock, so thrust is locked out — otherwise a manned helm could
        // drive the ship off the mating pose (collision is disabled while docked, so nothing would
        // stop it drifting free). Press T to undock first; that restores thrust + normal collision.
        turn_commanded = control_ship_global(s, dt);
    }

    // SIMULATION runs EVERY frame in BOTH modes. The crew keeps walking its ordered path and
    // the ship keeps coasting through space regardless of which one you're currently driving,
    // so an ordered crew member traverses the decks while you zoom out and fly. Looking
    // inside (or piloting) doesn't stop the world. The spin stabilizer lives in simulate_ship
    // (gated by turn_commanded) so a turn started in global mode still settles after a zoom-in.
    //
    // The job runner advances the crew's autonomous work BEFORE the motion sim: it issues the
    // crew's A* path (which simulate_crew then steers along) and maintains is_active_pilot. It
    // also runs in both modes, so a crew keeps heading to / manning its station while you fly.
    crew_update_jobs(s, s->crew, dt);
    simulate_crew(s, dt);

    // TIER 3 — deadlock resolution. Runs AFTER simulate_crew, which has just refreshed every
    // crew's block_timer (how long a PEER has been physically vetoing its motion). For anyone
    // stuck too long this replans a detour around the blockers, else makes the lower-priority
    // agent briefly yield so the higher-priority one passes, else aborts a hopeless order — so
    // the per-frame gate (which guarantees no overlap) can never harden into a permanent freeze.
    crew_resolve_deadlocks(s, dt);

    // Cross-ship seam handoff (Phase 3): any crew that just finished leg A of a board order AT its
    // airlock-interior tile hops the mated seam onto the boarded hull (flips ship_id, re-roots local
    // position to the landfall tile, plans leg B). Runs AFTER simulate_crew + the deadlock resolver
    // so it reads the settled end-of-frame arrival state, and BEFORE simulate_ship so the hop lands
    // before the pose integrates. No-op for any crew not mid-board.
    update_crew_handoff(s, dt);

    simulate_ship(s, dt, turn_commanded);

    // Ship-ship collision response runs immediately AFTER the pose is integrated, so any
    // penetration introduced by this frame's motion is corrected before anything else (camera,
    // render, next-frame docking handshake) reads the pose. Pushes the player out of the immovable
    // enemy hull and cancels inward velocity — precise, no phasing. No-op while docked.
    resolve_ship_collision(s);

    // Camera follows the free-roam local FOCUS (local) or the whole ship (global); lerp
    // position by the cross-fade factor so the view glides as the mode flips. The focus lives
    // in ship-local space, so it rides the ship's full pose; on zoom-out the target eases to
    // the ship origin (global view) and the hand-off still glides.
    Vec2 cam_local  = ship_local_to_world(&s->ship, s->cam_focus_local);
    Vec2 cam_global = s->ship.origin;
    s->camera.position = vec2_add(cam_local,
                                  vec2_scale(vec2_sub(cam_global, cam_local), s->roof_alpha));

    // Camera heading: in local mode cancel the ship's heading so the interior reads upright
    // (the view is screen-aligned, like looking down at a fixed room); in global mode hold
    // world-up so the ship rotates within a fixed world (Starsector campaign view). Interpolate
    // across the cross-fade so the scene smoothly rights itself as you zoom. Interior and roof
    // are drawn at the SAME world pose, so they stay perfectly aligned at every blend fraction.
    // (The mouse pick uses this exact rotation, so clicks track the drawn tiles.)
    //
    // wrap_angle is ESSENTIAL here: ship.angle accumulates unbounded, so after a full revolution
    // (>=360 deg) the raw value is ~2*PI+. Interpolating camera.rotation from 0 (global) toward an
    // unwrapped multi-turn angle on zoom-in unwinds every accumulated turn — the abrupt ~360 deg
    // camera spin near a full rotation. Wrapping to (-PI, PI] feeds the SHORTEST-arc equivalent,
    // so the view rights itself the short way. R() is 2*PI-periodic, so the settled local
    // orientation and the mouse-pick transform are bit-identical to the unwrapped value.
    s->camera.rotation = wrap_angle(s->ship.angle) * (1.0f - s->roof_alpha);
    return TRUE;
}

// =====================================================================================
// Render.
// =====================================================================================
// Draw a single ship tile at its true WORLD pose. ship_tile_center_world applies the full
// pose (origin AND angle), and the quad itself is spun by `ship->angle`, so the whole tilemap
// renders as one rigid body. Interior tiles and roof tiles use the SAME pose, so they overlap
// exactly during the zoom cross-fade. In local mode the camera counter-rotates by the heading,
// which cancels this spin and presents the interior upright on screen. Mirrors
// renderer_draw_quad's conventions (white texture, alpha blend when translucent).
// Draw a contiguous horizontal RUN of tiles [col0..col1] on `row` as ONE stretched quad. This is the
// sprite-count optimization: a 73-wide roof row becomes 1 sprite instead of 73. It is PIXEL-EXACT vs.
// drawing each tile separately — every tile in the run shares the ship's one rigid pose, so the run's
// world quad is just a single tile quad widened to the run length and re-centered on the run midpoint
// (same rotation, same edges). Mirrors draw_world_tile's conventions (white texture, alpha blend when
// translucent). Pass col0==col1 to draw a single tile.
static void draw_tile_span(const Ship* ship, i32 col0, i32 col1, i32 row, bs_color col_color, u32 layer, TileType t) {
    i32  n        = col1 - col0 + 1;
    Vec2 c0       = ship_tile_center_local(ship, col0, row);
    Vec2 c1       = ship_tile_center_local(ship, col1, row);
    Vec2 mid_loc  = Vec2{ (c0.x + c1.x) * 0.5f, c0.y }; // run center in ship-local space
    bs_sprite s{};
    s.position = ship_local_to_world(ship, mid_loc);
    s.size     = Vec2{ ship->tile_size * (f32)n, ship->tile_size };
    s.origin   = Vec2{ 0.5f, 0.5f };
    s.rotation = ship->angle;
    s.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    s.tint     = col_color;
    s.texture  = bs_texture{ };
    switch (t)
    {
    case TILE_WALL: {
        s.texture = ship->curr_wall_texture; 
        break;
    }
    case TILE_HULL:{
        s.texture = ship->curr_hull_texture; 
        break;
    }
    case TILE_HULL_WINDOW:{
        s.texture = ship->curr_window_texture; 
        break;
    }
    case TILE_HULL_DOOR:
        s.texture = ship->curr_door_texture;
        break;

    default:
        break;
    }
    s.blend    = (col_color.a < 0.999f) ? BLEND_ALPHA : BLEND_NONE;
    s.layer    = layer;
    renderer_draw_sprite(&s);
}

// Draw one ship's TILES for the current cross-fade — interior (floors/walls) at `interior_a` and/or
// the solid roof silhouette at `roof_a`. Pulled out of game_render so EVERY hull (player + enemy)
// renders through the exact same pose-aware, cross-faded path: interior tiles via color_for_tile,
// roof as one `roof_tint` silhouette over every structure tile. Crew/path/selection overlays are
// NOT here — those are player-only and stay in game_render. `roof_tint` lets each faction get a
// distinct roof color later (the combat seam); pass ROOF_COLOR for the neutral grey today.
//
// SPRITE-COUNT OPTIMIZATION (so a 73x151 = 9165-tile hull fits the 16384 sprite cap): instead of one
// quad per tile, two cheap reductions, BOTH pixel-exact (a hull rides one rigid pose, so a row of
// tiles is collinear and a merged quad covers the same pixels as the per-tile quads):
//   1. CULL — `cam_center`/`vis_r` describe the camera's world-space visible circle (half-screen
//      diagonal in world units, rotation-invariant). A whole hull whose bounding circle misses it is
//      skipped outright (the default off-screen enemy costs 0 sprites); individual off-screen tiles
//      are skipped too, so zoomed-in framing only pays for the visible band.
//   2. RUN-MERGE — each row's contiguous same-key tiles collapse to ONE stretched quad (draw_tile_span):
//      the roof merges runs of structure tiles; the interior merges runs of identical TileType (same
//      type => same color+layer+blend => merge is exact). A full 73-wide roof row: 73 quads -> 1.
static void draw_ship_tiles(const Ship* ship, f32 interior_a, f32 roof_a, bs_color roof_tint,
                            Vec2 cam_center, f32 vis_r) {
    // --- Broad phase: skip the whole hull if its bounding circle can't reach the visible circle. ---
    f32  reach = vis_r + ship_bounding_radius(ship);
    Vec2 d     = vec2_sub(ship->origin, cam_center);
    if (d.x * d.x + d.y * d.y > reach * reach) return;
    f32 vis_r2 = vis_r * vis_r;

    if (interior_a > 0.001f) {
        for (i32 row = 0; row < ship->rows; ++row) {
            i32 col = 0;
            while (col < ship->cols) {
                TileType t = ship_tile_at(ship, col, row);
                // Skip empty tiles and tiles whose center falls outside the visible circle.
                Vec2 wc = ship_tile_center_world(ship, col, row);
                f32  dx = wc.x - cam_center.x, dy = wc.y - cam_center.y;
                if (t == TILE_EMPTY || dx * dx + dy * dy > vis_r2) { ++col; continue; }
                // Greedy run of identical, on-screen tiles -> one stretched quad.
                i32 start = col;
                ++col;
                while (col < ship->cols && ship_tile_at(ship, col, row) == t) {
                    Vec2 wc2 = ship_tile_center_world(ship, col, row);
                    f32  ex = wc2.x - cam_center.x, ey = wc2.y - cam_center.y;
                    if (ex * ex + ey * ey > vis_r2) break;
                    ++col;
                }
                bs_color cc = color_for_tile(t);
                cc.a *= interior_a;
                u32 layer = (t == TILE_WALL || t == TILE_HULL || t == TILE_HULL_WINDOW || t == TILE_HULL_DOOR) ? LAYER_WALL : LAYER_FLOOR;
                draw_tile_span(ship, start, col - 1, row, cc, layer, t);
            }
        }
    }
    if (roof_a > 0.001f) {
        bs_color rc = roof_tint;
        rc.a *= roof_a;
        for (i32 row = 0; row < ship->rows; ++row) {
            i32 col = 0;
            while (col < ship->cols) {
                Vec2 wc = ship_tile_center_world(ship, col, row);
                f32  dx = wc.x - cam_center.x, dy = wc.y - cam_center.y;
                if (!ship_tile_is_structure(ship, col, row) || dx * dx + dy * dy > vis_r2) { ++col; continue; }
                // Greedy run of contiguous, on-screen structure tiles -> one silhouette quad.
                i32 start = col;
                ++col;
                while (col < ship->cols && ship_tile_is_structure(ship, col, row)) {
                    Vec2 wc2 = ship_tile_center_world(ship, col, row);
                    f32  ex = wc2.x - cam_center.x, ey = wc2.y - cam_center.y;
                    if (ex * ex + ey * ey > vis_r2) break;
                    ++col;
                }
                draw_tile_span(ship, start, col - 1, row, rc, LAYER_ROOF, TileType::TILE_EMPTY); // roof_tint silhouette over structure tiles
            }
        }
    }
}

// Draw a single tile-sized quad at an explicit WORLD pose (center + angle), for tiles that live in
// NO ship grid — specifically the docking CONNECTOR bridge that spans the gap between the two hulls.
// Same conventions as draw_tile (white texture, alpha blend when translucent), but the placement is a
// raw world transform instead of a (ship,col,row) lookup, so it rides the rigid mated pair directly.
static void draw_world_tile(Vec2 world_center, f32 angle, f32 tile_size, bs_color col_color, u32 layer) {
    bs_sprite s{};
    s.position = world_center;
    s.size     = Vec2{ tile_size, tile_size };
    s.origin   = Vec2{ 0.5f, 0.5f };
    s.rotation = angle;
    s.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    s.tint     = col_color;
    s.texture  = bs_texture{ BS_INVALID_HANDLE };
    s.blend    = (col_color.a < 0.999f) ? BLEND_ALPHA : BLEND_NONE;
    s.layer    = layer;
    renderer_draw_sprite(&s);
}

// Draw the DEBUG collider outline for one ship: the exact tight OBB that ships_collide tests, as a
// closed 4-segment loop over the world-space corners from ship_collider_corners. This is the SAT box
// hugging the ship's OCCUPIED tiles (so it sits flush with the hull, not the empty .tmap border), and
// because it pulls the very same geometry the collision code does, what you see is what actually
// collides. No-op for an empty ship (no box). Drawn on LAYER_ROOF so it stays legible over the decks.
static void draw_collider_outline(const Ship* ship, bs_color color, f32 thickness) {
    Vec2 k[4];
    if (!ship_collider_corners(ship, k)) return; // empty hull -> nothing to outline
    for (i32 i = 0; i < 4; ++i)
        renderer_draw_line(k[i], k[(i + 1) & 3], thickness, color, LAYER_ROOF);
}

// Stroke one ship's cosmetic smoothed silhouette: every loop of its cached HullContour as a closed
// line loop. The contour verts are ship-LOCAL and pose-independent (extracted once in game_init), so
// here we apply the ship's live pose per vertex via ship_local_to_world (origin + angle) — exactly the
// transform draw_tile_span uses — and the outline rides the hull rigidly at any position/heading. Each
// loop is implicitly closed, so the last vertex strokes back to the first (the `j+1==len ? 0` wrap).
// Drawn on LAYER_HULL_OUTLINE (just above the roof fill) so the rim reads as the ship's edge. No-op for
// an empty contour (no loops). thickness is screen-space (renderer_draw_line divides by zoom), so the
// rim keeps a constant on-screen width as you zoom the camera.
static void draw_hull_outline(const Ship* ship, const HullContour* hc, bs_color color, f32 thickness) {
    i32 nloops = (i32)hc->loop_start.size();
    for (i32 i = 0; i < nloops; ++i) {
        i32 start = hc->loop_start[i];
        i32 len   = hc->loop_len[i];
        if (len < 2) continue; // a degenerate loop has no segment to draw
        for (i32 j = 0; j < len; ++j) {
            Vec2 a = ship_local_to_world(ship, hc->verts[start + j]);
            Vec2 b = ship_local_to_world(ship, hc->verts[start + (j + 1 == len ? 0 : j + 1)]);
            renderer_draw_line(a, b, thickness, color, LAYER_HULL_OUTLINE);
        }
    }
}

static bool EDITOR_DRAW_HULL_BOUNDARY = false;

static void build_editor_panel(game_state* s) {
    if (bs_ui_begin_panel("EDITOR PANEL", BS_UI_ANCHOR_TOP_LEFT, 12.0f, BsUiType::BS_UI_TYPE_EDITOR)) {
        // Inside your rendering loop
        bs_ui_checkbox("Show hull boundary", &EDITOR_DRAW_HULL_BOUNDARY);
        if (EDITOR_DRAW_HULL_BOUNDARY) {
            // ---- Cosmetic smoothed hull silhouette. Stroke each ship's de-blocked (marching-squares) outline
            // over its roof fill so the hull reads as one solid shape instead of a staircase of tiles. The
            // loops are cached ship-local (extracted once in game_init) and transformed to the live pose here,
            // so the rim rides each hull rigidly at its own origin/angle. Drawn unconditionally at every zoom —
            // it sits on LAYER_HULL_OUTLINE above the roof, and the enemy's outline is its only silhouette cue
            // while undocked (its interior is hidden). Per-faction tints match each roof's faction colour.
            draw_hull_outline(&s->ship, &s->hull_outline, HULL_OUTLINE_COLOR, 2.0f);
            draw_hull_outline(&s->enemy_ship, &s->enemy_hull_outline, ENEMY_HULL_OUTLINE_COLOR, 2.0f);
        }
    }
    bs_ui_end_panel();
}

b8 game_render(Game* game_inst, f32 dt) {
    game_state* s = (game_state*)game_inst->state;
    if (!s) return TRUE;

    renderer_set_camera(s->camera);

    const Ship* ship = &s->ship;
    f32 interior_a = 1.0f - s->roof_alpha; // interior fades out as roof fades in
    f32 roof_a     = s->roof_alpha;

    // ---- Camera visible circle (world space), passed to draw_ship_tiles for cull. The view shows a
    // fb_width x fb_height rectangle scaled by 1/zoom and rotated by camera.rotation; its bounding
    // circle (rotation-invariant) has radius = half the screen diagonal in world units. One tile_size
    // of slack keeps a tile straddling the screen edge from being culled. Off-screen hulls/tiles cost
    // 0 sprites — this is what keeps the 9165-tile enemy off the batch until you actually fly near it.
    Vec2 cam_center = s->camera.position;
    f32  z          = (s->camera.zoom > 0.0001f) ? s->camera.zoom : 1.0f;
    f32  vis_r      = 0.5f * sqrtf((f32)s->fb_width * s->fb_width + (f32)s->fb_height * s->fb_height) / z
                      + s->ship.tile_size;

    // ---- Both hulls' tiles through the shared cross-fade path. One call per ship emits the
    // interior (LAYER_FLOOR/WALL) AND the roof silhouette (LAYER_ROOF); layer order — not emit
    // order — governs overlap, so the player crew overlay below correctly slots between them.
    // The enemy hull rides its own origin/angle, so it draws at its placed pose automatically.
    draw_ship_tiles(ship, interior_a, roof_a, ROOF_COLOR, cam_center, vis_r);

    // The ENEMY hull's interior is GATED on docking. You can't see inside a hostile ship: while
    // UNDOCKED it renders as a solid roof silhouette at EVERY zoom (interior alpha forced to 0, roof
    // forced fully opaque), so zooming into local mode shows only its outline — never its decks.
    // Once the airlocks mate (enemy_docked, computed in game_update), it falls back to the SAME
    // interior<->roof cross-fade as the player, so you can board/inspect the revealed interior in
    // local mode. This is the render half of the "dock to see inside the inoperative ship" rule.
    f32 enemy_interior_a = s->enemy_docked ? interior_a : 0.0f;
    f32 enemy_roof_a     = s->enemy_docked ? roof_a     : 1.0f;
    draw_ship_tiles(&s->enemy_ship, enemy_interior_a, enemy_roof_a, ENEMY_ROOF_COLOR, cam_center, vis_r);

    // ---- Docking CONNECTOR bridge. While the hulls are mated a single walkable tile spans the gap
    // between the two airlocks (spawned in update_docking via dock_connector_tile). It belongs to no
    // ship grid, so it's drawn here as a raw world-pose quad at the stored connector pose. It rides
    // the SAME interior<->roof cross-fade as the decks: interior tube colour when zoomed in (the crew
    // walks across it), roof silhouette when zoomed out, so it reads as one continuous structure with
    // both hulls at every blend fraction. Torn down on undock (connector_active clears), so it simply
    // stops drawing the instant the ships part.
    if (s->connector_active) {
        if (interior_a > 0.001f) {
            bs_color cc = CONNECTOR_COLOR;
            cc.a *= interior_a;
            draw_world_tile(s->connector_world, s->connector_angle, s->ship.tile_size, cc, LAYER_FLOOR);
        }
        if (roof_a > 0.001f) {
            bs_color rc = ROOF_COLOR;
            rc.a *= roof_a;
            draw_world_tile(s->connector_world, s->connector_angle, s->ship.tile_size, rc, LAYER_ROOF);
        }
    }

    // ---- DEBUG collider overlay. Trace the exact tight OBB ships_collide tests around BOTH hulls so
    // the hitbox is visible: it hugs the occupied tiles, rides each ship's full pose, and (while
    // docked) you can see the two boxes meet flush at the airlocks with the connector spanning the
    // gap. Same magenta loop on player and enemy; pulled straight from the collision geometry so the
    // outline can never drift from what actually collides. Drawn unconditionally — the collider exists
    // whether or not the hulls are touching.
    draw_collider_outline(ship, COLLIDER_COLOR, 1.5f);
    draw_collider_outline(&s->enemy_ship, COLLIDER_COLOR, 1.5f);

    // ---- Crew + move-order feedback. Each crew rides the hull it's BOUND to (crew_ship): a crew
    // that boarded the enemy draws on the enemy's deck at the enemy's pose. Player crew fade with
    // the player interior; ENEMY-bound crew fade with the enemy interior alpha (enemy_interior_a),
    // so a boarded crew is visible only while docked — once you undock, the enemy hull (and anyone
    // aboard it) goes back to an opaque roof silhouette, exactly like its decks. ----
    if (interior_a > 0.001f) 
    {
        // ---- Move-order feedback: path line (crew -> remaining waypoints) + destination
        // marker. Waypoints are ship-local; lift each to world so they ride the ship's pose
        // and stay glued to the deck as it moves/rotates. Drawn above the crew, fading with
        // the interior so it vanishes into the roof cross-fade.
        for(auto& c : s->crew)
        {
            // Resolve the hull this crew stands on, and the interior alpha of THAT hull. Enemy crew
            // are hidden (alpha 0) while undocked — skip drawing them entirely so no ghost leaks
            // through the enemy roof silhouette.
            const Ship* chull = crew_ship(s, &c);
            f32 crew_a = (c.ship_id == 0) ? interior_a : enemy_interior_a;
            if (crew_a <= 0.001f) continue;

            if (c.path_len > 0 && c.path_idx < c.path_len) {
                bs_color pc = PATH_COLOR;
                pc.a *= crew_a;
                Vec2 prev = ship_local_to_world(chull, c.position);
                for (i32 i = c.path_idx; i < c.path_len; ++i) {
                    Vec2 wp = ship_local_to_world(chull, c.path[i]);
                    renderer_draw_line(prev, wp, 1.0f, pc, LAYER_PATH);
                    prev = wp;
                }
                Vec2 goal = ship_local_to_world(chull, c.path[c.path_len - 1]);
                renderer_draw_circle(goal, 8.0f, 16, 1.0f, pc, LAYER_PATH);
            }
            // Crew member (a simple quad for now), only meaningful in local view. Its stored
            // position is ship-local; draw it at its hull's full pose (world position + heading)
            // so it's part of that rigid body. In local mode the camera cancels the PLAYER heading,
            // so player crew read screen-upright; enemy crew carry the enemy's relative yaw.
            bs_color crew = CREW_COLOR;
            crew.a *= crew_a;
            bs_sprite cs{};
            cs.position = ship_local_to_world(chull, c.position);
            cs.size     = Vec2{ c.radius * 2.0f, c.radius * 2.0f };
            cs.origin   = Vec2{ 0.5f, 0.5f };
            cs.rotation = chull->angle;
            cs.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
            cs.tint     = crew;
            cs.texture  = bs_texture{ BS_INVALID_HANDLE };
            cs.blend    = (crew.a < 0.999f) ? BLEND_ALPHA : BLEND_NONE;
            cs.layer    = LAYER_CREW;
            renderer_draw_sprite(&cs);

            // Selection ring around the selected crew (rotation-invariant circle, so it reads the
            // same whether or not the heading is cancelled). Sits above the path.
            if (c.crew_selected) {
                bs_color sc = SELECT_COLOR;
                sc.a *= crew_a;
                renderer_draw_circle(cs.position, c.radius + 4.0f, 20, 1.0f, sc, LAYER_SELECT);
            }

        }
    }

    // ---- Helm / flight-authority HUD (screen-anchored bitmap text) ----
    // Surfaces the Phase 4 flight gate so it's legible instead of mysterious: an unmanned ship
    // that ignores WASD must SAY so, or it just looks broken. Shown only in/around global mode
    // (where flight control applies); fades in with the roof cross-fade so it tracks the mode.
    if (roof_a > 0.01f) {
        b8 piloted = crew_is_piloting(s);
        const char* helm_line = piloted ? "HELM: MANNED  -  FLIGHT READY"
                                        : "HELM: UNMANNED  -  FLIGHT LOCKED";
        // Green when a pilot is at the helm, amber-red when the helm is empty.
        bs_color helm_col = piloted ? bs_color{ 0.40f, 0.85f, 0.45f, 1.0f }
                                    : bs_color{ 0.95f, 0.45f, 0.25f, 1.0f };
        helm_col.a *= roof_a; // fade with the mode cross-fade
        text_draw(helm_line, 12.0f, 12.0f, 2.0f, helm_col,
                  &s->camera, s->fb_width, s->fb_height, LAYER_HUD_TEXT);
    }

    // ---- Docking prompt HUD (Phase 2) ----
    // Surfaces the dock/undock affordance so the T key is discoverable. Three states:
    //   * DOCKED        -> "DOCKED  -  PRESS T TO UNDOCK" (cyan): the join is latched; T releases it.
    //   * dock_eligible -> "PRESS T TO DOCK" (green): airlocks are aligned & close; T mates the hulls.
    //   * otherwise     -> nothing (no affordance available; fly closer / align the airlocks).
    // Drawn one line below the helm HUD, screen-anchored. Always shown when docked (so the player can
    // always see how to leave); the eligibility prompt is independent of mode since you align hulls in
    // global mode but may inspect the join in local mode.
    {
        const char* dock_line = nullptr;
        bs_color    dock_col;
        if (s->enemy_docked) {
            dock_line = "DOCKED  -  PRESS T TO UNDOCK";
            dock_col  = bs_color{ 0.35f, 0.80f, 0.90f, 1.0f }; // cyan: actively mated
        } else if (s->dock_eligible) {
            dock_line = "PRESS T TO DOCK";
            dock_col  = bs_color{ 0.45f, 0.88f, 0.50f, 1.0f }; // green: ready to mate
        }
        if (dock_line) {
            // Place just under the helm line (helm is scale 2 => ~16px glyphs at y=12). Use the same
            // left margin so the HUD reads as one stacked block.
            text_draw(dock_line, 12.0f, 12.0f + 16.0f * 2.0f + 6.0f, 2.0f, dock_col,
                      &s->camera, s->fb_width, s->fb_height, LAYER_HUD_TEXT);
        }
    }

    // Crew Job Panel — immediate-mode ImGui, built between renderer_begin_frame/end_frame (we are
    // inside game_render). It surfaces the selected crew's jobs, resolves its own clicks, and
    // dispatches them to the job queue. ImGui composites it on top of the world automatically, and
    // it self-anchors to the screen (no camera-cancel needed). No-op when no crew is selected.
    build_crew_job_panel(s);

    // Editor Panel
    build_editor_panel(s);

    // Periodic stats to the log (the only on-screen text is the diagnostic helm-status HUD above).
    {
        // static f32 acc = 0.0f;
        // acc += dt;
        // if (acc >= 1.0f) {
        //     acc = 0.0f;
        //     bs_frame_stats fs = renderer_get_frame_stats();
        //     f32 spd = vec2_length(s->flight.velocity);
        //     const char* jstate = s->crew.has_current ? job_state_name(s->crew.current.state) : "idle";
        //     BS_LOG_INFO("proto: mode=%s zoom=%.2f roof=%.2f heading=%.0fdeg spd=%.0f crewpath=%d job=%s pilot=%d quads=%u draws=%u",
        //                 s->mode == MODE_LOCAL ? "local" : "global",
        //                 s->camera.zoom, s->roof_alpha,
        //                 s->ship.angle * BS_RAD2DEG, spd,
        //                 s->crew.path_len,
        //                 jstate, (i32)s->crew.is_active_pilot,
        //                 fs.sprite_count, fs.draw_calls);
        // }
    }

#if BS_DEBUG
    // Pick the ship tile col and row under the mouse.
    // Compute the LOCAL tile center for that tile, then convert to WORLD.
    // Draw a circle at that world center.
    // Works when docked by checking both ships.
    Vec2 t_world;
    i32 t_col, t_row;
    pick_tile_under_mouse(s, &t_world, &t_col, &t_row);

    // First check player ship
    const Ship* picked_ship = &s->ship;
    TileType t = ship_tile_at(picked_ship, t_col, t_row);

    // If docked and player ship tile is empty, check enemy ship
    if (s->enemy_docked && t == TILE_EMPTY) {
        ship_world_to_tile(&s->enemy_ship, t_world, &t_col, &t_row);
        t = ship_tile_at(&s->enemy_ship, t_col, t_row);
        if (t != TILE_EMPTY) {
            picked_ship = &s->enemy_ship;
        }
    }

    // Only draw if we found a valid tile
    if (t != TILE_EMPTY) {
        const Vec2 local_center = ship_tile_center_local(picked_ship, t_col, t_row);
        const Vec2 world_center = ship_local_to_world(picked_ship, local_center);
        renderer_draw_circle(world_center, 16.0f, 16, 1.0f, bs_color{1,1,0,0.75f}, LAYER_PATH);
    }

    renderer_draw_grid(Vec2{-1e5, -1e5}, Vec2{1e5, 1e5}, 1e3, 1.0f, bs_color{1,1,1,0.5f}, LAYER_DEBUG);
#endif

    return TRUE;
}

void game_on_resize(Game* game_inst, u32 width, u32 height) {
    game_state* s = (game_state*)game_inst->state;
    if (s) {
        s->fb_width  = (u16)width;
        s->fb_height = (u16)height;
    }
    BS_LOG_DEBUG("game_on_resize: %u x %u", width, height);
}
