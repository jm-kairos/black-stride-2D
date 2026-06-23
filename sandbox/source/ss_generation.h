#pragma once
#include <defines.h>
#include "game.h"
// =====================================================================================
// Procedural Star-System Generator
// =====================================================================================
// Generates stable, non-intersecting planetary orbits with realistic spacing,
// variable eccentricity, and per-system scale variety.
struct SSGenConfig {
    i32   min_planets     = 2;
    i32   max_planets     = 5;
    f32   spacing_min     = 1.40f;    // min spacing factor k
    f32   spacing_max     = 2.00f;    // max spacing factor k
    f32   safety_margin   = 0.05f;    // gap between apoapsis(inner) and periapsis(outer)
    f32   scale_min       = 0.6f;     // compact systems
    f32   scale_max       = 1.5f;     // sparse systems
    f32   inner_a_min     = 1.5e6f;   // innermost semi-major axis (world units)
    f32   inner_a_max     = 3.0e6f;
    f32   e_inner_max     = 0.05f;    // inner planets: nearly circular
    f32   e_outer_max     = 0.25f;    // outer planets: mildly eccentric
    f32   star_mass       = 1.0f;     // for period computation (GM = 1 in arbitrary units)
};
extern const SSGenConfig SSGEN_DEFAULT;
// Generate a complete star system (star + planets) at the given galaxy position.
// `seed` seeds the internal RNG so the same seed always produces the same system.
void generate_star_system(StarSystem* out, u64 seed, bs_math::Vec2 galaxy_pos,
                          const SSGenConfig& cfg = SSGEN_DEFAULT);
// Update all planet positions in a system using true elliptical orbital mechanics.
// Should be called each frame with the simulation dt.
void update_planet_positions(StarSystem* sys, f32 dt);
// Solve eccentric anomaly E from mean anomaly M and eccentricity e.
// Uses Newton-Raphson; typically converges in 4-5 iterations.
f32 solve_eccentric_anomaly(f32 M, f32 e);
