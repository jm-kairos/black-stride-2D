#include "sim/ai_ship.h"

#include "game.h"                 // full game_state (Ship, ShipFlight, projectiles, galaxy)

#include "sim/galaxy_map.h"       // galaxy_nearest_node

#include "sim/galaxy_history.h"   // owner_at_node / faction_is_hostile / player_raid

#include "sim/ship_mission.h"     // ship_mission_notify_destroyed (macro <-> local handoff)

#include "render/ship_visual.h"          // ship_visual_resolve_textures

#include "sim/steering.h"         // shared steering (arrive/seek/standoff + apply)

#include "sim/station_market.h"   // market writeback (miners deliver, ambient traders micro-haul)

#include "sim/discovery.h"        // discovery_npc_is_known (single-ship discovery system)

#include "sim/weapon_def.h"       // weapon_registry_find / weapon_instantiate (Phase 7 NPC guns)

#include <core/logger.h>          // BS_LOG_ERROR

#include <math.h>



using namespace bs_math;

using GalaxyState = game_state::GalaxyState;   // nested alias for the mission-handoff helpers



// =====================================================================================

// Per-archetype tuning. Distances scaled to the LOCAL combat scale (player SHIP_MAX_SPEED == 800

// units/s), not the galaxy scale.

// =====================================================================================

// Phase 7: how long (seconds) an agent remembers a lost contact and keeps sweeping toward it.
static const f32 AI_INVESTIGATE_HOLD = 8.0f;

// Phase 8: frames between target re-scans for a given agent. Sensing is O(agents x entities), so
// at a 256-hull pool an every-frame scan is ~65k distance tests per frame. Targets do not change
// meaningfully between frames, so results are cached and refreshed on this staggered cadence.
static const i32 AI_SENSE_INTERVAL = 4;

static const AiProfile g_profiles[ARCHETYPE_COUNT] = {

    // max_spd accel turn  sensor  engage standoff minR  patSpd patRad aggr flee  leash  fireT  states

    {  800.f, 500.f, 3.0f, 60000.f, 45000.f, 12000.f, 1500.f, 420.f, 12000.f, 0.85f, 0.25f, 80000.f, 1.5f, 0xFFFF }, // PATROL

    {  820.f, 560.f, 3.2f, 70000.f, 50000.f, 14000.f, 1500.f, 500.f, 14000.f, 1.00f, 0.15f, 90000.f, 1.2f, 0xFFFF }, // WARSHIP

    {  950.f, 640.f, 3.6f, 65000.f, 40000.f,  9000.f, 1200.f, 650.f, 12000.f, 1.00f, 0.30f, 90000.f, 1.4f, 0xFFFF }, // INTERCEPTOR

    {  760.f, 420.f, 2.6f, 55000.f, 20000.f, 18000.f, 2000.f, 520.f, 16000.f, 0.05f, 0.90f, 70000.f, 3.0f, 0xFFFF }, // TRADER

    {  900.f, 560.f, 3.4f, 90000.f, 15000.f, 30000.f, 2000.f, 560.f, 20000.f, 0.10f, 0.80f, 90000.f, 3.0f, 0xFFFF }, // SCOUT

    {  840.f, 560.f, 3.2f, 70000.f, 48000.f, 12000.f, 1400.f, 520.f, 14000.f, 1.00f, 0.20f, 99999.f, 1.2f, 0xFFFF }, // PIRATE

    {  600.f, 380.f, 2.4f, 45000.f,     0.f,     0.f,    0.f, 300.f,  9000.f, 0.00f, 0.90f, 60000.f, 3.0f, 0xFFFF }, // MINER

};



const AiProfile& ai_profile(u8 archetype) {

    if (archetype >= ARCHETYPE_COUNT) archetype = ARCHETYPE_PATROL;

    return g_profiles[archetype];

}



// ---- Deterministic-ish per-agent RNG (splitmix64) -----------------------------------------

static inline u64 sm64(u64& st) {

    u64 z = (st += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;

    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;

    return z ^ (z >> 31);

}

static inline f32 rf(u64& st) { return (f32)(sm64(st) & 0xFFFFFF) / (f32)0xFFFFFF; }

static inline f32 rrange(u64& st, f32 lo, f32 hi) { return lo + rf(st) * (hi - lo); }



// ---- Faction colour (civ banner, or pirate red) -------------------------------------------

static bs_color faction_tint(const game_state* s, i16 faction) {

    if (faction >= 0 && faction < s->galaxy.civ_count) return s->galaxy.civs[faction].color;

    return bs_color{ 0.95f, 0.35f, 0.30f, 1.0f };

}



// ---- Phase 7: per-archetype hulls + weapons ------------------------------------------------

// Hulls are DATA: each archetype names a .ship file, so a new ship kind is a new asset plus one

// row here -- no new code. Missing files fall back to the shared raider hull, so the sim never

// depends on art existing.

static const char* archetype_hull_path(u8 archetype) {

    switch (archetype) {

        case ARCHETYPE_WARSHIP:     return "assets/ships/npc/npc_warship.ship";

        case ARCHETYPE_PATROL:      return "assets/ships/npc/npc_escort.ship";

        case ARCHETYPE_INTERCEPTOR: return "assets/ships/npc/npc_escort.ship";

        case ARCHETYPE_SCOUT:       return "assets/ships/npc/npc_escort.ship";

        case ARCHETYPE_TRADER:      return "assets/ships/npc/npc_hauler.ship";

        case ARCHETYPE_MINER:       return "assets/ships/npc/npc_hauler.ship";

        default:                    return "assets/enemy_ship.ship";   // PIRATE keeps the raider hull

    }

}



// Armament is data too: one registry weapon id per archetype (nullptr = unarmed civilian).

static const char* archetype_weapon_id(u8 archetype) {

    switch (archetype) {

        case ARCHETYPE_WARSHIP:     return "gauss_mk1";        // heavy alpha line gun

        case ARCHETYPE_PATROL:      return "autocannon_mk1";   // cheap volume fire

        case ARCHETYPE_INTERCEPTOR: return "autocannon_mk1";

        case ARCHETYPE_PIRATE:      return "autocannon_mk1";

        default:                    return nullptr;            // traders, miners and scouts fly unarmed

    }

}



// ONE shared weapon instance per archetype (never per agent -- the pool stays allocation-free).

// Rate limiting is the agent's own AiProfile::fire_period via NpcShip::fire_cd, so the shared

// instance's internal cooldown is cleared before each shot.

static Weapon* g_npc_weapons[ARCHETYPE_COUNT] = { nullptr };



// =====================================================================================

void ai_ships_init(game_state* s) {

    s->npc_ship_count   = 0;

    s->npc_spawned_node = -1;

    s->npc_pop_timer    = 0.0f;

    s->npc_template_ready = FALSE;

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) s->npc_ships[i].active = FALSE;



    // Shared hull template: loaded once, struct-copied on each spawn (visual texture handles are

    // shared -- safe for rendering, never freed per-agent). This is the FALLBACK hull; Phase 7

    // loads a per-archetype template on top of it below.

    if (ship_load(&s->npc_template, "assets/enemy_ship.ship")) {

        ship_visual_resolve_textures(&s->npc_template.visual);

        s->npc_template.faction = VESSEL_PIRATE;

        s->npc_template.faction_id = FACTION_PIRATE;

        s->npc_template.active_weapon_idx = -1;

        for (i32 w = 0; w < SHIP_MAX_HARDPOINTS; ++w) s->npc_template.mounts[w] = nullptr;

        s->npc_template.glow = s->render.glow_params;

        s->npc_template.radiation_emission = 0.05f;

        s->npc_template_ready = TRUE;

    } else {

        BS_LOG_ERROR("ai_ships_init: failed to load NPC hull template.");

    }



    // ---- Phase 7: per-archetype hulls -------------------------------------------------------

    // One template per archetype so a miner, a freighter and a line warship are told apart at a

    // glance. Any archetype whose asset is missing simply keeps the fallback hull.

    i32 hulls_loaded = 0;

    for (i32 a = 0; a < ARCHETYPE_COUNT; ++a) {

        Ship& t = s->npc_hulls[a];

        s->npc_hull_ready[a] = FALSE;

        if (!s->npc_template_ready) continue;

        const char* path = archetype_hull_path((u8)a);

        if (!ship_load(&t, path)) {

            BS_LOG_WARN("ai_ships_init: archetype %d hull '%s' missing; using the fallback hull.", a, path);

            continue;

        }

        ship_visual_resolve_textures(&t.visual);

        t.faction           = VESSEL_PIRATE;      // binary friendly-fire enum; stance uses faction_id

        t.faction_id        = FACTION_PIRATE;

        t.active_weapon_idx = -1;

        for (i32 w = 0; w < SHIP_MAX_HARDPOINTS; ++w) t.mounts[w] = nullptr;

        t.glow               = s->render.glow_params;

        t.radiation_emission = 0.05f;

        s->npc_hull_ready[a] = TRUE;

        ++hulls_loaded;

    }



    // ---- Phase 7: NPCs fire through the real Weapon system ----------------------------------

    // Shared instances built from the same .weapon registry the player draws from, so NPC shells

    // carry real damage/speed/HP values and interact with point-defense and flak.

    i32 guns_loaded = 0;

    for (i32 a = 0; a < ARCHETYPE_COUNT; ++a) {

        if (g_npc_weapons[a]) { delete g_npc_weapons[a]; g_npc_weapons[a] = nullptr; }

        const char* wid = archetype_weapon_id((u8)a);

        if (!wid) continue;

        const WeaponDef* def = weapon_registry_find(&s->weapon_registry, wid);

        if (!def) { BS_LOG_WARN("ai_ships_init: weapon '%s' not in the registry.", wid); continue; }

        g_npc_weapons[a] = weapon_instantiate(def, VESSEL_PIRATE);

        if (g_npc_weapons[a]) ++guns_loaded;

    }



    BS_LOG_INFO("ShipAI hulls: %d/%d archetype hull(s) loaded, %d archetype weapon(s) armed from the registry",

                hulls_loaded, (i32)ARCHETYPE_COUNT, guns_loaded);

}



