# RtsControl

**Responsibility:** Owns the RTS interaction layer — hover detection, issuing move and attack
orders, FTL jump mode, free-camera movement, per-ship piloting entry/exit (the fleet-ship
context menu plus the directed `pilot_ship` / `release_to_autopilot` pair), and the overlays
that visualise all of it. **Box and click selection are always live while the camera is
detached — the command overlay (Space, 0.25x dilation) is RETIRED** (see invariants). It
explicitly does not *execute* orders:
`sim/rts_controls.h` records that orders are now executed by `Fleet`, leaving this module owning
"the input state + transition logic". It also does not own the camera transform (CoordinateFrames)
or zoom (CameraControl), though it moves the free camera.

**Public interface:** `sandbox/source/sim/rts_controls.h` — `inline ship_inside_world_box`;
`struct RtsSelectionBox`; `class RtsControls` with `update`, `draw`, `piloted_index`,
`jump_mode_active`, `pilot_ship`, `release_to_autopilot`.
`ship_inside_world_box` and `RtsSelectionBox` are driven by `update` whenever the camera is
detached.
The object is a `game_state` member, so it is reached through the hub rather than by include —
no other subsystem includes this header.

**Depends on:** FleetControl, ShipCombatModel, CoordinateFrames, GalaxyMapRendering
(`galaxy_pick_planet`), ActionLog (standing-order feedback), Discovery
(`discovery_npc_is_known`, nameplate identification gate), GameStateModel;
engine `core/input.h`, `renderer/renderer.h`,
`renderer/camera2d.h`, `renderer/bs_imgui.h`, `renderer/bs_rml.h`, `math/math_utils.h`,
`renderer/renderer_types.h`, `defines.h`.
**Depended on by:** FleetControl (`piloted_ship_origin` calls `piloted_index`),
FrameOrchestrator.

**Key invariants:**
- **LMB ownership is the MODE SPLIT itself; the command overlay that used to arbitrate it is
  RETIRED.** Detached, the left button is box/click selection; attached, it is the ballistic
  trigger (`game.cpp`'s fire gate tests `free_camera_active`, this module's selection gate
  tests `detached` — the button is never claimed by both). The overlay existed solely to lend
  selection the button for a bounded moment while the trigger was mode-independent; with
  auto-pilot/RTS the default mode and manual gunnery attached-only, commanding stopped being a
  mode and the overlay (plus its 0.25x time dilation and the Space binding) went with it.
  Deliberate pacing is the time-tier buttons. (`ship_inside_world_box` is still `inline` and
  min-relative via `hierpos_diff`, so it stays precise far from the origin.)
- **Selection may be deliberately EMPTY, and nothing re-asserts one.** Clicking empty space
  deselects everything (`select_at_point` with no hit) and RMB with no selection does nothing.
  Only `game_init` pre-selects member 0, so the first RMB of a fresh game still orders
  something. The old overlay-down "flagship permanently selected" fallback is gone with the
  overlay.
- **A drag below `RTS_CLICK_THRESHOLD` routes to `select_at_point`, not `select_in_box`.** The
  point path carries the screen-constant 22 px pick floor; a 3-pixel box would select nothing at
  combat zoom, where a hull is a couple of pixels wide.
- **Orders are ALWAYS live, in both control modes.** The RMB grammar (attack / shift-avoid /
  move), X cancel and F rally run attached or detached — commanding mid-flight needs no mode
  change. Only two pieces stay detached-only: box/click selection (the LMB split above) and
  jump mode (an RTS map interaction; J is gated and the armed mode drops on attach). The
  always-visible roster is the mid-flight selection surface, its clicks being UI-owned.
- **The fleet-ship context menu is THE piloting entry, and the mode toggle is retired.**
  Auto-pilot/RTS is the DEFAULT mode (`game_init` starts detached over the fleet); RMB over a
  hovered friendly hull — in BOTH camera modes — drops a cursor-anchored menu whose one action
  row is "Pilot" (any hull, including a direct hull-to-hull transfer while piloting) or
  "Auto-pilot" (the hull the player is flying right now), plus **"Escort"** whenever a
  selection exists (order the selected ships onto this hull — it absorbed the old overlay's
  RMB-escort gesture, making the menu the whole friendly-hull surface). It mirrors the station
  menu's shape: `ship_menu_*` state in `game_state`, fixed action strings
  ("ship_pilot"/"ship_autopilot"/"ship_escort") resolved against `ship_menu_member` game-side,
  static RML rows gated by `data-if`, a consumed flag so the opening RMB never also issues a
  move order, click-elsewhere dismissal. Suppressed while jump mode is armed (RMB executes the
  jump) and on HOSTILE hover (attack primacy: when a hostile and a friendly marker overlap,
  the order path must see that click); the station menu tests first and wins the rare
  hull-over-station overlap. TAB is gone; the fleet panel's button is release-only and hidden
  while detached.
- **`pilot_ship` transfers by detaching IN PLACE first.** Taking a hull while already piloting
  another sets `free_camera_active` at the current view and then runs the same recenter glide
  the detached path uses — one glide code path, and never two writers (glide + ship-follow) on
  the camera in the same frame. `Fleet::set_piloted` clears the taken hull's move/attack
  orders itself, so nothing stale flies it away after a later release.
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
  not merely present (`< 1.0`).** The cross-fade band covers the compressed engagement
  envelope, so "map look partly visible" overlaps the zooms where a fight is framed — a
  world-directed left click there (the trigger when attached, a selection click when detached)
  would have popped the inspector. Planet browsing stays on the map-look side of the band.
