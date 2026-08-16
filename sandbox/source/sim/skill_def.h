#pragma once

#include <defines.h>

// =====================================================================================
// Skill definition registry (the weapon/module/ship registry twin, fourth of the line).
//
// A SkillDef is an immutable, data-driven card for a fleet ability, loaded from a
// `.skill` text file:
//
//   id        missile_volley
//   name      "Alpha Strike"
//   icon      ic-missile           # ui-icons emblem sprite (RCSS-side, no texture phase)
//   targeting entity               # instant | point | entity
//   effect    missile_volley       # names a SkillEffect branch implemented in code
//   scope     selection            # selection (fleet fallback) | fleet (forced)
//   cooldown  45                   # fleet-level cooldown, seconds
//   stagger   0.12                 # seconds between ripple launches
//   desc      "..."
//
// The card picks and tunes an effect; the effect's behaviour lives in
// sim/skill_system.cpp (the WeaponKind split: data describes, code implements).
//
// The registry is a manifest (assets/skills/skills.list) naming one `.skill` path per
// line. MANIFEST ORDER IS HOTBAR ORDER: line N binds skill slot N, which is number key
// N on the detached camera. Defs load once at game init into a fixed pool; slots point
// into the pool (safe: it never reallocates).
// =====================================================================================

#define SKILL_REGISTRY_MAX 16

// How a cast is aimed. INSTANT fires on the hotkey press; POINT and ENTITY arm a modal
// targeting state (the jump-mode shape) and cast on the confirming click.
enum SkillTargeting : u8 { SKILL_TARGET_INSTANT = 0, SKILL_TARGET_POINT = 1, SKILL_TARGET_ENTITY = 2 };

// Which code-side behaviour the card tunes.
enum SkillEffect : u8 { SKILL_EFFECT_MISSILE_VOLLEY = 0 };

// Which ships participate. SELECTION acts on the selected ships, the whole fleet when
// nothing is selected (the stance/ROE chip semantics); FLEET always acts fleet-wide.
enum SkillScope : u8 { SKILL_SCOPE_SELECTION = 0, SKILL_SCOPE_FLEET = 1 };

struct SkillDef {

    char id[32];     // registry key ("missile_volley")
    char name[48];   // display name ("Alpha Strike")
    char icon[16];   // ui-icons emblem sprite name
    char desc[192];  // player-facing role description (tooltip paragraph)
    u8   targeting;  // SkillTargeting
    u8   effect;     // SkillEffect
    u8   scope;      // SkillScope
    f32  cooldown;   // fleet-level cooldown seconds
    f32  stagger;    // seconds between ripple launches

};

struct SkillRegistry {

    SkillDef defs[SKILL_REGISTRY_MAX];

    i32      count;

};

// Load every `.skill` file named in the manifest (one path per line, '#' comments).
// Returns FALSE only when the manifest itself cannot be opened; individual bad files
// are skipped with a warning. Call once during game init.
b8 skill_registry_load(SkillRegistry* reg, const char* manifest_path);

// Find a loaded def by its registry id, or nullptr.
const SkillDef* skill_registry_find(const SkillRegistry* reg, const char* id);
