#pragma once

// Arena-side HUD / modal overlays drawn at the end of game_render.
// Extracted verbatim from game.cpp (R1 scene_renderer decomposition).

struct game_state;

// Centered modal shown when an encounter is active (Engage / Avoid / Hail / Observe).
void draw_encounter_panel(game_state* s);

// Navigation HUD (sector / system / distance / zone) plus the ship-properties HUD.
// Fades in with the arena look; hidden in edit mode.
void draw_nav_ship_hud(game_state* s);
