# RtsControl

**Responsibility:** Owns the RTS interaction layer — hover detection, issuing move and attack
orders, FTL jump mode, free-camera movement, the pilot/auto-pilot toggle, and the overlays that
visualise all of it. **Box and click selection are live again, driven from the command overlay**
(see invariants). It explicitly does not *execute* orders:
`sim/rts_controls.h` records that orders are now executed by `Fleet`, leaving this module owning
"the input state + transition logic". It also does not own the camera transform (CoordinateFrames)
or zoom (CameraControl), though it moves the free camera.

**Public interface:** `sandbox/source/sim/rts_controls.h` — `inline ship_inside_world_box`;
`struct RtsSelectionBox`; `class RtsControls` with `update`, `draw`, `piloted_index`,
`jump_mode_active`, `hud_toggle_pilot_mode`.
`ship_inside_world_box` and `RtsSelectionBox` are driven again by `update`, gated on
`game_state::command_overlay_active`.
The object is a `game_state` member, so it is reached through the hub rather than by include —
no other subsystem includes this header.

**Depends on:** FleetControl, ShipCombatModel, CoordinateFrames, GalaxyMapRendering
(`galaxy_pick_planet`), InWorldOverlays (`fleet_roster_wants_mouse`), GameStateModel;
engine `core/input.h`, `renderer/renderer.h`,
`renderer/camera2d.h`, `renderer/bs_imgui.h`, `renderer/bs_rml.h`, `math/math_utils.h`,
`renderer/renderer_types.h`, `defines.h`.
**Depended on by:** FleetControl (`piloted_ship_origin` calls `piloted_index`),
FrameOrchestrator.

**Key invariants:**
- **Selection is RESTORED, and the command overlay is what made that possible.** LEFT BUTTON is
  the ballistic trigger in both control modes; selection takes it only while
  `command_overlay_active`, a bounded moment the player opened deliberately. `game.cpp`'s fire
  gate tests the same flag, so the button is never claimed by both. It was retired — not
  deleted — for exactly this: `RtsSelectionBox`, `m_box`, `ship_inside_world_box`,
  `draw_rect_from_screen_box` and `RTS_CLICK_THRESHOLD` were all kept compiling, and restoring
  multi-unit selection was re-driving them, not rewriting them. (`ship_inside_world_box` is
  still `inline` and min-relative via `hierpos_diff`, so it stays precise far from the origin.)
- **With the overlay DOWN, the flagship is still held permanently selected**, so the RMB order
  path, jump mode's `any_selected` gate and `draw()`'s selection rect behave as they did after a
  click, with no click — unchanged from the retired arrangement.
- **A drag below `RTS_CLICK_THRESHOLD` routes to `select_at_point`, not `select_in_box`.** The
  point path carries the screen-constant 22 px pick floor; a 3-pixel box would select nothing at
  combat zoom, where a hull is a couple of pixels wide.
- **The selection block runs when `detached || command_overlay_active`**, widened from
  detached-only so the player can command mid-flight without first handing the ship to the
  autopilot. That is the whole point of the overlay being separate from TAB.
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
- **A `sim/` module includes `render/` headers — now two of them.** `galaxy_pick_planet` from
  `render/galaxy_map_render.h`, and `fleet_roster_wants_mouse` from `render/fleet_roster.h`
  (the roster is drawn over the world rather than in a layer that arbitrates for itself, so it
  must be asked explicitly or a chip click also drags a selection box behind the panel). If a
  third appears, extract an input-arbitration seam instead.
- **The class both owns input state and draws**, with seven private rendering helpers, so it
  spans simulation and presentation in one object — unlike every other sim module, which
  exposes state for a render pass to consume.
- Free-camera movement is a **third locomotion model** with its own constants (1600 units/s,
  ×3 on shift, a 24-pixel edge-pan margin), independent of both the ship flight model and
  `steering`.
- ~16 appearance and behaviour constants are file-static `constexpr` with no editor exposure.
  (`RTS_CLICK_THRESHOLD` lost its `[[maybe_unused]]` when selection came back.)
- **The hover RING still draws at the raw `ship_bounding_radius`** while the hit-test uses the
  22 px floor, so at combat zoom the clickable area is slightly larger than the drawn ring — the
  same drawn-vs-clickable mismatch the floor fixed, now inverted and much smaller.
- Two `constexpr bs_color` initialisers rely on aggregate initialisation of an engine POD in a
  `constexpr` context — fine today, but it couples to that type staying an aggregate.
- `RtsControls` is one of only two real C++ classes in the sandbox (with `Fleet`).
- `m_hovered_enemy_idx` indexes combat entities while `m_hovered_ship_idx` indexes fleet
  members — two different index spaces in adjacent fields.

**Source paths:** `sandbox/source/sim/rts_controls.{cpp,h}`

**Last verified:** 2026-08-10, working tree on `game` (box/click selection restored behind the
command overlay; roster consulted for cursor ownership). The RTS number row stays retired — keys
1–5 are the weapon fire-group selector.
