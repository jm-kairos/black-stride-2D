# ShipRendering

**Responsibility:** Owns everything drawn *for a ship* — computing each entity's transient
render-space position, per-ship directional star lighting, hull sprite layers, procedural mount
art (turrets, radar dishes), engine exhaust, the sensor-gated enemy, NPC hulls, non-ship combat
quads, clustered fleet emblems, collider outlines, hardpoint annotations and drag feedback. It
explicitly does not own the ship *model* (ShipCombatModel, including `ShipVisual`), does not own
the overlays drawn *around* ships (InWorldOverlays), and does not own the loadout rules it
mirrors for drag feedback.

**Public interface:** `sandbox/source/render/ship_scene.h` — `draw_ship_scene`.
`sandbox/source/render/ship_render.h` — `draw_fleet_emblems`, `draw_collider_outline`,
`draw_hardpoint_overlay`, `draw_hardpoint_highlight`, `draw_hardpoint_drag_feedback`,
`draw_ship_mounts`.
Used from outside: `ship_render.h` by 3 subsystems, `ship_scene.h` by 2.

**Depends on:** ShipCombatModel (including `sim/weapon_def.h` for authored mount art),
CoordinateFrames, CelestialParallax, RenderLayerTable, BitmapText, CoordinateDiagnostics,
GameStateModel; engine `renderer/renderer.h`, `renderer/camera2d.h`, `defines.h`.
**Depended on by:** SceneOrchestration, InWorldOverlays, DevPanels, FrameOrchestrator.

**Key invariants:**
- **This pass writes `Ship::render_pos`, which five other subsystems read.** It populates it for
  every fleet ship, the enemy, every NPC and every non-ship combat entity. `ship_render.cpp`,
  `out_sensor_detection_fx.cpp` and the overlays all depend on running *after* it. This is the
  most consequential invariant in the render tree and it appears in **no header** — only
  `ship_scene.h`'s note that the pass "computes render-space positions".
- **Persistent `HierPos2` simulation state is never mutated by rendering.** `render_pos` is the
  transient shadow; the comment in `ship_scene.cpp` is explicit about the distinction.
- **`s->star_pos` is written by whichever of two passes runs.** `draw_ship_scene` recomputes it
  when `view_arena_w <= 0` (the deep galaxy-map side, where the parallax pass is skipped), so the
  field stays defined across the whole zoom range without a mode branch. Two passes write one
  field depending on zoom.
- **`bs_sprite.custom` is used as a shader flag field in three distinct ways here**, each
  documented against `sprite.frag.hlsl`: hull art zeroes `custom.x`/`custom.w` so ship-glow
  parameters cannot warp the hull, sets `custom.z = 1` to mark it self-emissive against
  map-look star light, and exhaust jets set `custom = {heat, time, 1, 0}` — heat drives the
  travelling distortion and the white-hot-head/red-tail temperature ramp, scaled by commanded
  thrust. The channel meanings exist only in HLSL.
- **The exhaust pass draws from `ShipFlight`'s thruster telemetry, never from velocity.**
  `draw_engine_exhaust` reads `thrust_vis`/`turn_vis` (see FleetControl for the write/consume
  contract): main drives burn with commanded FORWARD thrust, RCS puffs fire for
  strafe/reverse/turn on the flank a real control jet would exhaust from, and a hull nobody
  is thrusting shows only the idle pilot lights — a coasting ship burns nothing. Every jet
  is a teardrop plume (`exhaust_plume_texture`) anchored head-at-nozzle via `origin {0.5, 1}`
  — the same idiom as the missile plume in `projectile.cpp` — and sits on `LAYER_EXHAUST`
  (9), one below the hulls, so a flame can never glow on top of ship art. The enemy derelict
  draws no exhaust, correctly: it has no `ShipFlight` and no engines to light.
- **Nozzle SOURCES are hull-accurate: the card's authored `thruster` lines are the layout,
  and each nozzle's burn is a PROJECTION, not a wiring table.** `draw_engine_exhaust` walks
  `ship->def->thrusters` (art-pixel positions + exhaust facings — see ShipCombatModel for
  the format) and computes each nozzle's burn as the commanded thrust projected onto what
  that nozzle can do: linear thrust against its force direction, turn against its torque
  sign (`cross(pos, force)`, unit-normalized). That is what makes twin off-axis mains
  thrust differentially in a turn with zero per-nozzle code. A card that authors no
  thrusters falls back to a derived stern-plus-corners layout, so a bare hull card still
  reads as a ship. All five catalog cards author their nozzles today.
