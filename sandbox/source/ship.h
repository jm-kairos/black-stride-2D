#pragma once

#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>  // bs_texture (Ship::a_texture)

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
    TILE_HULL_DOOR,   // 'A' airlock: an exterior hull door on the perimeter. Solid (closed) when the
                      //     ship is undocked, so crew can't walk into space; it is the DOCKING PORT —
                      //     two ships dock only when one ship's HULL_DOOR mates with another's.
    TILE_TYPE_COUNT
};

#define SHIP_MAX_TILES 18000  // generous cap for the prototype (e.g. 64x64)

struct Ship {
    i32      cols;
    i32      rows;
    f32      tile_size;                 // world units per tile edge
    bs_math::Vec2 origin;               // world offset of the ship (moved in global mode)
    f32      angle;                     // world heading in radians (CCW); turned in global mode
    TileType tiles[SHIP_MAX_TILES];     // row-major: tiles[row*cols + col]
    bs_texture curr_wall_texture;
    bs_texture curr_hull_texture;
    bs_texture curr_window_texture;
    bs_texture curr_door_texture;
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

// ---- Ship-ship collision & docking ---------------------------------------------------
// World-space bounding-circle radius: half the diagonal of the tile footprint, so it encloses
// the hull at ANY heading (rotation-invariant). Cheap broad-phase bound for proximity tests.
f32 ship_bounding_radius(const Ship* ship);

// Broad-phase collision: TRUE if two ships' world bounding circles overlap (|origin_a - origin_b|
// < r_a + r_b). This is the coarse "are these two hulls in contact" test docking is gated on; it
// over-reports for non-circular hulls (a generous shell), which is the safe side for a docking
// PRECONDITION — the precise contact is then the airlock-to-airlock distance in ships_docked.
b8 ships_overlap(const Ship* a, const Ship* b);

// Docking test: TRUE iff the two ships are close enough to have mated airlocks — i.e. they pass
// ships_overlap AND some TILE_HULL_DOOR on `a` has its WORLD center within `tolerance` world units
// of some TILE_HULL_DOOR on `b`. Docking is therefore a real geometric handshake through the
// HULL_DOOR ports under the full pose (origin AND angle) of both hulls, not a flag. On a match,
// the mated door tiles are written to the out params when non-null (a==out_*_a, b==out_*_b) for
// future crew-transfer / airlock-open rendering; pass null to ignore. Returns FALSE if either
// ship has no HULL_DOOR. Suggested tolerance from ship_dock_tolerance() (~2.5 * tile_size; mated
// airlock doors sit two tiles apart to leave room for the connector bridge tile).
b8 ships_docked(const Ship* a, const Ship* b, f32 tolerance,
                i32* out_col_a, i32* out_row_a, i32* out_col_b, i32* out_row_b);

// Narrow-phase OBB collision: treats each ship as its full hull footprint — a (cols*ts x rows*ts)
// rectangle centered on `origin` and rotated by `angle` — and tests the two oriented boxes with the
// Separating-Axis Theorem (exact for convex rectangles). Returns TRUE when they overlap and writes
// the Minimum Translation Vector to *out_mtv (world units): the SHORTEST displacement that moves `a`
// clear of `b` (it points from `b` toward `a`, so adding it to a->origin fully separates them). This
// is the "no phasing" core — a caller that pushes `a` out by the MTV every frame can never let the
// hulls interpenetrate. Returns FALSE (out_mtv untouched) when a separating axis exists (disjoint).
// A bounding-circle broad-phase (ships_overlap) early-outs the common far-apart case. The OBB is a
// slightly generous shell at the hull's empty corners, which is the SAFE side for "don't phase".
b8 ships_collide(const Ship* a, const Ship* b, bs_math::Vec2* out_mtv);

// Debug/introspection: the four WORLD-space corners of the exact collider used by ships_collide —
// the tight OBB hugging the ship's OCCUPIED tiles (not the raw cols*rows grid). Corners are written
// in winding order (CCW in local space): [0]=(minx,miny) [1]=(maxx,miny) [2]=(maxx,maxy)
// [3]=(minx,maxy), each transformed by the ship's full pose. This is what the renderer outlines so
// the on-screen collider matches the SAT box byte-for-byte. Returns FALSE (out_corners untouched)
// for an empty ship with no structure tiles (no box exists).
b8 ship_collider_corners(const Ship* ship, bs_math::Vec2 out_corners[4]);

// Outward (exterior-facing) unit normal of a HULL_DOOR airlock, in WORLD space (honors the ship's
// full pose — origin AND angle). It points from the door's interior landfall tile THROUGH the door
// out into open space: the direction the airlock faces. Two airlocks are MATED — doors parallel and
// facing each other — exactly when their outward normals are ANTI-parallel (dot == -1). This is the
// primitive the docking snap uses to rotate the mover so the doors line up, and the harness uses to
// assert the doors end parallel. Returns FALSE (out_normal untouched) if (col,row) isn't a HULL_DOOR
// or has no interior deck neighbor (malformed map).
b8 ship_airlock_outward_normal(const Ship* ship, i32 door_col, i32 door_row, bs_math::Vec2* out_normal);

// Docking snap: compute the rigid-body correction (rotation THEN translation) that mates `mover`'s
// airlock to `anchor`'s without overlap. Two outputs, applied in order to `mover`:
//   *out_angle  — add to mover->angle: spins the mover so its airlock's outward normal becomes
//                 ANTI-parallel to the anchor's (the DOORS END PARALLEL, facing each other). Wrapped
//                 to (-PI,PI] so a near-aligned approach yields a tiny corrective turn, never a
//                 multi-turn unwind. This is the half that guarantees NO HULL OVERLAP at ANY approach
//                 angle: with anti-parallel door normals, each tight-OBB's door face is perpendicular
//                 to that normal and the two faces coincide, so the shared face line is a separating
//                 axis — the hulls touch flush instead of corner-poking into each other.
//   *out_delta  — add to mover->origin AFTER the rotation: slides the (now-rotated) mover so its door
//                 sits exactly TWO tile_sizes off the anchor's door along the anchor's outward normal
//                 — the clean "joined at the airlock, two-tile gap" pose. The gap is two tiles (not
//                 one) so a one-tile CONNECTOR bridge fits flush between the mated doors with no
//                 door-to-door overlap (see dock_connector_tile).
// Returns FALSE (both outputs untouched) if the two are not currently dock-eligible (no HULL_DOOR
// pair within `tolerance`); callers gate the T-press on the same ships_docked test, so a TRUE here
// means "the latch may fire and here is the snap". Pure function of the two poses + tilemaps — the
// game loop and the headless harness call THIS so the mating geometry is verified against the real
// code, never re-derived. `anchor` stays put (it is the immovable derelict).
b8 dock_snap_delta(const Ship* mover, const Ship* anchor, f32 tolerance,
                   bs_math::Vec2* out_delta, f32* out_angle);

// Docking proximity tolerance (world units): how close two airlock door centers must sit for the
// hulls to count as MATED — used by ships_docked, dock_snap_delta, ship_seam_landfall, and
// dock_connector_tile. Sized to the mated gap: the snap parks the doors TWO tile_sizes apart (so a
// one-tile connector bridge fits between them), so the tolerance is 2.5 tile_sizes — the two-tile
// mated separation plus a half-tile of slack so a hair of drift doesn't drop the latch. Centralized
// here so the snap gap and the tolerance can never drift apart. Returns 0 on a null ship.
f32 ship_dock_tolerance(const Ship* ship);

// Connector-bridge placement (pure). When two ships dock, a single walkable CONNECTOR tile is spawned
// in the gap between the mated airlock doors so the crews get a continuous bridge across the seam (the
// merged navmesh routes door -> connector -> door). This locates the mated HULL_DOOR pair (same
// narrow-phase as dock_snap_delta) and returns the connector's WORLD center — the MIDPOINT of the two
// door centers, which with the two-tile mated gap is exactly the one clear tile between them — and its
// world ANGLE (aligned with the anchor airlock's facing so the tile sits flush with both doors). The
// game loop spawns + renders it from this, and the headless harness asserts its placement, so the
// bridge geometry is verified against the real code. Returns FALSE (outputs untouched) if the ships
// aren't docked within `tolerance`. `out_angle` may be null if only the center is needed.
b8 dock_connector_tile(const Ship* a, const Ship* b, f32 tolerance,
                       bs_math::Vec2* out_world, f32* out_angle);

// Given a HULL_DOOR tile (door_col,door_row) on `ship`, find the walkable DECK tile just inside it
// — the tile a crew stands on to use that airlock. HULL_DOOR is itself solid/non-walkable, and an
// airlock sits on the hull perimeter: of its 4 orthogonal neighbors, exactly one is interior deck
// (walkable floor/door), one is open space outside the hull, and the other two are hull wall. This
// returns that single interior neighbor (the first walkable one), writing it to out_col/out_row and
// returning TRUE; returns FALSE if the tile isn't a HULL_DOOR or has no walkable neighbor (malformed
// map). It is the landfall tile for crew transfer: cross-ship routing walks a crew to its own hull's
// airlock-interior tile, hops the seam, and resumes from the other hull's airlock-interior tile.
b8 ship_airlock_interior_tile(const Ship* ship, i32 door_col, i32 door_row,
                              i32* out_col, i32* out_row);

// Cross-ship seam handoff geometry (pure). Given two DOCKED ships and which hull a crew is leaving
// (`from`) vs boarding (`to`), locate the mated airlock pair (the same narrow-phase ships_docked /
// dock_snap use) and return the BOARDING hull's airlock-INTERIOR landfall tile: its (col,row) via
// out_col/out_row and its `to`-LOCAL center via out_local. This is exactly where a crew crossing the
// seam from `from` re-roots on `to` — leg A walks the crew to `from`'s airlock-interior tile, this
// returns the `to`-side interior tile it steps onto, and leg B routes from there to the goal. The
// two interior tiles are a few tiles apart in WORLD space (the mated doors sit two tiles apart with
// the connector bridge between them, and each interior tile is one tile inboard), so the handoff is a
// short discrete hop across the airlock,
// not a walkable A* span. Returns FALSE if the ships aren't docked within `tolerance`, or the boarding
// airlock has no interior deck neighbor. `out_local` may be null if only the tile index is needed.
b8 ship_seam_landfall(const Ship* from, const Ship* to, f32 tolerance,
                      i32* out_col, i32* out_row, bs_math::Vec2* out_local);
