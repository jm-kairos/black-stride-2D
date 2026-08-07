# BitmapText

**Responsibility:** Owns screen-anchored bitmap text — the embedded 8×8 font data, the GPU atlas
baked from it at startup, string measurement, and drawing strings as one textured quad per
glyph. It explicitly does not own the HUD (that is RmlUi, driven from FrameOrchestrator's
`game_push_hud`); it is the primitive used by in-world debug and overlay labels. It also does
not own any font loading — the atlas is generated from a compiled-in table, and the separate
Consolas HUD font lives engine-side in the ImGui facade.

**Public interface:** `sandbox/source/render/text.h` — `text_init`, `text_shutdown`,
`text_width`, `text_height`, `text_draw`.
`sandbox/source/font8x8.h` — `g_font8x8` (95 glyphs × 8 row bytes), `FONT8X8_FIRST`,
`FONT8X8_LAST`, `FONT8X8_COUNT`, `FONT8X8_GLYPH_W`, `FONT8X8_GLYPH_H`; consumed only by
`render/text.cpp`.

**Depends on:** engine `renderer/renderer.h`, `renderer/camera2d.h`, `renderer/renderer_types.h`,
`math/math_utils.h`, `core/logger.h`, `defines.h`.
**Depended on by:** CoordinateDiagnostics, InWorldOverlays, ShipRendering, FrameOrchestrator.

**Key invariants:**
- **`text_init` must run after `renderer_initialize`.** Stated in `render/text.h`; satisfied by
  `game_init` calling it after the engine has brought the renderer up. Until it succeeds
  `text_draw` is a silent no-op, so a mis-ordered call produces invisible text rather than a
  crash.
- **The atlas pads each 8×8 glyph into a 10×10 cell with a 1 px transparent guard band.** The
  reason is a hard dependency on engine behaviour: the backend's sampler is NEAREST +
  CLAMP_TO_EDGE, so at non-integer scales sampling can land outside the glyph footprint and
  would otherwise pick up the neighbour. `render/text.cpp` documents this. Changing the engine's
  sampler configuration would invalidate the layout.
- **RGB is white everywhere; only alpha carries the glyph**, so tint controls colour and there
  is no dark fringe under interpolation.
- **`text_draw` cancels the camera.** It projects the requested screen pixel into world space,
  sets `sprite.rotation = cam->rotation` to null the view rotation, and sizes the quad as
  `screen_px / zoom`, so the live view-projection maps it back to an axis-aligned pixel-sized
  rectangle. This requires `cam` and the framebuffer size to be the **live** values for the
  frame — a stale camera silently misplaces text.
- **`font8x8`'s bit order is bit 0 = leftmost pixel**, the opposite of the intuitive reading.
  Documented at length in `font8x8.h`; a consumer that gets it backwards renders mirrored
  glyphs. Only `text.cpp` consumes it.
- Indexing requires subtracting `FONT8X8_FIRST`; nothing bounds-checks, but `text_draw` gates on
  the printable range before indexing.

**Extension points:** A new text-drawing variant belongs in `text.h` and should reuse the
camera-cancel construction in `text_draw` rather than positioning quads directly — the header
describes that trick as the same one "the upcoming UI panels use, proven here on plain text
first". Replacing the font means swapping the `g_font8x8` table and the four `FONT8X8_*` macros;
the atlas geometry in `text.cpp` derives cell size, columns and rows from those, so a different
glyph size would follow automatically as long as the bit-order convention holds.

**Known limitations / tech debt:**
- **`text_shutdown` has no callers anywhere**, so the atlas texture is never released. Harmless
  at process exit, but the lifecycle the header describes is only half-wired.
- The atlas is baked into a function-local `static u8 pixels[]` (~38 KB) and retained for the
  process lifetime although it is needed once.
- **Every glyph is a separate sprite in the shared batch**, so a long HUD string competes with
  game content for the engine's 16384-sprite budget. `render/debug_overlay.cpp` alone emits up
  to 49 text labels per frame.
- `text_width` returns the width of the **longest line**, not of a single line — correct for
  layout, surprising for the name.
- Only printable basic Latin (0x20–0x7E) is encoded; other bytes advance a blank cell silently.
- `font8x8.h` is vendored third-party data (Daniel Hepper's font8x8, public domain, derived from
  IBM VGA ROM fonts) living in `sandbox/source` rather than a vendor directory, so it compiles
  under the game's `-Wall -Werror`.
- Text is monospace-only with no kerning, no wrapping, and no clipping.

**Source paths:** `sandbox/source/render/text.{cpp,h}`, `sandbox/source/font8x8.h`

**Last verified:** 2026-08-07, commit `812680c`
