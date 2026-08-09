# RtsControl

**Responsibility:** Owns the RTS interaction layer — hover detection, issuing move and attack
orders, FTL jump mode, free-camera movement, the pilot/auto-pilot toggle, and the overlays that
visualise all of it. **Box and click selection are retired** (see invariants). It explicitly does not *execute* orders:
`sim/rts_controls.h` records that orders are now executed by `Fleet`, leaving this module owning
"the input state + transition logic". It also does not own the camera transform (CoordinateFrames)
or zoom (CameraControl), though it moves the free camera.

**Public interface:** `sandbox/source/sim/rts_controls.h` — `inline ship_inside_world_box`;
`struct RtsSelectionBox`; `class RtsControls` with `update`, `draw`, `piloted_index`,
`jump_mode_active`, `hud_toggle_pilot_mode`.
`ship_inside_world_box` and `RtsSelectionBox` are still declared and still correct, but nothing
drives them — see the retirement note below before assuming they are live.
The object is a `game_state` member, so it is reached through the hub rather than by include —
no other subsystem includes this header.

**Depends on:** FleetControl, ShipCombatModel, CoordinateFrames, GalaxyMapRendering
(`galaxy_pick_planet`), GameStateModel; engine `core/input.h`, `renderer/renderer.h`,
`renderer/camera2d.h`, `renderer/bs_imgui.h`, `renderer/bs_rml.h`, `math/math_utils.h`,
`renderer/renderer_types.h`, `defines.h`.
**Depended on by:** FleetControl (`piloted_ship_origin` calls `piloted_index`),
FrameOrchestrator.

**Key invariants:**
- **Box and click selection are RETIRED, and that is what freed the left button.** The game has
  one hull, so every box drag and every click resolved to "the ship" or "nothing", while LEFT
  BUTTON is the ballistic trigger in both control modes — and it could not be, because selection
  owned it while detached (that is exactly why `game.cpp`'s fire gate tested
  `free_camera_active`). The hull is instead held permanently selected, so everything downstream
  that asks "what is selected" — the RMB order path, jump mode's `any_selected` gate, X's
  per-selection attack clear, `draw()`'s selection rect — behaves as it did after a click, every
  frame, with no click. Kept and not deleted, per the convention the M and P keys and
  `combat_arena_update_enemy_orbit` already set: `RtsSelectionBox`, `m_box`,
  `ship_inside_world_box`, `draw_rect_from_screen_box` and `RTS_CLICK_THRESHOLD` all still
  compile and are still correct. `m_box.active` stays FALSE for the process lifetime, so
  `draw()`'s drag-box branch is inert without a change there. Restoring multi-unit selection
  means re-driving this, not rewriting it. (`ship_inside_world_box` remains `inline` and
  min-relative via `hierpos_diff` so it stays precise far from the galaxy origin — the property
  that made it correct is preserved for whoever revives it.)
- **Hull hit-testing has a SCREEN-CONSTANT pick floor (`SHIP_PICK_FLOOR_PX` 22).** A hull's
  bounding radius is a world quantity, so it shrinks with zoom: the cruiser's ~816 units is a
  2 px target once the view frames an engagement, while `draw_enemy_marker` paints its reticle at
  a screen-constant 22 px. The player aims at the marker and misses the hull. The floor matches
  the reticle so the drawn affordance and the click target are the same size at every zoom. With
  box selection retired this is no longer a convenience — **it is how a target gets designated.**
- **The number row is NOT ours.** Keys 1–5 are the weapon fire-group selector in both control
  modes (`game.cpp`). The old detached-only "number-row piloting selection" is retired in place:
  with one hull it was already inert (NUM2–NUM4 guarded on fleet counts that never occur, NUM1
  re-piloted the ship that was already piloted), so retiring it removed a live collision at no
  cost.
