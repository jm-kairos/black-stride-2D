#pragma once

// =====================================================================================
// Projectile visibility markers.
//
// Every projectile is drawn by ProjectileSystem::render in WORLD units — a shell's 4-unit
// radius is ~2 px across in the arena but goes sub-pixel long before galaxy-map zoom, so a
// shot fired out there is invisible even though it is live, moving and lethal. This pass adds
// a SCREEN-CONSTANT marker per active projectile so a shot stays readable at any zoom.
//
// One identical treatment for every ProjectileKind — shells, flak and missiles all get the
// same dot-and-tracer at the same screen size — so nothing about the marker implies a
// difference in kind. Only the tint follows the projectile's own colour, which is what keeps
// incoming fire distinguishable from your own at a zoom where nothing else is legible.
//
// PURELY COSMETIC. It takes a `const game_state*` so the compiler enforces what the comment
// promises: it reads the projectile pool and submits draw calls, and writes nothing. It does
// not touch ProjectileSystem::update, spawn/retire, point defense, collision, damage or the
// flight model. Deleting its single call site in draw_gameplay_overlays removes it with zero
// behavioural change.
// =====================================================================================

struct game_state;

// Draw the marker for every active projectile. Self-gating: the marker fades out and then
// stops being submitted entirely once projectiles are large enough on screen to read on their
// own, so it costs nothing at arena zoom and never clutters close-in combat.
void projectile_markers_draw(const game_state* s);
