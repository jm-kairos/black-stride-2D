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

**Depends on:** ShipCombatModel, CoordinateFrames, CelestialParallax, RenderLayerTable,
BitmapText, CoordinateDiagnostics, GameStateModel; engine `renderer/renderer.h`,
`renderer/camera2d.h`, `defines.h`.
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
  map-look star light, and exhaust sets `custom.x = speed_ratio * glow_mul` to drive heat
  distortion. The channel meanings exist only in HLSL.
- **`arsenal_drag_fits` must mirror `arsenal_drop_on_slot`** in `game.cpp` — the comment says so
  — or the green/red world feedback disagrees with what a drop actually does. Two switch
  statements over six drag kinds kept in step by hand.
- Mapped visual layers are skipped entirely unless all four textures resolved, so one missing map
  silently drops the layer.

**Extension points:** A new per-ship annotation is a function in `ship_render.h` following
`draw_hardpoint_overlay` / `draw_collider_outline`: read `ship->render_pos`, convert ship-local
positions with `ship_local_dir`, and draw on `LAYER_DEBUG` or `LAYER_GIZMO`. A new mount art
kind follows `draw_turret` / `draw_radar_dish` in `ship_render.cpp` — flat-shaded rectangles via
`draw_solid_rect`, dispatched from `draw_ship_mounts` on what occupies the hardpoint. A new hull
layer kind is a `VisualLayerKind` in ShipCombatModel plus a branch in `draw_ship_visual_ex`.

**Known limitations / tech debt:**
- **The `render_pos` write is an undocumented cross-subsystem contract.** Nothing in either
  header states that five other modules depend on this pass having run.
- **`arsenal_drag_fits` duplicates loadout validation from `game.cpp`**, acknowledged in-comment
  as a mirror. A render module encodes gameplay rules.
- **The two-entity assumption leaks in.** The flagship-inspector sub-pass gates on
  `ship == &s->player_ship()`, matching WorldEditor's hardcoded player/enemy model.
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
- Mount art exists because no turret PNGs do; the rectangles are a stand-in for missing assets.
- Two sizing conventions sit side by side and are correct for different calls: emblem geometry
  divides by zoom, while `renderer_draw_line` thickness is passed as a plain screen-pixel
  constant because the engine divides internally.

**Source paths:** `sandbox/source/render/ship_scene.{cpp,h}`,
`sandbox/source/render/ship_render.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