void ai_ships_shutdown(game_state* s) {
    (void)s;

    for (i32 a = 0; a < ARCHETYPE_COUNT; ++a) {

        delete g_npc_weapons[a];

        g_npc_weapons[a] = nullptr;

    }

}



// Spawn one agent at an explicit world position, tagged to `faction`, with the given archetype. Its

// home (loiter / return anchor) is the spawn point, so the population is LOCAL to wherever the player

// currently is within the system (dispersion follows the player; distant ships are culled). Returns

// the pool slot, or -1 if the pool is full.

static i32 spawn_npc(game_state* s, i16 faction, HierPos2 pos, i32 home_node, u8 archetype, u64 seed) {

    if (!s->npc_template_ready) return -1;

    i32 slot = -1;

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) if (!s->npc_ships[i].active) { slot = i; break; }

    if (slot < 0) return -1;

    const AiProfile& p = ai_profile(archetype);

    NpcShip& n = s->npc_ships[slot];

    n.ship = (archetype < ARCHETYPE_COUNT && s->npc_hull_ready[archetype])

                 ? s->npc_hulls[archetype]        // Phase 7: archetype hull (struct copy)

                 : s->npc_template;               // fallback: shared raider hull

    n.ship.faction = VESSEL_PIRATE;           // binary friendly-fire enum (stance uses faction_id)

    n.ship.faction_id = faction;

    u64 st = seed ^ (0x9E3779B97F4A7C15ull * (u64)(slot + 1));

    n.rng = st;

    n.ship.origin = pos;

    n.ship.angle  = rrange(st, 0.0f, 2.0f * BS_PI);

    n.flight.velocity = Vec2{ 0.0f, 0.0f };

    n.flight.angular_velocity = 0.0f;

    n.faction   = faction;

    n.archetype = archetype;

    n.state     = AI_PATROL;
    n.hp = n.max_hp = (archetype == ARCHETYPE_MINER) ? 25.0f : (archetype == ARCHETYPE_TRADER ? 30.0f : 40.0f);

    // Phase 7: hull toughness now tracks the archetype's role, not just civilian/combatant.
    if (archetype == ARCHETYPE_WARSHIP)          n.hp = n.max_hp = 70.0f;   // line combatant
    else if (archetype == ARCHETYPE_INTERCEPTOR) n.hp = n.max_hp = 32.0f;   // fast, fragile
    else if (archetype == ARCHETYPE_SCOUT)       n.hp = n.max_hp = 22.0f;   // eyes only

    n.last_contact  = pos;
    n.contact_timer = 0.0f;
    n.sense_countdown = (i16)(slot % AI_SENSE_INTERVAL);   // stagger the first scan across the pool
    n.wing_leader   = -1;                                  // independent until a wing is assigned
    n.wing_slot     = 0;

    n.home      = pos;                        // loiter / return anchor = local spawn point

    n.home_node = home_node;

    n.orbit_radius = p.patrol_radius * rrange(st, 0.6f, 1.4f);

    n.patrol_wp = pos;

    n.target_ce = -1;

    n.last_seen = pos;

    n.timer       = rrange(st, 0.0f, 2.0f * BS_PI);

    n.state_timer = 0.0f;

    n.fire_cd     = rrange(st, 0.0f, p.fire_period);

    n.work_asteroid    = -1;

    n.work_timer  = 0.0f;

    n.work_station = -1;

    n.cargo_units  = 0.0f;

    n.cargo_good   = 0;

    // Discovery system: key each agent by its (home_node, spawn_seed). Already-discovered agents

    // re-materialize identified so a re-visited system keeps its scanned ships.

    n.spawn_seed  = seed;

    n.discovered  = discovery_npc_is_known(s, home_node, seed);

    n.mission_id  = -1;                        // ambient/garrison by default; macro handoff sets it

    n.active = TRUE;

    if (slot + 1 > s->npc_ship_count) s->npc_ship_count = slot + 1;

    return slot;

}



// A deterministic placement anchor for `role` #index within `node`, derived from the star + planet

// orbit radii. Patrols guard the star/planet orbits; miners work the outer asteroid belt; traders sit

// on planet/station orbits. Positions belong to the SYSTEM (not the player), so ships exist where they

// should when the player arrives -- they are not conjured in a ring around the arrival point.

static HierPos2 system_anchor(game_state* s, i32 node, u8 role, i32 index, u64 seed) {

    const GalaxyNode& gn = s->galaxy.nodes[node];

    HierPos2 star = gn.galaxy_center;

    i32 oc = gn.orbit_count;

    u64 st = seed ^ (0x9E3779B97F4A7C15ull * (u64)(index + 1));

    f32 ang = rrange(st, 0.0f, 2.0f * BS_PI);

    f32 radius;

    if (role == ARCHETYPE_MINER) {

        f32 belt = (oc > 0) ? gn.orbit_radii[oc - 1] : 80000.0f;   // outer belt

        radius = belt * rrange(st, 0.90f, 1.15f);

    } else if (role == ARCHETYPE_TRADER) {

        f32 r = (oc > 0) ? gn.orbit_radii[index % oc] : 50000.0f;  // a planet lane

        radius = r * rrange(st, 0.95f, 1.05f);

    } else { // PATROL / combatants: half at planet orbits, half on a close star ring

        if (oc > 0 && (index % 2 == 0)) radius = gn.orbit_radii[index % oc] * rrange(st, 0.90f, 1.10f);

        else                            radius = ((oc > 0) ? gn.orbit_radii[0] * 0.5f : 40000.0f) * rrange(st, 0.7f, 1.3f);

    }

    return hierpos_add_vec2(&star, vec2_rotate(Vec2{ radius, 0.0f }, ang));

}



// ---- Phase 10: combat wings (triangular formation) ------------------------------------------

// A wing is one leader plus two wingmen holding a triangle: leader at the apex, wingmen off each

// rear quarter. Offsets are LEADER-LOCAL (ship convention: angle 0 => nose +Y, so -Y is astern)

// and get rotated by the leader's heading, so the triangle turns with the formation.

static const f32 WING_SPACING = 2600.0f;   // ~3 hull widths: reads as a formation at gameplay zoom

static Vec2 wing_slot_offset(u8 slot) {

    switch (slot) {

        case 1:  return Vec2{ -WING_SPACING,  -WING_SPACING };   // rear-left

        case 2:  return Vec2{  WING_SPACING,  -WING_SPACING };   // rear-right

        default: return Vec2{  0.0f, 0.0f };                     // leader / apex

    }

}



// Group a just-spawned set of combat hulls into wings of three.

static void assign_wings(game_state* s, const i32* slots, i32 count) {

    for (i32 i = 0; i < count; i += 3) {

        i32 lead = slots[i];

        if (lead < 0) continue;

        s->npc_ships[lead].wing_leader = -1;

        s->npc_ships[lead].wing_slot   = 0;

        for (i32 k = 1; k < 3 && (i + k) < count; ++k) {

            i32 w = slots[i + k];

            if (w < 0) continue;

            s->npc_ships[w].wing_leader = (i16)lead;

            s->npc_ships[w].wing_slot   = (u8)k;

        }

    }

}



// Materialize the FULL population of an owned system at its anchors (patrols at star/planets, miners at

// the belt, traders on planet lanes). Deterministic per node; all civ-tagged. Debug: everything is spawned

// on entry so the whole dispersion is visible immediately.

