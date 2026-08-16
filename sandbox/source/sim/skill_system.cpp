#include "sim/skill_system.h"
#include "game.h"
#include "sim/skill_def.h"
#include "sim/weapon.h"
#include "sim/action_log.h"
#include <math/math_utils.h>

using namespace bs_math;

// How long the ripple waits on a mount still training onto the target before giving up on
// it. SLEWING is the only fire state worth waiting for -- it resolves by itself in under a
// traverse -- but an autopilot attack order on a DIFFERENT target re-asserts its engaging
// mount's aim after ours each frame and can hold a barrel off-axis forever; the timeout
// bounds that contention to one skipped tube instead of a stalled volley.
static constexpr f32 SKILL_SLEW_TIMEOUT_S = 1.5f;

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
            if (w->disabled || !w->ready()) continue;
            if (sh.flak_screen_claimed[hp]) continue;   // defense preempts offense
            if (out) {
                if (n >= cap) return n;
                out[n].fleet_idx = (i8)i;
                out[n].hp_index  = (i8)hp;
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
        // Arm only when at least one tube could fire -- an unarmable cast is a deny NOW,
        // not a targeting mode that can only whiff.
        if (skill_system_collect_tubes(s, sl.def, nullptr, 0) == 0) {
            skill_deny(s, slot);
            action_log_push(s, "%s: no missile tube ready.", sl.def->name);
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
        action_log_push(s, "%s: no missile tube ready.", sl.def->name);
        return;
    }
    v.active  = TRUE;
    v.slot    = (i8)slot;
    v.target  = target;
    v.stagger = sl.def->stagger;
    // launch_timer starts at 0: the first launch fires on the next skill tick.
}

// End the volley. `target_lost` distinguishes the abort log from the completion log; the
// committed cooldown (if any) is kept either way -- ordnance already flew.
static void skill_volley_finish(game_state* s, b8 target_lost) {
    SkillVolley& v = s->skills.volley;
    const SkillDef* def = s->skills.slots[v.slot].def;
    if (v.fired == 0) {
        skill_deny(s, v.slot);
        action_log_push(s, "%s: no launcher could fire.", def ? def->name : "Skill");
    } else if (target_lost) {
        action_log_push(s, "%s: target destroyed.", def ? def->name : "Skill");
    } else {
        action_log_push(s, "%s: %d missile%s away.", def ? def->name : "Skill",
                        v.fired, v.fired == 1 ? "" : "s");
    }
    v.active = FALSE;
}

void skill_system_update(game_state* s, f32 sim_dt) {
    if (!s) return;
    for (i32 k = 0; k < s->skills.slot_count; ++k)
        if (s->skills.slots[k].cooldown > 0.0f) s->skills.slots[k].cooldown -= sim_dt;
    if (s->skills.denied_timer > 0.0f) s->skills.denied_timer -= sim_dt;

    SkillVolley& v = s->skills.volley;
    if (!v.active) return;
    // Target death = abort. The remaining ripple has nothing to aim at; missiles already
    // in flight keep seeking on their own.
    CombatEntity* ce = skill_find_combat_entity(s, v.target);
    if (!ce) { skill_volley_finish(s, TRUE); return; }
    Fleet& fleet = s->fleet_state.fleet;
    v.launch_timer -= sim_dt;
    while (v.active && v.next < v.count) {
        SkillVolleyEntry e = v.entries[v.next];
        // Re-validate the participant: the active window can shrink and loadouts mutate
        // mid-ripple. A stale entry skips in one frame; only real launches pay the beat.
        if (e.fleet_idx < 0 || e.fleet_idx >= fleet.count()) { ++v.next; v.slew_wait = 0.0f; continue; }
        FleetShip& fs = fleet.at(e.fleet_idx);
        Ship* sh = &fs.ship;
        Weapon* w = (e.hp_index >= 0 && e.hp_index < sh->hardpoint_count) ? sh->mounts[e.hp_index] : nullptr;
        if (!w || w->wkind != WEAPON_KIND_MISSILE || sh->flak_screen_claimed[e.hp_index]) {
            ++v.next; v.slew_wait = 0.0f; continue;
        }
        HierPos2 fire_origin = ship_hardpoint_fire_origin(sh, e.hp_index);
        Vec2 to_target = hierpos_diff(&v.target->origin, &fire_origin);
        f32  dist      = vec2_length(to_target);
        // Keep the barrel training while this entry is due or waiting on its beat.
        ship_turret_aim_at(sh, e.hp_index, to_target);
        if (v.launch_timer > 0.0f) break;   // stagger: the current entry is not due yet
        WeaponFireState st = ship_weapon_fire_state(sh, e.hp_index, to_target, dist);
        if (st == WEAPON_FIRE_SLEWING) {
            v.slew_wait += sim_dt;
            if (v.slew_wait < SKILL_SLEW_TIMEOUT_S) break;
            ++v.next; v.slew_wait = 0.0f; continue;   // never converged: skip the tube
        }
        if (st != WEAPON_FIRE_READY) { ++v.next; v.slew_wait = 0.0f; continue; }   // reload/arc/range/cap/dead
        if (!ship_try_spend_cap(sh, w->cap_cost())) { ++v.next; v.slew_wait = 0.0f; continue; }
        Vec2 aim_dir = weapon_lead_dir(fire_origin, v.target->origin, ce->velocity,
                                       w->projectile_speed(), to_target, dist);
        w->owner_faction_id = sh->faction_id;   // stamp attacker faction for hit attribution
        ship_hardpoint_fire(sh, e.hp_index, aim_dir, fs.flight.velocity, &s->projectiles);
        if (++v.fired == 1)   // >= 1 missile flew: the fleet cooldown starts NOW, not at cast
            s->skills.slots[v.slot].cooldown = s->skills.slots[v.slot].def->cooldown;
        ++v.next; v.slew_wait = 0.0f;
        v.launch_timer = v.stagger;   // ripple: the next tube waits its beat
    }
    if (v.active && v.next >= v.count) skill_volley_finish(s, FALSE);
}