- **A main drive is a layered effect stack keyed on TWO scalars — burn and heat — not one.**
  `draw_main_flame` composes: a bell glow whose colour runs ember-red → white-hot on
  `ShipFlight::heat_vis` (thermal, lags the throttle by seconds — a drive that just cut
  keeps glowing while it coasts); an ignition flare where burn runs far ahead of heat (a
  cold start's overbright transient, gone in half a second as the bell soaks); three plumes
  with per-layer flicker phases and a small tail sway about the head pivot, the halo drawn
  wider and shorter than its slot (vacuum expansion); a mach-diamond core train
  (`exhaust_core_texture`) fading in above `EXHAUST_DIAMOND_MIN` burn; a time-scrolled
  ember stream racing aft; and a light SPILL on `LAYER_EXHAUST_SPILL`, above the hull, so
  the drive visibly lights its own stern plating. Twin engines are de-phased by nozzle
  index so they never pulse in lockstep. RCS jets are deliberately NOT flames: a cold-gas
  blue outer cone with a bright dart core and a fast shimmer.
- **`arsenal_drag_fits` must mirror `arsenal_drop_on_slot`** in `game.cpp` — the comment says so
  — or the green/red world feedback disagrees with what a drop actually does. Two switch
  statements over six drag kinds kept in step by hand.
- Mapped visual layers are skipped entirely unless all four textures resolved, so one missing map
  silently drops the layer.

**Extension points:** A new per-ship annotation is a function in `ship_render.h` following
`draw_hardpoint_overlay` / `draw_collider_outline`: read `ship->render_pos`, convert ship-local
positions with `ship_local_dir`, and draw on `LAYER_DEBUG` or `LAYER_GIZMO`. **Giving a weapon
authored turret art is a data edit, not a code edit** — add `mount_art` / `mount_art_size` /
`mount_art_pivot` to its `.weapon` def and `draw_turret` picks it up through
`Weapon::def`; the geometry is authored in hardpoint half-extents so it scales with the slot.
**Every drawn feature of a mount now resolves its unit through `ship_hardpoint_unit`** rather
than multiplying `hardpoint_half_extent` by `world_scale` in each pass — that helper folds in the
slot's `art_scale`, so the hardpoint editor's size slider moves the authored art, the procedural
turret and dish rectangles, the barrel origins and the charge-up glow together. Editor
affordances (`draw_hardpoint_overlay`, `draw_hardpoint_highlight`, `draw_hardpoint_drag_feedback`
and the hit-tests) deliberately keep using the raw half-extent: they size to the SLOT, which the
scale does not change.
A mount with no art (railguns, missile racks, point defense) keeps the rectangles, so the two
looks coexist per weapon. A weapon's `muzzle` offsets are authored in those same units and
resolved against the same `mount_aim` this pass draws with (in `ship_hardpoint_fire`), so shots
leave the barrel the player is looking at; retuning `mount_art_size` means retuning the muzzles
with it. A new *procedural* mount art kind still follows `draw_turret` /
`draw_radar_dish` — flat-shaded rectangles via `draw_solid_rect`, dispatched from
`draw_ship_mounts` on what occupies the hardpoint. A new hull layer kind is a
`VisualLayerKind` in ShipCombatModel plus a branch in `draw_ship_visual_ex`.

**Known limitations / tech debt:**
- **The `render_pos` write is an undocumented cross-subsystem contract.** Nothing in either
  header states that five other modules depend on this pass having run.
- **`arsenal_drag_fits` duplicates loadout validation from `game.cpp`**, acknowledged in-comment
  as a mirror. A render module encodes gameplay rules.
- **The two-entity assumption leaks in** via WorldEditor's hardcoded player/enemy model. *(The
  inspector sub-pass no longer contributes: it gates on `ship == &s->inspected_ship()`, any
  fleet member, and `arsenal_drag_fits` takes the fleet-wide pool ship as a second parameter.)*
- Per-ship light direction is recomputed for every hull from that ship toward `s->star_pos`,
  which the code itself concedes is redundant for a distant star.
- It iterates `NPC_SHIP_MAX` (384) unconditionally and sets `render_pos` even for undiscovered
  NPCs before skipping their draw — necessary so the marker pass can position them, but not
  stated anywhere.
- **Enemy visibility uses `s->ship_sensor_range`**, a *different* field from the
  `galaxy.map_sensor_range` the celestial passes use and from the per-ship `SensorSuite` the
  overlays use — three sensor ranges in the render path.
- `draw_fleet_emblems` implements union-find with path halving purely for visual clustering, at
  O(n²) over `FLEET_MAX_SHIPS` recomputed every frame.
- Only two ship types have emblems (`SHIP_TYPE_DRONE`, `SHIP_TYPE_EXTRACTOR`); any other cluster
  is silently skipped *after* the clustering work is done.
- `EMBLEM_NO_GLOW` is a file-static glow struct attached as `glow_override` to every emblem —
  which both disables glow and keeps emblems in one draw run, since the engine breaks runs on
  glow-pointer identity. A performance dependency on a static's address.
- The procedural rectangles were a stand-in for missing turret PNGs. Cannons now have art
  (`assets/weapons/assets/tier_1_cannon_*.png`) and go through `draw_mount_art`; everything else
  is still the stand-in. **`mount_art_pivot` is eyeballed against the art**, not derived from it —
  the art's rotation centre is its base, which no metadata records, so re-authoring a turret PNG
  means re-tuning that number by looking at it.
