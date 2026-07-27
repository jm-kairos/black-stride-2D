#include "sim/galaxy_gen.h"
#include "game.h"                 // GalaxyState, StarSystem, GalaxyNode
#include "ss_generation.h"        // generate_star_system, SSGEN_DEFAULT
#include "sim/galaxy_spatial.h"
#include "sim/galaxy_rng.h"
#include <core/memory/bs_memory.h>
#include <math.h>
#include <stdlib.h>               // qsort
#include <stdio.h>                // snprintf

using namespace bs_math;

// GalaxyState is a named sub-struct of game_state; alias it so the internal helpers can name it.
using GalaxyState = game_state::GalaxyState;

// Unique catalogue designation "L###L" (e.g. "N327B"). An LCG permutation over the name space
// makes designations look scrambled yet stay collision-free for distinct indices: (i*A+B) mod M
// is a bijection when gcd(A, M) == 1, so no two systems ever share a name (a plain hash would
// collide ~birthday-paradox often). M = 26 * 1000 * 26 = 676,000 designations.
static void galaxy_system_name(i32 index, char* out, i32 out_size) {
    const u32 RANGE = 26u * 1000u * 26u;
    u32 s = ((u32)index * 48271u + 12345u) % RANGE;   // 48271 is coprime to 676000 (2^5*5^3*13^2)
    u32 l2 = s % 26u;   s /= 26u;
    u32 d  = s % 1000u; s /= 1000u;
    u32 l1 = s % 26u;
    snprintf(out, out_size, "%c%03u%c", (char)('A' + (i32)l1), d, (char)('A' + (i32)l2));
}

// =====================================================================================
// Node placement + attributes
// =====================================================================================
static void fill_node_summary(GalaxyNode* node, u64 seed, Vec2 world, SSGenEnv env) {
    // Derive the full system once to snapshot the deterministic summary (colour, radius, orbit
    // radii). The full StarSystem is discarded; the same seed + env re-derives it during lazy
    // materialisation, so the summary here always matches what the player later flies into. The
    // caller sets node->galaxy_center precisely (from the f64 coords) before calling this.
    StarSystem tmp;
    generate_star_system(&tmp, seed, world, env);
    node->seed          = seed;
    node->star_color    = tmp.star.color;
    node->star_radius   = tmp.star.radius;
    node->orbit_count   = tmp.planet_count < MAX_SYSTEM_PLANETS ? tmp.planet_count : MAX_SYSTEM_PLANETS;
    for (i32 p = 0; p < node->orbit_count; ++p)
        node->orbit_radii[p] = tmp.planets[p].semi_major_axis;
    // Insertion-sort ascending (<= MAX_SYSTEM_PLANETS entries).
    for (i32 a = 1; a < node->orbit_count; ++a) {
        f32 v = node->orbit_radii[a];
        i32 b = a - 1;
        while (b >= 0 && node->orbit_radii[b] > v) { node->orbit_radii[b + 1] = node->orbit_radii[b]; --b; }
        node->orbit_radii[b + 1] = v;
    }
    // Habitability substrate (FREE: `tmp` already carries per-planet habitability from worldgen).
    // Max habitability + count of habitable worlds seed the galaxy history's civilization cradles.
    f32 best = 0.0f; i32 hab = 0;
    for (i32 p = 0; p < tmp.planet_count; ++p) {
        f32 h = tmp.planet_props[p].habitability;
        if (h > best) best = h;
        if (h > 0.4f) ++hab;
    }
    node->best_habitability = (u8)(best >= 1.0f ? 255 : (best <= 0.0f ? 0 : best * 255.0f));
    node->habitable_count   = (u8)(hab > 255 ? 255 : hab);
}

