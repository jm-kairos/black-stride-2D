// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings (sandbox build doesn't
// define _CRT_SECURE_NO_WARNINGS the way the engine build does).
#define _CRT_SECURE_NO_WARNINGS
#include "ship.h"

#include <core/logger.h>

#include <stdio.h>
#include <string.h>
#include <math.h>  // fabsf (OBB SAT projection)

using namespace bs_math;

// ---- ASCII legend -> TileType --------------------------------------------------------
static TileType char_to_tile(char c) {
    switch (c) {
        case '#': return TILE_HULL;
        case 'W': return TILE_WALL;
        case 'F': return TILE_FLOOR;
        case 'D': return TILE_DOOR;
        case 'G': return TILE_HULL_WINDOW;
        case 'J': return TILE_FLOOR_WINDOW; 
        case 'H': return TILE_HELM;
        case 'A': return TILE_HULL_DOOR;
        case '.': return TILE_EMPTY;
        default:  return TILE_EMPTY; // unknown glyphs treated as open space
    }
}

// Trim trailing CR/LF/space from an in-place buffer, returning the new length.
static i32 rstrip(char* s) {
    i32 n = (i32)strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    return n;
}

b8 ship_load(Ship* out_ship, const char* path) {
    if (!out_ship || !path) {
        BS_LOG_ERROR("ship_load: null ship or path.");
        return FALSE;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        BS_LOG_ERROR("ship_load: could not open '%s'.", path);
        return FALSE;
    }

    *out_ship = Ship{};
    out_ship->tile_size = 32.0f;
    out_ship->origin    = Vec2{ 0.0f, 0.0f };
    out_ship->angle     = 0.0f;

    i32  declared_cols = 0, declared_rows = 0;
    i32  row_cursor    = 0;
    b8   in_grid       = FALSE;
    b8   have_size     = FALSE;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        rstrip(line);

        if (!in_grid) {
            // Skip blank lines and comments outside the grid block.
            if (line[0] == '\0' || line[0] == '#') continue;

            if (strncmp(line, "tile_size", 9) == 0) {
                f32 ts = 0.0f;
                if (sscanf(line + 9, "%f", &ts) == 1 && ts > 0.0f) out_ship->tile_size = ts;
                continue;
            }
            if (strncmp(line, "size", 4) == 0) {
                if (sscanf(line + 4, "%d %d", &declared_cols, &declared_rows) == 2 &&
                    declared_cols > 0 && declared_rows > 0) {
                    have_size = TRUE;
                } else {
                    BS_LOG_ERROR("ship_load: malformed 'size' line in '%s'.", path);
                    fclose(f); return FALSE;
                }
                continue;
            }
            if (strcmp(line, "grid") == 0) {
                if (!have_size) {
                    BS_LOG_ERROR("ship_load: 'grid' before 'size' in '%s'.", path);
                    fclose(f); return FALSE;
                }
                if (declared_cols * declared_rows > SHIP_MAX_TILES) {
                    BS_LOG_ERROR("ship_load: %dx%d exceeds SHIP_MAX_TILES (%d).",
                                 declared_cols, declared_rows, SHIP_MAX_TILES);
                    fclose(f); return FALSE;
                }
                out_ship->cols = declared_cols;
                out_ship->rows = declared_rows;
                in_grid = TRUE;
                continue;
            }
            // Unknown header line — ignore for forward-compat.
            continue;
        }

        // ---- inside the grid block ----
        if (strcmp(line, "end") == 0) break;

        if (row_cursor >= declared_rows) {
            BS_LOG_WARN("ship_load: extra grid rows past declared %d ignored.", declared_rows);
            break;
        }

        i32 len = (i32)strlen(line);
        if (len != declared_cols) {
            BS_LOG_ERROR("ship_load: row %d width %d != declared cols %d in '%s'.",
                         row_cursor, len, declared_cols, path);
            fclose(f); return FALSE;
        }
        for (i32 c = 0; c < declared_cols; ++c) {
            out_ship->tiles[row_cursor * declared_cols + c] = char_to_tile(line[c]);
        }
        ++row_cursor;
    }
    fclose(f);

    if (row_cursor != declared_rows) {
        BS_LOG_ERROR("ship_load: got %d grid rows, expected %d in '%s'.",
                     row_cursor, declared_rows, path);
        return FALSE;
    }

    BS_LOG_INFO("ship_load: '%s' -> %dx%d tiles, tile_size=%.1f.",
                path, out_ship->cols, out_ship->rows, out_ship->tile_size);
    return TRUE;
}