static void materialize_system(game_state* s, i32 node, i16 owner) {

    u64 seed = (u64)(node + 1) * 0x9E3779B97F4A7C15ull ^ 0xC1FF1EE5ull;

    i32 oc = s->galaxy.nodes[node].orbit_count;

    f32 power = s->galaxy.civs[owner].power;



    // Phase 9 (populate): a settled system should look settled. Counts scale with what the system
    // actually has -- garrison strength, orbital real estate, station count and civ power -- so
    // core worlds bustle while frontier systems stay thin.
    i32 station_ct = 0;

    {   // station count drives how much civilian traffic the system can support
        StationLayoutEntry layout[SYSTEM_STATION_MAX];

        station_ct = galaxy_node_station_layout(s, node, layout, SYSTEM_STATION_MAX);

    }

    i32 patrols = galaxy_history_garrison_at(s, node);   if (patrols > 20) patrols = 20; if (patrols < 3) patrols = 3;

    i32 miners  = 10 + oc * 2 + (i32)(seed % 6ull);      if (miners  > 24) miners  = 24;

    i32 traders = 4 + station_ct + (i32)(power * 0.3f) + (i32)((seed >> 8) % 4ull); if (traders > 14) traders = 14;



    // ---- Phase 7: SCOUT + INTERCEPTOR come alive --------------------------------------------

    // Interceptors are pirate hunters: a civ posts them where its lanes are actually bleeding, so

    // the Phase 6 node-risk table drives who shows up -- risk at this node OR at any node it feeds.

    // Scouts are recon: they watch any border, whether that is a rival at war or the lawless dark.

    f32 risk = ship_mission_node_risk(s, node);

    b8  war_border = FALSE, wild_border = FALSE;

    {

        const GalaxyLaneGraph& lg = s->galaxy.lanes;

        if (lg.adj_start && lg.adj_neighbor) {

            i32 a0 = lg.adj_start[node], a1 = lg.adj_start[node + 1];

            for (i32 a = a0; a < a1; ++a) {

                i32 nb = lg.adj_neighbor[a];

                i16 no = s->galaxy.node_owner[nb];

                f32 nr = ship_mission_node_risk(s, nb);

                if (nr > risk) risk = nr;                       // a neighbour under attack is our problem too

                if (no < 0)                                             wild_border = TRUE;

                else if (no != owner && galaxy_history_civ_at_war(s, (i32)owner, (i32)no)) war_border = TRUE;

            }

        }

    }

    i32 interceptors = (risk > 0.05f) ? 1 + (i32)(risk * 1.5f) : 0;

    if (interceptors > 4) interceptors = 4;

    i32 scouts = war_border ? 2 : (wild_border ? 1 : 0);



    i32 idx = 0;

    // Combat hulls are collected as they spawn so they can be grouped into wings of three.
    i32 wing_slots[64]; i32 wing_n = 0;

    for (i32 k = 0; k < patrols; ++k, ++idx) {

        i32 sl = spawn_npc(s, owner, system_anchor(s, node, ARCHETYPE_PATROL, idx, seed), node, ARCHETYPE_PATROL, seed + (u64)idx);

        if (sl >= 0 && wing_n < 64) wing_slots[wing_n++] = sl;

    }

    for (i32 k = 0; k < interceptors; ++k, ++idx) {

        i32 sl = spawn_npc(s, owner, system_anchor(s, node, ARCHETYPE_INTERCEPTOR, idx, seed), node, ARCHETYPE_INTERCEPTOR, seed + (u64)idx);

        if (sl >= 0 && wing_n < 64) wing_slots[wing_n++] = sl;

    }

    assign_wings(s, wing_slots, wing_n);

    for (i32 k = 0; k < scouts; ++k, ++idx)

        spawn_npc(s, owner, system_anchor(s, node, ARCHETYPE_SCOUT, idx, seed), node, ARCHETYPE_SCOUT, seed + (u64)idx);

    for (i32 k = 0; k < miners; ++k, ++idx)

        spawn_npc(s, owner, system_anchor(s, node, ARCHETYPE_MINER, idx, seed), node, ARCHETYPE_MINER, seed + (u64)idx);

    for (i32 k = 0; k < traders; ++k, ++idx)

        spawn_npc(s, owner, system_anchor(s, node, ARCHETYPE_TRADER, idx, seed), node, ARCHETYPE_TRADER, seed + (u64)idx);



    // Raiders do not stay politely in the dark: where a civ's lanes are actually bleeding (Phase 6
    // risk, or a lawless system next door) a pirate band works the system itself. This is what puts
    // piracy in front of the player inside civ space, and gives the local patrols something to hunt.
    i32 lurkers = 0;

    if (risk > 0.05f)      lurkers = 1 + (i32)(risk * 2.0f);

    else if (wild_border)  lurkers = 1 + (i32)((seed >> 16) % 2ull);

    if (lurkers > 5) lurkers = 5;

    for (i32 k = 0; k < lurkers; ++k, ++idx)

        spawn_npc(s, FACTION_PIRATE, system_anchor(s, node, ARCHETYPE_PIRATE, idx, seed), node, ARCHETYPE_PIRATE, seed + (u64)idx);



    if (interceptors > 0 || scouts > 0 || lurkers > 0)

        BS_LOG_INFO("ShipAI garrison: node %d fielded %d interceptor(s) (lane risk %.2f), %d scout(s) (%s border), %d pirate lurker(s)",

                    node, interceptors, risk, scouts, war_border ? "war" : (wild_border ? "lawless" : "quiet"), lurkers);

}



// Wild / unclaimed space is not empty: a deterministic band of pirate raiders (FACTION_PIRATE)
// camps the star ring of a lawless system and engages anyone on sight (pairwise stance: pirates are
// hostile to all factions). Every wild system carries a crew -- lawless space should feel lawless.
static void materialize_wild_system(game_state* s, i32 node) {

    u64 st = (u64)(node + 1) * 0x9E3779B97F4A7C15ull ^ 0xBADC0FFEEull;

    i32 pirates = 3 + (i32)(sm64(st) % 6ull);   // 3..8 raiders

    i32 wing_slots[16]; i32 wing_n = 0;

    for (i32 k = 0; k < pirates; ++k) {

        i32 sl = spawn_npc(s, FACTION_PIRATE, system_anchor(s, node, ARCHETYPE_PIRATE, k, st), node, ARCHETYPE_PIRATE, st + (u64)(k + 1));

        if (sl >= 0 && wing_n < 16) wing_slots[wing_n++] = sl;

    }

    assign_wings(s, wing_slots, wing_n);   // raiders hunt in packs of three

    if (pirates > 0)

        BS_LOG_INFO("ShipAI wild: node %d materialized %d pirate raider(s)", node, pirates);

}



// ---- Population manager: materialize the CURRENT owned system's full population at its anchors ----------

// Per-system (not player-relative): ships are placed at the system's real locations and only re-materialized

// when the player crosses into a DIFFERENT system. So an FTL jump lands you among the destination's existing

// fleet -- nothing spawns in a ring around you.

static void ai_ships_populate(game_state* s) {

    HierPos2 flag = s->fleet_state.fleet.flagship().ship.origin;

    i32 node = galaxy_nearest_node(s, &flag);

    if (node == s->npc_spawned_node) return;   // same system -> keep the existing population



    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) s->npc_ships[i].active = FALSE;   // leaving the old system

    s->npc_ship_count = 0;

    s->npc_spawned_node = node;



    i32 owner = galaxy_history_owner_at_node(s, node);

    if (owner < 0 || owner >= s->galaxy.civ_count || s->galaxy.civs[owner].status != 0) {

        materialize_wild_system(s, node);   // lawless space: pirates instead of a civ population

        return;

    }

    materialize_system(s, node, (i16)owner);

}



// ---- Macro <-> local handoff (Phase 2: full traveler materialisation) -----------------------------

// Contracts issued by mission-hub stations get a live NpcShip form whenever their in-system portion
// happens in the player's current system. ORIGIN_DOCK binds the CLOSEST unassigned ambient trader
// (the contract is "picked up" locally; if none is free the contract waits). Every other in-system
// stage (acquire / depart / cross / final approach / market dock) SPAWNS the traveler itself at the
// macro position. While bound (ship_slot >= 0) the live agent owns motion: it flies the macro's
// current leg (AI_TRAVEL_LEG) or runs the docking loop, mirrors its position into ShipMission::pos,
// and hands stage completion back through local_ready. Native traders released at contract end
// return to ambient duty; travelers that jump out or belong elsewhere despawn with the ship.



// Station anchor of the mission's CURRENT dock stage (origin while loading, destination while

// unloading). Positions/radii are cached on the mission at contract issue.

static void mission_dock_anchor(const ShipMission& m, HierPos2* out_pos, f32* out_radius) {

    if (m.stage == MISSION_STAGE_MARKET_DOCK) { *out_pos = m.dest_station_pos; *out_radius = m.dest_station_radius; }

    else                                      { *out_pos = m.station_pos;      *out_radius = m.station_radius; }

}



// Phase 2: TRUE while mission `m` should have a live agent in galaxy node `node`: docking at a
// station here, or flying an in-system leg here (acquire / depart / cross / final approach).
// JUMP (interstellar) and COOLDOWN never materialize.
static b8 mission_live_here(const ShipMission& m, i32 node) {

    if (!m.active || node < 0 || m.at_node != node) return FALSE;

    switch (m.stage) {

        case MISSION_STAGE_ORIGIN_DOCK:    return m.home_node == node;

        case MISSION_STAGE_MARKET_DOCK:    return m.dest_node == node;

        case MISSION_STAGE_ACQUIRE:

        case MISSION_STAGE_TO_JUMP:

        case MISSION_STAGE_CROSS:

        case MISSION_STAGE_FINAL_APPROACH: return TRUE;

        default:                           return FALSE;

    }

}



