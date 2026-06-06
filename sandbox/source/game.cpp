#include "game.h"
#include "nav.h"
#include "crew_jobs.h"
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
static const f32 ZOOM_MIN          = 0.35f;  // most zoomed-out (global)
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
static const f32 CREW_MAX_SPEED     = 100.0f;
static const f32 CREW_RADIUS        = 10.0f;
static const f32 CREW_ARRIVE_RADIUS = 6.0f;   // within this of a waypoint => arrived (advance)
static const f32 CREW_SLOW_RADIUS   = 30.0f;  // ease-in distance for the FINAL waypoint (arrival)
static const f32 CREW_PICK_RADIUS   = 22.0f;  // click within this (world units) of crew => select

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

// Strafe thrust fraction of SHIP_ACCEL by hull class: frigate/destroyer/cruiser/capital.
static const f32 STRAFE_FRACTION[4] = { 1.00f, 0.75f, 0.50f, 0.25f };

// ---- Render layers (lower draws first) ----
static const u32 LAYER_FLOOR  = 1;
static const u32 LAYER_WALL   = 2;
static const u32 LAYER_CREW   = 5;
static const u32 LAYER_PATH   = 6;   // move-order path line + destination marker (over crew)
static const u32 LAYER_SELECT = 7;   // selection ring (over the path)
static const u32 LAYER_ROOF   = 10;
static const u32 LAYER_HUD_TEXT = 100; // screen-space HUD/UI text — always on top

