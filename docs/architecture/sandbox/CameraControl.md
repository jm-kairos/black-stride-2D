# CameraControl

**Responsibility:** Owns camera zoom — reading the wheel into a target, easing the actual zoom
toward it in log space, flipping the arena/galaxy-map view-mode label at the threshold, and
pinning the point under the cursor (or the ship) while zooming. It
explicitly does not own the camera *transform* (CoordinateFrames), does not own camera panning
or the free camera's movement (RtsControl), **does not decide whether the camera is attached or
detached at all**, and does not own the visual cross-fade the mode flip
implies — `view_arena_weight` is CoordinateFrames', and render sites read `s->view_arena_w`
rather than the mode.

**Public interface:** `sandbox/source/sim/camera_controller.h` — `update_zoom_and_mode`. One
function, ten lines of header.

**Depends on:** CoordinateFrames (`game_screen_to_true_hierpos`, `game_camera_center_hierpos`,
and the `g_zoom_out_speed_gain` global), Backdrop
(`notify_system_changed`), GameStateModel; engine `core/input.h`, `defines.h`.
**Depended on by:** FrameOrchestrator.

**Key invariants:**
- **Easing and stepping must happen in log space.** Zoom is multiplicative and spans ten decades
  (`ZOOM_GLOBAL_MIN` 7.5e-10 to `ZOOM_MAX` 12.0), so linear interpolation would give wildly
  uneven perceived speed. Enforced by construction in `update_zoom_and_mode`.
- **`ZOOM_GLOBAL_MIN` is 7.5e-10 for a stated reason:** positions render linearly with no
  cosmetic compression, so framing a galaxy disc of radius ~3.2e11 requires that range. That
  single constant is the clearest statement of why the project needs hierarchical coordinates
  at all.
- **The mode flip is a pure label change — now literally.** Crossing `ZOOM_MIN` (0.015) sets
  `s->view.mode` and, on the inbound crossing only, notifies Backdrop which system it is drawing.
  Nothing else happens: both looks share one coordinate space, so there is no re-anchor and no
  jump.
- **Zoom NEVER changes the control mode.** TAB and the HUD pilot button are the only things that
  attach or detach the camera. The outbound crossing used to force `free_camera_active` on and the
  inbound crossing used to restore a remembered intent — which needed `global_free_camera_saved`,
  a ship-on-screen test, and two deliberately asymmetric crossing paths, roughly thirty lines
  whose whole job was undoing the surprise the coupling created. All of it is gone, and
  `global_free_camera_saved` no longer exists. Scrolling means "show me more", not "take the
  helm"; since the ballistic engagement envelope was compressed the tactical picture sits one
  scroll outside the arena band, so the old behaviour handed control away mid-fight exactly when
  the player zoomed out to look at the fight.
- **The backdrop notify is NOT part of the hand-off and must survive it.** It is a render side
  effect that merely lived in the same block; dropping it with the rest leaves a stale parallax
  backdrop after re-entering the arena.
- **The wheel accumulator is frame-scoped**, so this must run inside the game's update or the
  notch is lost — a contract stated in the engine's `core/input.h`.
- **Cursor-pin zoom is suppressed while a planet approach is engaged or a candidate**, because
  the approach logic re-centres on the planet and the two would fight.
- It early-returns when the eased zoom did not change, explicitly to avoid fighting pan and
  follow logic on idle frames.

**Extension points:** A new zoom-driven behaviour reads `s->camera_state.camera.zoom` and
derives a weight, following `view_arena_weight` in CoordinateFrames rather than branching on
`s->view.mode` — the comment in `core/view_transform.cpp` records that as the intended
direction. The mode-flip block is now the place for *render* consequences of crossing the
threshold only — a new one goes beside the backdrop notify. **Do not put control-mode changes
here:** attach/detach belongs to the deliberate toggle sites (TAB in `game.cpp`,
`RtsControls::hud_toggle_pilot_mode`), and re-coupling it to zoom is the thing this module was
explicitly untangled from.

**Known limitations / tech debt:**
- **A "zoom" function still pokes a render subsystem.** It calls
  `s->render.global_background.notify_system_changed(...)` on the inbound crossing, and nothing
  in the header's one-line summary signals it. (It no longer touches the control mode — that
  half of this entry is resolved.)
- Five tuning constants (`ZOOM_MIN`, `ZOOM_MAX`, `ZOOM_STEP`, `ZOOM_GLOBAL_MIN`,
  `ZOOM_SPEED_RAMP`) are file-static with no editor exposure, while the related
  `g_zoom_out_speed_gain` is a global owned by a *different* subsystem and driven by an editor
  slider in DevPanels. The tuning surface for one behaviour is split across three files.
- It reads the engine input singleton directly for both wheel and mouse position.
- **`ZOOM_MIN` (0.015) is the mode threshold while CoordinateFrames' blend band is
  `[0.009, 0.026]`** — the band must straddle the flip point, so the two constants must move
  together and nothing but the comments at both sites ties them. Both were widened from
  `0.08` / `[0.05, 0.14]` so the compressed ballistic engagement envelope (19k–58k units of
  reach) frames *inside* the arena look; half the screen spans `640/zoom` world units, so 0.015
  reaches ~42,700 units.
- **`ZOOM_SPEED_RAMP` (0.005) must stay below `VIEW_MAP_ZOOM`.** At its former 0.02 it sat inside
  the widened arena band, so the wheel accelerated while the player was still framing a fight.
  A fourth constant in the same family with the same untied coupling.
- The edit-mode branch writes `camera_hierpos` directly and zeroes the render residual, so this
  function is one of the few places outside CoordinateFrames that mutates the floating-origin
  anchor.
- No null check on `s`.

**Source paths:** `sandbox/source/sim/camera_controller.{cpp,h}`

**Last verified:** 2026-08-09, commit `b1baf31` (widened arena band; zoom decoupled
from the pilot/auto-pilot decision)
