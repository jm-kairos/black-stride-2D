#pragma once

#include <defines.h>

#include <math/math_utils.h>

#include <math/bs_hierpos.h>

#include <renderer/renderer_types.h>

#include "ship_visual.h"

// =====================================================================================

// Ship definition (art-texture based).

//

// Ships are loaded from `.ship` files: a flat text format defining name, faction, visual

// sprite layers, and an authored collision polygon. No tile grid — rendering is purely

// through the decoupled ShipVisual sprite layers.

//

// Coordinate convention: the ship is a RIGID BODY with a world pose of two parts — a

// world-space `origin` (position, moved in global mode) and an `angle` heading (radians,

// CCW, turned in global mode). The visual art and collider are authored in SHIP-LOCAL

// space centered on {0,0}. The two frames relate by the full pose:

//   world = origin + rotate(local * world_scale, angle);

//   local = rotate(world - origin, -angle) / world_scale;

// =====================================================================================

#define SHIP_MAX_COLLIDER_VERTS 32

#define SHIP_MAX_WEAPONS 4

enum VesselFaction {

    VESSEL_PIRATE     = 0,

    VESSEL_FEDERATION = 1,

    VESSEL_NEUTRAL    = 2,

    VESSEL_STATION    = 3,

    VESSEL_DERELICT   = 4,

};

const char* vessel_faction_name(VesselFaction f);

const char* vessel_faction_desc(VesselFaction f);

struct Ship {

    f32           world_scale;    // exterior scale multiplier

    bs_math::HierPos2 origin;    // world position (hierarchical grid cell + local offset)

    bs_math::Vec2 render_pos;    // TRANSIENT: render-space position, recomputed each frame

    f32           angle;          // world heading in radians (CCW)

    VesselFaction faction;        // who this ship belongs to

    const char*   vessel_name;    // display name shown in encounters / HUD

    // ---- Visual layers (sprite art, loaded from .ship file) -----------------------

    ShipVisual    visual;

    // ---- Footprint (ship-local width/height, from .ship file) ----------------------

    bs_math::Vec2 size_local;

    // ---- Authored collision polygon (ship-local space, convex, CCW) ----------------

    bs_math::Vec2 collider_verts[SHIP_MAX_COLLIDER_VERTS];

    i32           collider_count;

    // ---- Weapon inventory --------------------------------------------------------------

    struct Weapon* weapons[SHIP_MAX_WEAPONS];

    i32            weapon_count;

    i32            active_weapon_idx; // -1 = none selected

    // ---- Weapon hardpoint (ship-local, default center; authored in .ship later) ------

    bs_math::Vec2  weapon_fire_offset_local;

    // ---- Per-entity bloom/glow parameters ----------------------------------------------

    bs_glow_params glow;

    // ---- Radiation heat-source emission (0..1) ---------------------------------------

    f32           radiation_emission;

};

// Load a ship from a `.ship` file (path relative to the working directory). Returns FALSE

// on any parse/IO error (and logs why); on success `out_ship` is fully populated with

// origin = {0,0}. Call once during game init.

b8 ship_load(Ship* out_ship, const char* path);

// ---- Rigid-body transforms -----------------------------------------------------------

// Ship-local direction offset: rotates + scales a ship-local vector by the pose's angle and
// scale, but does NOT add the origin. Frame-independent (small Vec2); add it to a HierPos2
// origin (sim) or a render-space base (draw) as needed.

bs_math::Vec2 ship_local_dir(const Ship* ship, bs_math::Vec2 local);

// Ship-local point -> world point: applies the full pose (origin AND angle). Returns a
// hierarchical position (precise at any distance from the galaxy origin).

bs_math::HierPos2 ship_local_to_world(const Ship* ship, bs_math::Vec2 local);

// World point -> ship-local point: removes the full pose (inverse of ship_local_to_world).

bs_math::Vec2 ship_world_to_local(const Ship* ship, bs_math::HierPos2 world);

// ---- Ship-ship collision -----------------------------------------------------------

// World-space bounding-circle radius: half the diagonal of the collider bounding box,

// so it encloses the hull at ANY heading (rotation-invariant). Cheap broad-phase bound.

f32 ship_bounding_radius(const Ship* ship);

// Broad-phase collision: TRUE if two ships' world bounding circles overlap.

b8 ships_overlap(const Ship* a, const Ship* b);

// Narrow-phase polygon collision: tests the two authored collider polygons (transformed

// to world space) with the Separating-Axis Theorem. Returns TRUE when they overlap and

// writes the Minimum Translation Vector to *out_mtv (world units): the SHORTEST displacement

// that moves `a` clear of `b`. Returns FALSE when a separating axis exists (disjoint).

b8 ships_collide(const Ship* a, const Ship* b, bs_math::Vec2* out_mtv);

// Debug/introspection: writes the collider vertices relative to the ship ORIGIN (rotated +
// scaled ship-local offsets, small Vec2 each) in winding order. Add the ship's render-space
// base (or hierpos origin) to place them. Returns FALSE if the ship has no collider (count == 0).

b8 ship_collider_corners(const Ship* ship, bs_math::Vec2 out_corners[SHIP_MAX_COLLIDER_VERTS]);

