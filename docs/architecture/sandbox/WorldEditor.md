# WorldEditor

**Responsibility:** Owns in-world entity editing — hit-testing lights and ships under the cursor,
the drag state machine for translation and rotation, and the geometry of the transform gizmo. It
explicitly does not draw the gizmo (InWorldOverlays does, calling back into this module's
hit-test for hover feedback), does not own the editor *panels* (DevPanels), and does not own the
edit-mode toggle or selection storage — `EditSelection` and `EditorDrag` live on `game_state`.

**Public interface:** `sandbox/source/sim/editor_tools.h` — `update_edit_mode`;
`edit_entity_position`; `edit_pick_gizmo`; and the geometry helpers `gizmo_axis_len`,
`gizmo_ring_radius_ship`, `gizmo_ring_radius_light`, `gizmo_arrow_size`.
Used from outside by 2 subsystems.

**Depends on:** CoordinateFrames (`mouse_true_hierpos`, `mouse_true_world`), Geometry2D,
GameStateModel; engine `core/input.h`, `renderer/bs_imgui.h`, `renderer/bs_rml.h`,
`math/bs_hierpos.h`, `defines.h`.
**Depended on by:** InWorldOverlays, FrameOrchestrator.

**Key invariants:**
- **Hover feedback must match the real hit-test.** `edit_pick_gizmo` is exported specifically so
  the gizmo renderer can colour the hovered part using the *same* function
  `update_edit_mode` uses to decide what a click activates — `editor_tools.h` states this as the
  reason. Duplicating the logic in the renderer would let the highlight and the action diverge.
- **Editing must be suppressed while a UI panel owns the cursor.** Gated on
  `bs_imgui_wants_mouse` and `bs_rml_wants_mouse` — the header mentions the first; the
  implementation checks both.
- **Gizmo sizes are screen-constant.** Every geometry helper takes `zoom_inv` and returns target
  screen pixels × inverse zoom, documented in the header. Consumers must pass a consistent
  `zoom_inv` or the gizmo and its hit-test disagree.
- Lights are picked by their **centre** within a small screen-space tolerance, not by radius — a
  deliberate choice noted in `sim/editor_tools.cpp`.
- `edit_entity_position` returns `{0,0}` for `EDIT_NONE`, which the gizmo renderer relies on.

**Extension points:** A new editable entity kind is a value in `EditEntityKind`
(`state/game_state.h`) plus cases in the four accessor switches here — `edit_entity_position`,
`edit_entity_angle`, `edit_entity_set_position`, `edit_entity_set_angle` — and a branch in
`edit_pick`. A new gizmo handle is a value in `EditDragMode`, a hit-test branch in
`edit_pick_gizmo`, a drag branch in `update_edit_mode`, and drawing in InWorldOverlays; the
existing X axis, Y axis and rotation ring are the template.

**Known limitations / tech debt:**
- **Only two entities are editable, hardcoded.** Selection index 0 maps to the player ship and
  anything else to the enemy, repeated across four switch statements. The fleet's other members
  cannot be selected despite `EDIT_SHIP` looking general — and `InWorldOverlays` mirrors the same
  two-entity assumption when drawing the selection highlight.
- **Editor lights are the one entity class not natively in the hierarchical frame.** They are
  stored as `Vec2` and round-tripped through `HierPos2` on every pick and every write
  (`hierpos_from_vec2` / `hierpos_to_vec2`), unlike ships which are `HierPos2` throughout.
- The 20-pixel light pick tolerance is a magic constant with no tuning hook.
- `EditDragMode` is declared `: int` in `state/game_state.h` **specifically so this header can
  forward-declare it opaquely** — the only place in the sandbox using that C++11 technique, and
  a workaround for the god struct's size rather than a design choice.
- It reads the engine input singleton directly for the left mouse button.
- There is no undo, no multi-select, and no numeric entry; the transform panel in DevPanels is
  the only way to type a value.
- Nothing persists edits — changes to light positions or ship poses are lost on restart, unlike
  CelestialFx's planet parameters which are written to `bin/planet_editor.cfg`.

**Source paths:** `sandbox/source/sim/editor_tools.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
