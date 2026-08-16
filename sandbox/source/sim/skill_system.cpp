#include "sim/skill_system.h"
#include "game.h"
#include "sim/skill_def.h"
#include "sim/weapon.h"
#include "sim/action_log.h"
#include <core/logger.h>
#include <math/math_utils.h>

using namespace bs_math;

// Hard bound on a whole volley's lifetime. Pending tubes retry through their transient
// fire states (reload, slew, arc, capacitor) until this expires -- long enough for two
// full harpoon reload cycles plus traverse, short enough that a tube whose arc the
// geometry never unmasks cannot hold the volley (and its mount claims) open forever.
static constexpr f32 SKILL_VOLLEY_TIMEOUT_S = 10.0f;

// HUD denial flash duration (failed cast / on-cooldown press).
static constexpr f32 SKILL_DENY_FLASH_S = 0.35f;

// Mirror of fleet.cpp's find_combat_entity_for_ship (file-static there): the ONLY way a
// raw Ship* stays safe to hold across frames is that an active combat entity still wraps
// it -- a dead NPC simply stops being re-registered and vanishes from the pool.
static CombatEntity* skill_find_combat_entity(game_state* s, Ship* ship) {
    if (!s || !ship) return nullptr;
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        CombatEntity* ce = &s->combat_entities[i];
        if (ce->active && ce->ship == ship) return ce;
    }
    return nullptr;
}

static void skill_deny(game_state* s, i32 slot) {
    s->skills.denied_timer = SKILL_DENY_FLASH_S;
    s->skills.denied_slot  = (i8)slot;
}

void skill_system_init(game_state* s) {
    if (!s) return;
    s->skills = SkillSystemState{};
    s->skills.targeting_slot = -1;
    s->skills.denied_slot    = -1;
    for (i32 i = 0; i < s->skill_registry.count && i < SKILL_SLOT_MAX; ++i) {
        s->skills.slots[s->skills.slot_count].def = &s->skill_registry.defs[i];
        s->skills.slot_count++;
    }
}

i32 skill_system_collect_tubes(game_state* s, const SkillDef* def,
                               SkillVolleyEntry* out, i32 cap) {
    if (!s || !def) return 0;
    Fleet& fleet = s->fleet_state.fleet;
    // Selection scope with fleet fallback: the stance/ROE chip semantics. A `scope fleet`
    // card ignores the selection outright.
    b8 use_selection = (def->scope == SKILL_SCOPE_SELECTION) && fleet.any_selected();
    i32 n = 0;
    for (i32 i = 0; i < fleet.count(); ++i) {
        if (use_selection && !fleet.is_selected(i)) continue;
        Ship& sh = fleet.at(i).ship;
        for (i32 hp = 0; hp < sh.hardpoint_count; ++hp) {
            Weapon* w = sh.mounts[hp];
            if (!w || w->wkind != WEAPON_KIND_MISSILE) continue;
            // Deliberately NOT gated on ready(): a tube the autopilot just emptied still
            // counts -- the volley rides its remaining reload. Gating on it made the
            // badge sit at zero for the whole fight under MISSILE_FREE (the autopilot
            // respends each tube the frame it comes off cooldown).
            if (w->disabled) continue;
            if (sh.flak_screen_claimed[hp]) continue;   // defense preempts offense
            if (out) {
                if (n >= cap) return n;
                out[n].fleet_idx = (i8)i;
                out[n].hp_index  = (i8)hp;
                out[n].state     = SKILL_ENTRY_PENDING;
            }
            ++n;
        }
    }
    return n;
}

void skill_system_cancel_targeting(game_state* s) {
    if (!s) return;
    s->skills.targeting_active = FALSE;
    s->skills.targeting_slot   = -1;
}