TileType ship_tile_at(const Ship* ship, i32 col, i32 row) {
    if (!ship || col < 0 || row < 0 || col >= ship->cols || row >= ship->rows)
        return TILE_EMPTY;
    return ship->tiles[row * ship->cols + col];
}

b8 ship_tile_is_solid(const Ship* ship, i32 col, i32 row) {
    TileType t = ship_tile_at(ship, col, row);
    // HULL_DOOR is a CLOSED exterior airlock by default — solid like the hull, so crew can't walk
    // out into space. (When two ships dock, opening the mated airlocks for crew transfer is a future
    // step that will special-case the docked pair; the base tile stays solid.)
    return (t == TILE_HULL || t == TILE_WALL || t == TILE_HULL_WINDOW || t == TILE_HULL_DOOR) ? TRUE : FALSE;
}

b8 ship_tile_is_walkable(const Ship* ship, i32 col, i32 row) {
    TileType t = ship_tile_at(ship, col, row);
    // Floor, doors, transparent floor-windows, and the helm are deck the crew can occupy. Walls,
    // hull, hull-glass, and empty space are not. (Mirror of the spec's walkable column.)
    return (t == TILE_FLOOR || t == TILE_DOOR || t == TILE_FLOOR_WINDOW || t == TILE_HELM) ? TRUE : FALSE;
}

b8 ship_tile_is_structure(const Ship* ship, i32 col, i32 row) {
    return (ship_tile_at(ship, col, row) != TILE_EMPTY) ? TRUE : FALSE;
}

// Tile center in SHIP-LOCAL space: grid centered on {0,0}, row 0 at the top (+Y, camera is
// y-up), so local Y decreases as `row` increases. No origin, no angle — the authoring frame.
Vec2 ship_tile_center_local(const Ship* ship, i32 col, i32 row) {
    if (col < 0 || row < 0 || col >= ship->cols || row >= ship->rows) {
        return Vec2{0, 0}; // Return a default value for out-of-bounds tiles
    }
    f32 ts = ship->tile_size;
    f32 half_w = ship->cols * ts * 0.5f;
    f32 half_h = ship->rows * ts * 0.5f;
    f32 x = -half_w + (col + 0.5f) * ts;
    f32 y =  half_h - (row + 0.5f) * ts;
    return Vec2{ x, y };
}

Vec2 ship_local_to_world(const Ship* ship, Vec2 local) {
    return vec2_add(ship->origin, vec2_rotate(local, ship->angle));
}

Vec2 ship_world_to_local(const Ship* ship, Vec2 world) {
    return vec2_rotate(vec2_sub(world, ship->origin), -ship->angle);
}

// Axis-aligned tile lookup from a ship-local point (inverse of ship_tile_center_local).
void ship_local_to_tile(const Ship* ship, Vec2 local, i32* out_col, i32* out_row) {
    f32 ts = ship->tile_size;
    f32 half_w = ship->cols * ts * 0.5f;
    f32 half_h = ship->rows * ts * 0.5f;
    f32 gx = local.x + half_w;   // distance from the left edge
    f32 gy = half_h - local.y;   // distance from the top edge (y-up)
    *out_col = (i32)(gx / ts);
    *out_row = (i32)(gy / ts);
    // Guard against negative truncation rounding toward zero instead of down.
    if (gx < 0.0f) *out_col = -1;
    if (gy < 0.0f) *out_row = -1;
}

// World-space tile center: place the local center under the full pose (origin AND angle).
Vec2 ship_tile_center_world(const Ship* ship, i32 col, i32 row) {
    return ship_local_to_world(ship, ship_tile_center_local(ship, col, row));
}

