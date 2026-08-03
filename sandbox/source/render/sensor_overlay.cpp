#include "render/sensor_overlay.h"
#include "game.h"                 // game_state, Ship, player_ship()
#include "sim/sensor_system.h"    // SensorContact, sensor_gather_hostile_contacts
#include "core/view_transform.h"  // render_from_hierpos
#include "core/render_layers.h"   // LAYER_UI
#include <renderer/renderer.h>    // renderer_draw_circle / _line / _quad
#include <renderer/camera2d.h>    // camera2d_world_to_screen / _screen_to_world
#include <math.h>                 // sinf, sqrtf, fabsf, fminf

using namespace bs_math;

// Editor-tunable distance (world units) at which a contact's blip reaches its dim floor (see header).
f32 g_sensor_fade_distance = 4000000.0f;

// Fixed radar sweep period (seconds). Shared by the blink envelope and the position snapshot so
// the blip's fade and its sampled position advance on the exact same cadence.
static const f32 SENSOR_SWEEP_PERIOD = 0.5f;

// Fraction [0,1) of the current sweep already elapsed since the last refresh tick.
static f32 sweep_frac(f32 time) {
    f32 p = time / SENSOR_SWEEP_PERIOD;
    return p - floorf(p);
}

// ---- Shared contact styling ---------------------------------------------------------------
// Confidence t in [0,1]: 0 = just entering Layer 2, 1 = at/inside Layer 1 (identified). The
// contact behaves like a radar blip: it pops to full brightness instantly (as if the sweep just
// updated its position), fades out, holds dark, then pops again on a fixed refresh cadence. As
// the contact closes (t -> 1) the blink smoothly gives way to a steady lock via the alpha lerp.
static bs_color sensor_contact_color(f32 t, f32 time) {
    // Fixed refresh period. The blink phase must NOT be scaled by the distance-varying confidence
    // t: contacts are fast-moving projectiles, so t (and hence any t-scaled period) changes every
    // frame. Dividing the large, ever-growing `time` by a continuously shrinking period made the
    // fractional phase sweep through many cycles per second, which read as a rapid strobe instead
    // of a radar blip. A constant period keeps the cadence smooth and independent of contact motion.
    f32 phase = sweep_frac(time);              // sawtooth 0..1 across one refresh cycle
    const f32 FADE_PORTION = 1.0f;             // pop to full, then fade gradually across the whole cycle
    f32 env = 1.0f - phase / FADE_PORTION;     // instant pop to 1, then linear fade to 0 by next pop
    if (env < 0.0f) env = 0.0f;                // stays fully gone until the next pop

    f32 alpha  = env + (1.0f - env) * t;       // lerp(env, 1, t): steady (no blink) at Layer 1
    f32 bright = 0.55f + 0.45f * t;            // color intensity rises with t
    return bs_color{ 1.0f * bright, 0.30f * bright, 0.26f * bright, alpha };
}

static f32 safe_zoom(const game_state* s) {
    f32 z = s->camera_state.camera.zoom;
    return (z > 1.0e-4f) ? z : 1.0e-4f;
}

// Dim-with-distance for the unbounded Layer 2: full strength up to the Layer 2 falloff, then ramps
// down to a dim floor by g_sensor_fade_distance so far returns read as faint echoes but never vanish.
static f32 far_fade(f32 dist, const SensorSuite& suite) {
    const f32 FLOOR = 0.15f;                               // minimum visibility at extreme range
    f32 span = g_sensor_fade_distance - suite.layer2_radius;
    if (span < 1.0f) return 1.0f;                          // degenerate config -> no fade
    f32 t = (dist - suite.layer2_radius) / span;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return 1.0f - (1.0f - FLOOR) * t;                      // 1.0 near -> FLOOR far
}