// Box-Muller standard normal (deterministic; used to scatter systems around spiral arms).
static f64 rng_gaussian(GalaxyRng* r) {
    f64 u1 = (f64)galaxy_rng_f32(r); if (u1 < 1e-9) u1 = 1e-9;
    f64 u2 = (f64)galaxy_rng_f32(r);
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

f64 galaxy_arm_angle(const GalaxyGenParams* gp, f64 r, i32 arm_index) {
    i32 m    = gp->arm_count > 0 ? gp->arm_count : 1;
    f64 base = gp->orient_angle + (f64)arm_index * (6.283185307179586 / (f64)m);
    f64 rr   = r < 1.0 ? 1.0 : r;
    return base + log(rr / gp->arm_r0) / tan(gp->arm_pitch_rad);
}

static SSGenEnv galaxy_env_from_xy(const GalaxyGenParams* gp, f64 x, f64 y) {
    SSGenEnv e;
    f64 r  = sqrt(x * x + y * y);
    f32 rn = (f32)(r / gp->disc_rmax);
    e.r_norm = rn < 0.0f ? 0.0f : (rn > 1.0f ? 1.0f : rn);

    // Young/blue proximity: for arm morphologies, the angular distance to the nearest arm
    // centerline; for a ring, the radial distance to the star-forming annulus; otherwise none.
    f32 young = 0.0f;
    switch (gp->shape) {
        case GX_SPIRAL:
        case GX_BARRED:
        case GX_FLOCCULENT: {
            i32 m   = gp->arm_count > 0 ? gp->arm_count : 1;
            f64 phi = galaxy_arm_angle(gp, r, 0);      // arm-0 phase == the shared log-spiral term
            f64 seg = 6.283185307179586 / (f64)m;
            f64 d   = fmod(atan2(y, x) - phi, seg);
            if (d < 0.0) d += seg;
            if (d > seg * 0.5) d -= seg;
            young = (f32)exp(-(d * d) / (2.0 * gp->arm_width_rad * gp->arm_width_rad));
            break;
        }
        case GX_RING: {
            f64 w  = gp->ring_width > 1.0 ? gp->ring_width : 1.0;
            f64 dr = (r - gp->ring_radius) / w;
            young  = (f32)exp(-(dr * dr) * 0.5);
            break;
        }
        default: break;                                 // elliptical / irregular: no arm structure
    }
    if (young < gp->young_bias) young = gp->young_bias;  // global young/star-forming floor
    e.arm_strength = young > 1.0f ? 1.0f : young;

    // Old/red strength: a per-galaxy constant (ellipticals high). The central-bulge reddening is
    // still applied from r_norm inside worldgen_star, so this stays 0 for a plain spiral.
    e.age_bias = gp->old_bias > 1.0f ? 1.0f : gp->old_bias;
    return e;
}

SSGenEnv galaxy_env_at(const GalaxyGenParams* gp, const bs_math::HierPos2* pos) {
    f64 x, y; hierpos_to_f64(pos, BS_HIERPOS_CELL_SIZE, &x, &y);
    return galaxy_env_from_xy(gp, x, y);
}

// Concrete preset for `shape`. GX_SPIRAL reproduces the built-in constants exactly (so the default
// look is unchanged); other shapes override the relevant fields. Seed-dependent fields (bar /
// elliptical orientation) are derived from the master seed so successive seeds vary.
GalaxyGenParams galaxy_params_for_shape(GalaxyShape shape, u64 master_seed) {
    GalaxyGenParams gp{};
    gp.shape          = shape;
    gp.disc_scale_len = GALAXY_DISC_SCALE_LEN;
    gp.disc_rmax      = GALAXY_DISC_RMAX;
    gp.arm_count      = GALAXY_ARM_COUNT;
    gp.arm_pitch_rad  = GALAXY_ARM_PITCH_RAD;
    gp.arm_r0         = GALAXY_ARM_R0;
    gp.arm_width_rad  = GALAXY_ARM_WIDTH_RAD;
    gp.arm_core_flare = GALAXY_ARM_CORE_FLARE;
    gp.arm_fraction   = GALAXY_ARM_FRACTION;
    gp.bulge_fraction = GALAXY_BULGE_FRACTION;
    gp.bulge_scale    = GALAXY_BULGE_SCALE;
    gp.bar_fraction   = 0.0f;
    gp.bar_length     = 1.2e10;
    gp.bar_axis_ratio = 0.28f;
    gp.ring_radius    = 2.2e10;
    gp.ring_width     = 4.0e9;
    gp.clump_count    = 6;
    gp.ellipticity    = 1.0f;
    gp.orient_angle   = 0.0;
    gp.young_bias     = 0.0f;
    gp.old_bias       = 0.0f;

    // Seeded orientation in [0, 2pi) so bars / ellipticals aren't all axis-aligned.
    f64 orient = (f64)(galaxy_splitmix64(master_seed ^ 0xB5297A4D8CF3ULL) >> 11) *
                 (1.0 / 9007199254740992.0) * 6.283185307179586;

    switch (shape) {
        case GX_BARRED:
            gp.orient_angle   = orient;
            gp.bar_fraction   = 0.28f;
            gp.bulge_fraction = 0.08f;      // the centre is dominated by the bar, not a round bulge
            gp.arm_fraction   = 0.90f;
            gp.arm_r0         = gp.bar_length;  // arms spring from the bar ends
            gp.arm_pitch_rad  = 0.38;
            break;
        case GX_ELLIPTICAL:
            gp.orient_angle   = orient;
            gp.arm_fraction   = 0.0f;
            gp.bulge_fraction = 0.0f;
            gp.disc_scale_len = 1.6e10;     // large, centrally concentrated spheroid
            gp.disc_rmax      = 5.0e10;
            gp.ellipticity    = 0.62f;      // moderately flattened
            gp.old_bias       = 0.85f;      // old, red, metal-rich throughout
            break;
        case GX_RING:
            gp.arm_fraction   = 0.0f;
            gp.bulge_fraction = 0.12f;      // small central hub
            gp.ring_radius    = 2.4e10;
            gp.ring_width     = 3.5e9;
            gp.young_bias     = 0.12f;
            break;
        case GX_IRREGULAR:
            gp.arm_fraction   = 0.0f;
            gp.bulge_fraction = 0.0f;
            gp.clump_count    = 6;
            gp.disc_rmax      = 4.5e10;
            gp.young_bias     = 0.40f;      // pervasive star formation -> bluer
            break;
        case GX_FLOCCULENT:
            gp.arm_count      = 4;
            gp.arm_pitch_rad  = 0.55;
            gp.arm_width_rad  = 0.28;
            gp.arm_core_flare = 1.4;
            gp.arm_fraction   = 0.70f;
            break;
        case GX_SPIRAL:
        default:
            break;                           // defaults already match the built-in spiral
    }
    return gp;
}

// True if (wx,wy) keeps the hard minimum separation from every already-placed system (3x3 scan of
// the uniform hash grid whose cell == GALAXY_MIN_SEPARATION). Draws no RNG.
static b8 minsep_free(f64 wx, f64 wy, f64 cell, i32 half, i32 dim,
                      const i32* head, const i32* nxt, const f64* px, const f64* py, f64 min_sep2) {
    i32 cx = (i32)floor(wx / cell) + half;
    i32 cy = (i32)floor(wy / cell) + half;
    for (i32 dy = -1; dy <= 1; ++dy)
    for (i32 dx = -1; dx <= 1; ++dx) {
        i32 ncx = cx + dx, ncy = cy + dy;
        if (ncx < 0 || ncx >= dim || ncy < 0 || ncy >= dim) continue;
        for (i32 j = head[ncy * dim + ncx]; j >= 0; j = nxt[j]) {
            f64 ddx = px[j] - wx, ddy = py[j] - wy;
            if (ddx * ddx + ddy * ddy < min_sep2) return FALSE;
        }
    }
    return TRUE;
}

static void place_nodes(GalaxyState* g, u64 master_seed, i32 count) {
    g->nodes = (GalaxyNode*)bs_memory_allocator(sizeof(GalaxyNode) * count, MEMORY_TAG_GAME);
    g->node_count = count;
    g->galaxy_seed = master_seed;
    const GalaxyGenParams* gp = &g->gen_params;

    // Temp f64 positions + a uniform hash grid (cell == min separation) for blue-noise rejection:
    // a candidate is accepted only if no already-placed system lies within GALAXY_MIN_SEPARATION,
    // so neighbouring systems' orbits can never intersect. O(N) amortised (3x3 neighbour scan).
    f64* px = (f64*)bs_memory_allocator(sizeof(f64) * count, MEMORY_TAG_GAME);
    f64* py = (f64*)bs_memory_allocator(sizeof(f64) * count, MEMORY_TAG_GAME);
    const f64 cell = GALAXY_MIN_SEPARATION;
    const i32 half = (i32)(gp->disc_rmax / cell) + 2;
    const i32 dim  = half * 2 + 1;
    i32* head = (i32*)bs_memory_allocator(sizeof(i32) * dim * dim, MEMORY_TAG_GAME);
    i32* nxt  = (i32*)bs_memory_allocator(sizeof(i32) * count, MEMORY_TAG_GAME);
    for (i32 i = 0; i < dim * dim; ++i) head[i] = -1;
    const f64 min_sep2 = GALAXY_MIN_SEPARATION * GALAXY_MIN_SEPARATION;
    const f64 rmax2    = gp->disc_rmax * gp->disc_rmax;
    const HierPos2 origin{ GridCell{ 0, 0 }, Vec2{ 0.0f, 0.0f } };

    // Irregular galaxies scatter their systems across a handful of star-forming clumps; precompute
    // the clump centres/scales deterministically from the master seed (used only by GX_IRREGULAR).
    const i32 GX_MAX_CLUMPS = 16;
    i32 clump_n = gp->clump_count < 1 ? 1 : (gp->clump_count > GX_MAX_CLUMPS ? GX_MAX_CLUMPS : gp->clump_count);
    f64 clump_x[GX_MAX_CLUMPS], clump_y[GX_MAX_CLUMPS], clump_s[GX_MAX_CLUMPS];
    for (i32 k = 0; k < clump_n; ++k) {
        GalaxyRng cr = galaxy_rng_seed(master_seed ^ (0x51C7A3B9EF12ULL + (u64)k * 0x9E3779B97F4A7C15ULL));
        f64 cr_ang = (f64)galaxy_rng_f32(&cr) * 6.283185307179586;
        f64 cr_rad = gp->disc_rmax * 0.55 * (f64)galaxy_rng_f32(&cr);
        clump_x[k] = cos(cr_ang) * cr_rad;
        clump_y[k] = sin(cr_ang) * cr_rad;
        clump_s[k] = gp->disc_scale_len * (0.35 + 0.55 * (f64)galaxy_rng_f32(&cr));
    }

    for (i32 i = 0; i < count; ++i) {
        u64 seed = galaxy_seed_for(master_seed, i);
        f64 wx = 0.0, wy = 0.0;
        if (i != 0) {
            GalaxyRng r = galaxy_rng_seed(seed ^ 0xD1B54A32D192ED03ull);
            // Population is fixed per node (stable across min-sep re-rolls). Each morphology samples
            // its own radius/angle; all share the blue-noise rejection + disc_rmax clamp below.
            switch (gp->shape) {
            case GX_SPIRAL:
            case GX_FLOCCULENT: {
                // Compact central bulge, otherwise an exponential disc biased onto the arms.
                b8  is_bulge = galaxy_rng_f32(&r) < gp->bulge_fraction;
                b8  on_arm   = galaxy_rng_f32(&r) < gp->arm_fraction;
                i32 arm_k    = galaxy_rng_int(&r, 0, (gp->arm_count > 0 ? gp->arm_count : 1) - 1);
                for (i32 attempt = 0; attempt < 32; ++attempt) {
                    f64 radius, angle;
                    if (is_bulge) {
                        f64 u = (f64)galaxy_rng_f32(&r); if (u < 1e-6) u = 1e-6;
                        radius = gp->bulge_scale * sqrt(-2.0 * log(u));
                        angle  = (f64)galaxy_rng_f32(&r) * 6.283185307179586;
                    } else {
                        f64 u1 = (f64)galaxy_rng_f32(&r); if (u1 < 1e-6) u1 = 1e-6;
                        f64 u2 = (f64)galaxy_rng_f32(&r); if (u2 < 1e-6) u2 = 1e-6;
                        radius = -gp->disc_scale_len * (log(u1) + log(u2));
                        if (on_arm) {
                            // Bias the angle onto arm_k's log-spiral centerline; scatter widens toward
                            // the core so arms dissolve into the bulge instead of pinching to a point.
                            f64 scatter = gp->arm_width_rad *
                                          (1.0 + gp->arm_core_flare * exp(-radius / gp->disc_scale_len));
                            angle = galaxy_arm_angle(gp, radius, arm_k) + rng_gaussian(&r) * scatter;
                        } else {
                            angle = (f64)galaxy_rng_f32(&r) * 6.283185307179586;
                        }
                    }
                    if (radius > gp->disc_rmax) continue;
                    wx = cos(angle) * radius; wy = sin(angle) * radius;
                    if (minsep_free(wx, wy, cell, half, dim, head, nxt, px, py, min_sep2)) break;
                }
                break;
            }
            case GX_BARRED: {
                // Elongated central bar, small round bulge, then an exponential disc whose arms are
                // anchored (via gp->orient_angle + arm_r0 = bar_length) to spring from the bar ends.
                b8  is_bar   = galaxy_rng_f32(&r) < gp->bar_fraction;
                b8  is_bulge = galaxy_rng_f32(&r) < gp->bulge_fraction;
                b8  on_arm   = galaxy_rng_f32(&r) < gp->arm_fraction;
                i32 arm_k    = galaxy_rng_int(&r, 0, (gp->arm_count > 0 ? gp->arm_count : 1) - 1);
                f64 ca = cos(gp->orient_angle), sa = sin(gp->orient_angle);
                for (i32 attempt = 0; attempt < 32; ++attempt) {
                    if (is_bar) {
                        f64 along  = ((f64)galaxy_rng_f32(&r) * 2.0 - 1.0) * gp->bar_length;
                        f64 across = rng_gaussian(&r) * gp->bar_length * (f64)gp->bar_axis_ratio;
                        wx = along * ca - across * sa;
                        wy = along * sa + across * ca;
                    } else if (is_bulge) {
                        f64 u = (f64)galaxy_rng_f32(&r); if (u < 1e-6) u = 1e-6;
                        f64 radius = gp->bulge_scale * sqrt(-2.0 * log(u));
                        f64 angle  = (f64)galaxy_rng_f32(&r) * 6.283185307179586;
                        wx = cos(angle) * radius; wy = sin(angle) * radius;
                    } else {
                        f64 u1 = (f64)galaxy_rng_f32(&r); if (u1 < 1e-6) u1 = 1e-6;
                        f64 u2 = (f64)galaxy_rng_f32(&r); if (u2 < 1e-6) u2 = 1e-6;
                        f64 radius = -gp->disc_scale_len * (log(u1) + log(u2));
                        f64 angle;
                        if (on_arm) {
                            f64 scatter = gp->arm_width_rad *
                                          (1.0 + gp->arm_core_flare * exp(-radius / gp->disc_scale_len));
                            angle = galaxy_arm_angle(gp, radius, arm_k) + rng_gaussian(&r) * scatter;
                        } else {
                            angle = (f64)galaxy_rng_f32(&r) * 6.283185307179586;
                        }
                        wx = cos(angle) * radius; wy = sin(angle) * radius;
                    }
                    if (wx * wx + wy * wy > rmax2) continue;
                    if (minsep_free(wx, wy, cell, half, dim, head, nxt, px, py, min_sep2)) break;
                }
                break;
            }
            case GX_ELLIPTICAL: {
                // Smooth spheroid: Rayleigh radius, uniform angle, squashed along the minor axis.
                f64 ca = cos(gp->orient_angle), sa = sin(gp->orient_angle);
                for (i32 attempt = 0; attempt < 32; ++attempt) {
                    f64 u = (f64)galaxy_rng_f32(&r); if (u < 1e-6) u = 1e-6;
                    f64 radius = gp->disc_scale_len * sqrt(-2.0 * log(u));
                    f64 angle  = (f64)galaxy_rng_f32(&r) * 6.283185307179586;
                    f64 lx = cos(angle) * radius;
                    f64 ly = sin(angle) * radius * (f64)gp->ellipticity;
                    wx = lx * ca - ly * sa;
                    wy = lx * sa + ly * ca;
                    if (wx * wx + wy * wy > rmax2) continue;
                    if (minsep_free(wx, wy, cell, half, dim, head, nxt, px, py, min_sep2)) break;
                }
                break;
            }
            case GX_RING: {
                // A small central hub plus a Gaussian annulus of systems.
                b8 is_core = galaxy_rng_f32(&r) < gp->bulge_fraction;
                for (i32 attempt = 0; attempt < 32; ++attempt) {
                    f64 radius;
                    if (is_core) {
                        f64 u = (f64)galaxy_rng_f32(&r); if (u < 1e-6) u = 1e-6;
                        radius = gp->bulge_scale * sqrt(-2.0 * log(u));
                    } else {
                        radius = gp->ring_radius + rng_gaussian(&r) * gp->ring_width;
                        if (radius < 0.0) radius = -radius;
                    }
                    f64 angle = (f64)galaxy_rng_f32(&r) * 6.283185307179586;
                    if (radius > gp->disc_rmax) continue;
                    wx = cos(angle) * radius; wy = sin(angle) * radius;
                    if (minsep_free(wx, wy, cell, half, dim, head, nxt, px, py, min_sep2)) break;
                }
                break;
            }
            case GX_IRREGULAR: {
                // Scatter around one of the precomputed star-forming clumps (asymmetric, no bulge).
                i32 ck = clump_n > 1 ? galaxy_rng_int(&r, 0, clump_n - 1) : 0;
                for (i32 attempt = 0; attempt < 32; ++attempt) {
                    wx = clump_x[ck] + rng_gaussian(&r) * clump_s[ck];
                    wy = clump_y[ck] + rng_gaussian(&r) * clump_s[ck];
                    if (wx * wx + wy * wy > rmax2) continue;
                    if (minsep_free(wx, wy, cell, half, dim, head, nxt, px, py, min_sep2)) break;
                }
                break;
            }
            default: break;
            }
        }
        // Set the precise center FIRST so the structural env (hence the star population and colour)
        // is derived from the same quantised position the player later flies into.
        g->nodes[i].galaxy_center = hierpos_add_f64(&origin, wx, wy, BS_HIERPOS_CELL_SIZE);
        SSGenEnv env = galaxy_env_at(gp, &g->nodes[i].galaxy_center);
        fill_node_summary(&g->nodes[i], seed, Vec2{ (f32)wx, (f32)wy }, env);
        galaxy_system_name(i, g->nodes[i].name, (i32)sizeof(g->nodes[i].name));

        // Insert into the min-sep hash grid.
        px[i] = wx; py[i] = wy;
        i32 cx = (i32)floor(wx / cell) + half;
        i32 cy = (i32)floor(wy / cell) + half;
        if (cx < 0) cx = 0; else if (cx >= dim) cx = dim - 1;
        if (cy < 0) cy = 0; else if (cy >= dim) cy = dim - 1;
        i32 ci = cy * dim + cx;
        nxt[i] = head[ci]; head[ci] = i;
    }
    snprintf(g->nodes[0].name, sizeof(g->nodes[0].name), "Sol");

    bs_memory_free(head, sizeof(i32) * dim * dim, MEMORY_TAG_GAME);
    bs_memory_free(nxt,  sizeof(i32) * count, MEMORY_TAG_GAME);
    bs_memory_free(px,   sizeof(f64) * count, MEMORY_TAG_GAME);
    bs_memory_free(py,   sizeof(f64) * count, MEMORY_TAG_GAME);
}

// =====================================================================================
// Lane graph (kNN candidate edges -> MST via Kruskal -> add-back loop edges -> CSR)
// =====================================================================================
struct GEdge { i32 a, b; f64 len; };

static int gedge_cmp_len(const void* p, const void* q) {
    f64 d = ((const GEdge*)p)->len - ((const GEdge*)q)->len;
    return d < 0 ? -1 : (d > 0 ? 1 : 0);
}
static int gedge_cmp_ab(const void* p, const void* q) {
    const GEdge* e = (const GEdge*)p; const GEdge* f = (const GEdge*)q;
    if (e->a != f->a) return e->a < f->a ? -1 : 1;
    if (e->b != f->b) return e->b < f->b ? -1 : 1;
    return 0;
}

// Union-find with path compression + union by rank.
static i32 uf_find(i32* parent, i32 x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
}
static void uf_union(i32* parent, i32* rank, i32 a, i32 b) {
    a = uf_find(parent, a); b = uf_find(parent, b);
    if (a == b) return;
    if (rank[a] < rank[b]) { i32 t = a; a = b; b = t; }
    parent[b] = a;
    if (rank[a] == rank[b]) rank[a]++;
}

static void build_lanes(GalaxyState* g) {
    const i32 N = g->node_count;
    const GalaxyNode* nodes = g->nodes;
    GalaxyLaneGraph* lg = &g->lanes;
    *lg = GalaxyLaneGraph{};
    lg->node_count = N;
    if (N <= 1) return;

    const i32 K = GALAXY_KNN_K;
    const i32 SCRATCH_MAX = 256;
    i32* scratch = (i32*)bs_memory_allocator(sizeof(i32) * SCRATCH_MAX, MEMORY_TAG_GAME);

    // ---- 1. kNN candidate edges (canonical a<b, duplicates allowed; deduped below) ----
    i32 cand_cap = N * K;
    GEdge* cand = (GEdge*)bs_memory_allocator(sizeof(GEdge) * cand_cap, MEMORY_TAG_GAME);
    i32 cand_count = 0;

    for (i32 i = 0; i < N; ++i) {
        f64 wx, wy; hierpos_to_f64(&nodes[i].galaxy_center, BS_HIERPOS_CELL_SIZE, &wx, &wy);
        // Grow the query radius until at least K other nodes are in reach.
        f64 radius = GALAXY_GRID_CELL * 2.0;
        i32 found = 0;
        for (i32 tries = 0; tries < 24; ++tries) {
            found = galaxy_grid_query_radius(&g->grid, nodes, wx, wy, radius, scratch, SCRATCH_MAX);
            if (found >= K + 1 || found >= SCRATCH_MAX || radius > GALAXY_DISC_RMAX * 3.0) break;
            radius *= 2.0;
        }
        // Select the K nearest (excluding self) with a tiny bounded top-K.
        i32 best_idx[GALAXY_KNN_K]; f64 best_d[GALAXY_KNN_K];
        i32 kc = 0;
        for (i32 s = 0; s < found; ++s) {
            i32 ni = scratch[s];
            if (ni == i) continue;
            f64 nx, ny; hierpos_to_f64(&nodes[ni].galaxy_center, BS_HIERPOS_CELL_SIZE, &nx, &ny);
            f64 dx = nx - wx, dy = ny - wy, d = dx * dx + dy * dy;
            if (kc < K) {
                best_idx[kc] = ni; best_d[kc] = d; ++kc;
            } else {
                i32 worst = 0;
                for (i32 t = 1; t < K; ++t) if (best_d[t] > best_d[worst]) worst = t;
                if (d < best_d[worst]) { best_d[worst] = d; best_idx[worst] = ni; }
            }
        }
        for (i32 t = 0; t < kc; ++t) {
            i32 a = i, b = best_idx[t];
            if (a == b) continue;
            if (a > b) { i32 tmp = a; a = b; b = tmp; }
            if (cand_count < cand_cap)
                cand[cand_count++] = GEdge{ a, b, sqrt(best_d[t]) };
        }
    }
    bs_memory_free(scratch, sizeof(i32) * SCRATCH_MAX, MEMORY_TAG_GAME);

    // ---- 2. Dedup canonical edges ----
    qsort(cand, cand_count, sizeof(GEdge), gedge_cmp_ab);
    i32 uniq = 0;
    for (i32 i = 0; i < cand_count; ++i) {
        if (i == 0 || cand[i].a != cand[uniq - 1].a || cand[i].b != cand[uniq - 1].b)
            cand[uniq++] = cand[i];
    }
    cand_count = uniq;

    // ---- 3. Kruskal MST + deterministic add-back of loop edges ----
    i32* parent = (i32*)bs_memory_allocator(sizeof(i32) * N, MEMORY_TAG_GAME);
    i32* rank   = (i32*)bs_memory_allocator(sizeof(i32) * N, MEMORY_TAG_GAME);
    for (i32 i = 0; i < N; ++i) { parent[i] = i; rank[i] = 0; }

    i32 lane_cap = cand_count + N;   // MST (<= N-1) + add-back + connectivity bridges
    i32* la = (i32*)bs_memory_allocator(sizeof(i32) * lane_cap, MEMORY_TAG_GAME);
    i32* lb = (i32*)bs_memory_allocator(sizeof(i32) * lane_cap, MEMORY_TAG_GAME);
    i32  lane_count = 0;

    qsort(cand, cand_count, sizeof(GEdge), gedge_cmp_len);
    for (i32 i = 0; i < cand_count; ++i) {
        i32 a = cand[i].a, b = cand[i].b;
        if (uf_find(parent, a) != uf_find(parent, b)) {
            uf_union(parent, rank, a, b);
            la[lane_count] = a; lb[lane_count] = b; ++lane_count;   // tree edge
        } else {
            // Loop edge: keep ~GALAXY_LANE_ADDBACK of them, chosen deterministically.
            u64 h = galaxy_splitmix64(((u64)a << 32) ^ (u64)(u32)b);
            if ((h % 1000ull) < (u64)(GALAXY_LANE_ADDBACK * 1000.0f)) {
                la[lane_count] = a; lb[lane_count] = b; ++lane_count;
            }
        }
    }

    // ---- 4. Guarantee full connectivity: bridge any leftover components along the spatially
    // coherent grid ordering (short bridges), so no system is ever unreachable. ----
    if (g->grid.node_order) {
        i32 prev = -1;
        for (i32 k = 0; k < g->grid.node_count; ++k) {
            i32 cur = g->grid.node_order[k];
            if (prev >= 0 && uf_find(parent, prev) != uf_find(parent, cur)) {
                uf_union(parent, rank, prev, cur);
                if (lane_count < lane_cap) { la[lane_count] = prev; lb[lane_count] = cur; ++lane_count; }
            }
            prev = cur;
        }
    }

    bs_memory_free(parent, sizeof(i32) * N, MEMORY_TAG_GAME);
    bs_memory_free(rank,   sizeof(i32) * N, MEMORY_TAG_GAME);
    bs_memory_free(cand,   sizeof(GEdge) * cand_cap, MEMORY_TAG_GAME);

    // ---- 5. Compact into exact-size final lane arrays ----
    lg->lane_count = lane_count;
    lg->lane_a = (i32*)bs_memory_allocator(sizeof(i32) * lane_count, MEMORY_TAG_GAME);
    lg->lane_b = (i32*)bs_memory_allocator(sizeof(i32) * lane_count, MEMORY_TAG_GAME);
    for (i32 i = 0; i < lane_count; ++i) { lg->lane_a[i] = la[i]; lg->lane_b[i] = lb[i]; }
    bs_memory_free(la, sizeof(i32) * lane_cap, MEMORY_TAG_GAME);
    bs_memory_free(lb, sizeof(i32) * lane_cap, MEMORY_TAG_GAME);

    // ---- 6. CSR adjacency for O(1) neighbour iteration ----
    lg->adj_start = (i32*)bs_memory_allocator(sizeof(i32) * (N + 1), MEMORY_TAG_GAME);
    for (i32 i = 0; i <= N; ++i) lg->adj_start[i] = 0;
    for (i32 i = 0; i < lane_count; ++i) { lg->adj_start[lg->lane_a[i] + 1]++; lg->adj_start[lg->lane_b[i] + 1]++; }
    for (i32 i = 0; i < N; ++i) lg->adj_start[i + 1] += lg->adj_start[i];
    lg->adj_neighbor = (i32*)bs_memory_allocator(sizeof(i32) * (2 * lane_count), MEMORY_TAG_GAME);
    i32* cursor = (i32*)bs_memory_allocator(sizeof(i32) * N, MEMORY_TAG_GAME);
    for (i32 i = 0; i < N; ++i) cursor[i] = lg->adj_start[i];
    for (i32 i = 0; i < lane_count; ++i) {
        i32 a = lg->lane_a[i], b = lg->lane_b[i];
        lg->adj_neighbor[cursor[a]++] = b;
        lg->adj_neighbor[cursor[b]++] = a;
    }
    bs_memory_free(cursor, sizeof(i32) * N, MEMORY_TAG_GAME);
}

// =====================================================================================
void galaxy_generate(game_state* s, u64 master_seed, i32 target_count, GalaxyShape shape) {
    GalaxyState* g = &s->galaxy;
    if (target_count < 1) target_count = 1;
    if (target_count > GALAXY_TARGET_SYSTEMS) target_count = GALAXY_TARGET_SYSTEMS;
    if (shape >= GX_SHAPE_COUNT) shape = GX_SPIRAL;

    // Resolve the morphology preset ONCE and store it on the galaxy so the map-dot summaries and the
    // later fly-in materialisation derive the SAME per-position population/colour (see galaxy_env_at).
    g->gen_params = galaxy_params_for_shape(shape, master_seed);

    place_nodes(g, master_seed, target_count);
    galaxy_grid_build(&g->grid, g->nodes, g->node_count, GALAXY_GRID_CELL);
    build_lanes(g);
}

void galaxy_free(game_state* s) {
    if (!s) return;
    GalaxyState* g = &s->galaxy;
    if (g->nodes) bs_memory_free(g->nodes, sizeof(GalaxyNode) * g->node_count, MEMORY_TAG_GAME);
    galaxy_grid_free(&g->grid);
    GalaxyLaneGraph* lg = &g->lanes;
    if (lg->lane_a)      bs_memory_free(lg->lane_a, sizeof(i32) * lg->lane_count, MEMORY_TAG_GAME);
    if (lg->lane_b)      bs_memory_free(lg->lane_b, sizeof(i32) * lg->lane_count, MEMORY_TAG_GAME);
    if (lg->adj_neighbor) bs_memory_free(lg->adj_neighbor, sizeof(i32) * (2 * lg->lane_count), MEMORY_TAG_GAME);
    if (lg->adj_start)   bs_memory_free(lg->adj_start, sizeof(i32) * (lg->node_count + 1), MEMORY_TAG_GAME);
    if (g->node_owner)          bs_memory_free(g->node_owner, sizeof(i16) * g->node_count, MEMORY_TAG_GAME);
    if (g->node_colonized_year) bs_memory_free(g->node_colonized_year, sizeof(i32) * g->node_count, MEMORY_TAG_GAME);
    if (g->node_has_stations)   bs_memory_free(g->node_has_stations, sizeof(u8) * g->node_count, MEMORY_TAG_GAME);
    g->node_owner = nullptr; g->node_colonized_year = nullptr; g->node_has_stations = nullptr;
    g->nodes = nullptr; g->node_count = 0;
    *lg = GalaxyLaneGraph{};
}
