# Widgets

**Responsibility:** Owns an engine-native immediate-mode widget vocabulary — anchored panels,
floating windows, text, buttons, sliders, combos, progress bars and layout helpers — and
translates each call into Dear ImGui's backend-agnostic core. It exists so gameplay code can
build UI in `b8`/`f32`/`bs_color` without ever seeing an ImGui type. It explicitly does not own
ImGui's lifecycle or context (UiFacade creates it; this subsystem only draws into it), does not
own the platform or GPU backends (it includes `<imgui.h>` but never SDL or the GPU headers), and
owns no state of its own — everything lives in ImGui's shared global context.

**Public interface:** `engine/source/renderer/bs_ui.h` — `enum BsUiAnchor`, `enum BsUiType`;
containers `bs_ui_begin_panel`/`_end_panel`, `bs_ui_begin_window`/`_end_window`,
`bs_ui_begin_hud_panel`/`_end_hud_panel`; widgets `bs_ui_text`, `_text_colored`, `_progress`,
`_button`, `_button_sized`, `_checkbox`, `_slider_float`, `_color_edit3`, `_combo`,
`_selectable`, `_color_button`, `_label_at`; layout and state `bs_ui_same_line`,
`_set_cursor_pos_x`, `_separator`, `_is_window_hovered`, `_push_alpha`/`_pop_alpha`.
All 24 are `bs__api__`.

**Depends on:** UiFacade (for `bs_imgui_get_hud_font`), Foundation, RenderFrontend (only for
`bs_color`, used in one signature at `bs_ui.h:118`).
**Depended on by:** **nothing engine-side.** 8 sandbox files use 18 of the 24 functions —
`bs_ui_text` and `_text_colored` in 6 each, `_button` and `_separator` in 5.

**Key invariants:**
- **It shares ImGui's global context with the GPU backend with no handoff.** The context is
  created by `bs_imgui_initialize` inside `renderer_backend_sdlgpu.cpp`; because both TUs link
  into `engine.dll`, widgets built here land on the same draw list the backend records in
  `end_frame`. The coupling is entirely through ImGui's own global state — no shared engine
  variable, no accessor, nothing that would fail at link time if the ordering were wrong. Stated
  at `bs_ui.cpp:4-7`.
- **`bs_ui.cpp` includes `<imgui.h>` but never SDL or the backend header** (`:10-13`) — a second
  tier of the isolation rule, so the "only the backend touches SDL/GPU" seam still holds.
- **Begin/end must be paired even when begin returns FALSE.** ImGui's rule, stated emphatically
  at `bs_ui.h:24`; violating it corrupts ImGui's window stack at runtime with no compile-time
  signal. Unenforced.
- **Calls must happen between `renderer_begin_frame` and `renderer_end_frame`**, i.e. inside the
  game's `render(dt)` (`bs_ui.h:17`) — that is where the backend has an open ImGui frame.
  Unenforced.
- **Titles double as ImGui identity.** `bs_ui_begin_panel` passes `title` straight to
  `ImGui::Begin` (`bs_ui.cpp:68`), with the code noting gameplay panels are singletons so
  collisions are assumed away. The header repeatedly instructs callers to keep titles unique and
  suffix `"##<n>"` on repeated rows.
- `bs_ui_end_hud_panel` must unwind exactly what its opener pushed — a style colour, a style var
  and a font (`bs_ui.cpp:233-252`). Both ends guard on a null font, so a missing HUD font
  degrades gracefully rather than unbalancing the stack.

**Extension points:** Adding a widget is a two-place edit: a `bs__api__` declaration in
`bs_ui.h` and a translating implementation in `bs_ui.cpp` that marshals engine types into ImGui
types (`b8` → `bool`, `bs_color` → `ImVec4`). Every existing function follows that shape.
Adding a panel *style* follows `bs_ui_begin_hud_panel`/`_end_hud_panel` (`bs_ui.cpp:233-252`):
push style state, delegate to `bs_ui_begin_panel`, and pop symmetrically in the closer.
Adding an anchor means a value in `BsUiAnchor` (`bs_ui.h:34-41`) plus a `case` computing
position and pivot in `bs_ui_begin_panel` (`bs_ui.cpp:33-60`).

**Known limitations / tech debt:**
- **`BS_UI_TYPE_EDITOR` does nothing.** `bs_ui_begin_panel` applies anchoring and window flags
  only for `BS_UI_TYPE_GAME`; the editor case falls through the switch with no flags and no
  positioning (`bs_ui.cpp:21-66`), so the `anchor` and `margin` arguments are **silently ignored
  for editor panels** — which is what the two editor panels in `sandbox/source/ui/editor_ui.cpp`
  pass. The enum promises a distinction the code does not make.
- **`bs_ui_checkbox` takes a native `bool*`** (`bs_ui.h:100`) while every other boolean in the
  API is `b8` — the one leak of a C++ type through an otherwise engine-native surface.
- **`bs_ui_combo` takes a `"A\0B\0C\0"` NUL-separated item list** (`bs_ui.h:111`), an ImGui
  convention passed through unchanged despite the header claiming to hide ImGui.
- `bs_ui_button` reimplements ImGui's own button-width formula by hand
  (`bs_ui.cpp:128-131`) to stop long labels clipping inside an `AlwaysAutoResize` window. The
  comment argues it converges in one frame without oscillation — a layout feedback loop managed
  manually.
- `bs_ui_label_at` creates a full borderless ImGui window *per label* and calls
  `SetWindowFontScale` (`bs_ui.cpp:211-231`) — a heavyweight path for one line of text, and each
  needs a unique `id`.
- Six exported functions have no callers anywhere: `bs_ui_color_button`, `_is_window_hovered`,
  `_push_alpha`, `_pop_alpha`, `_begin_hud_panel`, `_end_hud_panel` — so the HUD-styled panel
  pair and the whole alpha-fade mechanism are currently unused.
- Several pairings the compiler cannot check: begin/end panel, begin/end window, push/pop alpha,
  and the font push inside the HUD pair.
- The header directs callers to `bs_imgui_wants_mouse`/`_keyboard` for input gating and notes
  this "replaces the old `ui_wants_mouse()`" (`bs_ui.h:26-28`) — a superseded API referenced but
  no longer present.

**Source paths:** `engine/source/renderer/bs_ui.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
