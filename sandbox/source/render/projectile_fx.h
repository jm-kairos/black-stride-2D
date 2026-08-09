#pragma once

#include <defines.h>

struct game_state;

// =====================================================================================
// Projectile FX rendering -- draws the muzzle flashes, impacts, flak airbursts and
// point-defense intercepts recorded in game_state::projectile_fx.
//
// The travel component is NOT here: an in-flight shot is drawn by ProjectileSystem::render
// (sim/projectile.cpp), because the streak is a function of the live projectile's velocity
// and has to be regenerated every frame from the pool. This module draws the two ENDS --
// events that outlive the projectile they belong to and therefore need their own storage.
//
// `draw` takes a CONST game_state on purpose, the same guard projectile_markers_draw uses:
// the compiler then enforces that this pass can only read the ring and submit sprites. It
// cannot spawn, retire, damage or steer anything, so no amount of editing here can make the
// VFX affect the simulation. Deleting the one call in draw_gameplay_overlays is a
// zero-behaviour edit.
// =====================================================================================

// Bake the three procedural textures the effects share. MANDATORY second phase, exactly like
// ship_visual_resolve_textures and weapon_registry_resolve_textures: it calls
// renderer_create_texture, so it must run at game init AFTER the renderer is initialized.
// Skipping it is quiet rather than fatal -- every handle stays 0, which the engine renders as
// its 1x1 white texture, so the effects degrade to hard-edged white squares.
void projectile_fx_render_init();

// Submit every live event. Costs nothing when the ring is empty.
void projectile_fx_draw(const game_state* s);

// Muzzle CHARGE-UP: a glow that builds at the barrel over the last fraction of a heavy
// weapon's reload, completing the reference document's "charge -> flash -> launch" sequence
// (section 28) from the front. Only weapons whose whole cycle is long enough for the build to
// be legible take part; a 12-shot-per-second autocannon would read as a permanently lit barrel.
//
// STRICTLY READ-ONLY, and it must stay that way: a charge-up that DELAYED the shot would stop
// being cosmetic and become a fire-rate change. This only reads cooldown_progress(), which the
// simulation computes for its own reasons and which one HUD site already consumes.
void projectile_fx_draw_charges(const game_state* s);
