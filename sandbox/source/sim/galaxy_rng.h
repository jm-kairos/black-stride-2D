#pragma once
#include <defines.h>
// =====================================================================================
// Shared deterministic PRNG for galaxy generation (splitmix64).
// =====================================================================================
// Header-only. Used by the galaxy-scale generators (node placement, attributes, lanes) so
// they share one hashing scheme: master seed -> per-node seed -> per-body seed. The per-body
// star-system generator (ss_generation.cpp) keeps its own equivalent splitmix64 seeded from
// the per-node seed produced here, so a node's contents are fully determined by its seed.

// Stateless bit-mixer: hashes a 64-bit value to a well-distributed 64-bit value.
static inline u64 galaxy_splitmix64(u64 x) {
    u64 z = x + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Derive a stable per-index seed from a master seed (index-invariant hashing so adding or
// removing systems never disturbs the seed of an unrelated index).
static inline u64 galaxy_seed_for(u64 master, i32 index) {
    return galaxy_splitmix64(master ^ (0x9e3779b97f4a7c15ull * (u64)(index + 1)));
}

// Small stateful stream for a single generation pass.
struct GalaxyRng { u64 state; };

static inline GalaxyRng galaxy_rng_seed(u64 seed) { return GalaxyRng{ seed }; }

static inline u64 galaxy_rng_next(GalaxyRng* r) {
    r->state += 0x9e3779b97f4a7c15ull;
    u64 z = r->state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static inline f32 galaxy_rng_f32(GalaxyRng* r) {
    return (f32)(galaxy_rng_next(r) & 0xFFFFFF) / (f32)0xFFFFFF;
}

static inline f32 galaxy_rng_range(GalaxyRng* r, f32 lo, f32 hi) {
    return lo + galaxy_rng_f32(r) * (hi - lo);
}

static inline i32 galaxy_rng_int(GalaxyRng* r, i32 lo, i32 hi) {
    return lo + (i32)(galaxy_rng_next(r) % (u64)(hi - lo + 1));
}
