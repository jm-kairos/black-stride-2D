#pragma once

struct game_state;

// ImGui/bs_ui editor & debug panels (built each frame from game_render).
//
// build_editor_panel: the big top-left EDITOR PANEL (edit mode, lights, glow, bloom, background
// layers, coordinate-space diagnostics, nebula, camera, starfield, travel, system view).
// build_transform_panel: top-right TRANSFORM window for the selected entity (edit mode only).
// build_profiler_panel: bottom-left per-subsystem CPU timing readout.
void build_editor_panel(game_state* s);
void build_transform_panel(game_state* s);
void build_profiler_panel(game_state* s);
