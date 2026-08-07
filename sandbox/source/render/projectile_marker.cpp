#include "render/projectile_marker.h"
#include "game.h"

#include "core/render_layers.h"   // LAYER_UI (no hand-copied layer constants)
#include "sim/projectile.h"       // Projectile, ProjectileSystem, MAX_PROJECTILES

#include <renderer/renderer.h>
#include <math.h>

using namespace bs_math;

// ---- Tuning (screen pixels) --------------------------------------------------------------
// The marker fades in over a band of APPARENT STREAK LENGTH rather than a raw zoom threshold.
// The streak is what the eye actually picks up -- a shell is only ~2 px of radius even in the
// arena, but its trail is hundreds of pixels long and perfectly legible -- so keying off the
// streak is what makes "can I see this shot?" the real trigger. The length below MIRRORS
// ProjectileSystem::render's own trail_length, so retuning the streak moves the marker with it.
static const f32 PM_FADE_HI   = 24.0f;   // >= this long on screen: marker off, the streak reads fine
static const f32 PM_FADE_LO   = 4.0f;    // <= this: full-strength marker (shot is invisible unaided)
static const f32 PM_DOT_PX    = 8.0f;    // marker dot diameter
static const f32 PM_TRACER_PX = 15.0f;   // direction tracer length, trailing behind the head
static const f32 PM_TRACER_TH = 2.0f;    // tracer thickness (renderer_draw_line takes screen px)
static const f32 PM_ALPHA     = 0.85f;   // marker opacity at full strength

void projectile_markers_draw(const game_state* s) {
    if (!s) return;

    const f32 zoom = s->camera_state.camera.zoom;
    if (zoom <= 1.0e-12f) return;          // degenerate camera: nothing sensible to size against

    const ProjectileSystem& ps = s->projectiles;

    // Screen-constant sizes converted to world units once: renderer_draw_sprite takes world
    // sizes, so dividing by zoom is what pins the marker to a fixed pixel size at any zoom.
    const f32 dot_world    = PM_DOT_PX / zoom;
    const f32 tracer_world = PM_TRACER_PX / zoom;

    for (i32 i = 0; i < MAX_PROJECTILES; ++i) {

        const Projectile& p = ps.pool[i];
        if (!p.active) continue;

        const f32 speed = vec2_length(p.velocity);

        // Apparent streak length drives the fade -- same expression ProjectileSystem::render
        // sizes the streak with. Above the band the shot is legible on its own and the marker
        // is not submitted at all, so this pass costs nothing at arena zoom and cannot clutter
        // close-in combat.
        f32 trail = speed * 0.04f;
        if (trail < p.radius * 4.0f) trail = p.radius * 4.0f;
        const f32 trail_px = trail * zoom;
        if (trail_px >= PM_FADE_HI) continue;

        f32 t = (PM_FADE_HI - trail_px) / (PM_FADE_HI - PM_FADE_LO);
        if (t > 1.0f) t = 1.0f;
        if (t < 0.0f) continue;
        const f32 alpha = PM_ALPHA * t;

        // Same transform ProjectileSystem::render uses, so the marker sits exactly on the
        // shot it marks instead of drifting from it far out on the hierarchical grid.
        const Vec2 draw_pos = hierpos_diff(&p.position, &s->camera_state.camera_hierpos);

        // Tint follows the projectile so hostile fire stays distinguishable from your own at a
        // zoom where nothing else is; the marker's SHAPE and SIZE are identical for every kind.
        bs_color col = p.color;
        col.a = alpha;

        // ---- Direction tracer: a short trail behind the head, so a shot reads as moving ----
        if (speed > 1.0e-4f) {
            const Vec2 back = vec2_scale(p.velocity, -tracer_world / speed);
            bs_color tail = col;
            tail.a = alpha * 0.55f;
            renderer_draw_line(draw_pos, vec2_add(draw_pos, back), PM_TRACER_TH, tail, LAYER_UI);
        }

        // ---- Head dot: the existing radial-gradient flash texture, screen-sized -------------
        bs_sprite dot{};
        dot.position      = draw_pos;
        dot.size          = Vec2{ dot_world, dot_world };
        dot.origin        = Vec2{ 0.5f, 0.5f };
        dot.rotation      = 0.0f;
        dot.uv            = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        dot.tint          = col;
        dot.custom        = bs_color{ alpha, p.age, 0.0f, 0.0f };  // x=glow, y=age(shimmer)
        dot.texture       = ps.flash_texture;
        dot.blend         = BLEND_ADDITIVE;
        dot.layer         = LAYER_UI;
        // Same long-lived glow storage the projectile pass points at. The backend breaks draw
        // runs on this pointer's IDENTITY, so reusing it keeps the markers batching with the
        // shots rather than splitting the run per sprite.
        dot.glow_override = &s->render.bullet_glow;
        renderer_draw_sprite(&dot);

    }
}
