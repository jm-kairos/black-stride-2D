# Discovery

**Responsibility:** Owns first-identification of world objects — scanning the player ship's
surroundings each frame for undiscovered NPC agents and per-system stations, marking them
discovered, remembering them across re-visits, and appending to the Discoveries browser feed. It
explicitly does not own how undiscovered objects *look* (three render subsystems draw the
generic "unidentified" marker), does not own the sensor model it uses as a range
(ShipCombatModel's `SensorSuite`), and does not own the browser window — that is an RmlUi
document driven from `game_push_hud`.

**Public interface:** `sandbox/source/sim/discovery.h` — `discovery_npc_is_known`,
`discovery_update`, `discovery_log_push`. `DiscoveredNpc`, `DiscoveryLogEntry`, `DiscoveryKind`,
`DISCOVERY_NPC_MAX` and `DISCOVERY_LOG_MAX` live in `state/game_state.h`.
Used from outside by 2 subsystems.

**Depends on:** LocalAgentAi (`ShipArchetype`), GalaxyRuntime (`galaxy_nearest_node`),
ActionLog, ShipCombatModel, GameStateModel; engine `math/bs_hierpos.h`, `defines.h`.
**Depended on by:** LocalAgentAi, FrameOrchestrator.

**Key invariants:**
- **Discovery range is the ship's `sensors.layer1_radius`, not a dedicated constant.**
  `discovery.h` records this as a deliberate change ("no longer a separate constant or a
  dedicated field"), so installing a sensor module widens discovery range as a side effect —
  the composed-stat behaviour `sim/module.h` describes.
- **Two object kinds use two different persistence mechanisms**, both documented in the header:
  NPC agents are remembered in a global registry keyed by `(home_node, spawn_seed)` because the
  agents themselves are transient; stations set a `discovered` flag that rides along with the
  cached `StarSystem`. Getting an NPC's key wrong means it is re-discovered every visit.
- **One discovery produces two user-visible records.** `discovery_log_push` appends to the
  Discoveries feed *and* calls `action_log_push` for immediate HUD feedback — stated in the
  header.
- Both registries evict the oldest by array shift at capacity, so discoveries can be silently
  forgotten (see tech debt).

**Extension points:** A new discoverable object kind means a value in `DiscoveryKind`, a scan
loop in `discovery_update` following the NPC and station loops, a persistence decision (registry
key versus a flag on the object), and a label mapping — `role_word` and `role_kind` in
`sim/discovery.cpp` are the template for turning an archetype into display text and a browser
category. The scan is a straightforward radius test against
`s->player_ship().sensors.layer1_radius`.

**Known limitations / tech debt:**
- **It scans everything every frame with no spatial index and no throttling** — every cached
  system's every station (`system_count × station_count`) plus all `NPC_SHIP_MAX` (384) slots.
  GalaxyRuntime already maintains a spatial grid this could use.
- **The NPC registry is bounded at `DISCOVERY_NPC_MAX` (512) and evicts the oldest**, with a
  comment acknowledging the bound is provisional ("save/load can widen this later"). Discoveries
  are therefore not permanent.
- Eviction is an O(n) array shift on both registries.
- **It maps AI archetypes to display labels and browser categories** through two parallel switch
  statements (`role_word`, `role_kind`), collapsing warship/interceptor/pirate into one kind —
  a presentation taxonomy living in a simulation module.
- Discovery names are composed from the owning civ plus a role word, so a label depends on
  GalaxyHistory being initialised; `civ_name_of` falls back to "Unknown" and `node_name_of` to
  "Deep Space".
- **In a 1/1 cycle with LocalAgentAi** (`ai_ship.cpp` → `discovery.h`, `discovery.cpp` →
  `ai_ship.h`).
- Only the *player ship* discovers — `discovery_update` uses `s->player_ship()`, so escorts and
  other fleet members contribute nothing, unlike the sensor system which unions across the fleet.
  *Inferred:* that this is a simplification rather than a design intent; the header says
  "single-ship discovery system" in its title, which suggests it is deliberate, but gives no
  reason.

**Source paths:** `sandbox/source/sim/discovery.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
