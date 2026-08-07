# RtsControl

**Responsibility:** Owns the RTS interaction layer — hover detection, box and click selection,
issuing move and attack orders, FTL jump mode, free-camera movement, the pilot/auto-pilot
toggle, and the overlays that visualise all of it. It explicitly does not *execute* orders:
`sim/rts_controls.h` records that orders are now executed by `Fleet`, leaving this module owning
"the input state + transition logic". It also does not own the camera transform (CoordinateFrames)
or zoom (CameraControl), though it moves the free camera.

**Public interface:** `sandbox/source/sim/rts_controls.h` — `inline ship_inside_world_box`;
`struct RtsSelectionBox`; `class RtsControls` with `update`, `draw`, `piloted_index`,
`jump_mode_active`, `hud_toggle_pilot_mode`.
The object is a `game_state` member, so it is reached through the hub rather than by include —
no other subsystem includes this header.

**Depends on:** FleetControl, ShipCombatModel, CoordinateFrames, GalaxyMapRendering
(`galaxy_pick_planet`), GameStateModel; engine `core/input.h`, `renderer/renderer.h`,
`renderer/camera2d.h`, `renderer/bs_imgui.h`, `renderer/bs_rml.h`, `math/math_utils.h`,
`renderer/renderer_types.h`, `defines.h`.
**Depended on by:** FleetControl (`piloted_ship_origin` calls `piloted_index`),
FrameOrchestrator.

**Key invariants:**
- **Box selection must stay precise far from the galaxy origin.** `ship_inside_world_box` is
  `inline` in the header and works in a min-relative frame via `hierpos_diff`, explicitly "so
  the test stays precise far from the galaxy origin". Naive `f32` comparison would fail at
  galaxy scale.
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
- ~16 appearance and behaviour constants are file-static `constexpr` with no editor exposure,
  including a 4-pixel drag threshold that distinguishes a click from a box selection.
- Two `constexpr bs_color` initialisers rely on aggregate initialisation of an engine POD in a
  `constexpr` context — fine today, but it couples to that type staying an aggregate.
- `RtsControls` is one of only two real C++ classes in the sandbox (with `Fleet`).
- `m_hovered_enemy_idx` indexes combat entities while `m_hovered_ship_idx` indexes fleet
  members — two different index spaces in adjacent fields.

**Source paths:** `sandbox/source/sim/rts_controls.{cpp,h}`

**Last verified:** 2026-08-07, commit `e4d88d1`
