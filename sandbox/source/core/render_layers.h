#pragma once
#include <renderer/renderer_types.h> // BS_LAYER_BLOOM_THRESHOLD

// ---- Render layers (lower draws first) ----
// Shared across game.cpp and the extracted render/* modules. Kept as file-static
// constants so each translation unit gets its own copy (no ODR/link concerns).
static const u32 LAYER_STARFIELD_FAR = 0;  // procedural far stars (custom GPU)
static const u32 LAYER_STARFIELD_MID = 2;  // procedural mid-distance stars (custom GPU)
static const u32 LAYER_MAPPED_SYSTEM = 3;  // current system star + planets
static const u32 LAYER_SHIP          = 10; // ship sprite art
static const u32 LAYER_CELESTIAL     = 11; // stars, planets, orbit rings (below bloom threshold)
static const u32 LAYER_UI            = 50; // debug overlays above ship, below HUD text
static const u32 LAYER_HUD_TEXT      = 100; // screen-space HUD/UI text -- always on top
// Debug overlays bypass the bloom pipeline (drawn after composite).
static const u32 LAYER_DEBUG = BS_LAYER_BLOOM_THRESHOLD;
static const u32 LAYER_GIZMO = BS_LAYER_BLOOM_THRESHOLD + 1;
