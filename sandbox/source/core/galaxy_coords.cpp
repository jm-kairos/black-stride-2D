#include "core/galaxy_coords.h"
#include "game.h"

using namespace bs_math;

// Nearest-system lookup: brute-force distance check over the small cluster.
// Returns index of the closest system, or -1 if count == 0.
i32 find_nearest_system(const HierPos2* ship_pos,
                        const StarSystem* systems, i32 count)
{
    if (count <= 0) return -1;
    i32 best = -1;
    f64 best_dist = 1e300;
    for (i32 i = 0; i < count; ++i) {
        f64 sx, sy, cx, cy;
        hierpos_to_f64(ship_pos, BS_HIERPOS_CELL_SIZE, &sx, &sy);
        hierpos_to_f64(&systems[i].galaxy_center, BS_HIERPOS_CELL_SIZE, &cx, &cy);
        f64 dx = sx - cx;
        f64 dy = sy - cy;
        f64 d = dx * dx + dy * dy;
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return best;
}

// Convert ship galaxy position to an offset within a specific system's local frame.
// Result is the same Vec2 the current MODE_SYSTEM render code expects.
Vec2 galaxy_to_system_local(const HierPos2* ship_galaxy,
                            const HierPos2* system_center)
{
    f64 sx, sy, cx, cy;
    hierpos_to_f64(ship_galaxy,   BS_HIERPOS_CELL_SIZE, &sx, &sy);
    hierpos_to_f64(system_center, BS_HIERPOS_CELL_SIZE, &cx, &cy);
    return Vec2{ (f32)(sx - cx), (f32)(sy - cy) };
}

// Convert a world-space Vec2 into the galaxy-map HierPos2 coordinate system.
HierPos2 world_to_galaxy_pos(Vec2 world) {
    return hierpos_from_vec2(world, BS_HIERPOS_CELL_SIZE);
}

// Determine which zone the player ship occupies in a given star system.
// Zones are concentric rings bounded by planet orbit radii, numbered from
// outside in: Zone 0 = beyond outermost orbit, Zone 1 = between outermost
// and middle orbit, ... Zone N = inside innermost orbit.
i32 get_system_zone(const game_state* s, i32 system_idx) {
    const StarSystem& sys = s->galaxy.systems[system_idx];
    Vec2 diff = hierpos_diff(&s->galaxy.map_entities[0].galaxy_pos, &sys.galaxy_center, BS_HIERPOS_CELL_SIZE);
    f32 dist = vec2_length(diff);
    // Collect valid orbit radii
    f32 orbits[MAX_SYSTEM_PLANETS];
    i32 orbit_count = 0;
    for (i32 i = 0; i < sys.planet_count; ++i) {
        if (sys.planets[i].semi_major_axis > 0.0f) {
            orbits[orbit_count++] = sys.planets[i].semi_major_axis;
        }
    }
    // Sort ascending (bubble sort, N <= 5)
    for (i32 i = 0; i < orbit_count; ++i) {
        for (i32 j = i + 1; j < orbit_count; ++j) {
            if (orbits[j] < orbits[i]) {
                f32 tmp = orbits[i]; orbits[i] = orbits[j]; orbits[j] = tmp;
            }
        }
    }
    // Zones are 0-indexed from outside in
    for (i32 i = orbit_count - 1; i >= 0; --i) {
        if (dist > orbits[i]) return (orbit_count - 1 - i);
    }
    return orbit_count; // inside innermost orbit
}
