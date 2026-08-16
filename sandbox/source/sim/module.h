#pragma once

#include <defines.h>

#include "sim/ship.h"

// =====================================================================================
// Ship module registry (Phase 4 of the ship-module system).
//
// A ModuleDef is an immutable, data-driven description of a mountable ship system
// (sensor array, and later utility/engine packages) loaded from a `.module` text file:
//
//   id surveyor_mk1
//   name "Surveyor Array Mk I"
//   type sensor                 # weapon|defense|sensor|engine|utility (single kind)
//   size S                      # S/M/L; fits hardpoints of this size OR LARGER
//   glyph S1                    # fallback text symbol for the Arsenal box
//   icon ic-sensor              # optional ui-icons emblem sprite (defaults by type)
//   sensor_mult 1.15 1.35 1.60  # sensor modules: layer 0/1/2 radius multipliers
//
// The registry itself is a manifest file (assets/modules/modules.list) naming one
// `.module` path per line. All defs are loaded once at game init into a fixed pool;
// ships mount them BY POINTER (Ship::module_mounts[] / module_stash[]) — module defs
// carry no per-instance runtime state, so sharing one def between ships is safe.
//
// Stat composition: mounted modules never write ship stats directly. Ships keep an
// authored BASELINE (Ship::sensors_base) and ship_recompute_stats() re-derives the
// effective SensorSuite from baseline x the product of all mounted sensor modules'
// multipliers. Everything that reads sensors live (discovery radius, asteroid reveal,
// point-defense range) picks the composed values up automatically.
// =====================================================================================

#define MODULE_REGISTRY_MAX 32

struct ModuleDef {

    char          id[32];         // registry key ("surveyor_mk1"), from the .module file
    char          name[48];       // display name ("Surveyor Array Mk I")
    char          glyph[8];       // fallback Arsenal-box text symbol ("S1")
    char          icon[16];       // ui-icons emblem sprite ("ic-sensor"; defaulted by type)
    char          desc[192];      // player-facing role description (stat-card paragraph)
    u32           type;           // single MODULE_TYPE_* kind bit
    HardpointSize size;           // physical size; mounts on slots of this size or larger
    f32           sensor_mult[3]; // sensor modules: layer 0/1/2 radius multipliers (else 1.0)

};

struct ModuleRegistry {

    ModuleDef defs[MODULE_REGISTRY_MAX];

    i32       count;

};

// Load every `.module` file named in the manifest (one path per line, '#' comments).
// Returns FALSE only when the manifest itself cannot be opened; individual bad module
// files are skipped with a warning. Call once during game init.
b8 module_registry_load(ModuleRegistry* reg, const char* manifest_path);

// Find a loaded def by its registry id, or nullptr.
const ModuleDef* module_registry_find(const ModuleRegistry* reg, const char* id);
