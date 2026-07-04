#include "sim/celestial_parallax.h"
#include "game.h"
#include "voronoi_galaxy.h"

using namespace bs_math;

// celestial_shared_anchor: the single parallax anchor used for every celestial body this frame.
// It is the galaxy_center of the system under the camera. Using one shared anchor (instead of each
// system's own center) keeps the depth term common to all systems, so their relative layout stays
// rigid at any depth. Fallback to camera_hierpos yields a zero parallax offset (still no fusing).
bs_math::HierPos2 celestial_shared_anchor(const game_state* s) {
    i32 idx = find_system_by_cell(&s->camera_state.camera_hierpos, &s->galaxy.galaxy_voronoi, s->galaxy.systems);
    if (idx >= 0 && idx < s->galaxy.system_count) return s->galaxy.systems[idx].galaxy_center;
    return s->camera_state.camera_hierpos;
}

// parallax_fade_weight: 0..1 ramp gating celestial parallax by zoom. Fully 0 at/below the editable
// s->render.bg_parallax_fade_zoom (stars sit at true positions -> ships stay in their systems on the map),
// smoothstepping up to 1 (full parallax) over a fixed band above it. Independent of the general
// arena<->map view weight so the disappear point can be tuned on its own.
static f32 parallax_fade_weight(const game_state* s) {
    f32 lo = s->render.bg_parallax_fade_zoom;
    if (lo <= 0.0f) return 1.0f;          // 0 disables the gate -> parallax always on
    f32 hi = lo * 2.8f;                    // fade band width (0.05->0.14 matches the view fade default)
    f32 z  = s->camera_state.camera.zoom;
    if (z <= lo) return 0.0f;
    if (z >= hi) return 1.0f;
    f32 t = (z - lo) / (hi - lo);
    return t * t * (3.0f - 2.0f * t);      // smoothstep
}

// celestial_center_render: depth-parallax'd camera-relative offset of a system's center. See
// declaration in celestial_parallax.h. Returns (system_center - cam) - (shared_anchor - cam)*depth;
// the caller adds element-local offsets and applies compression. Because the depth term uses the
// SHARED anchor, inter-system spacing is preserved exactly (no fusing at any depth). Master toggle
// disables parallax entirely (returns the raw camera-relative offset).
bs_math::Vec2 celestial_center_render(const game_state* s, const bs_math::HierPos2* system_center,
                                      const bs_math::HierPos2* shared_anchor, f32 depth) {
    bs_math::Vec2 sys_rel = hierpos_diff(system_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
    if (!s->render.bg_parallax_enabled) return sys_rel;
    // Parallax is an ARENA immersion effect (a distant sun drifting behind the ship). On the
    // galaxy map it only tears ships away from their own system, so fade it out below an editable
    // zoom threshold. Both the Map and Arena renderers apply the SAME factor, so the star still
    // coincides across the cross-fade (no seam) and sits at its true position on the map.
    f32 eff_depth = depth * parallax_fade_weight(s);
    if (eff_depth <= 0.0f) return sys_rel;
    bs_math::Vec2 anchor_rel = hierpos_diff(shared_anchor, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
    return vec2_sub(sys_rel, vec2_scale(anchor_rel, eff_depth));
}