// World -> tile: undo the pose into ship-local space, then axis-aligned lookup.
void ship_world_to_tile(const Ship* ship, Vec2 world, i32* out_col, i32* out_row) {
    ship_local_to_tile(ship, ship_world_to_local(ship, world), out_col, out_row);
}

Vec2 ship_world_size(const Ship* ship) {
    return Vec2{ ship->cols * ship->tile_size, ship->rows * ship->tile_size };
}

b8 ship_find_first_tile(const Ship* ship, TileType want, i32* out_col, i32* out_row) {
    if (!ship || !out_col || !out_row) return FALSE;
    for (i32 r = 0; r < ship->rows; ++r) {
        for (i32 c = 0; c < ship->cols; ++c) {
            if (ship->tiles[r * ship->cols + c] == want) {
                *out_col = c;
                *out_row = r;
                return TRUE;
            }
        }
    }
    return FALSE;
}

// ---- Ship-ship collision & docking ---------------------------------------------------
f32 ship_bounding_radius(const Ship* ship) {
    if (!ship) return 0.0f;
    Vec2 sz = ship_world_size(ship);           // full footprint (w,h) in world units
    return vec2_length(sz) * 0.5f;             // half-diagonal: encloses the hull at ANY heading
}

b8 ships_overlap(const Ship* a, const Ship* b) {
    if (!a || !b) return FALSE;
    f32 d  = vec2_length(vec2_sub(a->origin, b->origin)); // origin-to-origin distance (pose-correct)
    f32 rr = ship_bounding_radius(a) + ship_bounding_radius(b);
    return (d < rr) ? TRUE : FALSE;            // bounding circles intersect => broad-phase contact
}

b8 ships_docked(const Ship* a, const Ship* b, f32 tolerance,
                i32* out_col_a, i32* out_row_a, i32* out_col_b, i32* out_row_b) {
    if (!a || !b) return FALSE;
    // Broad phase first: if the bounding circles don't even touch, the airlocks can't be mated.
    if (!ships_overlap(a, b)) return FALSE;

    // Narrow phase: minimum WORLD distance between any HULL_DOOR on `a` and any on `b`. Both
    // centers come from ship_tile_center_world, which applies each hull's full pose (origin AND
    // angle), so this handshake is correct no matter how either ship is translated or rotated.
    b8  found = FALSE;
    f32 best  = tolerance;                     // only matches strictly within tolerance count
    for (i32 ra = 0; ra < a->rows; ++ra) {
        for (i32 ca = 0; ca < a->cols; ++ca) {
            if (a->tiles[ra * a->cols + ca] != TILE_HULL_DOOR) continue;
            Vec2 wa = ship_tile_center_world(a, ca, ra);
            for (i32 rb = 0; rb < b->rows; ++rb) {
                for (i32 cb = 0; cb < b->cols; ++cb) {
                    if (b->tiles[rb * b->cols + cb] != TILE_HULL_DOOR) continue;
                    f32 d = vec2_length(vec2_sub(wa, ship_tile_center_world(b, cb, rb)));
                    if (d <= best) {
                        best  = d;
                        found = TRUE;
                        if (out_col_a) *out_col_a = ca;
                        if (out_row_a) *out_row_a = ra;
                        if (out_col_b) *out_col_b = cb;
                        if (out_row_b) *out_row_b = rb;
                    }
                }
            }
        }
    }
    return found;
}

// ---- OBB collision (SAT) -------------------------------------------------------------
// An oriented bounding box in world space: center C, orthonormal axes (ux,uy) = the ship's
// rotated local axes, and half-extents (hx,hy) along them.
struct ShipOBB { Vec2 c; Vec2 ux; Vec2 uy; f32 hx; f32 hy; };

