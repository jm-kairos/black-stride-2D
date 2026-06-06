#pragma once

#include <defines.h>
#include <math/math_utils.h>

// =====================================================================================
// Ship tilemap (prototype).
//
// The ship interior is a fixed grid of discrete tiles loaded from a `.tmap` data file
// (see assets/ship.tmap). The grid is authored as ASCII; this module parses it into a
// flat TileType array plus metadata (tile size, dimensions).
//
// Coordinate convention: the ship is a RIGID BODY with a world pose of two parts — a
// world-space `origin` (position, moved in global mode) and an `angle` heading (radians,
// CCW, turned in global mode). Tiles are authored in SHIP-LOCAL space: an axis-aligned
// frame centered on the ship's geometric center, grid top-left at tile (col=0,row=0),
// row 0 at the TOP (+Y, camera is y-up). The two frames relate by the full pose:
//   world = origin + rotate(local, angle);    local = rotate(world - origin, -angle).
// Crew movement and collision run entirely in ship-local space, so they are axis-aligned
// and heading-independent (W always walks toward the nose); rendering and the camera
// convert to world, applying BOTH origin and angle. This keeps the local interior view and
// the global roof view in the same world space, so the zoom cross-fade stays aligned and
// the heading established in global mode is preserved when you zoom back in.
// =====================================================================================

enum TileType {
    TILE_EMPTY = 0,   // '.' open space outside the hull — not part of the ship
    TILE_HULL,        // '#' exterior hull wall (solid, visually distinct)
    TILE_WALL,        // 'W' interior wall (solid)
    TILE_FLOOR,       // 'F' walkable floor
    TILE_DOOR,        // 'D' walkable doorway (floor that sits in a wall line)
    TILE_HULL_WINDOW, // 'G' transparent hull window (solid but visually distinct)
    TILE_FLOOR_WINDOW,// 'J' transparent floor window (walkable but visually distinct)
    TILE_HELM,        // 'H' helm / control station — walkable; a crew member mans it to pilot
    TILE_TYPE_COUNT
};

#define SHIP_MAX_TILES 4096  // generous cap for the prototype (e.g. 64x64)

struct Ship {
    i32      cols;
    i32      rows;
    f32      tile_size;                 // world units per tile edge
    bs_math::Vec2 origin;               // world offset of the ship (moved in global mode)
    f32      angle;                     // world heading in radians (CCW); turned in global mode
    TileType tiles[SHIP_MAX_TILES];     // row-major: tiles[row*cols + col]
};

// Load a ship from a `.tmap` file (path relative to the working directory). Returns FALSE
// on any parse/IO error (and logs why); on success `out_ship` is fully populated with
// origin = {0,0}. Call once during game init.
b8 ship_load(Ship* out_ship, const char* path);

// Tile lookup. Out-of-range or non-loaded coords return TILE_EMPTY.
TileType ship_tile_at(const Ship* ship, i32 col, i32 row);

// TRUE if the tile blocks crew movement (hull or interior wall). Empty/floor/door are open.
b8 ship_tile_is_solid(const Ship* ship, i32 col, i32 row);

// TRUE if a crew member may stand on / walk through this tile (floor, door, or floor-window).
// NOTE: this is NOT simply !ship_tile_is_solid — TILE_EMPTY (open space outside the hull) is
// non-solid but NOT walkable, so navigation and move-orders must test THIS, not the negation
// of is_solid, or the crew could be ordered out into space.
b8 ship_tile_is_walkable(const Ship* ship, i32 col, i32 row);

// TRUE if any part of the ship is drawn here (everything except TILE_EMPTY). Used as the
// roof footprint in global mode.
b8 ship_tile_is_structure(const Ship* ship, i32 col, i32 row);

// ---- Rigid-body transforms -----------------------------------------------------------
// Tile CENTER in SHIP-LOCAL space (origin & angle NOT applied; grid centered on {0,0}).
// This is the authoring frame; use it for crew placement and collision.
bs_math::Vec2 ship_tile_center_local(const Ship* ship, i32 col, i32 row);

// Ship-local point -> world point: applies the full pose (origin AND angle).
bs_math::Vec2 ship_local_to_world(const Ship* ship, bs_math::Vec2 local);

// World point -> ship-local point: removes the full pose (inverse of ship_local_to_world).
bs_math::Vec2 ship_world_to_local(const Ship* ship, bs_math::Vec2 world);

// Ship-local point -> the tile (col,row) it falls in (axis-aligned; inverse of
// ship_tile_center_local). May return out-of-range indices (treat as empty via ship_tile_at).
void ship_local_to_tile(const Ship* ship, bs_math::Vec2 local, i32* out_col, i32* out_row);

// Convert a tile's (col,row) to the WORLD-space position of its CENTER, applying the full
// pose (origin AND angle). Convenience for rendering; = ship_local_to_world(center_local).
bs_math::Vec2 ship_tile_center_world(const Ship* ship, i32 col, i32 row);

// Convert a world-space point to the tile (col,row) it falls in, applying the inverse pose
// then an axis-aligned lookup. May return out-of-range indices.
void ship_world_to_tile(const Ship* ship, bs_math::Vec2 world, i32* out_col, i32* out_row);

// Total ship-local size (width,height) of the ship's tile footprint.
bs_math::Vec2 ship_world_size(const Ship* ship);

// Find the FIRST tile of type `want` in row-major (top-left -> bottom-right) order. On a hit,
// writes its (col,row) to out_col/out_row and returns TRUE; returns FALSE if none exists (or
// on null args). Used to locate stations (e.g. the single TILE_HELM) for the job system; a
// derived stations[] list can generalize this when multiple stations/types arrive.
b8 ship_find_first_tile(const Ship* ship, TileType want, i32* out_col, i32* out_row);
