# TravelDebug

**Responsibility:** Owns a parameterised point-to-point journey interpolator over hierarchical
positions — initialise, advance by a per-second progress fraction, reset, retarget. It is used
only by the editor-gated travel debug overlay. It explicitly does not own any gameplay travel:
actual cross-system movement is MacroMissions' multi-stage lane machine, and FTL jumps are
FleetControl's. It also does not own its own enable state — `travel_enabled` / `travel_paused`
live on `game_state` alongside the struct's own `active` / `paused`.

**Public interface:** `sandbox/source/sim/travel.h` — `enum TravelEaseMode`,
`struct TravelState`, `travel_init`, `travel_update`, `travel_reset`, `travel_set_destination`.
Used from outside by GameStateModel only (which embeds `TravelState` as `s->travel`); the
overlay that draws it lives in InWorldOverlays and reads the struct directly.

**Depends on:** engine `math/bs_hierpos.h`, `defines.h`. **Nothing else** — it is one of only
three sandbox subsystems that never touch `game_state`, which makes it independently testable.
**Depended on by:** GameStateModel (by embedding); InWorldOverlays reads its fields to draw the
path.

**Key invariants:**
- **Interpolation must use `hierpos_lerp`, not `Vec2` interpolation.** `travel_update` does, and
  this is exactly the ~50,000-unit scenario the engine's `bs_hierpos_selftest` hardcodes a case
  for — the engine test and this module are the two halves of that concern.
- **`speed` is a fraction per second, not a velocity.** `travel_init` sets `0.15f`, so any
  journey takes ~6.7 seconds regardless of distance. Documented in `sim/travel.h` as "fraction
  per second"; this is the one field whose units are surprising and the one place it is written
  down.
- `travel_update` is a no-op when inactive or paused, and clamps progress to 1.0 before
  deactivating.
- The cached `world_x` / `world_y` `f64` pair is refreshed on every state change and is
  documented as diagnostics-only — nothing should treat it as authoritative.

**Extension points:** Selecting a different easing is a one-line change: `ease_mode` is a field
on `TravelState` and `travel_ease` already implements smoothstep and quad-in-out alongside
linear. A new ease is a value in `TravelEaseMode` plus a case in `travel_ease`. Because the
module takes no `game_state`, a second concurrent journey is just a second `TravelState`
instance — nothing here assumes there is only one.

**Known limitations / tech debt:**
- **Near-dead.** The feature is editor-gated (`s->travel_enabled`) and exists to visualise the
  interpolator; no gameplay path uses it.
- **Two of the three ease modes are unreachable.** `travel_init` always selects
  `TRAVEL_EASE_LINEAR` and nothing else writes `ease_mode`, so smoothstep and quad-in-out are
  dead despite being implemented.
- **State is duplicated.** `TravelState` carries `active` and `paused`, and `game_state` carries
  separate `travel_enabled` and `travel_paused` flags — two layers of the same concept with no
  stated relationship.
- `travel_reset` reactivates the journey but leaves `paused` untouched, unlike `travel_init`
  which sets both — an asymmetry that will surprise a caller resetting a paused journey.
- The `f64` world cache is computed on every update purely for the debug overlay, so the update
  path pays a conversion for a display feature.
- Distance-independent timing means the overlay does not demonstrate anything about real travel
  speed; *inferred:* the module was written to exercise `hierpos_lerp` precision rather than to
  model travel, which would explain the fixed-fraction speed — but nothing says so.

**Source paths:** `sandbox/source/sim/travel.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
