#pragma once

#include <defines.h>

struct game_state;

// =====================================================================================
// Point-defense laser system (targeting + damage only -- no rendering).
//
// Each player fleet ship carries a DefenseLaser (see ship.h). Every frame this subsystem, for
// each ship with an enabled laser, acquires the nearest HOSTILE PROJECTILE inside the ship's
// engagement range (the ship's live Layer 1 sensor radius unless a non-zero override is set),
// snaps a beam onto it and tracks its exact position for a short dwell while applying
// damage-per-second to the projectile's HP. The projectile is destroyed when its HP is
// depleted; survivors are released after the dwell and may be re-engaged after a brief
// retarget cooldown. Active beams are recorded into s->defense_beams for the overlay to draw.
//
// Call once per simulation tick, AFTER combat_arena_sync_entities() (positions current) and
// BEFORE combat_arena_update_projectiles() (so destroyed threats never advance / collide).
// =====================================================================================
void point_defense_update(game_state* s, f32 dt);