// Build the OBB enclosing a ship's OCCUPIED tiles (not the raw cols*rows grid — the .tmap has
// empty '.' border cells, and the airlocks sit on the tight occupied edge). Using the tight
// footprint makes the box hug the real hull, so the airlock faces coincide with the box edges:
// two hulls touch exactly when their airlocks meet, which is what lets docking and collision
// coexist instead of the empty-corner padding shoving the airlocks apart. Returns FALSE for an
// empty ship (no structure tiles -> no box).
static b8 ship_obb(const Ship* ship, ShipOBB* out) {
    if (!ship) return FALSE;
    i32 min_c = ship->cols, min_r = ship->rows, max_c = -1, max_r = -1;
    for (i32 r = 0; r < ship->rows; ++r) {
        for (i32 c = 0; c < ship->cols; ++c) {
            if (ship->tiles[r * ship->cols + c] == TILE_EMPTY) continue;
            if (c < min_c) min_c = c;
            if (c > max_c) max_c = c;
            if (r < min_r) min_r = r;
            if (r > max_r) max_r = r;
        }
    }
    if (max_c < 0) return FALSE; // no occupied tiles

    const f32 ts = ship->tile_size;
    const f32 half_w = ship->cols * ts * 0.5f;
    const f32 half_h = ship->rows * ts * 0.5f;
    // Occupied extent in ship-LOCAL space (y-up: row 0 is the TOP, so larger row => smaller y).
    const f32 minx = -half_w + (f32)min_c * ts;
    const f32 maxx = -half_w + (f32)(max_c + 1) * ts;
    const f32 maxy =  half_h - (f32)min_r * ts;          // top edge of the topmost occupied row
    const f32 miny =  half_h - (f32)(max_r + 1) * ts;    // bottom edge of the bottommost row

    const Vec2 center_local = Vec2{ (minx + maxx) * 0.5f, (miny + maxy) * 0.5f };
    out->c  = ship_local_to_world(ship, center_local);   // full pose (origin AND angle)
    out->ux = vec2_rotate(Vec2{ 1.0f, 0.0f }, ship->angle);
    out->uy = vec2_rotate(Vec2{ 0.0f, 1.0f }, ship->angle);
    out->hx = (maxx - minx) * 0.5f;
    out->hy = (maxy - miny) * 0.5f;
    return TRUE;
}

// Projected radius of an OBB onto a unit axis L: how far the box extends from its center along L.
static f32 obb_project_radius(const ShipOBB* o, Vec2 L) {
    return o->hx * fabsf(vec2_dot(o->ux, L)) + o->hy * fabsf(vec2_dot(o->uy, L));
}

// Test one candidate separating axis L (need not be unit-length; we normalize so the returned
// overlap is a true world-space distance, directly comparable across the 4 axes for the MTV).
// Returns FALSE (separated) if the projections are disjoint; else TRUE and writes the penetration
// depth + the unit axis. `d` is (center_b - center_a), reused across axes.
static b8 sat_axis(const ShipOBB* a, const ShipOBB* b, Vec2 d, Vec2 L,
                   f32* out_overlap, Vec2* out_axis) {
    f32 len = vec2_length(L);
    if (len < 1.0e-6f) { *out_overlap = 1.0e30f; return TRUE; } // degenerate axis: ignore (huge overlap so it never wins MTV)
    L = vec2_scale(L, 1.0f / len);
    f32 ra = obb_project_radius(a, L);
    f32 rb = obb_project_radius(b, L);
    f32 s  = fabsf(vec2_dot(d, L));
    f32 overlap = ra + rb - s;
    if (overlap <= 0.0f) return FALSE; // separating axis found -> boxes disjoint
    *out_overlap = overlap;
    *out_axis    = L;
    return TRUE;
}