static void ai_ships_sync_missions(game_state* s) {

    GalaxyState& g = s->galaxy;

    // The galaxy node the player currently occupies (current_system is a cache SLOT, not a node).

    i32 node_here = (g.current_system >= 0 && g.current_system < g.system_count)

                        ? g.cache_node[g.current_system] : -1;



    // Stale-link hygiene (Phase 2): a linked agent whose contract is gone, jumped out, or disagrees
    // on the binding is resolved here. A hull that JUMPED OUT is legitimately gone -- it left with
    // the ship. Everything else STAYS as a local agent rather than blinking out of existence in
    // front of the player: freighters fall back to ambient intra-system hauling, raiders keep
    // hunting the system they just hit, warships hold station. Pool headroom guards the fallback so
    // a busy hub can never starve the agent pool (beyond the limit, foreign hulls are culled as
    // before). Ships popping out of existence at a dock is exactly the "decorative" behaviour the
    // autonomous-universe work exists to remove.

    i32 live_count = 0;

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) if (s->npc_ships[i].active) ++live_count;

    const i32 KEEP_LIMIT = (NPC_SHIP_MAX * 3) / 4;

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

        NpcShip& n = s->npc_ships[i];

        if (!n.active || n.mission_id < 0) continue;

        const ShipMission* m = (g.missions && n.mission_id < g.mission_count) ? &g.missions[n.mission_id] : nullptr;

        b8 stale = !m || !mission_live_here(*m, node_here) || m->ship_slot != i;

        if (!stale) continue;

        b8 jumped_out = m && m->active && m->stage == MISSION_STAGE_JUMP;

        b8 native     = (node_here >= 0) && (n.home_node == node_here);

        n.mission_id = -1;

        if (jumped_out) {

            n.active = FALSE;   // the traveler departed: the hull leaves with it

            --live_count;

            BS_LOG_INFO("ShipAI travel: mission %d jumped out (live agent %d released)", (i32)(m - g.missions), i);

            continue;

        }

        if (native) {

            if (n.state == AI_TRADE_DOCK || n.state == AI_TRADE_DOCKED || n.state == AI_TRAVEL_LEG) {

                n.state = AI_PATROL; n.state_timer = 0.0f;   // native hull returns to ambient duty

            }

            continue;

        }

        if (node_here >= 0 && live_count <= KEEP_LIMIT) {

            // Foreign hull, mission over, room in the pool: it settles in as a local. Re-anchor its
            // loiter/leash here so it behaves like a resident instead of trying to fly home.
            n.home        = n.ship.origin;

            n.home_node   = node_here;

            n.state       = AI_PATROL;

            n.state_timer = 0.0f;

            static i32 s_settle_n = 0;

            if ((++s_settle_n % 8) == 1) {

                BS_LOG_INFO("ShipAI travel: agent %d (archetype %u, faction %d) finished its run and settled at node %d as a local (settle #%d)",

                            i, (u32)n.archetype, (i32)n.faction, node_here, s_settle_n);

            }

            continue;

        }

        n.active = FALSE;   // pool pressure: cull the foreign hull as before

        --live_count;

        static i32 s_foreign_n = 0;

        if ((++s_foreign_n % 8) == 1) {

            BS_LOG_INFO("ShipAI travel: agent %d (archetype %u) culled at node %d for pool pressure (%d live) -- cull #%d",

                        i, (u32)n.archetype, node_here, live_count, s_foreign_n);

        }

    }

    if (!g.missions) return;

    // Mirror pass: clear mission->ship links whose ship no longer agrees.

    for (i32 mi = 0; mi < g.mission_count; ++mi) {

        ShipMission& m = g.missions[mi];

        if (m.ship_slot < 0) continue;

        b8 ok = m.active && m.ship_slot < NPC_SHIP_MAX &&

                s->npc_ships[m.ship_slot].active &&

                s->npc_ships[m.ship_slot].mission_id == mi;

        if (!ok) m.ship_slot = -1;

    }

    if (node_here < 0) return;



    // Bind or spawn the live form of every unbound contract present in this system (Phase 2):
    // ORIGIN_DOCK binds the CLOSEST free ambient trader (the contract is "picked up" locally);
    // every other live stage (in-system legs, market dock) spawns the traveler itself at the
    // macro position — a visiting ship the player can meet, follow, or destroy.

    for (i32 mi = 0; mi < g.mission_count; ++mi) {

        ShipMission& m = g.missions[mi];

        if ((m.archetype != ARCHETYPE_TRADER && m.archetype != ARCHETYPE_WARSHIP &&

             m.archetype != ARCHETYPE_PIRATE) || m.ship_slot >= 0) continue;

        if (!mission_live_here(m, node_here)) continue;

        if (m.stage != MISSION_STAGE_ORIGIN_DOCK) {

            // In-transit / visiting trader: materialize it exactly where the macro says it is.

            HierPos2 pos = ship_mission_position(s, m);

            i16 owner = (m.owner >= 0) ? m.owner : FACTION_PIRATE;

            i32 slot = spawn_npc(s, owner, pos, m.home_node, m.archetype, m.seed);

            if (slot < 0) continue;   // pool full: the macro keeps integrating this leg itself

            NpcShip& t = s->npc_ships[slot];

            t.mission_id  = mi;

            t.state       = (m.stage == MISSION_STAGE_MARKET_DOCK) ? AI_TRADE_DOCK : AI_TRAVEL_LEG;

            t.state_timer = 0.0f;

            m.ship_slot   = slot;

            // Phase 10: a military column of strength N arrives as a WING, not a lone hull. The
            // mission-bound leader keeps the macro binding; its escorts fly the triangle as
            // ordinary wingmen (no mission of their own), so the abstract strength number finally
            // shows up on screen as ships.
            if (m.objective == OBJ_RAID || m.objective == OBJ_REINFORCE || m.objective == OBJ_PATROL) {

                i32 wings = (i32)m.cargo_units - 1;

                if (wings > 2) wings = 2;

                for (i32 w = 0; w < wings; ++w) {

                    HierPos2 wp = hierpos_add_vec2(&pos, wing_slot_offset((u8)(w + 1)));

                    i32 ws = spawn_npc(s, owner, wp, m.home_node, m.archetype, m.seed + (u64)(w + 17));

                    if (ws < 0) break;

                    s->npc_ships[ws].wing_leader = (i16)slot;

                    s->npc_ships[ws].wing_slot   = (u8)(w + 1);

                }

            }

            BS_LOG_INFO("ShipAI travel: mission %d materialized in-system (stage %u, node %d, agent %d)",
                        mi, (u32)m.stage, node_here, slot);

            continue;

        }

        HierPos2 st_pos; f32 st_r;

        mission_dock_anchor(m, &st_pos, &st_r);

        i32 best = -1; f32 best_d2 = 3.4e38f;

        for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

            NpcShip& n = s->npc_ships[i];

            if (!n.active || n.archetype != ARCHETYPE_TRADER || n.mission_id >= 0) continue;

            if (n.home_node != node_here) continue;

            Vec2 d = hierpos_diff(&st_pos, &n.ship.origin, BS_HIERPOS_CELL_SIZE);

            f32 d2 = d.x * d.x + d.y * d.y;

            if (d2 < best_d2) { best_d2 = d2; best = i; }

        }

        if (best < 0) continue;   // no free trader: the contract waits (no spawn fallback)

        s->npc_ships[best].mission_id = mi;

        s->npc_ships[best].work_timer = 0.0f;

        s->npc_ships[best].state      = AI_TRADE_DOCK;

        s->npc_ships[best].state_timer = 0.0f;

        m.ship_slot = best;

        BS_LOG_INFO("ShipAI trade: contract %d picked up by trader %d (station %d)", mi, best, m.station_id);

    }

}



// ---- Perception: acquire the nearest hostile combat entity within sensor range ------------------
// Phase 1 (autonomous universe): agents sense EVERY combat entity (player fleet, patrol hull, other
// NPCs) and resolve hostility pairwise via galaxy_history_factions_hostile — so civ patrols fight
// warring civs and pirates, not just the player. `self` is this agent's npc_ships[] index, used to
// skip its own (one-frame-stale) registration in combat_entities[]. Sets target_ce (-1 if none)
// and remembers last_seen.
// Perception. Sensing is O(agents x entities), so at a 256-hull pool a naive every-frame scan is
// ~65k distance tests per frame. Two cheap guards keep it affordable: results are CACHED and
// refreshed on the staggered AI_SENSE_INTERVAL cadence, and the hostility lookup -- the expensive
// part -- runs only after a squared-distance reject.
static void ai_sense(game_state* s, NpcShip& n, i32 self) {

    const AiProfile& p = ai_profile(n.archetype);

    // Staggered by slot so the cost spreads evenly instead of spiking on one frame.
    if (n.sense_countdown > 0) {

        --n.sense_countdown;

        // Validate the cached target cheaply; a stale/destroyed one falls through to a full scan.
        if (n.target_ce >= 0 && n.target_ce < s->combat_entity_count &&
            s->combat_entities[n.target_ce].active) return;

    }

    n.sense_countdown = (i16)(AI_SENSE_INTERVAL + (self % AI_SENSE_INTERVAL));

    n.target_ce = -1;

    f32 best_d2 = p.sensor_range * p.sensor_range;

    i32 best = -1;

    for (i32 i = 0; i < s->combat_entity_count; ++i) {

        CombatEntity& ce = s->combat_entities[i];

        if (!ce.active) continue;

        if (ce.is_npc && ce.npc_index == self) continue;               // never target yourself

        Vec2 to = hierpos_diff(&ce.position, &n.ship.origin, BS_HIERPOS_CELL_SIZE);

        f32 d2 = to.x * to.x + to.y * to.y;

        if (d2 >= best_d2) continue;                                   // cheap reject before the

        if (!galaxy_history_factions_hostile(s, n.faction, ce.faction_id)) continue;  // stance lookup

        best_d2 = d2; best = i;

    }

    n.target_ce = best;

    if (best >= 0) n.last_seen = s->combat_entities[best].position;

}