- **World input must be suppressed while a UI panel owns the cursor.** Gated on both
  `bs_imgui_wants_mouse` and `bs_rml_wants_mouse` — the arbitration contract those engine
  headers describe. Enforced by explicit checks; nothing prevents a new call path from skipping
  them.
- **`pilot_ship` and `release_to_autopilot` are no-ops during a recenter glide** (and on a bad
  index / when already detached, respectively) — stated in the header. The context-menu
  trigger also refuses to open mid-glide.
- **Hostile hover does NOT require discovery.** Undiscovered contacts render as generic
  "unidentified" markers and are attack-orderable anyway, because long-range engagement means
  committing to a return the sensors have not identified yet. The hover loop previously skipped
  `!npc_ships[i].discovered`, which made everything outside Layer 1 unclickable.
- **Order links (escort AND attack) are hover-gated and draw in BOTH camera modes.** Hovering
  either end of an escort or attack relationship draws a pulsing dashed line whose dashes
  MARCH from the ordered ship toward its target — the relationship read as motion. Escort is
  green, attack is the attack marker's red; one shared cadence (`ORDER_LINK_*`), one helper
  (`draw_order_link`). Hovering the target shows every inbound link ("who is guarding /
  engaging this hull" at a glance); hovering one ordered ship shows just its own, and a ship
  holding both orders shows both lines. The friendly hover end comes from
  `m_hovered_ship_idx`, the hostile end from `m_hovered_enemy_idx`'s combat entity. Dash
  cadence, march speed and thickness are SCREEN-CONSTANT (px / zoom, the same convention as
  the hull pick floor), which also bounds the segment count by screen length rather than
  world length. Endpoints are inset by each hull's bounding radius, the clock is
  `elapsed_time` (REAL seconds, so the link keeps breathing while paused — it is a UI
  affordance, not a sim object), and the block sits OUTSIDE `draw()`'s detached-only gate,
  unlike the selection/move/attack markers. It is the newest worked example of the
  order-visualisation-as-private-draw-helper pattern.
- **It requires a `game_state*` at construction.** `RtsControls` holds `m_state` as a member,
  which is why `game_init` placement-news it separately (`new (&s->rts_controls)
  RtsControls(s)`) — the only `game_state` member needing a constructor argument. The default
  constructor leaves `m_state` null and runs first during the struct's placement-new.
- **The standing-order gestures live in the always-on grammar now.** Shift-RMB on a hovered
  hostile issues `order_avoid` in both control modes; escort moved into the ship context menu
  (RMB on a plain friendly never reaches the order path — the menu consumed it). Enemy hover
  keeps priority over friendly hover, so attack primacy is preserved when markers overlap.
  Each dispatch pushes an action-log line. *The old LSHIFT collision is RESOLVED:* the
  alt-movement toggle in `game.cpp` now DEFERS to the shift RELEASE and any RMB during the
  hold cancels it, so a shift-RMB avoid issued mid-pilot no longer flips the flight scheme —
  a plain shift tap toggles exactly as before, just on the release edge.
- A narrow accessor/handler window feeds the RmlUi HUD (`piloted_index`, `jump_mode_active`,
  `hovered_index` for the roster's row hover cue; `pilot_ship` / `release_to_autopilot` invoked
  from the action drain for the context-menu rows and the release button) — the HUD reads
  private state through it, documented as such.

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
- **A `sim/` module includes a `render/` header — back to one.** `galaxy_pick_planet` from
  `render/galaxy_map_render.h`. The second inverted edge (`fleet_roster_wants_mouse`) died with
  the roster's migration to the RML HUD — `bs_rml_wants_mouse` covers it now. If more appear,
  extract an input-arbitration seam instead.
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

**Last verified:** 2026-08-13, working tree on `game` (adds the hover NAMEPLATE — an RmlUi
HUD element (`hud.rml` `#nameplate`, hull-anchored via the same data-style-left/top contract
as the galaxy tooltip): a thin translucent plate centered on the hovered hull, vessel name
over the hull class from the registry card (`ship->def->name`). This module owns only WHICH
hull qualifies — `hovered_nameplate_ship()`: fleet always, hostiles once identified
(`discovery_npc_is_known`) — and `game_push_hud` fills the `np_*` snapshot fields; the plate
is `pointer-events: none`, or its own hover would flip `bs_rml_wants_mouse` and kill the
world hover that shows it. A hull whose vessel name IS its class name — the card-name
fallback — collapses to one line; live-verified on the flagship. Earlier the same day: adds
the hover-gated order links —
pulsing screen-constant dashes marching from the ordered ship to its target, escort green /
attack red, one `draw_order_link` helper, drawn in both camera modes; escort live-verified
from both ends, hidden unhovered, attached and detached; attack verified in play. Earlier the
same day: the command overlay — Space, 0.25x
dilation — is RETIRED: selection is always live while detached, the RMB grammar / X / F run in
both control modes, the roster is always visible, Escort moved into the ship context menu, the
manual trigger became attached-only, selection may be deliberately empty, and the LSHIFT
alt-movement toggle defers to release so shift-RMB avoid no longer flips the flight scheme.
`order_move` now clears escort/avoid, closing a latent hole in FleetControl's mutual-exclusion
invariant. Live-verified: fresh game starts with the roster up and flagship pre-selected at
1x; Space inert; box/click select + empty-click deselect + no-selection RMB inert; menu
Pilot/Auto-pilot/Escort rows; escort→move label handoff; attached LMB fires while detached
LMB only selects; shift release-toggle. Previously 2026-08-12: mode toggle retired, per-ship
piloting via the context menu). The RTS number row stays retired — keys 1–5 are the weapon
fire-group selector.
