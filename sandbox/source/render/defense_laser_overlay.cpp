#include "render/defense_laser_overlay.h"
#include "game.h"                 // game_state, DefenseBeam, camera_state
#include "core/view_transform.h"  // render_from_hierpos
#include "core/render_layers.h"   // LAYER_UI
#include <renderer/renderer.h>

using namespace bs_math;

void defense_laser_overlay_draw(game_state* s) {
    if (!s || s->defense_beam_count <= 0) return;

    f32 zoom = s->camera_state.camera.zoom;
    if (zoom < 1.0e-4f) zoom = 1.0e-4f;

    // Beam styling. Thicknesses are in screen pixels; the core colour is fixed while the glow
    // and impact flash intensify as the target's HP is worn down (b.intensity, 0..1).
    const f32      GLOW_TH  = 2.5f;
    const f32      CORE_TH  = 0.8f;
    const bs_color CORE_COL = bs_color{ 0.80f, 0.98f, 1.00f, 0.85f };

    for (i32 i = 0; i < s->defense_beam_count; ++i) {
        const DefenseBeam& b = s->defense_beams[i];

        Vec2 o = render_from_hierpos(s, &b.origin);
        Vec2 t = render_from_hierpos(s, &b.target);

        // Soft outer glow beam, then a hot white-cyan core on top.
        bs_color glow_col = bs_color{ 0.35f, 0.85f, 1.00f, 0.30f + 0.35f * b.intensity };
        renderer_draw_line(o, t, GLOW_TH, glow_col, LAYER_UI);
        renderer_draw_line(o, t, CORE_TH, CORE_COL, LAYER_UI);

        // Impact flash at the target, screen-constant size, brighter as the target is worn down.
        f32 r = (5.0f + 4.0f * b.intensity) / zoom;
        bs_color flash = bs_color{ 0.85f, 0.98f, 1.00f, 0.55f + 0.45f * b.intensity };
        renderer_draw_quad(t, Vec2{ r, r }, flash, LAYER_UI);
    }
}