b8 ships_collide(const Ship* a, const Ship* b, Vec2* out_mtv) {
    if (!a || !b) return FALSE;
    // Broad phase: bounding circles must touch for the hulls to possibly overlap (cheap reject).
    if (!ships_overlap(a, b)) return FALSE;

    ShipOBB oa, ob;
    if (!ship_obb(a, &oa) || !ship_obb(b, &ob)) return FALSE;

    // SAT: a pair of rectangles is separated iff a separating axis exists among the 4 face
    // normals (2 per box). If NONE separates, they overlap; the MTV is the axis of MINIMUM
    // penetration, signed to push `a` off `b`.
    const Vec2 d = vec2_sub(ob.c, oa.c); // center_b - center_a
    Vec2 axes[4] = { oa.ux, oa.uy, ob.ux, ob.uy };

    f32  min_overlap = 1.0e30f;
    Vec2 min_axis    = Vec2{ 0.0f, 0.0f };
    for (i32 i = 0; i < 4; ++i) {
        f32  overlap;
        Vec2 axis;
        if (!sat_axis(&oa, &ob, d, axes[i], &overlap, &axis))
            return FALSE; // found a separating axis -> no collision
        if (overlap < min_overlap) { min_overlap = overlap; min_axis = axis; }
    }

    if (out_mtv) {
        // Point the MTV from b toward a so adding it to a->origin separates them. d = C_b - C_a,
        // so a is in the -d direction from b; flip the axis if it currently points toward b.
        f32 sign = (vec2_dot(d, min_axis) > 0.0f) ? -1.0f : 1.0f;
        *out_mtv = vec2_scale(min_axis, sign * min_overlap);
    }
    return TRUE;
}

// The four world-space corners of the tight collider OBB (see ship.h). Reuses ship_obb so the
// drawn outline is the EXACT box ships_collide tests — center +/- the half-extents along the
// ship's rotated axes. Winding (CCW in local space, y-up): (-,-) (+,-) (+,+) (-,+).
b8 ship_collider_corners(const Ship* ship, Vec2 out_corners[4]) {
    if (!ship || !out_corners) return FALSE;
    ShipOBB o;
    if (!ship_obb(ship, &o)) return FALSE; // empty ship -> no box
    const Vec2 ex = vec2_scale(o.ux, o.hx); // half-extent vector along local x
    const Vec2 ey = vec2_scale(o.uy, o.hy); // half-extent vector along local y
    out_corners[0] = vec2_add(o.c, vec2_sub(vec2_scale(ex, -1.0f), ey)); // (minx, miny)
    out_corners[1] = vec2_add(o.c, vec2_sub(ex, ey));                    // (maxx, miny)
    out_corners[2] = vec2_add(o.c, vec2_add(ex, ey));                    // (maxx, maxy)
    out_corners[3] = vec2_add(o.c, vec2_add(vec2_scale(ex, -1.0f), ey)); // (minx, maxy)
    return TRUE;
}

// Outward airlock normal (see ship.h). A HULL_DOOR sits on the hull perimeter; its single interior
// landfall neighbor is INBOARD, so the door-minus-interior vector points OUTBOARD — the direction the
// airlock faces, in world space under the full pose. Returns the unit outward normal.
b8 ship_airlock_outward_normal(const Ship* ship, i32 door_col, i32 door_row, Vec2* out_normal) {
    if (!ship) return FALSE;
    if (ship_tile_at(ship, door_col, door_row) != TILE_HULL_DOOR) return FALSE;
    i32 ic, ir;
    if (!ship_airlock_interior_tile(ship, door_col, door_row, &ic, &ir)) return FALSE; // no interior deck

    Vec2 door_w     = ship_tile_center_world(ship, door_col, door_row);
    Vec2 interior_w = ship_tile_center_world(ship, ic, ir);
    Vec2 out        = vec2_sub(door_w, interior_w);  // interior -> door == outboard, world space
    f32  len        = vec2_length(out);
    if (len < 1.0e-6f) return FALSE;
    if (out_normal) *out_normal = vec2_scale(out, 1.0f / len);
    return TRUE;
}

