#pragma once

#include <defines.h> // f32

struct game_state;
struct Ship;
struct bs_color;

// System/galaxy-map fleet emblems. Clusters combat-mode ship emblems by type when their
// screen-space rings overlap (union-find), drawing one emblem per cluster at the centroid
// with the averaged velocity vector, a selection ring, and a fusion-count badge. Faded in
// by map weight as the view crosses from arena to galaxy map.
void draw_fleet_emblems(game_state* s);

// Draw a ship's collider polygon outline (world-space corners) on LAYER_DEBUG.
void draw_collider_outline(const Ship* ship, bs_color color, f32 thickness);

// Draw the ship's hardpoint skeleton on LAYER_DEBUG: one rotated box per hardpoint at
// its authored ship-local position, colour-coded by accepted module kind, with a tick
// line showing the mount's rest facing and fainter rays marking its traverse-arc edges.
void draw_hardpoint_overlay(const Ship* ship, f32 thickness);

// Pulsing white outline around one hardpoint's box (LAYER_GIZMO). Used by the in-game
// hardpoint editor to show which slot the gizmo is editing.
void draw_hardpoint_highlight(const Ship* ship, i32 hp_index, f32 time);

// Drag-and-drop feedback for one hardpoint's box (LAYER_GIZMO), drawn while an arsenal
// drag is airborne: a pulsing green outline when the dragged item fits this slot, a dim
// red one when the slot rejects it (wrong kind or too small).
void draw_hardpoint_drag_feedback(const Ship* ship, i32 hp_index, b8 fits, f32 time);

// Procedural mount art drawn on top of the hull (LAYER_SHIP + 1): a rotating gun turret per
// mounted weapon (aimed by Ship.mount_aim), a smaller twin-barrel turret on the point-defense
// mount, and a spinning radar dish on mounted sensor modules. `alpha` fades the art with the
// hull (e.g. the enemy ship's sensor-visibility fade).
void draw_ship_mounts(const Ship* ship, f32 time, f32 alpha);
