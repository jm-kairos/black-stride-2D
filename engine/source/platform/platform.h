#pragma once

#include "defines.h"

struct PlatformState{
    VOID_PTR internal_state; // type of this will be determined on the particular implementation on the cpp file
};

// width/height are IN/OUT: in windowed mode they are the requested client size and pass
// through unchanged; in fullscreen the window takes the desktop resolution and they are
// overwritten with the actual pixel size, so the caller seeds its state from reality.
b8 platform_initialize(
    PlatformState* plat_state,
    const char* app_name,
    i32 x,
    i32 y,
    i32* width,
    i32* height,
    b8 fullscreen);

void platform_terminate(PlatformState* plat_state);

b8 platform_pump_messages(PlatformState* plat_state);

// Returns an opaque handle to the OS/SDL window (an SDL_Window* under the hood).
// The renderer backend uses this to claim the window for the GPU device without
// the renderer needing to know about the platform's internal SDL types.
bs__api__ VOID_PTR platform_get_window_handle(PlatformState* plat_state);

VOID_PTR platform_allocate_virtual_memory_commit(VOID_PTR block, u64 commit_size);

VOID_PTR platform_allocate_virtual_memory_reserve(u64 reserve_size);

void platform_virtual_free(VOID_PTR block, u64 size);

VOID_PTR platform_allocate(u64 size, b8 aligned);
void platform_free(VOID_PTR block, b8 aligned);
VOID_PTR platform_zero_memory(VOID_PTR block, u64 size);
VOID_PTR platform_copy_memory(VOID_PTR dest, const VOID_PTR source, u64 size);
VOID_PTR platform_set_memory(VOID_PTR dest, i32 value, u64 size);

void platform_console_write(const char* message, u8 colour);
void platform_console_write_error(const char* message, u8 colour);

real platform_get_absolute_time();

// Sleep on the thread for the provided ms. This cloks the mais thread.
// Should only be used for giving time back to the OS for unused update power.
// Therefore it is not exported.
void platform_sleep(u64 ms);