// Docking snap (see ship.h). Find the mated HULL_DOOR pair with the SAME narrow-phase the dock gate
// uses (ships_docked), then solve a rigid-body correction in TWO parts applied in order to `mover`:
//
//   (1) ROTATION (out_angle): spin the mover so its airlock's outward normal becomes ANTI-parallel to
//       the anchor's — i.e. the two DOORS END PARALLEL and FACE each other. This is what makes the
//       mate overlap-free at ANY approach angle: with anti-parallel door normals the two tight-OBB
//       door faces are coplanar, so that face line is a separating axis and the hulls touch flush
//       instead of one hull's corner poking into the other. (Pure translation could only mate cleanly
//       when the pilot happened to fly in already parallel; the rotation removes that dependence.)
//
//   (2) TRANSLATION (out_delta): with the rotation applied, slide the mover so its door sits exactly
//       TWO tile_sizes off the anchor's door ALONG the anchor's outward normal — the clean
//       "joined at the airlock, two-tile gap, no overlap" pose. The gap is two tiles (not one) so a
//       one-tile CONNECTOR bridge fits flush between the mated doors (see dock_connector_tile).
//
// Both are pure functions of the two poses + tilemaps; the game loop and the headless harness call
// THIS so the mating geometry is verified against the real code, never re-derived.
b8 dock_snap_delta(const Ship* mover, const Ship* anchor, f32 tolerance,
                   Vec2* out_delta, f32* out_angle) {
    if (!mover || !anchor) return FALSE;

    // Reuse the verified narrow phase to locate the closest mated door pair. Pass anchor as `a`
    // (=> ca,ra is the anchor's door) and mover as `b` (=> cb,rb is the mover's door).
    i32 ca, ra, cb, rb;
    if (!ships_docked(anchor, mover, tolerance, &ca, &ra, &cb, &rb)) return FALSE;

    // ---- (1) Rotation: make the mover's airlock normal anti-parallel to the anchor's (doors parallel
    // & facing). atan2(cross, dot) is the signed MINIMAL turn that rotates nm onto the target, already
    // wrapped to (-PI,PI] — a near-aligned approach yields a tiny corrective turn, never a multi-turn
    // unwind. If either airlock normal is undefined (degenerate map) we fall back to no rotation.
    Vec2 na{ 0.0f, 0.0f }, nm{ 0.0f, 0.0f };
    b8   have_na = ship_airlock_outward_normal(anchor, ca, ra, &na);
    b8   have_nm = ship_airlock_outward_normal(mover,  cb, rb, &nm);
    f32  dtheta  = 0.0f;
    if (have_na && have_nm) {
        Vec2 target = vec2_scale(na, -1.0f);                 // mover normal should oppose the anchor's
        f32  crossv = nm.x * target.y - nm.y * target.x;     // z of nm x target (sign of the turn)
        f32  dotv   = vec2_dot(nm, target);                  // cos of the angle between them
        dtheta      = atan2f(crossv, dotv);                  // signed minimal rotation, in (-PI,PI]
    }
    if (out_angle) *out_angle = dtheta;

    // ---- (2) Translation, evaluated at the POST-rotation pose. Rotating the mover about its origin
    // moves the door; recompute the mover door's world position at (mover->angle + dtheta).
    Vec2 anchor_door    = ship_tile_center_world(anchor, ca, ra);
    Vec2 mover_local    = ship_tile_center_local(mover, cb, rb);
    Vec2 mover_door_rot = vec2_add(mover->origin, vec2_rotate(mover_local, mover->angle + dtheta));

    // Mating direction = anchor's outward normal (the side the airlock faces). Degenerate-airlock
    // fallback: the live door-to-door axis, then the anchor radial, so `dir` is always defined.
    Vec2 dir;
    if (have_na) {
        dir = na;
    } else {
        Vec2 sep = vec2_sub(mover_door_rot, anchor_door);
        f32  d   = vec2_length(sep);
        if (d > 1.0e-3f) {
            dir = vec2_scale(sep, 1.0f / d);
        } else {
            Vec2 radial = vec2_sub(anchor_door, anchor->origin);
            f32  rl     = vec2_length(radial);
            dir = (rl > 1.0e-3f) ? vec2_scale(radial, 1.0f / rl) : Vec2{ 1.0f, 0.0f };
        }
    }

    Vec2 target = vec2_add(anchor_door, vec2_scale(dir, 2.0f * anchor->tile_size)); // mover door, TWO tiles out (room for the connector)
    if (out_delta) *out_delta = vec2_sub(target, mover_door_rot); // translate rotated mover door to target
    return TRUE;
}

