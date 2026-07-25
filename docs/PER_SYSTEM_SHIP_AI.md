# Per-System Ship AI — Implementation Details

Cross-system ship AI: ships travel across star systems pursuing simple objectives, and
materialise into live, killable agents when they reach the player's current system.

This document describes the whole feature and then focuses on **Step 4** (the macro ↔ local
materialisation handoff) with its verification procedure.

---

## 1. Two-tier architecture

The feature is split into two deliberately separate tiers:

| Tier | Type | Where it lives | Lifetime | Units / motion |
|------|------|----------------|----------|----------------|
| **Macro** | `ShipMission` | `GalaxyState.missions[]` | Persistent, player-independent; exists everywhere at all times | Walks the galaxy lane graph in **in-game hours** |
| **Local** | `NpcShip` | `game_state.npc_ships[NPC_SHIP_MAX=96]` | Transient; only exists in the player's current system; cleared on system change | Live flight sim in **world units / seconds** |

- **Macro** owns galaxy-scale travel and objectives. Code: `sim/ship_mission.{h,cpp}`.
- **Local** owns live in-arena agents (flight, combat, perception). Code: `sim/ai_ship.{h,cpp}`.
- **Step 4** is the bridge: a macro mission parked in the player's system spawns a local agent, and
  the local agent's fate (e.g. destruction) writes back to the macro mission.

Rendering:
- Macro missions render as moving civ-coloured pips on the galaxy overview
  ([galaxy_map_render.cpp](../sandbox/source/render/galaxy_map_render.cpp), reusing
  `ship_mission_position()`).
- Local agents render as full ship hulls + markers in the arena
  ([ship_scene.cpp](../sandbox/source/render/ship_scene.cpp),
  [gameplay_overlays.cpp](../sandbox/source/render/gameplay_overlays.cpp)).

---

## 2. Data model

### `ShipMission` — [state/game_state.h](../sandbox/source/state/game_state.h)

```cpp
struct ShipMission {
    i16  owner;       // owning civilization index (>=0) or a static faction sentinel
    u8   archetype;   // ShipArchetype governing it when materialised
    u8   objective;   // MissionObjective
    i32  dest_node;   // final destination galaxy node (route target)

    // Graph position: parked at a node, or in transit along a lane
    i32  at_node;     // current node when parked; -1 while in transit
    i32  from_node;   // lane origin  (valid while in transit)
    i32  to_node;     // lane target  (valid while in transit)
    f32  progress;    // 0..1 fraction along the current from->to lane

    // Cached route toward dest_node (node hops, excluding the start node)
    i32  route[MISSION_ROUTE_MAX]; // MISSION_ROUTE_MAX = 32
    i32  route_len;
    i32  route_pos;

    // Objective state (Step 3: OBJ_TRADE)
    i32  home_node;   // trader's base system; shuttles home <-> a wealth-ranked market
    f32  dwell_hours; // >0 while parked "delivering" at an endpoint (counts down before departing)

    u64  seed;        // deterministic per-mission rng seed
    b8   active;
};
```

Constants: `#define MISSION_ROUTE_MAX 32`, `#define MISSION_MAX 512`.

```cpp
enum MissionObjective : u8 { OBJ_IDLE=0, OBJ_TRADE, OBJ_REINFORCE, OBJ_PATROL, OBJ_RAID };
```

### `NpcShip` — [state/game_state.h](../sandbox/source/state/game_state.h)

Step 4 added a single field linking a live agent back to its macro mission:

```cpp
struct NpcShip {
    // ... ship, flight, faction, archetype, hp, home_node, rng, spawn_seed, discovered ...
    i32 mission_id;   // >=0 = live form of galaxy.missions[mission_id]; -1 = ambient/garrison
    b8  active;
};
```

### `GalaxyState` (relevant fields) — [state/game_state.h](../sandbox/source/state/game_state.h)

- `ShipMission* missions; i32 mission_count; i32 mission_capacity;`
- `i16* node_owner;` — civ index per node (-1 = unclaimed).
- `i32 current_system;` — **a cache SLOT, not a node index**.
- `i32* cache_node;` — maps cache slot → galaxy node index.
- `GalaxyNode nodes[]; i32 node_count;` — each node has `galaxy_center` (HierPos2), `seed`, `best_habitability`, `orbit_radii[]`, etc.
- `Civilization civs[]; i32 civ_count;` — each has `color`, `power`, `status` (0 = alive).

