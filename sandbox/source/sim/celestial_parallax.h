#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>

// =====================================================================================
// Celestial depth parallax
// =====================================================================================
// Depth-based parallax for the star-system backdrop (stars, planets, orbit lines, test
// sprites). Gameplay entities (ships, enemies, projectiles, FX) are EXCLUDED and stay at
// parallax 1.0 (they use render_from_hierpos instead).
//
// Every system is parallaxed against ONE shared anchor (the system under the camera) so the
// inter-system layout stays rigid at any depth -- a per-system anchor would collapse all
// systems onto the screen center as depth->1 and fuse them. The effect also fades to zero
// below game_state::bg_parallax_fade_zoom so ships stay glued to their systems on the map.
// The Map and Arena renderers call these with identical arguments, so the backdrop coincides
// across the map<->arena cross-fade (no seam).

struct game_state; // forward declared; only accessed by pointer here

// The single parallax anchor used for every celestial body this frame: the galaxy_center of
// the system under the camera. Falls back to the camera position if none (zero parallax
// offset -> still no fusing). Computed once per frame and cached in game_state::celestial_anchor.
bs_math::HierPos2 celestial_shared_anchor(const game_state* s);

// Camera-relative render offset of a system's center with depth parallax applied. Returns
// (system_center - cam) - (shared_anchor - cam) * (depth * zoom_fade). The caller adds
// element-local offsets (star/planet/orbit positions).
// Precision-safe far from the galaxy origin (integer cell differencing).
bs_math::Vec2 celestial_center_render(const game_state* s, const bs_math::HierPos2* system_center,
                                      const bs_math::HierPos2* shared_anchor, f32 depth);

// The zoom-driven parallax fade weight in [0,1] used inside celestial_center_render (0 = parallax
// off, at/below bg_parallax_fade_zoom; 1 = full parallax). Returns 0 when parallax is disabled.
// Exposed so camera code can cancel a followed body's parallax render shift (keep it screen-centered).
f32 celestial_parallax_fade(const game_state* s);
