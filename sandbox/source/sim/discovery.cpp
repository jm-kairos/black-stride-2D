#include "sim/discovery.h"
#include "game.h"                 // full game_state (NpcShip)
#include "sim/ai_ship.h"          // ShipArchetype
#include "sim/galaxy_map.h"       // galaxy_nearest_node
#include "sim/action_log.h"       // action_log_push
#include "sim/ship.h"                 // ship_bounding_radius
#include <math/bs_hierpos.h>
#include <string.h>               // strncpy
#include <stdio.h>                // snprintf

using namespace bs_math;

// Discovery now uses the ship's existing Layer 1 sensor radius (sensors.layer1_radius); the tick
// below uses the player ship's configured value.

// ---- Persistent NPC discovery registry ----------------------------------------------------
b8 discovery_npc_is_known(const game_state* s, i32 home_node, u64 seed) {
    if (!s) return FALSE;
    for (i32 i = 0; i < s->npc_discovered_count; ++i) {
        if (s->npc_discovered[i].home_node == home_node && s->npc_discovered[i].seed == seed)
            return TRUE;
    }
    return FALSE;
}

static void discovery_npc_remember(game_state* s, i32 home_node, u64 seed) {
    if (discovery_npc_is_known(s, home_node, seed)) return;
    if (s->npc_discovered_count >= DISCOVERY_NPC_MAX) {
        // Evict the oldest to make room (bounded persistence; save/load can widen this later).
        for (i32 i = 1; i < DISCOVERY_NPC_MAX; ++i) s->npc_discovered[i - 1] = s->npc_discovered[i];
        s->npc_discovered_count = DISCOVERY_NPC_MAX - 1;
    }
    s->npc_discovered[s->npc_discovered_count++] = DiscoveredNpc{ home_node, seed };
}

// ---- Discoveries browser feed --------------------------------------------------------------
void discovery_log_push(game_state* s, const char* name, const char* system, u8 kind) {
    if (!s) return;
    if (s->discovery_log_count >= DISCOVERY_LOG_MAX) {
        for (i32 i = 1; i < DISCOVERY_LOG_MAX; ++i) s->discovery_log[i - 1] = s->discovery_log[i];
        s->discovery_log_count = DISCOVERY_LOG_MAX - 1;
    }
    DiscoveryLogEntry& e = s->discovery_log[s->discovery_log_count++];
    snprintf(e.name,   sizeof(e.name),   "%s", name   ? name   : "Unknown");
    snprintf(e.system, sizeof(e.system), "%s", system ? system : "");
    e.time = s->elapsed_time;
    e.kind = kind;
    action_log_push(s, "Discovered: %s", e.name);
}

// ---- Label helpers -------------------------------------------------------------------------
static const char* role_word(u8 archetype) {
    switch (archetype) {
        case ARCHETYPE_PATROL:      return "Patrol";
        case ARCHETYPE_WARSHIP:     return "Warship";
        case ARCHETYPE_INTERCEPTOR: return "Interceptor";
        case ARCHETYPE_TRADER:      return "Trader";
        case ARCHETYPE_SCOUT:       return "Scout";
        case ARCHETYPE_PIRATE:      return "Raider";
        case ARCHETYPE_MINER:       return "Miner";
        default:                    return "Ship";
    }
}

static u8 role_kind(u8 archetype) {
    switch (archetype) {
        case ARCHETYPE_PATROL:      return DISCOVERY_KIND_PATROL;
        case ARCHETYPE_MINER:       return DISCOVERY_KIND_MINER;
        case ARCHETYPE_TRADER:      return DISCOVERY_KIND_TRADER;
        case ARCHETYPE_WARSHIP:
        case ARCHETYPE_INTERCEPTOR:
        case ARCHETYPE_PIRATE:      return DISCOVERY_KIND_WARSHIP;
        default:                    return DISCOVERY_KIND_OTHER;
    }
}

static const char* civ_name_of(const game_state* s, i16 faction) {
    if (faction >= 0 && faction < s->galaxy.civ_count) return s->galaxy.civs[faction].name;
    return "Unknown";
}

static const char* node_name_of(const game_state* s, i32 node) {
    if (node >= 0 && node < s->galaxy.node_count) return s->galaxy.nodes[node].name;
    return "Deep Space";
}

// =====================================================================================
void discovery_update(game_state* s, f32 dt) {
    (void)dt;
    if (!s) return;
    const HierPos2 player = s->player_ship().origin;
    const f32 disc_r = s->player_ship().sensors.layer1_radius;
    const f32 r2     = disc_r * disc_r;
    // ---- NPC garrison ships ------------------------------------------------------------
    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {
        NpcShip& n = s->npc_ships[i];
        if (!n.active || n.discovered) continue;
        Vec2 to = hierpos_diff(&n.ship.origin, &player, BS_HIERPOS_CELL_SIZE);
        if (to.x * to.x + to.y * to.y > r2) continue;
        n.discovered = TRUE;
        discovery_npc_remember(s, n.home_node, n.spawn_seed);
        char name[64];
        snprintf(name, sizeof(name), "%s %s", civ_name_of(s, n.faction), role_word(n.archetype));
        discovery_log_push(s, name, node_name_of(s, n.home_node), role_kind(n.archetype));
    }

    // ---- Per-system civilian stations --------------------------------------------------
    for (i32 si = 0; si < s->galaxy.system_count; ++si) {
        StarSystem& ss = s->galaxy.systems[si];
        for (i32 st = 0; st < ss.station_count; ++st) {
            SystemStation& station = ss.stations[st];
            if (station.discovered) continue;
            Vec2 to = hierpos_diff(&station.pos, &player, BS_HIERPOS_CELL_SIZE);
            if (to.x * to.x + to.y * to.y > r2) continue;
            station.discovered = TRUE;
            i32 sys_node = galaxy_nearest_node(s, &station.pos);
            char name[64];
            snprintf(name, sizeof(name), "%s Station", civ_name_of(s, station.owner_civ));
            discovery_log_push(s, name, node_name_of(s, sys_node), DISCOVERY_KIND_STATION);
        }
    }
}