---

## 3. Macro tier — `sim/ship_mission.{h,cpp}`

### Public API — [ship_mission.h](../sandbox/source/sim/ship_mission.h)

```cpp
void            ship_missions_seed(game_state* s);
void            ship_missions_update(game_state* s, f32 sim_dt_hours);
HierPos2        ship_mission_position(const game_state* s, const ShipMission& m);
void            ship_mission_notify_destroyed(game_state* s, i32 mission_id); // Step 4 writeback
```

### Tuning constants — [ship_mission.cpp](../sandbox/source/sim/ship_mission.cpp)

| Constant | Value | Meaning |
|----------|-------|---------|
| `MISSION_SPEED` | `2.0e7` u/hr | Lane spacing ~2.5e8 u ⇒ ~12 in-game hours per hop (~12 real s at 1×) |
| `MISSION_SEED_COUNT` | `16` | Debug travelers seeded at generation end |
| `ROUTE_SCRATCH` | `256` | Full-route scratch buffer (galaxy diameter ~66 hops > `MISSION_ROUTE_MAX`) |
| `TRADE_DWELL_HOURS` | `18.0` | In-game hours a trader dwells "delivering" at each endpoint |
| `TRADE_MARKET_SAMPLES` | `8` | Owned nodes sampled when choosing a market |
| `TRADE_INTERNAL_BONUS` | `4.0` | Score ×multiplier for a market inside the trader's own empire |

### Behaviour

- **Seeding** (`ship_missions_seed`): deterministically spawns up to 16 traders launched from
  **owned** nodes, each `ARCHETYPE_TRADER` / `OBJ_TRADE`, with `home_node = start` and a per-mission
  `seed`. Called once at generation end (after garrison seeding).
- **Routing** (`mission_repath`): full path found via `galaxy_route_find` into `ROUTE_SCRATCH`, then
  only the first `MISSION_ROUTE_MAX` hops are cached; long routes are walked in chunks and re-pathed
  on exhaustion.
- **Trade objective**: `trade_value(node) = (0.5 + hab/255) * (1 + civs[owner].power)`, 0 for
  unowned. `trade_pick_market` samples owned nodes, scores `value × (0.5 + roll)`, ×`TRADE_INTERNAL_BONUS`
  when same owner as the trader (most trade stays intra-empire; occasional cross-border routes).
  A trader shuttles **home ↔ market**, dwelling `TRADE_DWELL_HOURS` at each endpoint.
- **Motion** (`mission_travel_step`): dwell countdown before departing; on arrival either
  `trade_arrive` (flip home↔market, re-path) or a random retarget; transit advances `progress` by
  `MISSION_SPEED * hours / lane_length`.
- **Position** (`ship_mission_position`): node centre when parked, else `hierpos_lerp` along the
  current lane by `progress`.
- **Update** (`ship_missions_update`): per-frame; **no-op when `sim_dt_hours <= 0`** (paused).

---

## 4. Local tier — `sim/ai_ship.{h,cpp}`

- **Archetypes** — [ai_ship.h](../sandbox/source/sim/ai_ship.h):
  `ARCHETYPE_PATROL, WARSHIP, INTERCEPTOR, TRADER, SCOUT, PIRATE, MINER`.
- **Shared hull template**: `s->npc_template` is loaded **once** from `assets/enemy_ship.ship` in
  `ai_ships_init`. Every `NpcShip` is `n.ship = s->npc_template` (struct copy sharing textures) — so
  **all archetypes share one hull**, differentiated only by owner-civ colour tint (and, in the
  overlay, by marker shape). There is no per-archetype/per-trader art.
- **`spawn_npc(s, faction, pos, home_node, archetype, seed) -> slot`**: finds a free pool slot; sets
  ship/faction/hp/home/etc.; defaults `mission_id = -1`.
- **`system_anchor(s, node, role, index, seed) -> HierPos2`**: role-based spawn position
  (MINER → outer belt, TRADER → planet lane, PATROL/else → star/planet ring).
