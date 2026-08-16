#pragma once
#include <defines.h>
// =====================================================================================
// Fleet skill system: hotkey-triggered fleet abilities with fleet-level cooldowns.
//
// The number row (1..9) is the skill hotbar while the camera is DETACHED (attached, it
// stays the weapon fire-group selector -- the mode split in game.cpp). An entity-targeted
// skill arms a modal targeting state (the jump-mode shape, arbitrated in rts_controls):
// LMB on a hostile casts, ESC / X / RMB / re-press / empty-space LMB cancels.
//
// v1 effect: MISSILE_VOLLEY ("Alpha Strike") -- every participating ship ripples its
// ready missile tubes at the designated hull, one launch per stagger beat. The volley
// does NOT cheat: every launch runs the canonical ship_weapon_fire_state ->
// ship_try_spend_cap -> ship_hardpoint_fire chain, skips flak-claimed mounts, and waits
// only for WEAPON_FIRE_SLEWING (bounded) -- any other non-READY state skips the tube.
// It also never reads missile_policy / roe / stance / cap_fire_floor: an explicit cast
// is player intent, the same contract as the manual trigger.
//
// Call-order contract (game.cpp): tick AFTER the weapon-cooldown/capacitor block (a tube
// that just came off reload this frame is castable this frame), BEFORE rts_controls
// (which may cast this frame) and update_autopilot (whose attack orders re-assert their
// engaging mount's aim after ours -- the slew timeout bounds that contention).
//
// The skill's fleet-level cooldown commits at the moment the FIRST missile actually
// spawns; a cast that ends with zero launches costs nothing (denial feedback instead).
// =====================================================================================
struct game_state;
struct SkillDef;
struct Ship;

#define SKILL_SLOT_MAX   9     // number row 1..9, detached camera
#define SKILL_VOLLEY_MAX 128   // FLEET_MAX_SHIPS (8) x SHIP_MAX_HARDPOINTS (16)

// One queued launch: indices, not pointers. FleetShip storage is pointer-stable (the
// fleet is a fixed array) but the ACTIVE count can shrink (Fleet::set_count) and loadouts
// mutate mid-ripple, so both halves are re-validated at launch time -- the attack_target
// convention.
struct SkillVolleyEntry {
    i8 fleet_idx;
    i8 hp_index;
};

struct SkillVolley {
    b8    active;
    i8    slot;           // hotbar slot that cast this (cooldown commit target)
    Ship* target;         // validated against s->combat_entities EVERY tick; vanish = abort
    f32   stagger;        // seconds between launches (from the def at cast time)
    f32   launch_timer;   // counts down to the next launch attempt
    f32   slew_wait;      // seconds the CURRENT entry has sat in WEAPON_FIRE_SLEWING
    i32   next;           // cursor into entries[]
    i32   count;
    i32   fired;          // launches that actually spawned; cooldown commits at 0 -> 1
    SkillVolleyEntry entries[SKILL_VOLLEY_MAX];
};

// One hotbar slot: a registry def (bound from manifest order at init) + its countdown.
struct SkillSlot {
    const SkillDef* def;      // nullptr = empty slot
    f32             cooldown; // seconds remaining; <= 0 = ready
};

struct SkillSystemState {
    SkillSlot   slots[SKILL_SLOT_MAX];
    i32         slot_count;
    b8          targeting_active;  // modal entity-targeting armed (jump-mode shape)
    i8          targeting_slot;
    f32         denied_timer;      // HUD denial flash seconds remaining
    i8          denied_slot;
    SkillVolley volley;            // v1: one in-flight volley fleet-wide
};

// Bind hotbar slots from the skill registry (manifest order = slot order). Call once at
// game init, after skill_registry_load.
void skill_system_init(game_state* s);

// Tick cooldowns, the denial flash and the pending volley ripple. Pass sim_dt: cooldowns
// and the ripple respect the time scale exactly like weapon reloads.
void skill_system_update(game_state* s, f32 sim_dt);

// Hotkey / hotbar-click entry point (number key N and the HUD's "skill:N" action both
// land here): arm targeting, cancel on re-press, or deny with feedback (on cooldown,
// volley in flight, no tube able to fire).
void skill_system_hotkey(game_state* s, i32 slot);

// Disarm the modal targeting state (ESC / X / RMB / empty-space click / camera attach).
void skill_system_cancel_targeting(game_state* s);

// Targeting click on a hostile hull: build and start the volley. Zero castable tubes
// denies without starting the cooldown.
void skill_system_cast_entity(game_state* s, Ship* target);

// THE participant query: castable tubes among the participants (selected ships, whole
// fleet when nothing is selected; a `scope fleet` card forces fleet-wide). A castable
// tube is a mounted missile launcher that is operational, off reload and not claimed by
// the flak screen this tick. The cast enqueues EXACTLY what this returns and the HUD
// badge counts the same call -- what you see is what fires. `out` may be nullptr
// (count-only, for the badge).
i32 skill_system_collect_tubes(game_state* s, const SkillDef* def,
                               SkillVolleyEntry* out, i32 cap);
