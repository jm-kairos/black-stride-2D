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