- **`materialize_system(s, node, owner)`**: spawns the deterministic **ambient** population
  (patrols from garrison, miners, traders) on system change.
- **`ai_ships_populate(s)`**: on player system change, **clears the whole pool** and re-materialises
  ambient pop for the new owned system. Runs throttled (≈0.3 s) inside `ai_ships_update`.

---

## 5. Step 4 — Macro ↔ local materialisation handoff

**Goal.** When a cross-system **trade** mission is parked (dwelling) in the player's current system,
spawn it as a live, killable `NpcShip`; when it departs or the player leaves, remove that live agent;
and if the player destroys it, permanently retire the macro mission (it stops travelling/rendering).

**Scope decision (current):** *materialise only `ARCHETYPE_TRADER` missions.* Other objectives stay
macro-only until their own step. There is **no** auto-respawn of destroyed missions.

### Key timing insight

Missions only **dwell** at endpoints (`at_node` stays set for ~18 in-game hours). Intermediate hops
pass *within a single* `mission_travel_step` call, so a mission is essentially never observed
"parked" mid-route. Therefore materialisation keys on **`m.at_node == node_here`** — i.e. the trader
is docked/delivering in your system. This yields a clean "a trader is visiting your system" moment.

### 5.1 `NpcShip.mission_id` (link field)

Added to `NpcShip`: `>= 0` means this agent is the live form of `galaxy.missions[mission_id]`;
`-1` means it's an ambient/garrison ship. `spawn_npc` defaults it to `-1`.

### 5.2 `ai_ships_sync_missions(s)` — [ai_ship.cpp](../sandbox/source/sim/ai_ship.cpp)

Static helper, called **every frame after** `ai_ships_populate` (so mission ships survive the
on-system-change pool clear):

```cpp
static void ai_ships_sync_missions(game_state* s) {
    GalaxyState& g = s->galaxy;
    // current_system is a cache SLOT; map it to the galaxy node.
    i32 node_here = (g.current_system >= 0 && g.current_system < g.system_count)
                        ? g.cache_node[g.current_system] : -1;

    // (1) DESPAWN stale: any mission-backed agent whose mission ended or is no longer parked here.
    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {
        NpcShip& n = s->npc_ships[i];
        if (!n.active || n.mission_id < 0) continue;
        b8 stale = (node_here < 0) || !g.missions ||
                   n.mission_id >= g.mission_count ||
                   !g.missions[n.mission_id].active ||
                   g.missions[n.mission_id].at_node != node_here;
        if (stale) n.active = FALSE;
    }

    if (node_here < 0 || !g.missions) return;

    // (2) SPAWN new: trade missions parked at node_here with no live agent yet.
    for (i32 mi = 0; mi < g.mission_count; ++mi) {
        ShipMission& m = g.missions[mi];
        if (!m.active || m.at_node != node_here) continue;
        if (m.archetype != ARCHETYPE_TRADER) continue;   // traders only, for now
        b8 already = FALSE;
        for (i32 i = 0; i < NPC_SHIP_MAX; ++i)
            if (s->npc_ships[i].active && s->npc_ships[i].mission_id == mi) { already = TRUE; break; }
        if (already) continue;
        i16 owner = (m.owner >= 0) ? m.owner : (i16)galaxy_history_owner_at_node(s, node_here);
        HierPos2 pos = system_anchor(s, node_here, ARCHETYPE_TRADER, 900 + mi, m.seed);
        i32 slot = spawn_npc(s, owner, pos, node_here, ARCHETYPE_TRADER, m.seed);
        if (slot >= 0) {
            s->npc_ships[slot].mission_id = mi;
            BS_LOG_INFO("ShipAI trade: mission %d materialized in current system", mi);
        }
    }
}
```

Cost is negligible: pool 96 × `mission_count` (≤16) per frame.

### 5.3 Kill writeback — `ai_ship_damage` — [ai_ship.cpp](../sandbox/source/sim/ai_ship.cpp)

On death, a mission-backed agent retires its **macro** mission instead of decrementing the local
garrison:

```cpp
if (n.hp <= 0.0f) {
    n.active = FALSE;
    if (n.faction >= 0) galaxy_history_player_raid(s, n.faction, 3.0f); // aggression vs the civ
    if (n.mission_id >= 0) {
        ship_mission_notify_destroyed(s, n.mission_id);   // retire the traveler
    } else {
        galaxy_history_garrison_add(s, n.home_node, -1);  // ambient garrison shrinks
    }
}
```

