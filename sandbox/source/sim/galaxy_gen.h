#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>   // bs_math::HierPos2 / Vec2 (galaxy_env_at)
#include "sim/galaxy_params.h" // GalaxyShape / GalaxyGenParams

struct game_state;

// =====================================================================================
// Galaxy generator — places ~10,000 star-system nodes on a gaussian disc, derives each
// node's summary attributes deterministically from its seed, and builds the spatial grid
// plus the travel-lane connectivity graph (kNN -> MST -> add-back).
// =====================================================================================
// Deterministic: the same master seed always yields the same galaxy. Node contents
// (planets, orbits) are NOT stored here — they materialise lazily from node.seed near the
// camera (see galaxy_map.cpp). Allocates node/grid/lane arrays from MEMORY_TAG_GAME.

// Tunables (world units). The disc extent (see the spiral-structure block below) MUST keep typical
// neighbour spacing well above a star system's outer-orbit DIAMETER (~1.8e8 for the biggest
// systems) or neighbouring systems' orbits intersect. A hard minimum separation (blue-noise
// rejection during placement) additionally prevents the rare close pairs the density would produce.
// R_max clamps the disc radius.
static const i32 GALAXY_TARGET_SYSTEMS = 10000;
static const f64 GALAXY_DISC_RMAX      = 4.0e10;
static const f64 GALAXY_MIN_SEPARATION = 2.0e8;   // >= biggest system's outer-orbit diameter
static const f64 GALAXY_GRID_CELL      = 2.5e8;   // ~= typical neighbour spacing (1-2 nodes/cell)
static const i32 GALAXY_KNN_K          = 8;
static const f32 GALAXY_LANE_ADDBACK   = 0.20f;   // fraction of leftover kNN edges kept as loops

// Spiral-structure tunables. place_nodes() builds a compact central bulge + an exponential disc
// whose systems are biased onto GALAXY_ARM_COUNT logarithmic arms (the rest fill the inter-arm
// field). The disc radius is drawn from an exponential surface-density profile (r ~ Gamma(2,r_d));
// arm scatter widens toward the core so the arms dissolve into the bulge. Star COLOUR then follows
// this structure via the per-position SSGenEnv (young/blue in arms, old/gold in the metal-rich core).
static const f64 GALAXY_DISC_SCALE_LEN = 1.0e10;  // exponential disc radial scale length (r_d)
static const i32 GALAXY_ARM_COUNT      = 2;       // grand-design spiral arm count
static const f64 GALAXY_ARM_PITCH_RAD  = 0.45;    // log-spiral pitch angle (~26 deg); smaller = tighter
static const f64 GALAXY_ARM_R0         = 1.0e10;  // reference radius anchoring the arm phase
static const f64 GALAXY_ARM_WIDTH_RAD  = 0.16;    // angular sigma of an arm (crisp grand-design lanes)
static const f64 GALAXY_ARM_CORE_FLARE = 0.8;     // extra arm scatter toward the core (arms dissolve)
static const f32 GALAXY_ARM_FRACTION   = 0.85f;   // fraction of disc systems bound to an arm
static const f32 GALAXY_BULGE_FRACTION = 0.18f;   // fraction of all systems in the central bulge
static const f64 GALAXY_BULGE_SCALE    = 6.0e9;   // bulge Rayleigh sigma (compact spheroidal core)

// Structural "environment" at a galaxy position: galactocentric radius fraction (0 core .. 1 rim),
// spiral-arm/young-region proximity (0 inter-arm .. 1 arm spine), and old-population strength
// (0 young .. 1 old bulge/elliptical). A PURE function of position + the galaxy's GalaxyGenParams,
// so a system's summarised map dot (generated from its true world position) and the system the
// player later flies into (materialised from {0,0} then re-anchored, for precision) derive the SAME
// star population and colour. Consumed by worldgen_star to bias spectral class + metallicity.
struct SSGenEnv {
    f32 r_norm;       // |pos| / disc_rmax, clamped to [0,1]
    f32 arm_strength; // Gaussian proximity to the nearest spiral-arm / young region, [0,1]
    f32 age_bias;     // old-population strength (cool/red + metal-rich boost), [0,1]
};

// Default GalaxyGenParams preset for `shape`, with seed-dependent fields (bar/elliptical
// orientation, etc.) derived from `master_seed`. GX_SPIRAL reproduces the built-in constants.
GalaxyGenParams galaxy_params_for_shape(GalaxyShape shape, u64 master_seed);

// Log-spiral centerline angle (radians) of arm `arm_index` at galactocentric radius `r`.
f64 galaxy_arm_angle(const GalaxyGenParams* gp, f64 r, i32 arm_index);

// Structural environment at a galaxy position (pure; see SSGenEnv).
SSGenEnv galaxy_env_at(const GalaxyGenParams* gp, const bs_math::HierPos2* pos);

// Generate the full galaxy into `s->galaxy` (nodes, grid, lanes) with the given morphology.
// `target_count` is clamped to [1, GALAXY_TARGET_SYSTEMS]. Node 0 is forced to the origin ("Sol")
// so the player start position stays valid.
void galaxy_generate(game_state* s, u64 master_seed, i32 target_count, GalaxyShape shape);

// Free all generator-owned arrays (nodes, grid CSR, lane graph). Safe on a zeroed GalaxyState.
void galaxy_free(game_state* s);
