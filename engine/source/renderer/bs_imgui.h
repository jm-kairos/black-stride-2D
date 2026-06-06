#pragma once

#include "defines.h"

// =====================================================================================
// Dear ImGui facade — the SDL-free, ImGui-free public surface the rest of the engine and
// the game link against. The implementation lives INSIDE the SDL GPU backend translation
// unit (renderer_backend_sdlgpu.cpp), the only TU permitted to include <SDL3/...> and the
// ImGui headers. That keeps the "only the backend touches SDL/GPU" seam intact: nothing
// here exposes an SDL or ImGui type.
//
// Lifecycle ownership:
//   * bs_imgui_initialize / bs_imgui_shutdown are driven by renderer.cpp, bracketing the
//     backend's own initialize/shutdown (init AFTER the device+window exist; shutdown
//     BEFORE the device is destroyed).
//   * The per-frame NewFrame / Render / draw-record calls are NOT exposed here — they are
//     sequenced internally by the backend's begin_frame / end_frame so they can never fall
//     out of balance with the GPU frame.
//   * bs_imgui_process_event is called by the platform event pump for every SDL event.
//   * bs_imgui_wants_mouse / _keyboard let the game suppress world input while ImGui has
//     keyboard/mouse focus (e.g. cursor over a panel).
// =====================================================================================

// Create the ImGui context and initialize the SDL3 + SDL_GPU ImGui backends. Must be called
// AFTER the renderer backend has created the GPU device and claimed the window. Returns FALSE
// if ImGui or either backend fails to initialize.
bs__api__ b8 bs_imgui_initialize(void);

// Destroy the ImGui backends and context. Must be called BEFORE the renderer backend destroys
// the GPU device (the SDL_GPU ImGui backend owns GPU resources tied to that device).
bs__api__ void bs_imgui_shutdown(void);

// Feed one platform event to ImGui. `sdl_event` is an opaque pointer to an SDL_Event; the
// backend TU casts it back. Safe to call before initialization (no-op until the context exists).
bs__api__ void bs_imgui_process_event(const void* sdl_event);

// TRUE when ImGui currently wants the mouse / keyboard (cursor over a window, editing a text
// field, etc.). The game should gate its own world input on the negation of these so clicks
// and keys consumed by the UI do not also act on the game.
bs__api__ b8 bs_imgui_wants_mouse(void);
bs__api__ b8 bs_imgui_wants_keyboard(void);