### 5.4 `ship_mission_notify_destroyed` — [ship_mission.cpp](../sandbox/source/sim/ship_mission.cpp)

```cpp
void ship_mission_notify_destroyed(game_state* s, i32 mission_id) {
    GalaxyState& g = s->galaxy;
    if (!g.missions || mission_id < 0 || mission_id >= g.mission_count) return;
    ShipMission& m = g.missions[mission_id];
    if (!m.active) return;
    m.active = FALSE;   // frees the pool slot; stops travelling & rendering
    BS_LOG_INFO("ShipAI trade: mission %d destroyed in-system (macro retired)", mission_id);
}
```

---

## 6. Per-frame wiring — [game.cpp](../sandbox/source/game.cpp)

Order matters (all within one `game_update`):

1. `sim_dt = dt * time_scale`, clamped to **≤ 0.05 in-game hours/frame** (caps time-accel motion),
   then `sim_hours += sim_dt`.
2. `galaxy_history_live_tick(s, sim_dt)`.
3. `ship_missions_update(s, sim_dt)` — macro travel advances.
4. `galaxy_materialize_update(s)` — **sets `current_system`** (nearest node's cache slot).
5. `ai_ships_update(s, sim_dt)` (inside `!editor.edit_mode_active`) →
   `ai_ships_populate` (throttled) → **`ai_ships_sync_missions`** (every frame) → per-agent ticks.

Generation: `ship_missions_seed(s)` is called at generation stage 3, after
`galaxy_history_seed_garrison`.

---

## 7. Invariants & gotchas

- **`current_system` is a cache SLOT, not a node.** Always map via
  `node = galaxy.cache_node[current_system]`.
- **`sim_dt` is clamped to 0.05 h/frame** — time acceleration is bounded by this per-frame cap.
- **Sync must run after populate**, because populate clears the whole pool on a system change;
  running sync afterward re-adds any co-located mission agents that frame.
- **Mission ships skip garrison writeback** — they are travelers, not garrison; only their macro
  mission is affected on death.
- **One shared hull template** — all NPC/mission ships use `assets/enemy_ship.ship`, tinted by
  owner civ colour. No per-trader sprite exists.

---

## 8. Step 4 verification procedure

Build (from `sandbox/`): `build.bat` — expect a clean build (no errors/warnings).

In-game (New Game):

1. **Materialise:** Fly to a system where a trader dot is **dwelling** (parked/delivering). A live
   trader `NpcShip` should appear at a planet-lane anchor. Debug log:
   `ShipAI trade: mission <id> materialized in current system`.
2. **Dematerialise on depart/leave:** Wait for the trader to finish its dwell and depart, **or**
   leave the system yourself. The live agent disappears; the macro pip resumes travelling on the
   galaxy overview.
3. **Kill writeback:** While the trader is materialised, destroy it. Expect:
   - Its macro pip disappears **permanently** (mission retired). Debug log:
     `ShipAI trade: mission <id> destroyed in-system (macro retired)`.
   - A reputation hit against its civilization (`galaxy_history_player_raid`).
   - The local **garrison is unaffected** (mission ships skip `galaxy_history_garrison_add`).
4. **No duplicate spawns:** A parked mission yields exactly **one** live agent (the `already` guard),
   and re-entering the system re-materialises it cleanly.

Expected: materialisation is limited to `ARCHETYPE_TRADER`; other objectives remain macro-only.

---

## 9. Known limitations / next steps

- **Traders only** materialise; other objectives (Reinforce, Patrol, Raid) are macro-only.
- **No auto-respawn** of destroyed missions (attrition is permanent until a top-up step is added).
- **No in-transit interception** — materialisation happens only when a mission is parked at a node,
  not in interstellar space.
- **No economy mutation** — trade is directional/behavioural only (no credits/goods fields exist yet).
- **Single shared hull** — no per-archetype or per-trader visuals.
- **Step 5 (planned):** richer objectives — Reinforce coupled to garrison/war, Pirate raids to
  wild space — which will unlock the remaining deferred Ship AI gaps.
