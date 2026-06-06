// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings (sandbox build doesn't
// define _CRT_SECURE_NO_WARNINGS the way the engine build does).
#define _CRT_SECURE_NO_WARNINGS
#include "ship.h"

#include <core/logger.h>

#include <stdio.h>
#include <string.h>

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
    return (t == TILE_HULL || t == TILE_WALL || t == TILE_HULL_WINDOW) ? TRUE : FALSE;
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