static const u32 LAYER_DEBUG  = 0;

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
        default:         return bs_color{ 0.0f, 0.0f, 0.0f, 0.0f };    // empty: nothing
    }
}
static const bs_color ROOF_COLOR   = bs_color{ 0.30f, 0.33f, 0.39f, 1.0f };
static const bs_color CREW_COLOR   = bs_color{ 0.35f, 0.92f, 1.00f, 1.0f };
static const bs_color SELECT_COLOR = bs_color{ 1.00f, 0.95f, 0.40f, 1.0f }; // yellow ring
static const bs_color PATH_COLOR   = bs_color{ 0.40f, 0.95f, 0.55f, 0.9f }; // green path + marker

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

    // Camera starts zoomed in (local mode), centered on the ship.
    s->camera          = camera2d_default();
    s->camera.zoom     = ZOOM_START;
    s->camera.position = s->ship.origin;
    s->mode            = MODE_LOCAL;
    s->roof_alpha      = 0.0f;

    // Drop the crew on a known floor tile near the ship's center, in SHIP-LOCAL space.
    s->crew.position = ship_tile_center_local(&s->ship, s->ship.cols / 2, s->ship.rows / 2);
    s->crew.velocity = Vec2{ 0.0f, 0.0f };
    s->crew.radius   = CREW_RADIUS;
    s->crew.path_len = 0;   // idle: no move order
    s->crew.path_idx = 0;
    s->crew_selected = FALSE;

    // Crew job system: start IDLE. The player assigns work at runtime — select the crew, hover a
    // station tile (e.g. the helm), and Shift+Right-Click to queue that tile's job; the runner then
    // walks the crew there to perform it. (Phase 3 used to auto-enqueue a PILOTING job here as a
    // runner demo; Phase 5 makes assignment player-driven, so the crew begins with an empty queue.)
    s->crew.job_count       = 0;
    s->crew.has_current     = FALSE;
    s->crew.is_active_pilot = FALSE;
    s->crew.current         = Job{};
    s->crew.current.type    = JOB_NONE;
    s->crew.skills          = SkillSet{};

    // Free-roam local camera starts focused on the crew (ship-local), so the opening view is
    // centered on it; WASD pans this focus from here.
    s->cam_focus_local = s->crew.position;

    // Global-mode flight starts at rest. Prototype hull is a frigate (full strafe thrust).
    s->flight.velocity         = Vec2{ 0.0f, 0.0f };
    s->flight.angular_velocity = 0.0f;
    s->flight.hull             = HULL_FRIGATE;

    renderer_set_clear_color(bs_color{ 0.03f, 0.03f, 0.06f, 1.0f });

    // Bake the bitmap-font atlas now that the renderer is live (it backs the HUD/UI text).
    if (!text_init()) {
        BS_LOG_ERROR("game_init: text_init failed; HUD text will be disabled.");
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

// ---- Crew COMMAND: RTS-style select + move-order + camera pan. LOCAL-mode only (this is
// control, not simulation). Left-click selects/deselects the crew; right-click orders the
// selected crew to walk to the clicked walkable tile (runs A*); WASD pans the free camera.
static void update_crew_command(game_state* s, f32 dt) {
    // ---- Left-click: select the crew if the click landed on/near it, else deselect ----
    // Skip when the cursor is over the UI: a click on a panel/button must not also
    // select/deselect the crew (the ImGui panel takes mouse precedence over the world).
    if (!bs_imgui_wants_mouse() &&
        input_is_button_down(BUTTON_LEFT) && !input_was_button_down(BUTTON_LEFT)) {
        Vec2 world;
        i32 col, row;
        pick_tile_under_mouse(s, &world, &col, &row);
        Vec2 crew_world = ship_local_to_world(&s->ship, s->crew.position);
        f32  d = vec2_length(vec2_sub(world, crew_world));
        s->crew_selected = (d <= CREW_PICK_RADIUS) ? TRUE : FALSE;
    }

    // ---- Right-click: assign a job (Shift held) or order a move (plain). UI yields first ----
    // Shift + Right-Click on a tile that offers a job (e.g. TILE_HELM -> Piloting) ASSIGNS that
    // job to the selected crew, targeting that exact tile: the job is queued and the runner walks
    // the crew there to perform it. Plain Right-Click is the existing move order. Both yield to the
    // UI so a click on a panel never reaches the world.
    if (s->crew_selected && !bs_imgui_wants_mouse() &&
        input_is_button_down(BUTTON_RIGHT) && !input_was_button_down(BUTTON_RIGHT)) {
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
            if (crew_enqueue_job(&s->crew, job)) {
                BS_LOG_INFO("assign: %s job at tile (%d,%d) -> crew queue (%d queued)",
                            job_type_name(job.type), goal_col, goal_row, s->crew.job_count);
            } else {
                BS_LOG_WARN("assign: crew job queue full (%d); ignored", CREW_MAX_JOBS);
            }
        } else if (!shift && ship_tile_is_walkable(&s->ship, goal_col, goal_row)) {
            // ---- MOVE ORDER: walk the selected crew to the clicked tile (A* in ship-local) ----
            i32 start_col, start_row;
            ship_local_to_tile(&s->ship, s->crew.position, &start_col, &start_row);

            i32 len = 0;
            // nav_find_path leaves the crew's path untouched on failure, so a bad order is a
            // no-op (the crew keeps whatever it was doing).
            if (nav_find_path(&s->ship, start_col, start_row, goal_col, goal_row,
                              s->crew.path, &len)) {
                s->crew.path_len = len;
                s->crew.path_idx = 0; // path[0] is the crew's own tile; arrival advances at once
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
    Crew* c = &s->crew;

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

    // Integrate with per-axis tile collision (move X, then Y; cancel the blocked axis). With a
    // valid path this rarely fires — it's a safety net that keeps the crew out of solid tiles.
    Vec2 p = c->position;

    Vec2 nx = Vec2{ p.x + c->velocity.x * dt, p.y };
    if (!crew_blocked(&s->ship, nx, c->radius))
        p.x = nx.x;
    else
        c->velocity.x = 0.0f;

    Vec2 ny = Vec2{ p.x, p.y + c->velocity.y * dt };
    if (!crew_blocked(&s->ship, ny, c->radius))
        p.y = ny.y;
    else
        c->velocity.y = 0.0f;

    c->position = p;
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
    if (!s->crew_selected) return;   // no selection -> no panel (clean screen)

    const Crew* c = &s->crew;

    // ---- palette (RGBA, matches the retired ui.cpp panel) ----
    const f32 TITLE[4] = { 0.55f, 0.85f, 1.00f, 1.00f };
    const f32 TEXT [4] = { 0.86f, 0.90f, 0.96f, 1.00f };
    const f32 DIM  [4] = { 0.60f, 0.64f, 0.72f, 1.00f };

    // Collect at most one clicked action; dispatch AFTER the panel closes so a remove/reorder
    // never mutates c->queue while we are still iterating it to build rows.
    UiAction fired = UI_ACTION_NONE;
    i32      fired_param = 0;

    // Pinned top-right (the helm HUD owns top-left), auto-sized, 12px margin.
    if (bs_ui_begin_panel("CREW - JOB CONTROL", BS_UI_ANCHOR_TOP_RIGHT, 12.0f)) {
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

// Apply a Crew Job Panel button click to the selected crew. `param` is the queue slot (for the
// per-row reorder/remove buttons) or 0 (for the global Assign/Cancel actions). This is the bridge
// from UI intents to crew_jobs.* queue operations — the mirror of the Shift+Right-Click assign path.
static void apply_crew_job_action(game_state* s, UiAction action, i32 param) {
    Crew* c = &s->crew;
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

b8 game_update(Game* game_inst, f32 dt) {
    game_state* s = (game_state*)game_inst->state;
    if (!s) return TRUE;
    if (dt > 0.05f) dt = 0.05f; // clamp hitches

    update_zoom_and_mode(s, dt);

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
    } else if (s->crew.is_active_pilot) {
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
    crew_update_jobs(s, &s->crew, dt);
    simulate_crew(s, dt);
    simulate_ship(s, dt, turn_commanded);

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
    s->camera.rotation = s->ship.angle * (1.0f - s->roof_alpha);
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
static void draw_tile(const Ship* ship, i32 col, i32 row, bs_color col_color, u32 layer) {
    bs_sprite s{};
    s.position = ship_tile_center_world(ship, col, row);
    s.size     = Vec2{ ship->tile_size, ship->tile_size };
    s.origin   = Vec2{ 0.5f, 0.5f };
    s.rotation = ship->angle;
    s.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    s.tint     = col_color;
    s.texture  = bs_texture{ BS_INVALID_HANDLE };
    s.blend    = (col_color.a < 0.999f) ? BLEND_ALPHA : BLEND_NONE;
    s.layer    = layer;
    renderer_draw_sprite(&s);
}

b8 game_render(Game* game_inst, f32 dt) {
    game_state* s = (game_state*)game_inst->state;
    if (!s) return TRUE;

    renderer_set_camera(s->camera);

    const Ship* ship = &s->ship;
    f32 interior_a = 1.0f - s->roof_alpha; // interior fades out as roof fades in
    f32 roof_a     = s->roof_alpha;

    // ---- Interior (local view): floors/doors then walls/hull, then the crew ----
    if (interior_a > 0.001f) {
        for (i32 row = 0; row < ship->rows; ++row) {
            for (i32 col = 0; col < ship->cols; ++col) {
                TileType t = ship_tile_at(ship, col, row);
                if (t == TILE_EMPTY) continue;
                bs_color cc = color_for_tile(t);
                cc.a *= interior_a;
                u32 layer = (t == TILE_WALL || t == TILE_HULL || t == TILE_HULL_WINDOW) ? LAYER_WALL : LAYER_FLOOR;
                draw_tile(ship, col, row, cc, layer);
            }
        }

        // ---- Move-order feedback: path line (crew -> remaining waypoints) + destination
        // marker. Waypoints are ship-local; lift each to world so they ride the ship's pose
        // and stay glued to the deck as it moves/rotates. Drawn above the crew, fading with
        // the interior so it vanishes into the roof cross-fade.
        Crew* c = &s->crew;
        if (c->path_len > 0 && c->path_idx < c->path_len) {
            bs_color pc = PATH_COLOR;
            pc.a *= interior_a;
            Vec2 prev = ship_local_to_world(ship, c->position);
            for (i32 i = c->path_idx; i < c->path_len; ++i) {
                Vec2 wp = ship_local_to_world(ship, c->path[i]);
                renderer_draw_line(prev, wp, 2.0f, pc, LAYER_PATH);
                prev = wp;
            }
            Vec2 goal = ship_local_to_world(ship, c->path[c->path_len - 1]);
            renderer_draw_circle(goal, 8.0f, 16, 2.0f, pc, LAYER_PATH);
        }

        // Crew member (a simple quad for now), only meaningful in local view. Its stored
        // position is ship-local; draw it at the ship's full pose (world position + heading)
        // so it's part of the rigid body. In local mode the camera cancels the heading, so the
        // crew reads screen-upright — and a future directional sprite will face correctly.
        bs_color crew = CREW_COLOR;
        crew.a *= interior_a;
        bs_sprite cs{};
        cs.position = ship_local_to_world(ship, c->position);
        cs.size     = Vec2{ c->radius * 2.0f, c->radius * 2.0f };
        cs.origin   = Vec2{ 0.5f, 0.5f };
        cs.rotation = ship->angle;
        cs.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        cs.tint     = crew;
        cs.texture  = bs_texture{ BS_INVALID_HANDLE };
        cs.blend    = (crew.a < 0.999f) ? BLEND_ALPHA : BLEND_NONE;
        cs.layer    = LAYER_CREW;
        renderer_draw_sprite(&cs);

        // Selection ring around the selected crew (rotation-invariant circle, so it reads the
        // same whether or not the heading is cancelled). Sits above the path.
        if (s->crew_selected) {
            bs_color sc = SELECT_COLOR;
            sc.a *= interior_a;
            renderer_draw_circle(cs.position, c->radius + 4.0f, 20, 2.0f, sc, LAYER_SELECT);
        }
    }

    // ---- Roof (global view): every structure tile as one solid silhouette. draw_tile already
    // renders at the ship's full world pose, so turning (A/D) and heading-relative thrust
    // (W/S/Q/E) are visible, and the roof overlaps the interior exactly during the cross-fade.
    if (roof_a > 0.001f) {
        bs_color rc = ROOF_COLOR;
        rc.a *= roof_a;
        for (i32 row = 0; row < ship->rows; ++row)
            for (i32 col = 0; col < ship->cols; ++col)
                if (ship_tile_is_structure(ship, col, row))
                    draw_tile(ship, col, row, rc, LAYER_ROOF);
    }

    // ---- Helm / flight-authority HUD (screen-anchored bitmap text) ----
    // Surfaces the Phase 4 flight gate so it's legible instead of mysterious: an unmanned ship
    // that ignores WASD must SAY so, or it just looks broken. Shown only in/around global mode
    // (where flight control applies); fades in with the roof cross-fade so it tracks the mode.
    if (roof_a > 0.01f) {
        b8 piloted = s->crew.is_active_pilot;
        const char* helm_line = piloted ? "HELM: MANNED  -  FLIGHT READY"
                                        : "HELM: UNMANNED  -  FLIGHT LOCKED";
        // Green when a pilot is at the helm, amber-red when the helm is empty.
        bs_color helm_col = piloted ? bs_color{ 0.40f, 0.85f, 0.45f, 1.0f }
                                    : bs_color{ 0.95f, 0.45f, 0.25f, 1.0f };
        helm_col.a *= roof_a; // fade with the mode cross-fade
        text_draw(helm_line, 12.0f, 12.0f, 2.0f, helm_col,
                  &s->camera, s->fb_width, s->fb_height, LAYER_HUD_TEXT);
    }

    // Crew Job Panel — immediate-mode ImGui, built between renderer_begin_frame/end_frame (we are
    // inside game_render). It surfaces the selected crew's jobs, resolves its own clicks, and
    // dispatches them to the job queue. ImGui composites it on top of the world automatically, and
    // it self-anchors to the screen (no camera-cancel needed). No-op when no crew is selected.
    build_crew_job_panel(s);

    // Periodic stats to the log (the only on-screen text is the diagnostic helm-status HUD above).
    {
        static f32 acc = 0.0f;
        acc += dt;
        if (acc >= 1.0f) {
            acc = 0.0f;
            bs_frame_stats fs = renderer_get_frame_stats();
            f32 spd = vec2_length(s->flight.velocity);
            const char* jstate = s->crew.has_current ? job_state_name(s->crew.current.state) : "idle";
            BS_LOG_INFO("proto: mode=%s zoom=%.2f roof=%.2f heading=%.0fdeg spd=%.0f crewpath=%d job=%s pilot=%d quads=%u draws=%u",
                        s->mode == MODE_LOCAL ? "local" : "global",
                        s->camera.zoom, s->roof_alpha,
                        s->ship.angle * BS_RAD2DEG, spd,
                        s->crew.path_len,
                        jstate, (i32)s->crew.is_active_pilot,
                        fs.sprite_count, fs.draw_calls);
        }
    }

#if BS_DEBUG
    renderer_draw_grid(Vec2{-1000.0f, -1000.0f}, Vec2{1000.0f, 1000.0f}, 100.0f, 1.0f, bs_color{1,1,1,0.5f}, LAYER_DEBUG);
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