// ---- 1. Flagship sensor rings -------------------------------------------------------------
static void draw_sensor_rings(game_state* s) {
    if (!s->show_sensor_layers) return;
    const Ship& flag = s->player_ship();
    Vec2 center = render_from_hierpos(s, &flag.origin);
    const SensorSuite& suite = flag.sensors;

    const bs_color L1_COL = bs_color{ 1.00f, 0.78f, 0.35f, 0.60f }; // identification (amber)
    const bs_color L0_COL = bs_color{ 0.40f, 1.00f, 0.55f, 0.60f }; // comfort zone (green)

    // renderer_draw_circle thickness is already in SCREEN PIXELS (the renderer divides it by
    // zoom internally), so pass a plain constant to keep a steady ~2 px outline at every zoom.
    // Layer 2 is unbounded (no outer boundary) so only the L1 and L0 rings are drawn.
    const f32 th = 2.0f;
    renderer_draw_circle(center, suite.layer1_radius, 96, th, L1_COL, LAYER_UI);
    renderer_draw_circle(center, suite.layer0_radius, 96, th, L0_COL, LAYER_UI);
}

// ---- 2. World-space radar contacts --------------------------------------------------------
static void draw_sensor_contacts(game_state* s, const SensorContact* contacts, i32 n) {
    f32 zoom = safe_zoom(s);
    f32 r    = 6.0f / zoom;   // screen-constant blip half-extent (~6 px)

    const Ship& flag   = s->player_ship();
    const SensorSuite& suite = flag.sensors;

    for (i32 i = 0; i < n; ++i) {
        const SensorContact& c = contacts[i];
        bs_color col = sensor_contact_color(c.confidence, s->elapsed_time);
        // Dim distant (unbounded Layer 2) returns with range.
        f32 dist = vec2_length(hierpos_diff(&c.position, &flag.origin, BS_HIERPOS_CELL_SIZE));
        col.a   *= far_fade(dist, suite);
        Vec2 center  = render_from_hierpos(s, &c.position);

        // Core blip.
        renderer_draw_quad(center, Vec2{ r, r }, col, LAYER_UI);

        // Heading tick (direction of travel) so the player can read the incoming vector.
        f32 vlen = vec2_length(c.velocity);
        if (vlen > 1.0e-3f) {
            Vec2 dir = vec2_scale(c.velocity, 1.0f / vlen);
            Vec2 tip = vec2_add(center, vec2_scale(dir, r * 2.6f));
            renderer_draw_line(center, tip, 2.0f, col, LAYER_UI);
        }

        // Identified contacts (inside Layer 1) get a crisp ring so they read as "locked".
        if (c.confidence >= 0.999f) {
            bs_color ring = bs_color{ 1.0f, 0.30f, 0.26f, 0.95f };
            renderer_draw_circle(center, r * 2.0f, 16, 1.5f, ring, LAYER_UI);
        }
    }
}

// ---- 3. Screen-edge HUD indicators --------------------------------------------------------
// Draw a short line between two SCREEN-pixel endpoints by projecting them back into render
// space (renderer primitives live in render space). Thickness is given in screen pixels.
static void draw_screen_line(const game_state* s, Vec2 a_px, Vec2 b_px, f32 th_px, bs_color col) {
    const Camera2D* cam = &s->camera_state.camera;
    Vec2 a = camera2d_screen_to_world(cam, s->fb_width, s->fb_height, a_px);
    Vec2 b = camera2d_screen_to_world(cam, s->fb_width, s->fb_height, b_px);
    renderer_draw_line(a, b, th_px, col, LAYER_UI);
}