// ---- MINER: work an asteroid belt by mining per-system asteroids --------------------------------

// Asteroids now live with each StarSystem (see state/game_state.h). The miner remembers its target

// by (home node index -> materialized cache slot, asteroid index). Asteroids are indestructible:

// mining never removes them; the miner just dwells, then moves to another.



// Materialized cache slot holding galaxy node `node_idx` (-1 if not currently resident).

static i32 miner_system_slot(game_state* s, i32 node_idx) {

    if (node_idx < 0) return -1;

    for (i32 i = 0; i < s->galaxy.system_count; ++i)

        if (s->galaxy.cache_node[i] == node_idx) return i;

    return -1;

}



static b8 miner_target_valid(game_state* s, NpcShip& n) {

    if (n.work_asteroid < 0) return FALSE;

    i32 slot = miner_system_slot(s, (i32)n.work_node);   // work_node stores the home node index

    if (slot < 0) return FALSE;

    StarSystem& ss = s->galaxy.systems[slot];

    if (n.work_asteroid >= ss.asteroid_count) return FALSE;

    n.work_pos = ss.asteroids[n.work_asteroid].pos;         // refresh (position is static, but keep in sync)

    return TRUE;

}



static void miner_acquire(game_state* s, NpcShip& n) {

    n.work_asteroid = -1;

    i32 slot = miner_system_slot(s, n.home_node);

    if (slot < 0) return;

    StarSystem& ss = s->galaxy.systems[slot];

    const AiProfile& p = ai_profile(n.archetype);

    f32 r2 = p.sensor_range * p.sensor_range;

    i32 seen = 0;

    for (i32 ai = 0; ai < ss.asteroid_count; ++ai) {

        Vec2 d = hierpos_diff(&ss.asteroids[ai].pos, &n.ship.origin, BS_HIERPOS_CELL_SIZE);

        if (d.x * d.x + d.y * d.y > r2) continue;

        ++seen;

        if ((sm64(n.rng) % (u64)seen) == 0) {          // reservoir-sample one asteroid in range

            n.work_node = n.home_node; n.work_asteroid = ai; n.work_pos = ss.asteroids[ai].pos;

        }

    }

}



// Nearest station (index into StarSystem::stations[]) of materialized slot `slot`; -1 if none.

static i32 nearest_station(game_state* s, i32 slot, const HierPos2* from) {

    if (slot < 0) return -1;

    StarSystem& ss = s->galaxy.systems[slot];

    i32 best = -1; f32 best_d2 = 3.4e38f;

    for (i32 i = 0; i < ss.station_count; ++i) {

        Vec2 d = hierpos_diff(&ss.stations[i].pos, from, BS_HIERPOS_CELL_SIZE);

        f32 d2 = d.x * d.x + d.y * d.y;

        if (d2 < best_d2) { best_d2 = d2; best = i; }

    }

    return best;

}



// What a dwell at the rock yields: the node's dominant resource, occasionally its rare form.

static u8 miner_pick_good(game_state* s, NpcShip& n) {

    if (n.home_node < 0 || n.home_node >= s->galaxy.node_count) return GOOD_IRON_ORE;

    const GalaxyNode& gn = s->galaxy.nodes[n.home_node];

    b8 rare = (sm64(n.rng) % 4ull) == 0;   // 1-in-4 hauls strike the rare form

    if (gn.res_metal >= gn.res_volatiles) return rare ? GOOD_RARE_METALS   : GOOD_IRON_ORE;

    return                                       rare ? GOOD_HYDROGEN_FUEL : GOOD_WATER_ICE;

}



// Phase 3: AI_DELIVER — full hold, fly to the chosen station, dock, and SELL the ore into its

// market (station_market_apply: stock up, local price down). Mining now moves the economy.

