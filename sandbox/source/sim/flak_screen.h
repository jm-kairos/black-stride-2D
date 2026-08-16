#pragma once
#include <defines.h>
// =====================================================================================
// Autonomous FLAK screen (Phase 2). Every fleet hull with a ballistic mount toggled to
// MODE_FLAK engages incoming hostile ordnance on its own -- the OUTER layer of the
// missile-defense onion. Envelope = the shell's real flight (proj speed * FLAK_SPEED_MUL *
// FLAK_LIFETIME_S, ~10k for the autocannon); the point-defense laser is the 5k last-ditch
// inner layer. Threats are scored fleet-wide -- an escort's screen covers the flagship --
// missiles strictly outrank shells, soonest impact first.
//
// Call-order contract (same family as point_defense_update): AFTER
// combat_arena_sync_entities (positions current), BEFORE point_defense_update and BEFORE
// combat_arena_update_projectiles (so this frame's claims and shells exist when the laser
// and the collision pass look).
//
// A MODE_FLAK mount belongs to the screen EXCLUSIVELY: update_attack demotes it from the
// engaging-mount pick and the broadside skips it outright, because flak shells cannot hit
// hulls (the collision pass ignores PROJ_FLAK) and any hull-duty aim assertion drags the
// barrel off its screening axis -- the aim ping-pong between hull target and ordnance was
// what starved the screen originally. Between volleys the screen WATCHES: the barrel
// pre-aims at the nearest hostile ship inside twice the envelope, so the next inbound round
// is met with the turret already on axis. The player keeps mode authority: only MODE_FLAK
// mounts screen (T toggles), FLAK_MANUAL doctrine opts a hull out, and the ATTACHED piloted
// hull is skipped (its cursor owns the mounts' aim). Firing runs through
// ship_weapon_fire_state + ship_hardpoint_fire -- the same validator/spawner as every other
// gun -- and acquisition honours the PD reserve floor, so the screen never bottoms the bank
// under the laser.
// =====================================================================================
struct game_state;
void flak_screen_update(game_state* s, f32 dt);
