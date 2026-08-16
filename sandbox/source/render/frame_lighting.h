#pragma once

struct game_state;

// Assemble and submit this frame's lighting to the renderer: speed-driven dynamic bloom, the
// unified volumetric star light + ambient cross-fade (by map weight), the frame-local point-light
// array (star light + editor lights), and the glow/bloom params. Must run after the star position
// is established and before the lit sprite passes. Extracted verbatim from game_render.
void submit_frame_lighting(game_state* s);