static void draw_sensor_hud_indicators(game_state* s, const SensorContact* contacts, i32 n) {
    const Camera2D* cam = &s->camera_state.camera;
    f32 fbw = (f32)s->fb_width;
    f32 fbh = (f32)s->fb_height;
    Vec2 sc = Vec2{ fbw * 0.5f, fbh * 0.5f };
    const f32 margin = 30.0f; // keep the chevron inside the viewport edge
    const f32 arm    = 14.0f; // arrowhead size (px)

    for (i32 i = 0; i < n; ++i) {
        const SensorContact& c = contacts[i];
        Vec2 render_pos = render_from_hierpos(s, &c.position);
        Vec2 sp = camera2d_world_to_screen(cam, s->fb_width, s->fb_height, render_pos);

        // On-screen contacts are already shown by their world blip; only flag off-screen ones.
        b8 offscreen = (sp.x < margin) || (sp.x > fbw - margin) ||
                       (sp.y < margin) || (sp.y > fbh - margin);
        if (!offscreen) continue;

        Vec2 d = Vec2{ sp.x - sc.x, sp.y - sc.y };
        f32  dl = sqrtf(d.x * d.x + d.y * d.y);
        if (dl < 1.0e-3f) continue;
        d = Vec2{ d.x / dl, d.y / dl };

        // Intersect the center->contact ray with the inset viewport rectangle.
        f32 hx = fbw * 0.5f - margin;
        f32 hy = fbh * 0.5f - margin;
        f32 tx = (fabsf(d.x) > 1.0e-4f) ? (hx / fabsf(d.x)) : 1.0e30f;
        f32 ty = (fabsf(d.y) > 1.0e-4f) ? (hy / fabsf(d.y)) : 1.0e30f;
        f32 tt = fminf(tx, ty);
        Vec2 edge = Vec2{ sc.x + d.x * tt, sc.y + d.y * tt };

        // Arrowhead pointing outward (along d) at the clamped edge position.
        Vec2 perp = Vec2{ -d.y, d.x };
        Vec2 back = Vec2{ edge.x - d.x * arm, edge.y - d.y * arm };
        Vec2 lft  = Vec2{ back.x + perp.x * arm * 0.6f, back.y + perp.y * arm * 0.6f };
        Vec2 rgt  = Vec2{ back.x - perp.x * arm * 0.6f, back.y - perp.y * arm * 0.6f };

        bs_color col = sensor_contact_color(c.confidence, s->elapsed_time);
        col.a = 0.55f + 0.45f * col.a; // keep edge markers legible even while pulsing
        draw_screen_line(s, edge, lft, 2.5f, col);
        draw_screen_line(s, edge, rgt, 2.5f, col);
    }
}

// ---- Radar position snapshot --------------------------------------------------------------
// Freeze each LAYER 2 contact at the position it occupied at the most recent sweep tick, so the
// blip reads as a discrete radar update: it appears at a sampled position, holds there while it
// fades (the real projectile keeps flying, unseen), then jumps to the freshly sampled position on
// the next sweep. Ballistic contacts move at constant velocity, so the sampled position is
// recovered analytically -- back-project along velocity by the time since the tick -- no history.
// Layer 1 & 0 contacts (confidence saturated at the identification threshold) are left at their
// live position so they track continuously, reading as a steady, real-time lock rather than a step.
static void snapshot_contacts_to_last_sweep(SensorContact* c, i32 n, f32 time) {
    f32 back = sweep_frac(time) * SENSOR_SWEEP_PERIOD; // seconds since the last sweep tick
    for (i32 i = 0; i < n; ++i) {
        if (c[i].confidence >= 0.999f) continue;       // Layer 1 & 0: track continuously (live pos)
        Vec2 offset   = vec2_scale(c[i].velocity, -back);
        c[i].position = hierpos_add_vec2(&c[i].position, offset);
    }
}

// ---- Entry point --------------------------------------------------------------------------
void sensor_overlay_draw(game_state* s) {
    if (!s) return;

    draw_sensor_rings(s);

    // Gather the fleet's detected contacts ONCE; both visualizations share the result.
    static SensorContact contacts[MAX_PROJECTILES];
    i32 n = sensor_gather_hostile_contacts(s, contacts, MAX_PROJECTILES);

    // Snapshot positions to the last radar sweep so blips step (not slide) across the layer.
    snapshot_contacts_to_last_sweep(contacts, n, s->elapsed_time);

    draw_sensor_contacts(s, contacts, n);
    draw_sensor_hud_indicators(s, contacts, n);
}