- **The planet-inspector left-click requires the map look to be DOMINANT (`view_arena_w < 0.5`),
  not merely present (`< 1.0`).** The left button is now the ballistic trigger and the cross-fade
  band was widened to cover the compressed engagement envelope, so "map look partly visible" now
  overlaps the zooms where a fight is framed — firing past a planet would have popped its
  inspector.
- **World input must be suppressed while a UI panel owns the cursor.** Gated on both
  `bs_imgui_wants_mouse` and `bs_rml_wants_mouse` — the arbitration contract those engine
  headers describe. Enforced by explicit checks; nothing prevents a new call path from skipping
  them.
- **`hud_toggle_pilot_mode` is a no-op during a recenter glide**, with the HUD button rendered
  dimmed in that state — stated in the header.
- **Hostile hover does NOT require discovery.** Undiscovered contacts render as generic
  "unidentified" markers and are attack-orderable anyway, because long-range engagement means
  committing to a return the sensors have not identified yet. The hover loop previously skipped
  `!npc_ships[i].discovered`, which made everything outside Layer 1 unclickable.
- **It requires a `game_state*` at construction.** `RtsControls` holds `m_state` as a member,
  which is why `game_init` placement-news it separately (`new (&s->rts_controls)
  RtsControls(s)`) — the only `game_state` member needing a constructor argument. The default
  constructor leaves `m_state` null and runs first during the struct's placement-new.
- Three accessors exist solely to feed the RmlUi HUD snapshot (`piloted_index`,
  `jump_mode_active`, `hud_toggle_pilot_mode`), documented as such — the HUD reads private state
  through a narrow window.

**Extension points:** A new order or selection gesture is handled in `RtsControls::update`
(input interpretation and transition) and issued through a `Fleet` method — the split the header
describes. Its visualisation is a private draw helper called from `RtsControls::draw`, following
`draw_move_marker` / `draw_attack_marker` / `draw_selection_rect`. A new HUD-driven control is
an accessor here plus a field in `bs_rml_hud_state` and a handler in `game_push_hud`'s action
drain.

**Known limitations / tech debt:**
- **`HOVER_CIRCLE_LAYER` is the literal `50` with the comment "same as `LAYER_UI` in
  game.cpp"** — a hand-copied layer constant rather than an include of `core/render_layers.h`,
  so the two can drift.
- **A `sim/` module includes a `render/` header.** It calls `galaxy_pick_planet` from
  `render/galaxy_map_render.h` — one of only two places that inverted dependency occurs.
- **The class both owns input state and draws**, with seven private rendering helpers, so it
  spans simulation and presentation in one object — unlike every other sim module, which
  exposes state for a render pass to consume.
- Free-camera movement is a **third locomotion model** with its own constants (1600 units/s,
  ×3 on shift, a 24-pixel edge-pan margin), independent of both the ship flight model and
  `steering`.
- ~16 appearance and behaviour constants are file-static `constexpr` with no editor exposure.
  One of them, `RTS_CLICK_THRESHOLD` (the 4-pixel click-vs-drag threshold), is now
  `[[maybe_unused]]`: the build is `-Werror` on unused constants and nothing references it while
  selection is retired.
- **The hover RING still draws at the raw `ship_bounding_radius`** while the hit-test uses the
  22 px floor, so at combat zoom the clickable area is slightly larger than the drawn ring — the
  same drawn-vs-clickable mismatch the floor fixed, now inverted and much smaller.
- Two `constexpr bs_color` initialisers rely on aggregate initialisation of an engine POD in a
  `constexpr` context — fine today, but it couples to that type staying an aggregate.
- `RtsControls` is one of only two real C++ classes in the sandbox (with `Fleet`).
- `m_hovered_enemy_idx` indexes combat entities while `m_hovered_ship_idx` indexes fleet
  members — two different index spaces in adjacent fields.

**Source paths:** `sandbox/source/sim/rts_controls.{cpp,h}`

**Last verified:** 2026-08-09, commit `b1baf31` (box/click selection and the RTS
number row retired; screen-constant hull pick floor)