void skill_system_hotkey(game_state* s, i32 slot) {
    if (!s || slot < 0 || slot >= s->skills.slot_count) return;
    SkillSlot& sl = s->skills.slots[slot];
    if (!sl.def) return;
    // Re-press of the armed slot is a cancel, not a deny.
    if (s->skills.targeting_active && s->skills.targeting_slot == (i8)slot) {
        skill_system_cancel_targeting(s);
        return;
    }
    if (sl.cooldown > 0.0f) {
        skill_deny(s, slot);
        action_log_push(s, "%s ready in %.0fs.", sl.def->name, sl.cooldown);
        return;
    }
    if (s->skills.volley.active) {
        skill_deny(s, slot);
        action_log_push(s, "%s: volley already in flight.", sl.def->name);
        return;
    }
    if (sl.def->targeting == SKILL_TARGET_ENTITY) {
        // Arm only when at least one tube exists to commit -- an unarmable cast is a deny
        // NOW, not a targeting mode that can only whiff.
        if (skill_system_collect_tubes(s, sl.def, nullptr, 0) == 0) {
            skill_deny(s, slot);
            action_log_push(s, "%s: no missile launcher mounted.", sl.def->name);
            return;
        }
        s->skills.targeting_active = TRUE;
        s->skills.targeting_slot   = (i8)slot;
        action_log_push(s, "%s: select a target.", sl.def->name);
        return;
    }
    // INSTANT / POINT have no v1 producer (no card authors them yet).
    action_log_push(s, "%s: targeting mode not implemented.", sl.def->name);
}

void skill_system_cast_entity(game_state* s, Ship* target) {
    if (!s || !target || !s->skills.targeting_active) return;
    i32 slot = s->skills.targeting_slot;
    skill_system_cancel_targeting(s);
    if (slot < 0 || slot >= s->skills.slot_count) return;
    SkillSlot& sl = s->skills.slots[slot];
    if (!sl.def || s->skills.volley.active) return;
    SkillVolley& v = s->skills.volley;
    v = SkillVolley{};
    v.count = skill_system_collect_tubes(s, sl.def, v.entries, SKILL_VOLLEY_MAX);
    if (v.count == 0) {
        skill_deny(s, slot);
        action_log_push(s, "%s: no missile launcher mounted.", sl.def->name);
        return;
    }
    v.active  = TRUE;
    v.slot    = (i8)slot;
    v.target  = target;
    v.stagger = sl.def->stagger;
    // launch_timer starts at 0: the first launch fires on the next skill tick.
    BS_LOG_INFO("skill_volley: cast '%s' at '%s', %d tube(s) committed.", sl.def->id,
                target->vessel_name ? target->vessel_name : "contact", v.count);
}

// End the volley. `target_lost` distinguishes the abort log from the completion log; the
// committed cooldown (if any) is kept either way -- ordnance already flew. Releases every
// surviving mount claim NOW rather than leaning on next tick's clear, so update_autopilot
// never sees a claim from a volley that no longer exists.
static void skill_volley_finish(game_state* s, b8 target_lost) {
    SkillVolley& v = s->skills.volley;
    const SkillDef* def = s->skills.slots[v.slot].def;
    Fleet& fleet = s->fleet_state.fleet;
    i32 abandoned = 0;
    for (i32 i = 0; i < v.count; ++i) {
        SkillVolleyEntry& e = v.entries[i];
        if (e.state != SKILL_ENTRY_PENDING) continue;
        ++abandoned;
        if (e.fleet_idx >= 0 && e.fleet_idx < fleet.count() &&
            e.hp_index >= 0 && e.hp_index < fleet.at(e.fleet_idx).ship.hardpoint_count)
            fleet.at(e.fleet_idx).ship.skill_claimed[e.hp_index] = FALSE;
    }
    if (v.fired == 0) {
        skill_deny(s, v.slot);
        action_log_push(s, "%s: no launcher could fire.", def ? def->name : "Skill");
    } else if (target_lost) {
        action_log_push(s, "%s: target destroyed.", def ? def->name : "Skill");
    } else {
        action_log_push(s, "%s: %d missile%s away.", def ? def->name : "Skill",
                        v.fired, v.fired == 1 ? "" : "s");
    }
    BS_LOG_INFO("skill_volley: finished after %.2fs -- %d fired, %d abandoned, %d invalidated%s.",
                v.age, v.fired, abandoned, v.count - v.fired - abandoned,
                target_lost ? " (target lost)" : "");
    v.active = FALSE;
}