// Docking proximity tolerance (see ship.h). 2.5 tile_sizes: the two-tile mated gap plus a half-tile
// of slack. Single source of truth so the snap gap (dock_snap_delta) and every proximity query
// (ships_docked, ship_seam_landfall, dock_connector_tile, the game's dock latch) stay in lockstep.
f32 ship_dock_tolerance(const Ship* ship) {
    return ship ? ship->tile_size * 2.5f : 0.0f;
}

// Connector-bridge placement (see ship.h). Locate the mated door pair with the SAME narrow-phase the
// dock gate uses, then return the connector tile's world center — the midpoint of the two door world
// centers (the single clear tile in the two-tile gap) — and its world angle (the anchor airlock's
// facing, so the square tile sits flush with both mated doors). Pure function of the two poses, so the
// game loop and the headless harness compute the bridge position identically.
b8 dock_connector_tile(const Ship* a, const Ship* b, f32 tolerance,
                       Vec2* out_world, f32* out_angle) {
    if (!a || !b) return FALSE;

    i32 ca, ra, cb, rb;
    if (!ships_docked(a, b, tolerance, &ca, &ra, &cb, &rb)) return FALSE;

    Vec2 door_a = ship_tile_center_world(a, ca, ra);
    Vec2 door_b = ship_tile_center_world(b, cb, rb);
    if (out_world) *out_world = vec2_scale(vec2_add(door_a, door_b), 0.5f); // midpoint = the gap's clear tile

    if (out_angle) {
        Vec2 n{ 0.0f, 0.0f };
        // Align the tile with the anchor airlock's outward facing (a grid-axis direction under a's
        // pose) so its edges sit parallel to both mated doors. Fall back to the anchor heading.
        *out_angle = ship_airlock_outward_normal(a, ca, ra, &n) ? atan2f(n.y, n.x) : a->angle;
    }
    return TRUE;
}

// Airlock interior landfall tile (see ship.h). A HULL_DOOR sits on the hull perimeter; of its four
// orthogonal neighbors exactly one is interior deck. Scan E/W/N/S and return the first walkable one.
b8 ship_airlock_interior_tile(const Ship* ship, i32 door_col, i32 door_row,
                              i32* out_col, i32* out_row) {
    if (!ship) return FALSE;
    if (ship_tile_at(ship, door_col, door_row) != TILE_HULL_DOOR) return FALSE;

    static const i32 DC[4] = { 1, -1, 0, 0 };
    static const i32 DR[4] = { 0, 0, 1, -1 };
    for (i32 i = 0; i < 4; ++i) {
        const i32 nc = door_col + DC[i];
        const i32 nr = door_row + DR[i];
        if (ship_tile_is_walkable(ship, nc, nr)) {
            if (out_col) *out_col = nc;
            if (out_row) *out_row = nr;
            return TRUE;
        }
    }
    return FALSE; // malformed map: airlock with no interior deck neighbor
}

// Cross-ship seam handoff geometry (see ship.h). Locate the mated door pair (ships_docked finds the
// closest HULL_DOOR pair under both full poses), take the BOARDING hull's door, and return its
// interior landfall tile. Passing `to` as ships_docked's `a` makes its out_(col,row)_a the boarding
// hull's mated door directly.
b8 ship_seam_landfall(const Ship* from, const Ship* to, f32 tolerance,
                      i32* out_col, i32* out_row, Vec2* out_local) {
    if (!from || !to) return FALSE;

    // `to` as `a` => (cto,rto) is the boarding hull's mated door; `from` as `b` (ignored here).
    i32 cto, rto, cfrom, rfrom;
    if (!ships_docked(to, from, tolerance, &cto, &rto, &cfrom, &rfrom)) return FALSE;

    i32 ic, ir;
    if (!ship_airlock_interior_tile(to, cto, rto, &ic, &ir)) return FALSE;
    if (out_col)   *out_col   = ic;
    if (out_row)   *out_row   = ir;
    if (out_local) *out_local = ship_tile_center_local(to, ic, ir);
    return TRUE;
}
