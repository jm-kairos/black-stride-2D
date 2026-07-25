#pragma once

#include <defines.h> // f32

struct game_state;

// Galaxy-map "look" render pass (was MODE_SYSTEM): Voronoi lanes/cells, star sunbursts,
// planet/orbit rings, map entities, jump/sensor range rings, and the hover tooltip. Cross-fades
// in by map weight (1 - view_arena_w) and no-ops when fully in the arena look.
// Extracted verbatim from game_render (R1 scene_renderer decomposition).
void draw_galaxy_map_look(game_state* s, f32 dt);

// Hit-test the zoomed-out map for a planet under a screen-pixel position, matching exactly how
// planets are drawn in the map look (per-type render size, and the same visibility
// gate). On a hit, writes the cache slot of the hit system and the planet index and returns TRUE;
// otherwise returns FALSE and leaves the outputs untouched. The slot is only valid this frame;
// callers that track the planet across frames should stash the system's galaxy_center.
b8 galaxy_pick_planet(const game_state* s, f32 screen_x, f32 screen_y, i32* out_slot, i32* out_planet_index);

// Build the galaxy-map hover tooltip for the cursor at (mx, my) screen pixels. Runs the map-entity
// hit-test first (short "name + distance"), else the star-system hit-test (physical properties +
// per-planet + owning civilization). On a hit, writes the pre-formatted, '\n'-separated text into
// `out` (capacity `cap`), the suggested anchor screen position into `out_x`/`out_y`, and returns
// TRUE; otherwise returns FALSE and leaves the outputs untouched. Call from the update path so the
// RmlUi HUD picks it up the same frame (no cursor lag). No drawing — the HUD renders the string.
b8 galaxy_map_hover_tooltip(game_state* s, i32 mx, i32 my, char* out, i32 cap, i32* out_x, i32* out_y);
