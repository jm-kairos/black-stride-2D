#pragma once

#include <defines.h> // f32/i32

struct game_state;

// Ship rendering pass: computes render-space positions for fleet/enemy/combat entities, the
// HierPos2 debug grid, per-ship directional star lighting, ship sprites, engine exhaust, collider
// overlays, the sensor-gated enemy ship, non-ship combat quads, and fleet emblems. Extracted
// verbatim from game_render. Must run after the star position + lighting are established.
void draw_ship_scene(game_state* s);

// ---- Ship portrait (the full-screen inspector's hull view) --------------------------------
// Submits the INSPECTED ship's hull, mounts, hardpoint skeleton and drag fit-feedback into the
// engine's offscreen portrait target (renderer_portrait_begin/end). Call at the end of
// render_scene -- after draw_ship_scene has written render_pos. No-op while the inspector is
// closed, so the pass costs nothing in normal play.
void ship_portrait_submit(game_state* s);

// The portrait square's SCREEN rect: centred in the inspector's middle zone, sized to fit.
// game_push_hud pushes the same rect (zone-relative, via ship_portrait_center_origin) to the
// RML <img> element, so the element and the hit-test below share one source of truth.
void ship_portrait_rect(const game_state* s, f32* out_x, f32* out_y, f32* out_size);

// Screen origin of the inspector's middle zone (the .insp-center element) -- the coordinate
// the RML element's left/top are relative to.
void ship_portrait_center_origin(const game_state* s, f32* out_x, f32* out_y);

// Which of the inspected ship's hardpoints sits under screen pixel (mx,my), hit-tested through
// the portrait mapping? -1 when the cursor is outside the portrait square or over no box.
i32 ship_portrait_hardpoint_at(game_state* s, f32 mx, f32 my);
