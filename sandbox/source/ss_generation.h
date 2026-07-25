#pragma once
#include <defines.h>
#include "game.h"
#include "sim/galaxy_gen.h"   // SSGenEnv (structural population environment)
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
// `seed` seeds the internal RNG so the same seed always produces the same system. `env` carries the
// system's structural context (galactocentric radius + spiral-arm proximity) so the star population
// follows galactic structure; the default is a neutral mid-disc, non-arm environment.
void generate_star_system(StarSystem* out, u64 seed, bs_math::Vec2 galaxy_pos,
                          SSGenEnv env = SSGenEnv{ 0.5f, 0.0f, 0.0f },
                          const SSGenConfig& cfg = SSGEN_DEFAULT);
// Update all planet positions in a system using true elliptical orbital mechanics.
// Should be called each frame with the simulation dt.
void update_planet_positions(StarSystem* sys, f32 dt);
// Solve eccentric anomaly E from mean anomaly M and eccentricity e.
// Uses Newton-Raphson; typically converges in 4-5 iterations.
f32 solve_eccentric_anomaly(f32 M, f32 e);

// =====================================================================================
// Worldgen property model (physical star & planet attributes). Types live in game_state.h
// (StarProperties / PlanetProperties / SpectralClass / PlanetType) so StarSystem can embed them.
// =====================================================================================

// Roll a full star (spectral class -> mass/temp/luminosity/radius/age/metallicity -> HZ + frost).
// `env` biases the spectral-class mix (hot/blue in spiral arms, cool/red in the bulge) and the
// metallicity gradient (metal-rich core -> metal-poor rim).
StarProperties worldgen_star(u64 seed, SSGenEnv env = SSGenEnv{ 0.5f, 0.0f, 0.0f });

// The AU span planets should occupy for this star so a system always ranges from hot inner worlds
// through the habitable zone out past the frost line (min ~= 0.35*hz_inner, max ~= 2.5*frost).
void worldgen_orbit_range_au(const StarProperties& star, f32* out_au_min, f32* out_au_max);

// Classify + roll a planet's physical properties from its star and orbital distance (AU).
PlanetProperties worldgen_planet(const StarProperties& star, f32 orbit_au, u64 seed);

// Roll a planet's appearance genome (named subtype, 4-stop palette, feature genes, rare anomaly)
// deterministically from the star, the planet's physical properties, and a seed. A blend of the
// physical theme (temperature/mass/age/metallicity bias the subtype + palette) and free random
// variety, so every planet of a type looks distinct. Called from worldgen_planet.
PlanetGenome worldgen_planet_genome(const StarProperties& star, const PlanetProperties& planet, u64 seed);

// Human-readable labels (HUD / debug).
const char* spectral_class_name(SpectralClass c);
const char* planet_type_name(PlanetType t);

// Genome labels for UI: subtype name for a (type, subtype index); anomaly name (0 = none);
// decode trait_bits into up to `max` short tag strings (returns the count written).
const char* planet_subtype_name(PlanetType type, u8 subtype);
const char* planet_anomaly_name(u8 anomaly);
i32         planet_trait_names(u16 trait_bits, const char** out, i32 max);

// Approximate blackbody RGB for a temperature in Kelvin.
bs_color blackbody_color(f32 temp_k);