void skill_system_update(game_state* s, f32 sim_dt) {
    if (!s) return;
    for (i32 k = 0; k < s->skills.slot_count; ++k)
        if (s->skills.slots[k].cooldown > 0.0f) s->skills.slots[k].cooldown -= sim_dt;
    if (s->skills.denied_timer > 0.0f) s->skills.denied_timer -= sim_dt;

    Fleet& fleet = s->fleet_state.fleet;
    // Claims are per-tick, the flak-screen contract: cleared here every tick and
    // re-asserted below for the entries still pending, so a claim can never outlive the
    // volley (or survive an abort) no matter how it ended. This tick runs BEFORE
    // update_autopilot, so the claims are in place when update_attack reads them.
    for (i32 i = 0; i < fleet.count(); ++i)
        for (i32 hp = 0; hp < SHIP_MAX_HARDPOINTS; ++hp)
            fleet.at(i).ship.skill_claimed[hp] = FALSE;

    SkillVolley& v = s->skills.volley;
    if (!v.active) return;
    // Target death = abort. The remaining volley has nothing to aim at; missiles already
    // in flight keep seeking on their own.
    CombatEntity* ce = skill_find_combat_entity(s, v.target);
    if (!ce) { skill_volley_finish(s, TRUE); return; }
    v.age += sim_dt;
    v.launch_timer -= sim_dt;
    // One pass over the committed tubes: every PENDING entry is re-validated, claimed and
    // kept training on the target; when a stagger beat is due, the first entry the shared
    // validator clears fires and consumes the beat. Transient states (reload, slew, arc,
    // range, capacitor) just stay pending -- they resolve on their own and the volley
    // timeout bounds the total wait.
    i32 pending = 0;
    for (i32 i = 0; i < v.count; ++i) {
        SkillVolleyEntry& e = v.entries[i];
        if (e.state != SKILL_ENTRY_PENDING) continue;
        // Permanent invalidations only: participant left the active fleet window, or the
        // tube was unmounted / replaced mid-volley (the attack_target convention).
        if (e.fleet_idx < 0 || e.fleet_idx >= fleet.count()) { e.state = SKILL_ENTRY_SKIPPED; continue; }
        FleetShip& fs = fleet.at(e.fleet_idx);
        Ship* sh = &fs.ship;
        Weapon* w = (e.hp_index >= 0 && e.hp_index < sh->hardpoint_count) ? sh->mounts[e.hp_index] : nullptr;
        if (!w || w->wkind != WEAPON_KIND_MISSILE) { e.state = SKILL_ENTRY_SKIPPED; continue; }
        ++pending;
        sh->skill_claimed[e.hp_index] = TRUE;   // the standing attack order yields this mount
        HierPos2 fire_origin = ship_hardpoint_fire_origin(sh, e.hp_index);
        Vec2 to_target = hierpos_diff(&v.target->origin, &fire_origin);
        f32  dist      = vec2_length(to_target);
        // EVERY pending tube trains while it waits, so slews converge in parallel and the
        // beat is spent on launching, not traversing.
        ship_turret_aim_at(sh, e.hp_index, to_target);
        if (v.launch_timer > 0.0f) continue;   // beat not due: keep training
        WeaponFireState st = ship_weapon_fire_state(sh, e.hp_index, to_target, dist);
        if (st != WEAPON_FIRE_READY) continue;             // transient: retry next beat
        if (!ship_try_spend_cap(sh, w->cap_cost())) continue;
        Vec2 aim_dir = weapon_lead_dir(fire_origin, v.target->origin, ce->velocity,
                                       w->projectile_speed(), to_target, dist);
        w->owner_faction_id = sh->faction_id;   // stamp attacker faction for hit attribution
        ship_hardpoint_fire(sh, e.hp_index, aim_dir, fs.flight.velocity, &s->projectiles);
        e.state = SKILL_ENTRY_FIRED;
        sh->skill_claimed[e.hp_index] = FALSE;  // released: the reload belongs to the autopilot again
        --pending;
        BS_LOG_INFO("skill_volley: launch %d/%d from '%s' hp%d at t+%.2fs.",
                    v.fired + 1, v.count, sh->vessel_name ? sh->vessel_name : "ship",
                    e.hp_index, v.age);
        if (++v.fired == 1)   // >= 1 missile flew: the fleet cooldown starts NOW, not at cast
            s->skills.slots[v.slot].cooldown = s->skills.slots[v.slot].def->cooldown;
        v.launch_timer = v.stagger;   // one launch per beat: the rest wait their turn
    }
    if (pending == 0)                        { skill_volley_finish(s, FALSE); return; }
    if (v.age >= SKILL_VOLLEY_TIMEOUT_S)     { skill_volley_finish(s, FALSE); }
}
