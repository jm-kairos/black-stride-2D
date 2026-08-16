#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>

struct game_state;
struct StarSystem;

// Galaxy coordinate helpers -- conversions between world space, the galaxy-map HierPos2
// frame, and per-system local frames, plus nearest-system and orbital-zone lookups.

// Nearest-system lookup: brute-force distance check over the small cluster.
// Returns index of the closest system, or -1 if count == 0.
i32 find_nearest_system(const bs_math::HierPos2* ship_pos,
                        const StarSystem* systems, i32 count);

// Convert ship galaxy position to an offset within a specific system's local frame.
bs_math::Vec2 galaxy_to_system_local(const bs_math::HierPos2* ship_galaxy,
                                     const bs_math::HierPos2* system_center);

// Convert a world-space Vec2 into the galaxy-map HierPos2 coordinate system.
bs_math::HierPos2 world_to_galaxy_pos(bs_math::Vec2 world);

// Determine which concentric orbital zone the player ship occupies in a given system.
// Zones are numbered outside-in: 0 = beyond outermost orbit ... N = inside innermost orbit.
i32 get_system_zone(const game_state* s, i32 system_idx);
