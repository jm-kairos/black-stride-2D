#pragma once
#include <defines.h>

// =====================================================================================
// Galaxy morphology parameters (shared POD)
// =====================================================================================
// GalaxyShape + GalaxyGenParams live in their own tiny header so both game_state.h (which
// embeds the chosen params in GalaxyState and the shape in GalaxySetupParams) and the galaxy
// generator (galaxy_gen.{h,cpp}) can see them without an include cycle. Pure data, no logic.
//
// The player picks a GalaxyShape on the New Game screen; galaxy_params_for_shape() turns it into
// a concrete GalaxyGenParams preset (some fields randomised from the master seed). place_nodes()
// samples positions from it, and galaxy_env_at() derives each star's population/colour from it.

enum GalaxyShape : u8 {
    GX_SPIRAL = 0,     // grand-design 2-arm spiral (the classic look)
    GX_BARRED,         // central bar with arms springing from its ends
    GX_ELLIPTICAL,     // smooth spheroid, no arms, old/gold population
    GX_RING,           // stars concentrated in an annulus around a small core
    GX_IRREGULAR,      // asymmetric star-forming clumps, no symmetry
    GX_FLOCCULENT,     // many shorter, patchier arms (4-arm)
    GX_SHAPE_COUNT
};

// Concrete generation parameters. Unused fields for a given shape are simply ignored. All lengths
// are world units. A GalaxyGenParams is a pure function of (shape, master_seed).
struct GalaxyGenParams {
    GalaxyShape shape;

    // Disc / overall extent
    f64 disc_scale_len;    // exponential disc radial scale length r_d (also spheroid scale)
    f64 disc_rmax;         // hard radius clamp (also sizes the placement hash grid)

    // Spiral arms (spiral / barred / flocculent)
    i32 arm_count;         // number of logarithmic arms
    f64 arm_pitch_rad;     // log-spiral pitch angle (smaller = tighter winding)
    f64 arm_r0;            // reference radius anchoring the arm phase
    f64 arm_width_rad;     // angular sigma of an arm (also the colour arm-proximity width)
    f64 arm_core_flare;    // extra arm scatter toward the core (arms dissolve into the bulge)
    f32 arm_fraction;      // fraction of disc systems bound to an arm (0 = smooth disc)

    // Central bulge / core
    f32 bulge_fraction;    // fraction of all systems in the central spheroid
    f64 bulge_scale;       // bulge Rayleigh sigma (compact core)

    // Bar (barred spiral)
    f32 bar_fraction;      // fraction of systems in the elongated bar
    f64 bar_length;        // along-bar half-extent
    f32 bar_axis_ratio;    // cross-axis / along-axis (thin bar << 1)

    // Ring
    f64 ring_radius;       // annulus mean radius
    f64 ring_width;        // annulus radial sigma

    // Irregular
    i32 clump_count;       // number of star-forming clumps

    // Elliptical
    f32 ellipticity;       // spheroid minor/major axis ratio (1 = circular)

    // Shared seeded orientation (bar axis / elliptical major axis), radians
    f64 orient_angle;

    // Colouring biases (fed to the star population model via SSGenEnv)
    f32 young_bias;        // global hot/blue floor  (arm_strength floor: irregular/ring high)
    f32 old_bias;          // global cool/old + metal-rich boost (age_bias: elliptical high)
};
