#include "sim/ss_generation.h"
#include "sim/galaxy_rng.h"        // galaxy_splitmix64 / GalaxyRng (deterministic worldgen rolls)
#include "sim/system_evolution.h"  // evolve_star_system (four-phase epoch pipeline)
#include <math.h>
using namespace bs_math;
// =====================================================================================
// Internal RNG (splitmix64)
// =====================================================================================
static u64 s_rng_state = 0x123456789ABCDEF0ull;
static void rng_seed(u64 seed) { s_rng_state = seed; }
static u64 rng_next() {
    u64 z = (s_rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static f32 rng_f32()  { return (f32)(rng_next() & 0xFFFFFF) / (f32)0xFFFFFF; }
static f32 rng_range(f32 min, f32 max) { return min + rng_f32() * (max - min); }
// =====================================================================================
const SSGenConfig SSGEN_DEFAULT{};
// =====================================================================================
// Worldgen: physical star & planet properties (deterministic, seed-derived)
// =====================================================================================

// Per-class base ranges + integer selection weight (~ the real stellar distribution; M dominant).
struct WGClassBase { f32 mass_lo, mass_hi, temp_lo, temp_hi, radius_lo, radius_hi; u32 weight; };
static const WGClassBase WG_CLASS[SPEC_COUNT] = {
    /*O*/ { 16.0f, 90.0f, 30000.0f, 50000.0f, 6.6f, 15.0f,    1u },
    /*B*/ {  2.1f, 16.0f, 10000.0f, 30000.0f, 1.8f,  6.6f,   13u },
    /*A*/ {  1.4f,  2.1f,  7500.0f, 10000.0f, 1.4f,  1.8f,   60u },
    /*F*/ {  1.04f, 1.4f,  6000.0f,  7500.0f, 1.15f, 1.4f,  300u },
    /*G*/ {  0.8f,  1.04f, 5200.0f,  6000.0f, 0.96f, 1.15f, 760u },
    /*K*/ {  0.45f, 0.8f,  3700.0f,  5200.0f, 0.7f,  0.96f,1210u },
    /*M*/ {  0.08f, 0.45f, 2400.0f,  3700.0f, 0.1f,  0.7f, 7650u },
};

static inline f32 wg_clamp01(f32 x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static SpectralClass wg_roll_class(GalaxyRng* r, SSGenEnv env) {
    // Population weighting by galactic structure: young hot stars (O..F) form in the spiral arms;
    // old cool stars (K,M) dominate the metal-rich central bulge. Modulate the base MK weights.
    f32 hot = 1.0f + 3.0f * env.arm_strength;                        // arms -> more O/B/A/F (bluer)
    f32 bw  = env.r_norm < 0.25f ? (1.0f - env.r_norm * 4.0f) : 0.0f;
    bw = bw * bw * (3.0f - 2.0f * bw);                               // smoothstep bulge weight
    f32 cool = 1.0f + 2.0f * bw + 2.5f * env.age_bias;               // bulge/old-population -> K/M (gold/red)
    f32 w[SPEC_COUNT]; f32 total = 0.0f;
    for (i32 i = 0; i < SPEC_COUNT; ++i) {
        f32 wi = (f32)WG_CLASS[i].weight;
        if (i <= (i32)SPEC_F) wi *= hot;                            // O, B, A, F
        if (i >= (i32)SPEC_K) wi *= cool;                           // K, M
        w[i] = wi; total += wi;
    }
    f32 pick = galaxy_rng_f32(r) * total;
    f32 acc = 0.0f;
    for (i32 i = 0; i < SPEC_COUNT; ++i) { acc += w[i]; if (pick < acc) return (SpectralClass)i; }
    return SPEC_M;
}

// Per-type render tint (RGB); generate_star_system jitters it slightly per planet.
static bs_color wg_planet_base_color(PlanetType t) {
    switch (t) {
        case PLANET_LAVA:      return bs_color{ 0.78f, 0.24f, 0.12f, 1.0f };
        case PLANET_ROCKY:     return bs_color{ 0.52f, 0.47f, 0.42f, 1.0f };
        case PLANET_DESERT:    return bs_color{ 0.82f, 0.66f, 0.40f, 1.0f };
        case PLANET_OCEAN:     return bs_color{ 0.20f, 0.46f, 0.78f, 1.0f };
        case PLANET_TERRAN:    return bs_color{ 0.32f, 0.60f, 0.42f, 1.0f };
        case PLANET_GAS_GIANT: return bs_color{ 0.80f, 0.66f, 0.46f, 1.0f };
        case PLANET_ICE_GIANT: return bs_color{ 0.48f, 0.70f, 0.86f, 1.0f };
        case PLANET_FROZEN:    return bs_color{ 0.82f, 0.86f, 0.94f, 1.0f };
        default:               return bs_color{ 0.6f, 0.6f, 0.6f, 1.0f };
    }
}

bs_color blackbody_color(f32 temp_k) {
    // Tanner Helland approximation (1000..40000 K), normalized to 0..1.
    f32 t = temp_k / 100.0f;
    f32 r, g, b;
    if (t <= 66.0f) {
        r = 1.0f;
        g = 0.39008157f * logf(fmaxf(t, 1.0f)) - 0.63184144f;
    } else {
        r = 1.29293618f * powf(fmaxf(t - 60.0f, 1.0f), -0.1332047592f);
        g = 1.12989086f * powf(fmaxf(t - 60.0f, 1.0f), -0.0755148492f);
    }
    if (t >= 66.0f)      b = 1.0f;
    else if (t <= 19.0f) b = 0.0f;
    else                 b = 0.54320679f * logf(fmaxf(t - 10.0f, 1.0f)) - 1.19625408f;
    return bs_color{ wg_clamp01(r), wg_clamp01(g), wg_clamp01(b), 1.0f };
}

StarProperties worldgen_star(u64 seed, SSGenEnv env) {
    GalaxyRng r = galaxy_rng_seed(seed ^ 0x5DEECE66Dull);
    StarProperties s{};
    s.spectral_class = wg_roll_class(&r, env);
    const WGClassBase& cb = WG_CLASS[s.spectral_class];
    s.mass_solar    = galaxy_rng_range(&r, cb.mass_lo,   cb.mass_hi);
    s.temperature_k = galaxy_rng_range(&r, cb.temp_lo,   cb.temp_hi);
    s.radius_solar  = galaxy_rng_range(&r, cb.radius_lo, cb.radius_hi);
    // Main-sequence mass-luminosity relation (L ~ M^3.5) with mild scatter.
    s.luminosity_solar = powf(s.mass_solar, 3.5f) * galaxy_rng_range(&r, 0.8f, 1.25f);
    // Main-sequence lifetime ~ 10 * M^-2.5 Gyr; age uniform up to min(12, lifetime).
    f32 lifetime = 10.0f * powf(s.mass_solar, -2.5f);
    f32 age_cap  = lifetime < 12.0f ? lifetime : 12.0f;
    s.age_gyr     = galaxy_rng_range(&r, 0.05f, age_cap > 0.06f ? age_cap : 0.06f);
    // Metallicity gradient: metal-rich core -> metal-poor rim (drives giant/planet frequency and,
    // with arm heating, shapes the galactic habitable zone), plus per-star scatter.
    f32 metal_mean = 2.0f - 1.5f * env.r_norm + 0.5f * env.age_bias; // core & old ellipticals metal-rich
    f32 metal = metal_mean + galaxy_rng_range(&r, -0.5f, 0.5f);
    s.metallicity = metal < 0.2f ? 0.2f : (metal > 2.5f ? 2.5f : metal);
    // Habitable zone + snow/frost line (AU) from luminosity.
    f32 sqrtL = sqrtf(s.luminosity_solar);
    s.hz_inner_au   = sqrtf(s.luminosity_solar / 1.10f);
    s.hz_outer_au   = sqrtf(s.luminosity_solar / 0.53f);
    s.frost_line_au = 4.85f * sqrtL;
    return s;
}

void worldgen_orbit_range_au(const StarProperties& star, f32* out_au_min, f32* out_au_max) {
    if (out_au_min) *out_au_min = 0.35f * star.hz_inner_au;
    if (out_au_max) *out_au_max = 2.50f * star.frost_line_au;
}

PlanetProperties worldgen_planet(const StarProperties& star, f32 orbit_au, u64 seed) {
    GalaxyRng r = galaxy_rng_seed(seed ^ 0x9E3779B97F4A7C15ull);
    PlanetProperties p{};
    p.orbit_au = orbit_au;

    f32 hz_i = star.hz_inner_au, hz_o = star.hz_outer_au, frost = star.frost_line_au;
    f32 giant_bias = wg_clamp01((star.metallicity - 0.5f) / 2.0f); // metal-rich -> more gas giants
    // Generous "temperate" band around the strict habitable zone so near-miss orbits still produce
    // ocean/terran candidates; the temperature-driven habitability SCORE separates good from marginal.
    f32 temperate_lo = 0.70f * hz_i;
    f32 temperate_hi = 1.30f * hz_o;
    // ---- Classify by orbital distance relative to the star's HZ / frost line ----
    if      (orbit_au < 0.5f * hz_i)  p.type = PLANET_LAVA;
    else if (orbit_au < temperate_lo) p.type = (galaxy_rng_f32(&r) < 0.6f) ? PLANET_ROCKY : PLANET_DESERT;
    else if (orbit_au <= temperate_hi) {
        f32 x = galaxy_rng_f32(&r);   // ~40% habitable-typed in the temperate band; rest dry desert
        p.type = (x < 0.22f) ? PLANET_OCEAN : (x < 0.42f ? PLANET_TERRAN : PLANET_DESERT);
    }
    else if (orbit_au < frost)        p.type = (galaxy_rng_f32(&r) < 0.5f) ? PLANET_ROCKY : PLANET_DESERT;
    else if (orbit_au < 1.8f * frost) p.type = (galaxy_rng_f32(&r) < 0.4f + 0.5f * giant_bias) ? PLANET_GAS_GIANT : PLANET_ICE_GIANT;
    else if (orbit_au < 3.5f * frost) p.type = (galaxy_rng_f32(&r) < 0.6f) ? PLANET_ICE_GIANT : PLANET_FROZEN;
    else                              p.type = PLANET_FROZEN;

    // ---- Mass / radius by type (Earth units) ----
    switch (p.type) {
        case PLANET_LAVA:      p.mass_earth = galaxy_rng_range(&r, 0.2f, 2.0f);   break;
        case PLANET_ROCKY:     p.mass_earth = galaxy_rng_range(&r, 0.1f, 1.5f);   break;
        case PLANET_DESERT:    p.mass_earth = galaxy_rng_range(&r, 0.3f, 2.5f);   break;
        case PLANET_OCEAN:     p.mass_earth = galaxy_rng_range(&r, 0.5f, 3.0f);   break;
        case PLANET_TERRAN:    p.mass_earth = galaxy_rng_range(&r, 0.5f, 2.0f);   break;
        case PLANET_GAS_GIANT: p.mass_earth = galaxy_rng_range(&r, 50.0f, 320.0f);break;
        case PLANET_ICE_GIANT: p.mass_earth = galaxy_rng_range(&r, 8.0f, 30.0f);  break;
        case PLANET_FROZEN:    p.mass_earth = galaxy_rng_range(&r, 0.05f, 1.2f);  break;
        default:               p.mass_earth = 1.0f;                               break;
    }
    if (p.type == PLANET_GAS_GIANT)      p.radius_earth = galaxy_rng_range(&r, 8.0f, 12.5f);
    else if (p.type == PLANET_ICE_GIANT) p.radius_earth = galaxy_rng_range(&r, 3.5f, 5.0f);
    else                                 p.radius_earth = powf(p.mass_earth, 0.27f); // rocky scaling

    // ---- Equilibrium temperature (albedo ~0.3): T = 255 * L^0.25 / sqrt(a_au) ----
    f32 a = orbit_au < 0.01f ? 0.01f : orbit_au;
    p.temperature_k = 255.0f * powf(star.luminosity_solar, 0.25f) / sqrtf(a);

    // ---- Atmosphere + liquid-water habitability heuristic ----
    p.has_atmosphere = (p.type == PLANET_GAS_GIANT || p.type == PLANET_ICE_GIANT ||
                        ((p.type == PLANET_OCEAN || p.type == PLANET_TERRAN || p.type == PLANET_DESERT) && p.mass_earth > 0.3f));
    // Ring systems: common on gas giants, occasional on ice giants.
    p.has_rings = (p.type == PLANET_GAS_GIANT && galaxy_rng_f32(&r) < 0.55f) ||
                  (p.type == PLANET_ICE_GIANT && galaxy_rng_f32(&r) < 0.20f);
    p.habitability = 0.0f;
    if (p.type == PLANET_OCEAN || p.type == PLANET_TERRAN) {
        // Ocean/Terran only spawn inside the habitable zone, so they are habitable by construction;
        // equilibrium temperature (HZ centre ~240 K) and Earth-like mass refine the score.
        f32 temp_score = wg_clamp01(1.0f - fabsf(p.temperature_k - 240.0f) / 70.0f);
        f32 mass_score = wg_clamp01(1.0f - fabsf(p.mass_earth - 1.0f) / 2.5f);
        p.habitability = wg_clamp01(0.35f + 0.40f * temp_score + 0.25f * mass_score);
    }
    // Appearance genome (named subtype, jittered palette, feature genes, rare anomaly) — derived
    // from the same seed so it is deterministic and regenerates lazily with the system.
    p.genome = worldgen_planet_genome(star, p, seed);
    return p;
}

const char* spectral_class_name(SpectralClass c) {
    switch (c) {
        case SPEC_O: return "O"; case SPEC_B: return "B"; case SPEC_A: return "A";
        case SPEC_F: return "F"; case SPEC_G: return "G"; case SPEC_K: return "K";
        case SPEC_M: return "M"; default: return "?";
    }
}

const char* planet_type_name(PlanetType t) {
    switch (t) {
        case PLANET_LAVA:      return "Lava";      case PLANET_ROCKY:     return "Rocky";
        case PLANET_DESERT:    return "Desert";    case PLANET_OCEAN:     return "Ocean";
        case PLANET_TERRAN:    return "Terran";    case PLANET_GAS_GIANT: return "Gas Giant";
        case PLANET_ICE_GIANT: return "Ice Giant"; case PLANET_FROZEN:    return "Frozen";
        default: return "?";
    }
}
// =====================================================================================
// Planet appearance genome: named subtypes, rare anomalies, palette derivation
// =====================================================================================
// Each planet TYPE keeps its structural identity (lava cracks / gas bands / continents), but a
// per-planet genome selects a named SUBTYPE, jitters a 4-stop palette in HSV, and tunes surface-
// feature genes so no two planets of a type look alike. A ~2% mutation pulls the palette toward an
// exotic anomaly tint. Everything is deterministic from a dedicated genome sub-seed.

struct WGSubtype {
    const char* name;
    f32 deep[3], mid[3], light[3], accent[3];   // base 4-stop palette (RGB 0..1)
    f32 noise_freq, warp_amount, feature_density, band_detail, cap_extent, roughness, cloud_cover;
    u32 weight;                                   // base selection weight
    u16 traits;                                   // trait bits implied by this subtype
};

static const WGSubtype WG_SUB_LAVA[] = {
    { "Molten Sea",    {0.20f,0.04f,0.02f},{0.75f,0.18f,0.05f},{1.00f,0.55f,0.15f},{1.00f,0.80f,0.30f}, 1.1f,0.8f,0.35f,0.0f,0.0f,0.5f,0.0f, 100, TRAIT_VOLCANIC },
    { "Cooling Crust", {0.06f,0.04f,0.04f},{0.22f,0.14f,0.12f},{0.55f,0.30f,0.16f},{1.00f,0.45f,0.10f}, 1.3f,0.7f,0.55f,0.0f,0.0f,0.7f,0.0f, 100, TRAIT_VOLCANIC },
    { "Sulfur World",  {0.25f,0.16f,0.03f},{0.66f,0.50f,0.10f},{0.92f,0.80f,0.30f},{1.00f,0.55f,0.12f}, 1.0f,0.6f,0.40f,0.0f,0.0f,0.5f,0.0f,  70, (u16)(TRAIT_VOLCANIC|TRAIT_ARID) },
    { "Obsidian",      {0.03f,0.03f,0.05f},{0.10f,0.09f,0.12f},{0.28f,0.24f,0.30f},{0.90f,0.30f,0.15f}, 1.4f,0.5f,0.45f,0.0f,0.0f,0.8f,0.0f,  55, TRAIT_VOLCANIC },
};
static const WGSubtype WG_SUB_ROCKY[] = {
    { "Cratered",  {0.18f,0.17f,0.16f},{0.42f,0.40f,0.38f},{0.70f,0.68f,0.64f},{0.55f,0.52f,0.48f}, 1.3f,0.5f,0.80f,0.0f,0.0f,0.7f,0.0f, 100, TRAIT_CRATERED },
    { "Ferrous",   {0.18f,0.09f,0.06f},{0.46f,0.24f,0.15f},{0.72f,0.45f,0.30f},{0.60f,0.32f,0.20f}, 1.2f,0.6f,0.60f,0.0f,0.0f,0.6f,0.0f,  85, (u16)(TRAIT_METALLIC|TRAIT_CRATERED) },
    { "Basaltic",  {0.10f,0.10f,0.12f},{0.26f,0.26f,0.30f},{0.48f,0.48f,0.54f},{0.40f,0.40f,0.46f}, 1.4f,0.5f,0.55f,0.0f,0.0f,0.75f,0.0f, 80, TRAIT_CRATERED },
    { "Regolith",  {0.30f,0.28f,0.24f},{0.55f,0.51f,0.44f},{0.82f,0.78f,0.68f},{0.66f,0.62f,0.52f}, 1.1f,0.5f,0.45f,0.0f,0.0f,0.5f,0.0f,  70, (u16)(TRAIT_ARID|TRAIT_CRATERED) },
};
static const WGSubtype WG_SUB_DESERT[] = {
    { "Dune Sea",    {0.42f,0.30f,0.12f},{0.78f,0.62f,0.34f},{0.94f,0.84f,0.58f},{0.85f,0.70f,0.40f}, 1.0f,0.6f,0.35f,0.0f,0.06f,0.45f,0.05f, 100, TRAIT_ARID },
    { "Rust Desert", {0.32f,0.14f,0.08f},{0.66f,0.34f,0.20f},{0.86f,0.56f,0.38f},{0.72f,0.38f,0.24f}, 1.1f,0.65f,0.45f,0.0f,0.05f,0.55f,0.05f, 90, (u16)(TRAIT_ARID|TRAIT_METALLIC) },
    { "Salt Flats",  {0.55f,0.52f,0.46f},{0.82f,0.79f,0.72f},{0.96f,0.95f,0.90f},{0.80f,0.76f,0.66f}, 0.9f,0.5f,0.30f,0.0f,0.08f,0.35f,0.05f, 60, TRAIT_ARID },
    { "Rocky Waste", {0.28f,0.24f,0.18f},{0.52f,0.46f,0.36f},{0.76f,0.70f,0.58f},{0.60f,0.52f,0.40f}, 1.2f,0.55f,0.55f,0.0f,0.06f,0.6f,0.05f, 75, (u16)(TRAIT_ARID|TRAIT_CRATERED) },
};
static const WGSubtype WG_SUB_OCEAN[] = {
    { "Blue Marble",  {0.02f,0.09f,0.28f},{0.05f,0.20f,0.50f},{0.20f,0.45f,0.75f},{0.60f,0.80f,0.95f}, 1.0f,0.85f,0.45f,0.0f,0.30f,0.5f,0.55f, 100, (u16)(TRAIT_OCEANIC|TRAIT_CLOUDY) },
    { "Tropical",     {0.02f,0.14f,0.28f},{0.06f,0.34f,0.48f},{0.20f,0.62f,0.68f},{0.55f,0.88f,0.85f}, 1.0f,0.9f,0.50f,0.0f,0.18f,0.5f,0.60f,  80, (u16)(TRAIT_OCEANIC|TRAIT_CLOUDY) },
    { "Storm World",  {0.03f,0.07f,0.16f},{0.10f,0.18f,0.34f},{0.30f,0.40f,0.55f},{0.70f,0.78f,0.86f}, 1.1f,0.9f,0.55f,0.0f,0.25f,0.6f,0.80f,  65, (u16)(TRAIT_OCEANIC|TRAIT_STORMY|TRAIT_CLOUDY) },
    { "Frozen Ocean", {0.06f,0.16f,0.30f},{0.20f,0.40f,0.58f},{0.60f,0.76f,0.88f},{0.85f,0.93f,1.00f}, 1.0f,0.8f,0.40f,0.0f,0.55f,0.5f,0.45f,  55, (u16)(TRAIT_OCEANIC|TRAIT_ICY_CAPS) },
};
static const WGSubtype WG_SUB_TERRAN[] = {
    { "Verdant", {0.03f,0.14f,0.34f},{0.20f,0.42f,0.20f},{0.45f,0.62f,0.32f},{0.70f,0.80f,0.55f}, 1.0f,0.85f,0.50f,0.0f,0.22f,0.5f,0.50f, 100, (u16)(TRAIT_VERDANT|TRAIT_OCEANIC|TRAIT_CLOUDY) },
    { "Steppe",  {0.05f,0.16f,0.32f},{0.42f,0.44f,0.22f},{0.66f,0.62f,0.36f},{0.78f,0.74f,0.50f}, 1.0f,0.8f,0.45f,0.0f,0.20f,0.5f,0.40f,  85, (u16)(TRAIT_VERDANT|TRAIT_ARID) },
    { "Jungle",  {0.02f,0.12f,0.28f},{0.10f,0.34f,0.14f},{0.28f,0.52f,0.24f},{0.55f,0.72f,0.40f}, 1.1f,0.9f,0.55f,0.0f,0.16f,0.55f,0.65f, 70, (u16)(TRAIT_VERDANT|TRAIT_OCEANIC|TRAIT_CLOUDY) },
    { "Tundra",  {0.06f,0.16f,0.30f},{0.34f,0.42f,0.34f},{0.60f,0.66f,0.58f},{0.85f,0.90f,0.92f}, 1.0f,0.8f,0.45f,0.0f,0.50f,0.5f,0.50f,  65, (u16)(TRAIT_VERDANT|TRAIT_ICY_CAPS) },
};
static const WGSubtype WG_SUB_GAS[] = {
    { "Jovian",       {0.40f,0.28f,0.18f},{0.72f,0.58f,0.42f},{0.90f,0.82f,0.66f},{0.96f,0.62f,0.42f}, 1.0f,1.2f,0.5f,0.60f,0.0f,0.5f,0.0f, 100, (u16)(TRAIT_BANDED|TRAIT_STORMY) },
    { "Amber Giant",  {0.42f,0.24f,0.08f},{0.80f,0.52f,0.18f},{0.95f,0.78f,0.44f},{1.00f,0.70f,0.30f}, 1.0f,1.2f,0.5f,0.55f,0.0f,0.5f,0.0f,  80, TRAIT_BANDED },
    { "Banded Cream", {0.55f,0.48f,0.36f},{0.80f,0.74f,0.60f},{0.95f,0.92f,0.82f},{0.88f,0.82f,0.66f}, 1.0f,1.1f,0.4f,0.70f,0.0f,0.4f,0.0f,  70, TRAIT_BANDED },
    { "Storm Belt",   {0.34f,0.18f,0.14f},{0.62f,0.36f,0.28f},{0.84f,0.62f,0.48f},{1.00f,0.50f,0.34f}, 1.1f,1.3f,0.7f,0.55f,0.0f,0.6f,0.0f,  60, (u16)(TRAIT_BANDED|TRAIT_STORMY) },
};
static const WGSubtype WG_SUB_ICE[] = {
    { "Neptunian",   {0.05f,0.12f,0.34f},{0.14f,0.30f,0.60f},{0.45f,0.62f,0.85f},{0.80f,0.90f,1.00f}, 1.0f,1.1f,0.4f,0.55f,0.0f,0.5f,0.0f, 100, TRAIT_BANDED },
    { "Cyan Giant",  {0.05f,0.20f,0.30f},{0.12f,0.44f,0.52f},{0.40f,0.72f,0.78f},{0.75f,0.95f,0.98f}, 1.0f,1.1f,0.4f,0.55f,0.0f,0.5f,0.0f,  85, TRAIT_BANDED },
    { "Pale Azure",  {0.18f,0.30f,0.44f},{0.40f,0.56f,0.72f},{0.68f,0.80f,0.92f},{0.88f,0.94f,1.00f}, 1.0f,1.0f,0.35f,0.60f,0.0f,0.4f,0.0f,  70, TRAIT_BANDED },
    { "Methane Blue",{0.04f,0.16f,0.28f},{0.10f,0.38f,0.48f},{0.34f,0.64f,0.70f},{0.70f,0.92f,0.90f}, 1.0f,1.15f,0.45f,0.55f,0.0f,0.55f,0.0f, 60, (u16)(TRAIT_BANDED|TRAIT_STORMY) },
};
static const WGSubtype WG_SUB_FROZEN[] = {
    { "Ice Ball",       {0.55f,0.66f,0.80f},{0.75f,0.84f,0.94f},{0.92f,0.96f,1.00f},{0.80f,0.90f,1.00f}, 1.2f,0.5f,0.4f,0.0f,0.5f,0.5f,0.0f, 100, TRAIT_ICY_CAPS },
    { "Nitrogen Frost", {0.60f,0.58f,0.66f},{0.82f,0.78f,0.82f},{0.96f,0.92f,0.94f},{0.95f,0.85f,0.88f}, 1.1f,0.5f,0.35f,0.0f,0.6f,0.4f,0.0f,  75, TRAIT_ICY_CAPS },
    { "Dirty Ice",      {0.30f,0.30f,0.34f},{0.52f,0.52f,0.56f},{0.74f,0.76f,0.80f},{0.60f,0.58f,0.58f}, 1.3f,0.55f,0.6f,0.0f,0.4f,0.65f,0.0f,  70, (u16)(TRAIT_ICY_CAPS|TRAIT_CRATERED) },
    { "Glacier World",  {0.48f,0.60f,0.74f},{0.70f,0.80f,0.90f},{0.90f,0.95f,1.00f},{0.75f,0.88f,1.00f}, 1.4f,0.6f,0.55f,0.0f,0.55f,0.7f,0.0f,  60, TRAIT_ICY_CAPS },
};

struct WGSubtypeTable { const WGSubtype* items; i32 count; };
#define WG_TBL(a) { a, (i32)(sizeof(a) / sizeof((a)[0])) }
static const WGSubtypeTable WG_SUBTYPES[PLANET_TYPE_COUNT] = {
    WG_TBL(WG_SUB_LAVA),   WG_TBL(WG_SUB_ROCKY),  WG_TBL(WG_SUB_DESERT), WG_TBL(WG_SUB_OCEAN),
    WG_TBL(WG_SUB_TERRAN), WG_TBL(WG_SUB_GAS),    WG_TBL(WG_SUB_ICE),    WG_TBL(WG_SUB_FROZEN),
};
#undef WG_TBL

// Rare-mutation palette overrides. Index 0 is a "none" sentinel; genomes store 1..N.
struct WGAnomaly { const char* name; f32 tint[3]; f32 strength; u16 traits; };
static const WGAnomaly WG_ANOMALIES[] = {
    { "None",          {0.0f, 0.0f, 0.0f},  0.0f,  0 },
    { "Viridian",      {0.20f, 0.85f, 0.35f}, 0.55f, TRAIT_EXOTIC },
    { "Amethyst",      {0.62f, 0.30f, 0.88f}, 0.55f, TRAIT_EXOTIC },
    { "Crimson",       {0.90f, 0.12f, 0.18f}, 0.55f, TRAIT_EXOTIC },
    { "Aurelian Gold", {0.95f, 0.78f, 0.20f}, 0.55f, TRAIT_EXOTIC },
    { "Void-Touched",  {0.06f, 0.05f, 0.10f}, 0.70f, TRAIT_EXOTIC },
    { "Rose Quartz",   {0.98f, 0.62f, 0.74f}, 0.55f, TRAIT_EXOTIC },
    { "Verdigris",     {0.30f, 0.75f, 0.68f}, 0.55f, TRAIT_EXOTIC },
};
static const i32 WG_ANOMALY_COUNT = (i32)(sizeof(WG_ANOMALIES) / sizeof(WG_ANOMALIES[0]));

// ---- HSV <-> RGB (used to jitter palettes coherently in hue/saturation/value) ----
static void wg_rgb_to_hsv(f32 r, f32 g, f32 b, f32* h, f32* s, f32* v) {
    f32 mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = (mx <= 1e-5f) ? 0.0f : d / mx;
    if (d <= 1e-5f) { *h = 0.0f; return; }
    f32 hh;
    if (mx == r)      hh = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) hh = (b - r) / d + 2.0f;
    else              hh = (r - g) / d + 4.0f;
    *h = hh / 6.0f;
}
static void wg_hsv_to_rgb(f32 h, f32 s, f32 v, f32* r, f32* g, f32* b) {
    h -= floorf(h);
    f32 i = floorf(h * 6.0f);
    f32 f = h * 6.0f - i;
    f32 p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
    switch (((i32)i) % 6) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default:*r = v; *g = p; *b = q; break;
    }
}
static bs_color wg_palette_stop(const f32 base[3], f32 hue_shift, f32 sat_mul, f32 val_mul, GalaxyRng* r) {
    f32 h, s, v; wg_rgb_to_hsv(base[0], base[1], base[2], &h, &s, &v);
    h += hue_shift + (galaxy_rng_f32(r) - 0.5f) * 0.03f;
    s = wg_clamp01(s * sat_mul * (0.90f + 0.20f * galaxy_rng_f32(r)));
    v = wg_clamp01(v * val_mul * (0.90f + 0.20f * galaxy_rng_f32(r)));
    f32 cr, cg, cb; wg_hsv_to_rgb(h, s, v, &cr, &cg, &cb);
    return bs_color{ cr, cg, cb, 1.0f };
}
static bs_color wg_blend_toward(bs_color c, const f32 tint[3], f32 t) {
    return bs_color{ c.r + (tint[0] - c.r) * t, c.g + (tint[1] - c.g) * t, c.b + (tint[2] - c.b) * t, 1.0f };
}
// Physics-driven multiplier on a subtype's base selection weight so a planet's look is partly
// "earned" from its physical properties (hot -> volcanic, cold -> icy, old -> cratered, etc.).
static f32 wg_subtype_physics_weight(const WGSubtype& st, const StarProperties& star, const PlanetProperties& p) {
    f32 w = 1.0f;
    f32 age_norm = wg_clamp01(star.age_gyr / 12.0f);
    f32 cold = wg_clamp01((300.0f - p.temperature_k) / 200.0f);
    f32 hot  = wg_clamp01((p.temperature_k - 400.0f) / 600.0f);
    f32 metal = wg_clamp01((star.metallicity - 0.5f) / 2.0f);
    if (st.traits & TRAIT_ICY_CAPS) w *= 0.6f + 1.2f * cold;
    if (st.traits & TRAIT_VOLCANIC) w *= 0.6f + 1.2f * hot;
    if (st.traits & TRAIT_METALLIC) w *= 0.6f + 1.0f * metal;
    if (st.traits & TRAIT_CRATERED) w *= 0.7f + 0.8f * age_norm;
    if (st.traits & TRAIT_VERDANT)  w *= 0.3f + 1.7f * p.life;                        // living looks need an actual biosphere
    if (st.traits & TRAIT_OCEANIC)  w *= 0.25f + 1.5f * wg_clamp01(p.water_frac / 0.6f); // sea-covered looks need actual water
    if (st.traits & TRAIT_ARID)     w *= 1.3f - p.water_frac;                          // desert looks fade as water rises
    return w;
}

PlanetGenome worldgen_planet_genome(const StarProperties& star, const PlanetProperties& planet, u64 seed) {
    GalaxyRng r = galaxy_rng_seed(seed ^ 0x51ED270B9C1E7A3Dull);   // dedicated genome sub-seed
    PlanetGenome g{};
    const WGSubtypeTable& tbl = WG_SUBTYPES[(i32)planet.type];

    // ---- (1) physics-biased weighted subtype roll ----
    f32 wv[8]; f32 wsum = 0.0f;
    for (i32 i = 0; i < tbl.count; ++i) {
        f32 w = (f32)tbl.items[i].weight * wg_subtype_physics_weight(tbl.items[i], star, planet);
        wv[i] = w; wsum += w;
    }
    i32 sub = 0;
    { f32 pick = galaxy_rng_f32(&r) * wsum, acc = 0.0f;
      for (i32 i = 0; i < tbl.count; ++i) { acc += wv[i]; if (pick < acc) { sub = i; break; } } }
    const WGSubtype& st = tbl.items[sub];
    g.subtype    = (u8)sub;
    g.trait_bits = st.traits;

    // ---- (2) palette: base stops perturbed in HSV (coherent hue shift + per-stop jitter) ----
    f32 hue_shift = (galaxy_rng_f32(&r) - 0.5f) * 0.10f;   // one coherent shift for the whole palette
    f32 age_norm  = wg_clamp01(star.age_gyr / 12.0f);
    f32 sat_mul   = 1.0f - 0.20f * age_norm;               // older worlds weather / desaturate
    f32 val_mul   = 1.0f;
    if (planet.type == PLANET_LAVA) {                      // hotter lava -> whiter, brighter
        f32 heat = wg_clamp01((planet.temperature_k - 700.0f) / 800.0f);
        val_mul *= 1.0f + 0.25f * heat; sat_mul *= 1.0f - 0.25f * heat;
    }
    g.pal_deep   = wg_palette_stop(st.deep,   hue_shift, sat_mul, val_mul, &r);
    g.pal_mid    = wg_palette_stop(st.mid,    hue_shift, sat_mul, val_mul, &r);
    g.pal_light  = wg_palette_stop(st.light,  hue_shift, sat_mul, val_mul, &r);
    g.pal_accent = wg_palette_stop(st.accent, hue_shift, sat_mul, val_mul, &r);

    // ---- (3) feature genes around the subtype centres + physics nudges ----
    f32 cold      = wg_clamp01((300.0f - planet.temperature_k) / 200.0f);
    f32 old_solid = (planet.type == PLANET_ROCKY || planet.type == PLANET_FROZEN) ? age_norm : 0.0f;
    g.noise_freq      = st.noise_freq  * galaxy_rng_range(&r, 0.82f, 1.20f);
    g.warp_amount     = st.warp_amount * galaxy_rng_range(&r, 0.80f, 1.25f);
    g.feature_density = wg_clamp01(st.feature_density + (galaxy_rng_f32(&r) - 0.5f) * 0.30f + 0.20f * old_solid);
    g.band_detail     = wg_clamp01(st.band_detail + (galaxy_rng_f32(&r) - 0.5f) * 0.25f);
    g.cap_extent      = wg_clamp01(st.cap_extent + (galaxy_rng_f32(&r) - 0.5f) * 0.20f + (st.cap_extent > 0.01f ? 0.30f * cold : 0.0f));
    g.roughness       = wg_clamp01(st.roughness + (galaxy_rng_f32(&r) - 0.5f) * 0.25f - 0.15f * wg_clamp01((planet.mass_earth - 1.0f) / 4.0f));
    g.cloud_cover     = planet.has_atmosphere ? wg_clamp01(st.cloud_cover + (galaxy_rng_f32(&r) - 0.5f) * 0.25f) : 0.0f;
    if (g.cloud_cover > 0.20f) g.trait_bits |= TRAIT_CLOUDY;

    // Cloud + atmosphere tints: near-white clouds with a faint bias, atmosphere from the accent.
    static const f32 atmo_bias[3] = { 0.55f, 0.70f, 1.00f };
    f32 ct = 0.90f + 0.10f * galaxy_rng_f32(&r);
    g.cloud_tint = bs_color{ ct, ct, wg_clamp01(ct + 0.04f), 1.0f };
    g.atmo_tint  = wg_blend_toward(g.pal_accent, atmo_bias, 0.35f);

    // ---- (4) rare mutation: pull the palette toward an exotic anomaly tint (~2%) ----
    g.anomaly = 0;
    if (galaxy_rng_f32(&r) < 0.02f) {
        i32 a = 1 + (i32)(galaxy_rng_f32(&r) * (f32)(WG_ANOMALY_COUNT - 1));
        if (a >= WG_ANOMALY_COUNT) a = WG_ANOMALY_COUNT - 1;
        const WGAnomaly& an = WG_ANOMALIES[a];
        g.anomaly     = (u8)a;
        g.trait_bits |= an.traits;
        g.pal_deep    = wg_blend_toward(g.pal_deep,   an.tint, an.strength * 0.7f);
        g.pal_mid     = wg_blend_toward(g.pal_mid,    an.tint, an.strength);
        g.pal_light   = wg_blend_toward(g.pal_light,  an.tint, an.strength * 0.8f);
        g.pal_accent  = wg_blend_toward(g.pal_accent, an.tint, an.strength);
    }
    return g;
}

const char* planet_subtype_name(PlanetType type, u8 subtype) {
    if ((i32)type < 0 || (i32)type >= PLANET_TYPE_COUNT) return "?";
    const WGSubtypeTable& t = WG_SUBTYPES[(i32)type];
    if ((i32)subtype >= t.count) return "?";
    return t.items[subtype].name;
}
const char* planet_anomaly_name(u8 anomaly) {
    if (anomaly == 0 || (i32)anomaly >= WG_ANOMALY_COUNT) return "";
    return WG_ANOMALIES[anomaly].name;
}
i32 planet_trait_names(u16 trait_bits, const char** out, i32 max) {
    static const struct { u16 bit; const char* name; } T[] = {
        { TRAIT_VOLCANIC, "Volcanic" }, { TRAIT_CRATERED, "Cratered" }, { TRAIT_BANDED, "Banded" },
        { TRAIT_STORMY, "Stormy" },     { TRAIT_ICY_CAPS, "Icy Caps" }, { TRAIT_OCEANIC, "Oceanic" },
        { TRAIT_CLOUDY, "Cloudy" },     { TRAIT_ARID, "Arid" },         { TRAIT_METALLIC, "Metallic" },
        { TRAIT_VERDANT, "Verdant" },   { TRAIT_EXOTIC, "Exotic" },
    };
    i32 n = 0;
    for (i32 i = 0; i < (i32)(sizeof(T) / sizeof(T[0])) && n < max; ++i)
        if (trait_bits & T[i].bit) out[n++] = T[i].name;
    return n;
}
// =====================================================================================
f32 solve_eccentric_anomaly(f32 M, f32 e)
{
    // Normalize M to [0, 2π)
    f32 two_pi = 2.0f * BS_PI;
    M = fmodf(M, two_pi);
    if (M < 0.0f) M += two_pi;
    // Initial guess: E ≈ M + e * sin(M)  (good for e < 0.3)
    f32 E = M + e * sinf(M);
    // Newton-Raphson: E_{n+1} = E_n - (E_n - e*sin(E_n) - M) / (1 - e*cos(E_n))
    for (i32 iter = 0; iter < 8; ++iter) {
        f32 sinE = sinf(E);
        f32 cosE = cosf(E);
        f32 f  = E - e * sinE - M;
        f32 fp = 1.0f - e * cosE;
        f32 dE = f / fp;
        E -= dE;
        if (fabsf(dE) < 1e-6f) break;
    }
    return E;
}
// =====================================================================================
void generate_star_system(StarSystem* sys, u64 seed, Vec2 galaxy_pos,
                            SSGenEnv env, const SSGenConfig& cfg)
{
    rng_seed(seed);
    // ---- Galaxy position (already computed by caller, just store it) ----
    sys->galaxy_center = hierpos_from_vec2(galaxy_pos, BS_HIERPOS_CELL_SIZE);

    // ---- Star: roll physical properties, derive the render body from them ----
    StarProperties sp = worldgen_star(galaxy_splitmix64(seed ^ 0xA24BAED4963EE407ull), env);
    sys->star_props = sp;
    f32 star_r = clampf(850.0f * powf(sp.radius_solar, 0.40f), 500.0f, 2600.0f); // compress giants
    sys->star = CelestialBody{ Vec2{ 0.0f, 0.0f }, star_r, blackbody_color(sp.temperature_k),
                                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    sys->star_pulse_phase   = rng_f32() * 2.0f * BS_PI;
    sys->corona_pulse_phase = rng_f32() * 2.0f * BS_PI;
    sys->halo_pulse_phase   = rng_f32() * 2.0f * BS_PI;
    sys->system_scale = rng_range(cfg.scale_min, cfg.scale_max);

    // ---- Evolve the system (four-phase epoch pipeline; the authoritative model) ----
    evolve_star_system(&sys->evo, sp, seed);
    sys->planet_count = sys->evo.planet_count;

    // ---- Derive planet render views: log-map evolved AU orbits into world units ----
    // Physical orbits live in AU on the evolved bodies; render orbits stay bounded in world
    // units so systems fit the galaxy spacing. Inverse of the old world->AU log-map.
    f32 au_lo = sys->evo.bodies[1].orbit_au;
    f32 au_hi = sys->evo.bodies[sys->planet_count].orbit_au;
    if (au_hi <= au_lo * 1.0001f) au_hi = au_lo * 1.5f;
    f32 world_a0   = rng_range(cfg.inner_a_min, cfg.inner_a_max) * sys->system_scale;
    f32 world_span = powf(rng_range(1.55f, 1.85f), (f32)(sys->planet_count - 1));
    f32 prev_world_a = 0.0f, prev_e = 0.0f;
    for (i32 i = 0; i < sys->planet_count; ++i) {
        const EvolvedBody* b = &sys->evo.bodies[1 + i];
        f32 t = logf(b->orbit_au / au_lo) / logf(au_hi / au_lo);
        f32 a = world_a0 * powf(world_span, t);
        f32 e = clampf(b->eccentricity, 0.0f, (i < sys->planet_count / 2) ? cfg.e_inner_max
                                                                          : cfg.e_outer_max);
        // Non-intersection guarantee: apoapsis(prev) + margin < periapsis(this).
        if (i > 0 && prev_world_a * (1.0f + prev_e) + cfg.safety_margin >= a * (1.0f - e)) {
            a = (prev_world_a * (1.0f + prev_e) + cfg.safety_margin) / (1.0f - e) * 1.08f;
        }
        prev_world_a = a; prev_e = e;
        f32 w  = rng_f32() * 2.0f * BS_PI;
        f32 M0 = rng_f32() * 2.0f * BS_PI;

        // Physical properties view from the evolved body; genome rolled per planet.
        PlanetProperties pp{};
        pp.type           = b->type;
        pp.orbit_au       = b->orbit_au;
        pp.mass_earth     = b->mass_earth;
        pp.radius_earth   = b->radius_earth;
        pp.temperature_k  = b->temperature_k;
        pp.habitability   = b->habitability;
        pp.water_frac     = b->water_frac;
        pp.life           = b->life;
        pp.has_atmosphere = b->atmo_pressure > 0.05f;
        pp.has_rings      = b->comp.gas > 0.35f && rng_f32() < 0.45f;
        pp.genome = worldgen_planet_genome(sp, pp,
                        galaxy_splitmix64(seed ^ (0xC2B2AE3D27D4EB4Full * (u64)(i + 1))));
        // Physical-claim traits are authoritative from the evolved state: clear whatever the
        // visual archetype baked in and re-derive each from history. Purely visual bits
        // (CLOUDY/STORMY/BANDED/EXOTIC) stay with the archetype/anomaly.
        const u16 PHYS_TRAITS = (u16)(TRAIT_OCEANIC | TRAIT_VERDANT | TRAIT_ARID | TRAIT_ICY_CAPS
                                    | TRAIT_VOLCANIC | TRAIT_METALLIC | TRAIT_CRATERED);
        pp.genome.trait_bits &= (u16)~PHYS_TRAITS;
        b8 solid = b->comp.gas <= 0.35f;
        if (solid && b->water_frac > 0.55f) pp.genome.trait_bits |= TRAIT_OCEANIC;
        if (solid && b->water_frac < 0.12f && b->temperature_k > 250.0f
                  && b->atmo_pressure > 0.05f) pp.genome.trait_bits |= TRAIT_ARID;
        if (solid && b->water_frac > 0.05f && b->temperature_k > 185.0f
                  && b->temperature_k < 285.0f) pp.genome.trait_bits |= TRAIT_ICY_CAPS;
        if (b->volcanism  > 0.65f) pp.genome.trait_bits |= TRAIT_VOLCANIC;
        if (b->life       > 0.50f) pp.genome.trait_bits |= TRAIT_VERDANT;
        if (solid && b->comp.metal > 0.40f) pp.genome.trait_bits |= TRAIT_METALLIC;
        if (b->env_hazard > 0.60f && b->atmo_pressure < 0.05f) pp.genome.trait_bits |= TRAIT_CRATERED;
        if (b->atmo_pressure > 1.5f && b->comp.gas < 0.35f) pp.genome.trait_bits |= TRAIT_CLOUDY;
        sys->planet_props[i] = pp;

        // Render color from planet type (slight per-planet jitter) + radius from physical size.
        bs_color pcol = wg_planet_base_color(pp.type);
        f32 jitter = 0.9f + 0.2f * rng_f32();
        pcol.r = clampf(pcol.r * jitter, 0.0f, 1.0f);
        pcol.g = clampf(pcol.g * jitter, 0.0f, 1.0f);
        pcol.b = clampf(pcol.b * jitter, 0.0f, 1.0f);
        f32 planet_r = clampf(180.0f * powf(pp.radius_earth, 0.33f), 150.0f, 900.0f);
        // Initial position from eccentric anomaly, rotated by arg of periapsis.
        f32 E0 = solve_eccentric_anomaly(M0, e);
        f32 pos_x = a * (cosf(E0) - e);
        f32 pos_y = a * sqrtf(1.0f - e * e) * sinf(E0);
        f32 cw = cosf(w), sw = sinf(w);
        Vec2 ppos = Vec2{ cw * pos_x - sw * pos_y, sw * pos_x + cw * pos_y };
        // Orbital angular speed (rad/s). Slow, stately drift so the system reads as realistic
        // rather than whizzing around; inner worlds still move a touch faster than outer ones.
        f32 speed  = 0.006f / sqrtf(a / 1000000.0f);
        f32 period = 2.0f * BS_PI / speed;
        sys->planets[i] = CelestialBody{ ppos, planet_r, pcol, a, speed, M0, a, e, w, M0, period };
    }

    // Clear any unused planet slots.
    for (i32 i = sys->planet_count; i < MAX_SYSTEM_PLANETS; ++i) {
        sys->planets[i] = CelestialBody{};
        sys->planet_props[i] = PlanetProperties{};
    }

    // ---- Derive moon render views: small bodies circling their parent planet ----
    // Moon world orbits are presentational (parent render radius multiples), not scaled AU:
    // real Hill-sphere distances would be sub-pixel at system zoom.
    sys->moon_count = sys->evo.moon_count;
    i32 moon_base = 1 + sys->planet_count;
    for (i32 m = 0; m < sys->moon_count; ++m) {
        const EvolvedBody* mb = &sys->evo.bodies[moon_base + m];
        i32 pi = (i32)mb->parent - 1; // bodies[] parent -> planets[] index
        if (pi < 0 || pi >= sys->planet_count) { sys->moons[m] = CelestialBody{}; continue; }
        const CelestialBody& parent = sys->planets[pi];
        // Stack multiple moons of one parent outward by their order in the moon block.
        i32 rank = 0;
        for (i32 k = 0; k < m; ++k)
            if (sys->evo.bodies[moon_base + k].parent == mb->parent) ++rank;
        f32 orbit_r = parent.radius * (2.6f + 1.4f * (f32)rank + rng_f32() * 0.8f);
        f32 moon_r  = clampf(90.0f * powf(mb->radius_earth, 0.33f), 55.0f, 240.0f);
        f32 ang     = rng_f32() * 2.0f * BS_PI;
        f32 speed   = (0.03f + rng_f32() * 0.03f) * (rank % 2 == 0 ? 1.0f : -1.0f);
        bs_color mcol = mb->type == PLANET_FROZEN ? bs_color{ 0.72f, 0.78f, 0.85f, 1.0f }
                                                  : bs_color{ 0.58f, 0.55f, 0.52f, 1.0f };
        Vec2 mpos = vec2_add(parent.position, Vec2{ cosf(ang) * orbit_r, sinf(ang) * orbit_r });
        // semi_major_axis stores the parent-relative orbit radius; orbit_angle the phase.
        sys->moons[m] = CelestialBody{ mpos, moon_r, mcol, orbit_r, speed, ang,
                                       orbit_r, 0.0f, 0.0f, ang, 2.0f * BS_PI / fabsf(speed) };
        // Physical props view (drives draw_planet_3d shading; moons reuse rocky/frozen params).
        PlanetProperties mp{};
        mp.type           = mb->type == PLANET_FROZEN ? PLANET_FROZEN : PLANET_ROCKY;
        mp.orbit_au       = mb->orbit_au;
        mp.mass_earth     = mb->mass_earth;
        mp.radius_earth   = mb->radius_earth;
        mp.temperature_k  = mb->temperature_k;
        mp.habitability   = mb->habitability;
        mp.water_frac     = mb->water_frac;
        mp.life           = mb->life;
        mp.has_atmosphere = mb->atmo_pressure > 0.05f;
        mp.has_rings      = FALSE;
        mp.genome = worldgen_planet_genome(sp, mp,
                        galaxy_splitmix64(seed ^ (0x8AE8F87FDE30A967ull * (u64)(m + 1))));
        // Same physical-trait reconciliation as planets (moons mostly clear the mask).
        const u16 PHYS_TRAITS = (u16)(TRAIT_OCEANIC | TRAIT_VERDANT | TRAIT_ARID | TRAIT_ICY_CAPS
                                    | TRAIT_VOLCANIC | TRAIT_METALLIC | TRAIT_CRATERED);
        mp.genome.trait_bits &= (u16)~PHYS_TRAITS;
        if (mb->volcanism > 0.5f) mp.genome.trait_bits |= TRAIT_VOLCANIC; // tidal-heated moons glow
        if (mb->water_frac > 0.55f) mp.genome.trait_bits |= TRAIT_OCEANIC;
        if (mb->comp.metal > 0.40f) mp.genome.trait_bits |= TRAIT_METALLIC;
        if (mb->env_hazard > 0.60f && mb->atmo_pressure < 0.05f) mp.genome.trait_bits |= TRAIT_CRATERED;
        sys->moon_props[m] = mp;
    }
    for (i32 m = sys->moon_count; m < MAX_SYSTEM_MOONS; ++m) {
        sys->moons[m] = CelestialBody{};
        sys->moon_props[m] = PlanetProperties{};
    }
}
// =====================================================================================
void update_planet_positions(StarSystem* sys, f32 dt)
{
    for (i32 i = 0; i < sys->planet_count; ++i) {
        CelestialBody& p = sys->planets[i];
        // orbit_angle accumulates the mean anomaly; Kepler-solve for the true position.
        p.orbit_angle += p.orbit_speed * dt;
        f32 E = solve_eccentric_anomaly(p.orbit_angle, p.eccentricity);
        // Position in orbital plane (periapsis on +x axis)
        f32 x = p.semi_major_axis * (cosf(E) - p.eccentricity);
        f32 y = p.semi_major_axis * sqrtf(1.0f - p.eccentricity * p.eccentricity) * sinf(E);
        // Rotate by arg_periapsis
        f32 cw = cosf(p.arg_periapsis);
        f32 sw = sinf(p.arg_periapsis);
        p.position.x = sys->star.position.x + cw * x - sw * y;
        p.position.y = sys->star.position.y + sw * x + cw * y;
    }
    // Moons ride their parent planet on cheap circular orbits (parents updated above).
    i32 moon_base = 1 + sys->planet_count;
    for (i32 m = 0; m < sys->moon_count; ++m) {
        CelestialBody& mn = sys->moons[m];
        i32 pi = (i32)sys->evo.bodies[moon_base + m].parent - 1;
        if (pi < 0 || pi >= sys->planet_count) continue;
        mn.orbit_angle += mn.orbit_speed * dt;
        const CelestialBody& parent = sys->planets[pi];
        mn.position.x = parent.position.x + cosf(mn.orbit_angle) * mn.semi_major_axis;
        mn.position.y = parent.position.y + sinf(mn.orbit_angle) * mn.semi_major_axis;
    }
}