static void ai_miner_deliver(game_state* s, NpcShip& n, f32 dt) {

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    i32 slot = miner_system_slot(s, n.home_node);

    if (slot < 0 || n.work_station < 0 || n.work_station >= s->galaxy.systems[slot].station_count) {

        n.work_station = -1; n.state = AI_PATROL; n.state_timer = 0.0f;   // system swapped out: abort

        return;

    }

    SystemStation& st = s->galaxy.systems[slot].stations[n.work_station];

    Vec2 to = hierpos_diff(&st.pos, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32 dist = vec2_length(to);

    f32 dock_range = st.radius * 1.5f;

    if (dock_range < 600.0f) dock_range = 600.0f;

    const f32 UNLOAD_TIME = 4.0f;

    if (dist > dock_range) {

        n.work_timer = 0.0f;

        steering::apply(sh, fl, steering::arrive(to, p.max_speed, dock_range * 2.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

        return;

    }

    steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

    n.work_timer += dt;

    if (n.work_timer < UNLOAD_TIME) return;

    station_market_apply(s, st.station_id, n.cargo_good, n.cargo_units);

    BS_LOG_INFO("ShipAI mining: miner %d delivered %.0f %s to station N%d/%d",

                (i32)(&n - s->npc_ships), n.cargo_units, TRADE_GOOD_NAMES[n.cargo_good],

                station_id_node(st.station_id), station_id_index(st.station_id));

    n.cargo_units = 0.0f; n.work_timer = 0.0f; n.work_station = -1;

    n.state = AI_PATROL; n.state_timer = 0.0f;

}



// ---- TRADER (contract-bound): dock at the contract's station, dwell, hand back to the macro ----

// Only traders bound to a mission run this loop; unbound ambient traders keep the generic FSM.

// The trader flies to the current dock-stage station, halts facing it, dwells for TRADE_DOCK_TIME,

// then flags the mission's local_ready so the macro stage machine advances (depart / retire).

// ---- Shared mission-leg locomotion ---------------------------------------------------------

// EVERY mission-bound hull -- freighter, reinforcement column, patrol, raid party -- flies its

// macro leg exactly the same way: out to the jump ring, freely across the system, in to the

// target, at macro speed so a materialised ship stays schedule-coherent with the unobserved ones.

// Mirrors the hull into m.pos (the galaxy-map pip tracks the real ship) and hands arrival back

// through local_ready. One locomotion policy, no per-archetype special cases.

static void ai_fly_mission_leg(game_state* s, NpcShip& n, ShipMission& m, f32 dt) {

    const GalaxyState& g = s->galaxy;

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    if (n.state != AI_TRAVEL_LEG) { n.state = AI_TRAVEL_LEG; n.state_timer = 0.0f; }

    f32 spd = (g.ai_speed_in_system > 0.0f) ? g.ai_speed_in_system : 50000.0f;

    Vec2 leg = hierpos_diff(&m.leg_target, &sh->origin, BS_HIERPOS_CELL_SIZE);

    steering::apply(sh, fl, steering::arrive(leg, spd, spd * 2.0f), spd, spd, p.turn_rate * 2.0f, dt);

    m.pos = sh->origin;   // macro mirrors the live hull

    // The arrival band has to cover a full frame of travel at macro speed, or a fast hull steps

    // straight over it and the stage never completes.

    f32 band = vec2_length(fl->velocity) * dt * 3.0f;

    if (band < 1500.0f) band = 1500.0f;

    if (vec2_length(leg) <= band && !m.local_ready) m.local_ready = TRUE;

}



static void ai_trader_tick(game_state* s, NpcShip& n, f32 dt) {

    GalaxyState& g = s->galaxy;

    if (!g.missions || n.mission_id < 0 || n.mission_id >= g.mission_count) { n.mission_id = -1; return; }

    ShipMission& m = g.missions[n.mission_id];

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    n.state_timer += dt;



    // Phase 2 movement stages: the live agent OWNS the macro's current in-system leg. It flies at
    // macro speed (schedule coherence with unobserved missions), mirrors its position into m.pos so
    // the galaxy-map pip tracks the real hull, and hands arrival back via local_ready.

    if (m.stage == MISSION_STAGE_ACQUIRE || m.stage == MISSION_STAGE_TO_JUMP ||

        m.stage == MISSION_STAGE_CROSS   || m.stage == MISSION_STAGE_FINAL_APPROACH) {

        ai_fly_mission_leg(s, n, m, dt);

        return;

    }



    HierPos2 st_pos; f32 st_r;

    mission_dock_anchor(m, &st_pos, &st_r);

    Vec2 to = hierpos_diff(&st_pos, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32 dist = vec2_length(to);

    f32 dock_range = st_r * 1.5f;

    if (dock_range < 600.0f) dock_range = 600.0f;

    const f32 TRADE_DOCK_TIME = 8.0f;   // seconds halted at the station before the stage completes



    if (dist > dock_range) {

        if (n.state != AI_TRADE_DOCK) { n.state = AI_TRADE_DOCK; n.state_timer = 0.0f; }

        n.work_timer = 0.0f;

        steering::apply(sh, fl, steering::arrive(to, p.max_speed, dock_range * 2.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

    } else {

        if (n.state != AI_TRADE_DOCKED) { n.state = AI_TRADE_DOCKED; n.state_timer = 0.0f; }

        steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

        n.work_timer += dt;

        if (n.work_timer >= TRADE_DOCK_TIME && !m.local_ready) {

            m.local_ready = TRUE;          // macro stage machine departs / retires next tick

            n.work_timer = 0.0f;

        }

    }

}



static void ai_miner_tick(game_state* s, NpcShip& n, f32 dt) {

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    n.state_timer += dt;



    if (!miner_target_valid(s, n)) miner_acquire(s, n);

    // Full hold takes priority: run the delivery leg (Phase 3).

    if (n.state == AI_DELIVER) { ai_miner_deliver(s, n, dt); return; }

    if (!miner_target_valid(s, n)) {

        // No asteroid in reach: loiter around the local home while looking for one.

        f32 orad = (n.orbit_radius > 1.0f) ? n.orbit_radius : p.patrol_radius;

        n.timer += (p.patrol_speed / orad) * dt;

        HierPos2 wp = hierpos_add_vec2(&n.home, vec2_rotate(Vec2{ orad, 0.0f }, n.timer));

        Vec2 to = hierpos_diff(&wp, &sh->origin, BS_HIERPOS_CELL_SIZE);

        steering::apply(sh, fl, steering::arrive(to, p.patrol_speed, 2000.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

        return;

    }

    // Stop distance scales to the asteroid's radius so the miner halts at the surface.

    f32 ast_r = 800.0f;

    {

        i32 slot = miner_system_slot(s, (i32)n.work_node);

        if (slot >= 0 && n.work_asteroid < s->galaxy.systems[slot].asteroid_count)

            ast_r = s->galaxy.systems[slot].asteroids[n.work_asteroid].radius;

    }

    // Fly to the asteroid; dwell + mine when close, then move on (asteroids are NOT destroyed).

    Vec2 to = hierpos_diff(&n.work_pos, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32 dist = vec2_length(to);

    f32 MINE_RANGE = ast_r * 1.4f;

    if (MINE_RANGE < 600.0f) MINE_RANGE = 600.0f;

    const f32 MINE_TIME = 6.0f;

    if (dist > MINE_RANGE) {

        n.work_timer = 0.0f;

        steering::apply(sh, fl, steering::arrive(to, p.patrol_speed, 3000.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

    } else {

        steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

        n.work_timer += dt;

        if (n.work_timer >= MINE_TIME) {

            n.work_asteroid = -1; n.work_timer = 0.0f;      // done dwelling -> pick another asteroid next tick

            // Phase 3: the dwell actually yields ore into the hold; a full hold triggers delivery.

            const f32 MINE_YIELD = 8.0f, HOLD_MAX = 24.0f;

            if (n.cargo_units <= 0.0f) n.cargo_good = miner_pick_good(s, n);

            n.cargo_units += MINE_YIELD;

            if (n.cargo_units >= HOLD_MAX) {

                i32 st = nearest_station(s, miner_system_slot(s, n.home_node), &sh->origin);

                if (st >= 0) { n.work_station = st; n.state = AI_DELIVER; n.state_timer = 0.0f; }

                else n.cargo_units = 0.0f;   // no stations in reach (wild/unowned): vent and keep working

            }

        }

    }

}



// ---- TRADER (ambient, Phase 3): intra-system micro-hauls -----------------------------------------

// Unbound traders in a materialized system run a real buy->sell loop: load the biggest-stock good

// at one station (stock down -> price up), sell it at another (stock up -> price down). Systems

// with a single station run a planet<->station shuttle delivering planetary exports instead.

// Returns FALSE when no local market work is possible (caller falls back to the generic FSM).

static u8 planet_export_good(game_state* s, i32 node, u64& rng) {

    if (node < 0 || node >= s->galaxy.node_count) return GOOD_GRAIN;

    const GalaxyNode& gn = s->galaxy.nodes[node];

    if (gn.biosphere >= gn.res_metal && gn.biosphere >= gn.res_volatiles)

        return ((sm64(rng) & 1ull) == 0) ? GOOD_GRAIN : GOOD_ORGANICS;

    return (gn.res_metal >= gn.res_volatiles) ? GOOD_IRON_ORE : GOOD_WATER_ICE;

}



static b8 ai_ambient_trader_tick(game_state* s, NpcShip& n, f32 dt) {

    i32 slot = miner_system_slot(s, n.home_node);

    if (slot < 0) return FALSE;

    StarSystem& ss = s->galaxy.systems[slot];

    if (ss.station_count <= 0) return FALSE;

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    n.state_timer += dt;

    const f32 HAUL_UNITS = 10.0f, DOCK_TIME = 5.0f;

    b8 single = ss.station_count < 2;



    // Single-station system, empty hold: shuttle out to a planet lane and load its exports.

    if (single && n.cargo_units <= 0.0f) {

        HierPos2 anchor = system_anchor(s, n.home_node, ARCHETYPE_TRADER, 1, n.spawn_seed);

        Vec2 to = hierpos_diff(&anchor, &sh->origin, BS_HIERPOS_CELL_SIZE);

        if (vec2_length(to) > 1500.0f) {

            if (n.state != AI_TRADE_DOCK) { n.state = AI_TRADE_DOCK; n.state_timer = 0.0f; }

            n.work_timer = 0.0f;

            steering::apply(sh, fl, steering::arrive(to, p.max_speed, 3000.0f),

                            p.accel, p.max_speed, p.turn_rate, dt);

            return TRUE;

        }

        steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

        n.work_timer += dt;

        if (n.work_timer >= DOCK_TIME) {

            n.cargo_good  = planet_export_good(s, n.home_node, n.rng);

            n.cargo_units = HAUL_UNITS * 0.6f;

            n.work_station = 0; n.work_timer = 0.0f;

        }

        return TRUE;

    }



    // Choose a station target if none: empty hold -> a buy stop, loaded -> a sell stop.

    if (n.work_station < 0 || n.work_station >= ss.station_count) {

        n.work_station = (i32)(sm64(n.rng) % (u64)ss.station_count);

        n.work_timer   = 0.0f;

    }

    SystemStation& st = ss.stations[n.work_station];

    Vec2 to = hierpos_diff(&st.pos, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32 dist = vec2_length(to);

    f32 dock_range = st.radius * 1.5f;

    if (dock_range < 600.0f) dock_range = 600.0f;

    if (dist > dock_range) {

        if (n.state != AI_TRADE_DOCK) { n.state = AI_TRADE_DOCK; n.state_timer = 0.0f; }

        n.work_timer = 0.0f;

        steering::apply(sh, fl, steering::arrive(to, p.max_speed, dock_range * 2.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

        return TRUE;

    }

    if (n.state != AI_TRADE_DOCKED) { n.state = AI_TRADE_DOCKED; n.state_timer = 0.0f; }

    steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

    n.work_timer += dt;

    if (n.work_timer < DOCK_TIME) return TRUE;

    n.work_timer = 0.0f;

    if (n.cargo_units > 0.0f) {

        station_market_apply(s, st.station_id, n.cargo_good, n.cargo_units);

        BS_LOG_INFO("ShipAI micro-trade: trader %d sold %.0f %s at station N%d/%d",

                    (i32)(&n - s->npc_ships), n.cargo_units, TRADE_GOOD_NAMES[n.cargo_good],

                    station_id_node(st.station_id), station_id_index(st.station_id));

        n.cargo_units = 0.0f;

        n.work_station = -1;   // next: pick a buy stop (or the planet leg in single-station systems)

        return TRUE;

    }

    // Buy the good this station is most glutted with (respecting available stock).

    MarketGood mg[GOOD_COUNT];

    station_market_get(s, st.station_id, mg);

    i32 best = -1; f32 best_stock = 1.0f;

    for (i32 g = 0; g < GOOD_COUNT; ++g)

        if (mg[g].stock > best_stock) { best_stock = mg[g].stock; best = g; }

    if (best < 0) { n.work_station = -1; return TRUE; }   // bare shelves: try another station

    f32 units = (best_stock < HAUL_UNITS) ? best_stock : HAUL_UNITS;

    station_market_apply(s, st.station_id, best, -units);

    n.cargo_good  = (u8)best;

    n.cargo_units = units;

    i32 nxt = (i32)(sm64(n.rng) % (u64)ss.station_count);

    if (nxt == n.work_station) nxt = (nxt + 1) % ss.station_count;

    n.work_station = nxt;   // sell stop: a different station

    return TRUE;

}



// ---- WARSHIP (mission-bound, Phase 4): fly the column's leg, break off to engage hostiles --------

// Reinforcement columns and patrols in the player's system fly their macro leg exactly like a

// materialized trader, but they are combatants: any hostile contact hands control to the generic

// combat FSM (return FALSE) until the threat is gone, then the column resumes its leg.

static b8 ai_mission_warship_tick(game_state* s, NpcShip& n, i32 self, f32 dt) {

    GalaxyState& g = s->galaxy;

    if (!g.missions || n.mission_id < 0 || n.mission_id >= g.mission_count) { n.mission_id = -1; return FALSE; }

    ShipMission& m = g.missions[n.mission_id];

    ai_sense(s, n, self);

    if (n.target_ce >= 0) {

        m.pos = n.ship.origin;   // keep the map pip on the hull while it fights

        return FALSE;            // hostiles in sensor range: fight first

    }

    const AiProfile& p = ai_profile(n.archetype);

    (void)p;

    n.state_timer += dt;

    // Same locomotion as every other mission hull: jump ring out, free flight across the system,
    // approach in. A warship is a freighter with guns as far as movement is concerned.
    ai_fly_mission_leg(s, n, m, dt);

    return TRUE;

}



// ---- Per-agent behavior: FSM (PATROL -> PURSUE -> ATTACK -> RETURN) driven by perception ---------
// `self` = this agent's npc_ships[] index (perception self-skip + damage attribution).
static void ai_ship_tick(game_state* s, NpcShip& n, i32 self, f32 dt) {

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship;

    ShipFlight* fl = &n.flight;



    // Civilian miners run their own work loop (no combat pursuit).

    if (n.archetype == ARCHETYPE_MINER) { ai_miner_tick(s, n, dt); return; }



    // Contract-bound traders run the station docking loop; unbound traders run intra-system

    // micro-hauls when the local market is materialized (Phase 3), else the generic FSM.

    if (n.archetype == ARCHETYPE_TRADER && n.mission_id >= 0) { ai_trader_tick(s, n, dt); return; }

    if (n.archetype == ARCHETYPE_TRADER && ai_ambient_trader_tick(s, n, dt)) return;

    // Mission-bound combatants (Phase 4/5) fly their leg unless there is something to fight.

    if ((n.archetype == ARCHETYPE_WARSHIP || n.archetype == ARCHETYPE_PIRATE) &&

        n.mission_id >= 0 && ai_mission_warship_tick(s, n, self, dt)) return;



    n.state_timer += dt;

    if (n.fire_cd > 0.0f) n.fire_cd -= dt;



    ai_sense(s, n, self);



    Vec2 to_home = hierpos_diff(&n.home, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32  home_dist = vec2_length(to_home);



    // Resolve the current target (any live, still-hostile combat entity).

    b8 have_target = (n.target_ce >= 0 && n.target_ce < s->combat_entity_count &&

                      s->combat_entities[n.target_ce].active &&

                      galaxy_history_factions_hostile(s, n.faction, s->combat_entities[n.target_ce].faction_id));

    Vec2 to_target{ 0.0f, 0.0f };

    f32  tdist = 0.0f;

    if (have_target) {

        to_target = hierpos_diff(&s->combat_entities[n.target_ce].position, &sh->origin, BS_HIERPOS_CELL_SIZE);

        tdist = vec2_length(to_target);

    }



    // ---- State transitions ----

    // Phase 7: the whole FSM is live. EVADE (badly hurt agents break off), INVESTIGATE (chase the

    // last known contact instead of forgetting instantly) and SCOUT shadowing are all reachable;

    // before this only PATROL/PURSUE/ATTACK/RETURN ever ran.

    u8 st = n.state;

    f32 hp_frac = (n.max_hp > 0.0f) ? (n.hp / n.max_hp) : 1.0f;

    b8 shadower = (n.archetype == ARCHETYPE_SCOUT);   // observes, never opens fire (unarmed)

    // A mission-bound hull is anchored to its MISSION, not to the point it materialised at. A
    // column crossing a system is legitimately 100k+ units from that point, so the loiter leash
    // would drag it backwards -- and because the leash is tested before everything else, it would
    // refuse to fight at all (the ship yo-yos: retreat while a hostile is in range, resume the leg
    // when it drops out, retreat again). Garrison and ambient agents keep their leash.
    b8 leashed = (n.mission_id < 0) && (n.wing_leader < 0) && (home_dist > p.leash_range);

    if (have_target) { n.last_contact = s->combat_entities[n.target_ce].position; n.contact_timer = AI_INVESTIGATE_HOLD; }

    else if (n.contact_timer > 0.0f) n.contact_timer -= dt;

    if (leashed) {

        st = AI_RETURN;                                  // leashed: break off and go home

    } else if (have_target && hp_frac < p.flee_hp_frac && st != AI_EVADE) {

        st = AI_EVADE;                                   // badly hurt: run, do not trade shots

    } else {

        switch (st) {

            case AI_PATROL:  if (have_target && (p.aggression >= 0.5f || shadower)) st = AI_PURSUE;

                             else if (n.contact_timer > 0.0f && p.aggression >= 0.5f) st = AI_INVESTIGATE; break;

            case AI_INVESTIGATE: if (have_target) st = AI_PURSUE;

                             else if (n.contact_timer <= 0.0f) st = AI_PATROL; break;

            case AI_PURSUE:  if (!have_target) st = (n.contact_timer > 0.0f) ? AI_INVESTIGATE : AI_PATROL;

                             else if (tdist <= p.engage_range) st = AI_ATTACK; break;

            case AI_ATTACK:  if (!have_target) st = (n.contact_timer > 0.0f) ? AI_INVESTIGATE : AI_PATROL;

                             else if (tdist > p.engage_range * 1.2f) st = AI_PURSUE; break;

            case AI_EVADE:   if (!have_target || tdist > p.sensor_range || hp_frac >= p.flee_hp_frac) st = AI_RETURN; break;

            case AI_RETURN:  if (home_dist <= p.patrol_radius) st = AI_PATROL; break;

            default:         st = AI_PATROL; break;

        }

    }

    if (st != n.state) {

        // Throttled engagement evidence: proves NPC-vs-NPC combat is actually starting (and who
        // with) without spamming a line per frame per agent.
        if (st == AI_ATTACK && have_target) {

            static i32 s_engage_n = 0;

            if ((++s_engage_n % 4) == 1)

                BS_LOG_INFO("ShipAI combat: npc %d (archetype %u, faction %d) opens fire on faction %d at %.0f units (engagement #%d)",

                            self, (u32)n.archetype, (i32)n.faction,

                            (i32)s->combat_entities[n.target_ce].faction_id, tdist, s_engage_n);

        }

        n.state = st; n.state_timer = 0.0f;

    }



    // ---- Act ----

    switch (n.state) {

        case AI_PATROL: {

            // Phase 10: a wingman holds its slot on the leader instead of running its own loiter.
            // Formation is a CRUISE behaviour only -- PURSUE/ATTACK/EVADE are separate states, so
            // engaging automatically breaks the wing and disengaging reforms it.
            if (n.wing_leader >= 0 && n.wing_leader < NPC_SHIP_MAX) {

                const NpcShip& L = s->npc_ships[n.wing_leader];

                if (L.active) {

                    Vec2     off      = vec2_rotate(wing_slot_offset(n.wing_slot), L.ship.angle);

                    HierPos2 slot_pos = hierpos_add_vec2(&L.ship.origin, off);

                    Vec2     to_slot  = hierpos_diff(&slot_pos, &sh->origin, BS_HIERPOS_CELL_SIZE);

                    // The leader may be flying a macro leg at ai_speed_in_system, far above a
                    // combat profile's max_speed. Wingmen inherit enough speed/thrust headroom to
                    // stay with it, otherwise the formation strings out and breaks on every transit.
                    f32 lead_spd = vec2_length(L.flight.velocity);

                    f32 wspd     = lead_spd * 1.35f + p.max_speed;

                    f32 wacc     = (p.accel > 0.0f ? p.accel : 500.0f) * (1.0f + lead_spd / (p.max_speed > 1.0f ? p.max_speed : 1.0f));

                    steering::apply(sh, fl, steering::arrive(to_slot, wspd, WING_SPACING * 1.5f),

                                    wacc, wspd, p.turn_rate, dt);

                    break;

                }

            }

            f32 orad  = (n.orbit_radius > 1.0f) ? n.orbit_radius : p.patrol_radius;

            f32 omega = p.patrol_speed / orad;

            n.timer += omega * dt;

            Vec2 orbit = vec2_rotate(Vec2{ orad, 0.0f }, n.timer);

            n.patrol_wp = hierpos_add_vec2(&n.home, orbit);

            Vec2 to_wp = hierpos_diff(&n.patrol_wp, &sh->origin, BS_HIERPOS_CELL_SIZE);

            steering::apply(sh, fl, steering::arrive(to_wp, p.patrol_speed, 3000.0f),

                            p.accel, p.max_speed, p.turn_rate, dt);

        } break;

        case AI_PURSUE: {

            steering::apply(sh, fl, steering::seek(to_target, p.max_speed),

                            p.accel, p.max_speed, p.turn_rate, dt);

        } break;

        case AI_ATTACK: {

            steering::apply_face(sh, fl, steering::standoff(to_target, tdist, p.standoff, p.max_speed),

                                 to_target, p.accel, p.max_speed, p.turn_rate, dt);

            // Fire when roughly aligned and in the firing band.

            if (tdist >= p.min_range && tdist <= p.engage_range && n.fire_cd <= 0.0f) {

                Weapon* w = (n.archetype < ARCHETYPE_COUNT) ? g_npc_weapons[n.archetype] : nullptr;

                // Lead the target. At these ranges a shell is in flight for SECONDS (47km at
                // 12k u/s = 3.9s) while both hulls move at up to 820 u/s -- roughly 3200 units of
                // drift, several ship-widths. Firing at where the target IS therefore misses every
                // time. Solve the intercept iteratively; the shell inherits our velocity, so the
                // lead is driven by the RELATIVE velocity.
                f32 pspd = (w && w->projectile_speed() > 1.0f) ? w->projectile_speed() : 12000.0f;

                Vec2 tvel = s->combat_entities[n.target_ce].velocity;

                Vec2 rvel = vec2_sub(tvel, fl->velocity);

                Vec2 aim  = to_target;

                for (i32 it = 0; it < 2; ++it) {

                    f32 tof = vec2_length(aim) / pspd;

                    aim = vec2_add(to_target, vec2_scale(rvel, tof));

                }

                f32 desired_angle = atan2f(-aim.x, aim.y);

                f32 ad = desired_angle - sh->angle;

                while (ad >  BS_PI) ad -= 2.0f * BS_PI;

                while (ad < -BS_PI) ad += 2.0f * BS_PI;

                if (fabsf(ad) < 0.30f) {

                    // Phase 7: fire through the real Weapon system so NPC shells carry registry
                    // damage/speed/HP and interact with point-defense and flak like the player's.
                    // The shared instance is rate-limited by this agent's own fire_cd, so its
                    // internal cooldown is cleared first (update with a large dt).
                    Weapon* w2 = w;

                    if (w2) {

                        w2->owner_faction    = VESSEL_PIRATE;

                        w2->owner_faction_id = n.faction;

                        w2->update(1.0e6f);

                        w2->fire(sh->origin, aim, fl->velocity, &s->projectiles);

                        n.fire_cd = p.fire_period;

                    } else if (archetype_weapon_id(n.archetype)) {

                        // Archetype is meant to be armed but its registry weapon failed to load.
                        f32 al = vec2_length(aim);

                        Vec2 v = vec2_add(fl->velocity, vec2_scale(aim, pspd / (al > 1.0f ? al : 1.0f)));

                        s->projectiles.spawn(sh->origin, v, 8.0f, 4.0f,

                                             bs_color{ 1.0f, 0.5f, 0.3f, 1.0f }, VESSEL_PIRATE, n.faction, 0.4f, 1.0f);

                        n.fire_cd = p.fire_period;

                    }

                    // Unarmed archetypes (scouts, traders, miners) hold station and observe.
                }

            }

        } break;

        case AI_RETURN: {

            f32 slow = (n.orbit_radius > 1.0f) ? n.orbit_radius : p.patrol_radius;

            steering::apply(sh, fl, steering::arrive(to_home, p.max_speed, slow),

                            p.accel, p.max_speed, p.turn_rate, dt);

        } break;

        case AI_EVADE: {

            // Run directly away from the threat at full burn, nose leading the escape vector.

            Vec2 away = vec2_scale(to_target, -1.0f);

            steering::apply(sh, fl, steering::seek(away, p.max_speed),

                            p.accel, p.max_speed, p.turn_rate, dt);

        } break;

        case AI_INVESTIGATE: {

            // Sweep toward the last known contact position; the transition table drops back to

            // PATROL when the trail goes cold.

            Vec2 to_contact = hierpos_diff(&n.last_contact, &sh->origin, BS_HIERPOS_CELL_SIZE);

            steering::apply(sh, fl, steering::arrive(to_contact, p.patrol_speed, p.patrol_radius),

                            p.accel, p.max_speed, p.turn_rate, dt);

        } break;

        default: break;

    }

}



void ai_ships_update(game_state* s, f32 dt) {

    if (!s) return;

    // Population manager (throttled): keep the current owned system garrisoned.

    s->npc_pop_timer -= dt;

    if (s->npc_pop_timer <= 0.0f) {

        s->npc_pop_timer = 0.3f;

        ai_ships_populate(s);

    }

    // Macro handoff: keep co-located trade missions in sync with live agents. Runs every frame

    // (after populate) so mission ships survive the on-system-change pool clear and track arrivals.

    ai_ships_sync_missions(s);

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

        if (s->npc_ships[i].active) ai_ship_tick(s, s->npc_ships[i], i, dt);

    }

}



// Register every active agent into the NPC window of combat_entities[] so projectiles hit them and

// the RTS layer can hover / order-attack them exactly like the player fleet + enemy hull.

void ai_ships_register_combat(game_state* s) {

    if (!s) return;

    s->combat_entity_count = s->npc_combat_base;

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

        NpcShip& n = s->npc_ships[i];

        if (!n.active) continue;

        if (s->combat_entity_count >= MAX_COMBAT_ENTITIES) break;

        CombatEntity* ce = &s->combat_entities[s->combat_entity_count++];

        *ce = CombatEntity{};

        ce->active     = TRUE;

        ce->position   = n.ship.origin;

        ce->velocity   = n.flight.velocity;

        ce->radius     = ship_bounding_radius(&n.ship);

        ce->faction    = VESSEL_PIRATE;

        ce->faction_id = n.faction;

        ce->is_npc     = TRUE;

        ce->npc_index  = i;

        ce->hp         = n.hp;

        ce->ship       = &n.ship;

        ce->tint       = faction_tint(s, n.faction);

        ce->radiation_emission = n.ship.radiation_emission;

        ce->is_drone   = FALSE;

    }

}



void ai_ship_damage(game_state* s, i32 npc_index, f32 dmg, i16 attacker_faction) {
    if (!s || npc_index < 0 || npc_index >= NPC_SHIP_MAX) return;

    NpcShip& n = s->npc_ships[npc_index];

    if (!n.active) return;

    n.hp -= dmg;

    // Throttled hit evidence: distinguishes "ships are shooting and missing" from "ships are
    // landing hits", which is the difference between a ballistics bug and a balance question.
    if (n.hp > 0.0f && attacker_faction != n.faction) {

        static i32 s_hit_n = 0;

        if ((++s_hit_n % 8) == 1)

            BS_LOG_INFO("ShipAI combat: npc %d (faction %d) HIT by faction %d for %.0f (hp %.0f/%.0f, hit #%d)",

                        npc_index, (i32)n.faction, (i32)attacker_faction, dmg, n.hp, n.max_hp, s_hit_n);

    }

    if (n.hp <= 0.0f) {

        n.active = FALSE;

        // Phase 10: a dead leader does not strand its wing. Promote the first surviving wingman
        // and re-slot the rest onto it, so the formation closes up instead of scattering.
        {

            i32 new_lead = -1;

            for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

                NpcShip& w = s->npc_ships[i];

                if (!w.active || w.wing_leader != (i16)npc_index) continue;

                if (new_lead < 0) { new_lead = i; w.wing_leader = -1; w.wing_slot = 0; }

                else              { w.wing_leader = (i16)new_lead; w.wing_slot = 1; }

            }

        }

        BS_LOG_INFO("ShipAI combat: npc %d (archetype %u, faction %d) destroyed by faction %d",
                    npc_index, (u32)n.archetype, (i32)n.faction, (i32)attacker_faction);

        // Reputation only reacts to PLAYER kills: destroying a civ ship is an act of aggression
        // against its civilization. NPC-vs-NPC kills (wars, pirates) carry no player rep change.

        if (n.faction >= 0 && attacker_faction == FACTION_PLAYER)
            galaxy_history_player_raid(s, n.faction, 3.0f);

        if (n.mission_id >= 0) {

            // A cross-system traveler (not a garrison ship): retire the macro mission instead of

            // decrementing the local garrison, so the destroyed trader stops travelling/rendering.

            ship_mission_notify_destroyed(s, n.mission_id);

        } else {

            // Step B: the loss persists in the macro fleet layer (garrison shrinks; re-materialises fewer).

            galaxy_history_garrison_add(s, n.home_node, -1);

        }

    }

}



// ---- DEBUG / test harness: force an NPC-vs-NPC engagement ----------------------------------

// Raids only reach the player's system occasionally, which makes combat almost impossible to test

// or observe on demand. This drops a hostile strike group right next to the player: warships of a

// faction the local garrison is guaranteed to fight (a civ at war with the owner if one exists,

// otherwise FACTION_PIRATE, which everyone engages). Spawned just outside weapons range so the

// approach and the opening exchange are both visible.

i32 ai_ships_debug_spawn_strike(game_state* s, i32 count) {

    if (!s || count <= 0) return 0;

    GalaxyState& g = s->galaxy;

    HierPos2 flag = s->fleet_state.fleet.flagship().ship.origin;

    i32 node  = galaxy_nearest_node(s, &flag);

    i32 owner = (node >= 0) ? galaxy_history_owner_at_node(s, node) : -1;



    // Pick an aggressor the local garrison will actually shoot at.

    i16 aggressor = FACTION_PIRATE;

    if (owner >= 0) {

        for (i32 c = 0; c < g.civ_count; ++c) {

            if (c == owner || g.civs[c].status != 0) continue;

            if (galaxy_history_civ_at_war(s, owner, c)) { aggressor = (i16)c; break; }

        }

    }



    u64 st = (u64)(s->elapsed_time * 1000.0f) ^ 0xA11CE5DEADBEEF01ull;

    i32 spawned = 0;

    for (i32 k = 0; k < count; ++k) {

        f32 ang = rrange(st, 0.0f, 2.0f * BS_PI);

        f32 rad = rrange(st, 20000.0f, 32000.0f);   // just outside engage range: watch them close

        HierPos2 pos = hierpos_add_vec2(&flag, vec2_rotate(Vec2{ rad, 0.0f }, ang));

        if (spawn_npc(s, aggressor, pos, node, ARCHETYPE_WARSHIP, st + (u64)(k + 1)) >= 0) ++spawned;

    }

    BS_LOG_INFO("ShipAI TEST: spawned %d hostile warship(s) of faction %d at node %d (local owner %d) -- expect engagement",

                spawned, (i32)aggressor, node, owner);

    return spawned;

}