- Two sizing conventions sit side by side and are correct for different calls: emblem geometry
  divides by zoom, while `renderer_draw_line` thickness is passed as a plain screen-pixel
  constant because the engine divides internally.

**Source paths:** `sandbox/source/render/ship_scene.{cpp,h}`,
`sandbox/source/render/ship_render.{cpp,h}`

**Last verified:** 2026-08-15, working tree on `game` (same day, latest: the portrait well grows
to the WHOLE middle zone — `ship_portrait_rect` now returns a generally NON-square x/y/w/h (the
zone minus margin, no longer a centred square), the square 1024² target shows through it as an
RML image decorator with `cover` fit (crops the central band via texcoords; an oversized `<img>`
under an overflow:hidden wrapper does NOT clip in RmlUi and leaked over the tab panel —
verified live, see hud.rcss), `ship_portrait_camera` fits the hull against the visible band
(`1024·min/max` target px) and `ship_portrait_screen_to_world` folds the crop offset into the
cursor mapping; the HUD model trades `insp_portrait_size` for `insp_portrait_w/h`
(engine `bs_rml.h` + backend bindings). Live-verified end-to-end: full-zone well, undistorted
fit, clean crop at the well edges under zoom, and a bay-tile drop onto a hardpoint through the
zoomed+panned+cropped mapping mounts on the exact box under the cursor. Same day, earlier: the
inspector portrait
becomes an interactive view — `ship_portrait_camera` folds `game_state.insp_view_zoom/_pan`
(wheel zoom pinned to the cursor, left-drag pan on empty portrait space; written by game.cpp's
`update_portrait_view_input`, reset on open/hull switch) over the fit framing, and the new
`ship_portrait_screen_to_world` in `ship_scene.h` is the one cursor→world mapping game.cpp's
pin/pan math and `ship_portrait_hardpoint_at` both go through — every portrait consumer builds
its camera from `ship_portrait_camera`, so drawn hull, hardpoint hit-test and input math cannot
disagree; live-verified: zoom pins the bow under the cursor, pan tracks the drag, a press on a
hardpoint box suppresses the pan so the loadout drag keeps priority. Same day, earlier: the flagship card
`assets/ships/ship/ship.ship` trades its `layer sprite` for the 4-map `layer mapped` — the
maps are regenerated by `tools/map_extractor` (see `docs/MAP_EXTRACTOR_TOOL.md`) from the
top-down art plus the side-view silhouette's dorsal elevation profile, and the engine's
mapped pass now also shades scene point lights (thruster burns, weapon flashes — collected in
SceneOrchestration's frame lighting) per-pixel against the hull's normal map; live-verified,
a held main burn warms the stern plating. Same day, earlier: the main drive
becomes a burn+heat effect stack — thermal bell glow, cold-start ignition flare,
per-layer flicker/tail-sway plumes with a ballooned halo, mach-diamond core, ember stream,
and stern light spill on `LAYER_EXHAUST_SPILL`; RCS becomes a two-layer cold-gas dart;
live-verified: twin flares on cold ignition, bells stay lit coasting after cutoff,
diamond-banded cores at full burn, spill visible on the stern plating. Earlier: nozzles
become AUTHORED data: the pass
walks the card's `thruster` lines and projects the thrust telemetry onto each nozzle — linear
against force direction, turn against torque sign — with the derived stern-plus-corners
layout demoted to a no-lines fallback; live-verified on the vanguard: twin mains burn from
the art's two nozzle blocks, the CCW diagonal fires bow-stbd + stern-port plus a faint
differential burn on the starboard main). Previously 2026-08-14 (the exhaust pass is rewritten around
thruster telemetry: `draw_engine_exhaust` takes the ship's `ShipFlight`, draws a head-anchored
teardrop main plume + six corner RCS jets + an idle nozzle glow on the new `LAYER_EXHAUST`,
and the speed-ratio parameter is gone — live-verified across pilot keys, coasting, and an RTS
move order's brake/arrive cycle). Previously 2026-08-12 (adds the ship-portrait sub-pass in
`ship_scene.cpp`: `ship_portrait_submit` captures the inspected hull + mounts + hardpoint
skeleton + drag fit-feedback into the engine's offscreen portrait scope with its own camera,
plus one thumbnail scope per fleet member (`renderer_thumb_begin`, `portrait_fit_camera`) for
the inspector's live fleet list; `ship_portrait_hardpoint_at` maps the cursor through the
portrait rect for the drag-mount flow, and the layout constants now mirror the one-window
inspector (title bar, 250/400 columns, tab panel). The world-side inspector sub-pass is
retired, `draw_weapon_group_digits` retired in place with it). Previously verified 2026-08-10 (mount draw units from
`ship_hardpoint_unit`, adding the slot's `art_scale`)
