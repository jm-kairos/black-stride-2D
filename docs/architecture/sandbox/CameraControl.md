# CameraControl

**Responsibility:** Owns camera zoom — reading the wheel into a target, easing the actual zoom
toward it in log space, flipping the arena/galaxy-map view-mode label at the threshold with a
control hand-off, and pinning the point under the cursor (or the ship) while zooming. It
explicitly does not own the camera *transform* (CoordinateFrames), does not own camera panning
or the free camera's movement (RtsControl), and does not own the visual cross-fade the mode flip
implies — `view_arena_weight` is CoordinateFrames', and render sites read `s->view_arena_w`
rather than the mode.

**Public interface:** `sandbox/source/sim/camera_controller.h` — `update_zoom_and_mode`. One
function, ten lines of header.

**Depends on:** CoordinateFrames (`game_screen_to_true_hierpos`, `game_camera_center_hierpos`,
and the `g_zoom_out_speed_gain` global), FleetControl (`piloted_ship_origin`), Backdrop
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
- **The mode flip is a pure label change.** Crossing `ZOOM_MIN` (0.08) sets `s->view.mode`, and
  the comment stresses both looks share one coordinate space so there is no re-anchor and no
  jump — the remaining work is a free-camera hand-off.
- **Control intent survives the round trip.** `global_free_camera_saved` is deliberately *not*
  snapshotted on the outbound crossing, with a comment explaining that doing so would let a
  temporary off-screen fallback overwrite a genuine piloting intent. Re-entering the arena
  restores piloting, glides onto the ship if it is on-screen, and falls back to free camera
  otherwise.
- **The wheel accumulator is frame-scoped**, so this must run inside the game's update or the
  notch is lost — a contract stated in the engine's `core/input.h`.
- **Cursor-pin zoom is suppressed while a planet approach is engaged or a candidate**, because
  the approach logic re-centres on the planet and the two would fight.
- It early-returns when the eased zoom did not change, explicitly to avoid fighting pan and
  follow logic on idle frames.

**Extension points:** A new zoom-driven behaviour reads `s->camera_state.camera.zoom` and
derives a weight, following `view_arena_weight` in CoordinateFrames rather than branching on
`s->view.mode` — the comment in `core/view_transform.cpp` records that as the intended
direction. A new camera mode or hand-off rule belongs in the mode-flip block here; note it must
handle both crossing directions, since the inbound and outbound paths are deliberately
asymmetric.

**Known limitations / tech debt:**
- **A "zoom" function also changes control mode and pokes a render subsystem.** It sets
  `free_camera_active`, initiates the recenter glide, and calls
  `s->render.global_background.notify_system_changed(...)`. The header's one-line summary does
  mention the mode flip, but nothing signals the background call.
- Five tuning constants (`ZOOM_MIN`, `ZOOM_MAX`, `ZOOM_STEP`, `ZOOM_GLOBAL_MIN`,
  `ZOOM_SPEED_RAMP`) are file-static with no editor exposure, while the related
  `g_zoom_out_speed_gain` is a global owned by a *different* subsystem and driven by an editor
  slider in DevPanels. The tuning surface for one behaviour is split across three files.
- It reads the engine input singleton directly for both wheel and mouse position.
- `ZOOM_MIN` (0.08) is the mode threshold while CoordinateFrames' blend band is
  `[0.05, 0.14]` — the band is documented as deliberately straddling the flip point, so the two
  constants must move together and nothing ties them.
- The edit-mode branch writes `camera_hierpos` directly and zeroes the render residual, so this
  function is one of the few places outside CoordinateFrames that mutates the floating-origin
  anchor.
- No null check on `s`.

**Source paths:** `sandbox/source/sim/camera_controller.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
