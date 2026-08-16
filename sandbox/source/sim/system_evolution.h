#pragma once
#include <defines.h>
#include "game.h"            // EvolvedSystem / StarProperties (game_state.h)
#include "sim/galaxy_gen.h"  // SSGenEnv

// =====================================================================================
// Epoch-based planetary evolution — simulated formation history of one star system.
// =====================================================================================
// Replaces single-pass worldgen with four chronological phases over macro-time epochs
// (no real N-body). Deterministic pure function of (seed, env): per-body/per-epoch RNG
// salts keep every body's stream independent, so one body's outcome never reorders
// another's. Budget ~30-50 us/system: fill_node_summary runs this for every galaxy node.
//
//   Phase 1  Disk condensation      — star + 1-D disk profile, protoplanet cores seeded
//                                     at Hill-spaced orbits with per-annulus feedstock.
//   Phase 2  Accretion & migration  — ~8 epochs: core growth, runaway gas giants before
//                                     disk dispersal, type-II migration, merge/eject,
//                                     frustrated annuli -> belts, impact/captured moons.
//   Phase 3  Geophysics/atmosphere  — ~6 epochs over the star's age: differentiation ->
//                                     magnetic field, decaying tectonics/volcanism,
//                                     outgassing vs escape, bombardment water, life.
//   Phase 4  Present-day synthesis  — classify PlanetType from evolved state, habitability,
//                                     env_hazard, resource richness, finalize chronicle.
//
// Output is the EvolvedSystem body array ([0]=star, planets sorted by a, moons, belts) plus
// the evolution event log. generate_star_system() drives this and derives render views.

// Run the full four-phase pipeline. `star` must already be rolled (worldgen_star) so the
// caller controls the star RNG stream; `seed` is the per-node seed for the body streams.
void evolve_star_system(EvolvedSystem* out, const StarProperties& star, u64 seed);

// Human-readable label for an evolution event kind (inspector UI / logs).
const char* evo_event_name(u8 kind);

// Debug self-test: evolves a batch of seeds and BS_LOG_INFO's per-phase statistics plus
// invariant checks (determinism, composition sums, Hill spacing, moon orbits, finiteness)
// and a full epoch trace for one seed. Call once at init (BS_DEBUG builds).
void system_evolution_selftest();
