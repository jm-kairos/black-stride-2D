#pragma once
#include <defines.h>
#include <renderer/renderer_types.h>
struct GalaxyVoronoi;
struct StarSystem;
struct game_state;
namespace bs_math { struct Vec2; struct HierPos2; }
// Update the hovered cell and advance the head along the perimeter.
// mouse_world: camera-relative world position under the cursor (from camera2d_screen_to_world).
// camera_hierpos: the camera's absolute hierarchical position.
// zoom: current camera zoom (currently unused; retained for the call signature).
void update_cell_hover_effect(GalaxyVoronoi* v, f32 dt,
                               const bs_math::Vec2& mouse_world,
                               const bs_math::HierPos2* camera_hierpos,
                               f32 zoom,
                               const StarSystem* systems);
// Render the hovered cell with the rotating neon trail.
void draw_cell_hover_effect(const GalaxyVoronoi* v,
                            const game_state* s,
                            bs_color base_col);
