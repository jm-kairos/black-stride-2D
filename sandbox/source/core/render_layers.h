#pragma once
#include <renderer/renderer_types.h> // BS_LAYER_BLOOM_THRESHOLD

// ---- Render layers (lower draws first) ----
// Shared across game.cpp and the extracted render/* modules. Kept as file-static
// constants so each translation unit gets its own copy (no ODR/link concerns).
static const u32 LAYER_STARFIELD_FAR = 0;  // procedural far stars (custom GPU)
static const u32 LAYER_STARFIELD_MID = 2;  // procedural mid-distance stars (custom GPU)
static const u32 LAYER_MAPPED_SYSTEM = 3;  // current system star + planets
// Engine plumes + RCS jets: one below LAYER_SHIP so exhaust renders UNDER every hull -- a
// plume must never glow on top of ship art. Below BS_LAYER_BLOOM_THRESHOLD (so jets bloom)
// and below the unlit_layer cutoff (so sprites here are LIT): everything drawn on it must
// set custom.z = 1 to stay self-emissive, same as the projectile layers below.
static const u32 LAYER_EXHAUST       = 9;
static const u32 LAYER_SHIP          = 10; // ship sprite art
static const u32 LAYER_CELESTIAL     = 11; // stars, planets, orbit rings (below bloom threshold)
// Engine light SPILL: the warm glow a burning drive throws onto its own stern plating. Must
// sit ABOVE the hull (the whole point is additively lighting hull pixels), so it shares 11
// with LAYER_CELESTIAL and ship_render.cpp's private LAYER_MOUNT_ART -- the same documented
// collision; additive glows are order-insensitive within the layer.
static const u32 LAYER_EXHAUST_SPILL = 11;
// Weapon fire. BOTH sit below BS_LAYER_BLOOM_THRESHOLD deliberately: shots used to draw on
// LAYER_UI, which IS the threshold, so every tracer and impact in the game was composited after
// the bloom pass and got no bloom at all. Moving them under it is what makes a hit read as hot
// rather than as a coloured decal.
//
// Two consequences to keep in mind when adding to these layers:
//   * Layers below the threshold are also below the `unlit_layer` cutoff that frame_lighting
//     passes to renderer_set_lights (also LAYER_UI), so sprites here are LIT. Everything drawn
//     on them sets custom.z = 1 to flag itself self-emissive, or the galaxy-map look's star
//     light and ambient will shade it.
//   * 15..19 are taken by render/out_sensor_detection_fx.cpp's private LAYER_FX, which is why
//     these are 12/13 and not 15/16.
static const u32 LAYER_PROJECTILE    = 12; // in-flight shot streaks + heads (above ships)
static const u32 LAYER_PROJECTILE_FX = 13; // muzzle flashes, impacts, flak bursts, intercepts
static const u32 LAYER_UI            = 50; // debug overlays above ship, below HUD text
static const u32 LAYER_HUD_TEXT      = 100; // screen-space HUD/UI text -- always on top
// Debug overlays bypass the bloom pipeline (drawn after composite).
static const u32 LAYER_DEBUG = BS_LAYER_BLOOM_THRESHOLD;
static const u32 LAYER_GIZMO = BS_LAYER_BLOOM_THRESHOLD + 1;
