#include "platform.h"

#include "core/logger.h"
#include "core/input.h"
#include "core/event.h"
#include "renderer/bs_imgui.h" // SDL-free ImGui facade: feed events to ImGui from the pump

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

#if BS_PLATFORM_WINDOWS
#include <windows.h>
#endif

struct InternalState {
    SDL_Window* window;
};

static Uint64 perf_freq;
static Uint64 perf_start;

static keys sdl_scancode_to_bs_key(SDL_Scancode sc);

b8 platform_initialize(
    PlatformState* plat_state,
    const char* app_name,
    i32 x,
    i32 y,
    i32 width,
    i32 height)
{
    plat_state->internal_state = malloc(sizeof(InternalState));
    InternalState* state = (InternalState*)plat_state->internal_state;
    state->window = NULL;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        BS_LOG_FATAL("SDL_Init failed: %s", SDL_GetError());
        return FALSE;
    }

    state->window = SDL_CreateWindow(app_name, width, height, SDL_WINDOW_RESIZABLE);
    if (!state->window)
    {
        BS_LOG_FATAL("SDL_CreateWindow failed: %s", SDL_GetError());
        return FALSE;
    }

    SDL_SetWindowPosition(state->window, x, y);

    perf_freq = SDL_GetPerformanceFrequency();
    perf_start = SDL_GetPerformanceCounter();

    return TRUE;
}

void platform_terminate(PlatformState* plat_state)
{
    InternalState* state = (InternalState*)plat_state->internal_state;
    if (state)
    {
        if (state->window)
        {
            SDL_DestroyWindow(state->window);
            state->window = NULL;
        }
        free(state);
        plat_state->internal_state = NULL;
    }
    SDL_Quit();
}

// SDL3 reports mouse-motion in LOGICAL window coordinates, but the renderer drawable (and
// hence game_state.fb_width/height and the camera projection) is sized in PIXELS. On a HiDPI
// display (e.g. 125%% scaling: 1024x576 logical vs 1280x720 pixels) those spaces differ, so
// raw motion coords would land ~0.8x off from where the UI and world are drawn. Scale logical
// -> pixel here, at the single input boundary, so every downstream consumer (UI hit-test, crew
// pick) agrees with fb_width/height. On a 1.0x display the ratio is 1.0, so this is a no-op.
static void mouse_logical_to_pixel(SDL_Window* win, float lx, float ly, i16* out_x, i16* out_y)
{
    int lw = 0, lh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(win, &lw, &lh);
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    float sx = (lw > 0) ? ((float)pw / (float)lw) : 1.0f;
    float sy = (lh > 0) ? ((float)ph / (float)lh) : 1.0f;
    *out_x = (i16)(lx * sx);
    *out_y = (i16)(ly * sy);
}

b8 platform_pump_messages(PlatformState* plat_state)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
    {
        // Let ImGui see every event first (mouse, keyboard, focus, text). This updates ImGui's
        // WantCaptureMouse/Keyboard flags; the game gates its own world input on bs_imgui_wants_*
        // so input consumed by a UI panel does not also drive the game. Passing &ev as void* keeps
        // this TU free of ImGui headers (the facade casts it back inside the backend TU).
        bs_imgui_process_event(&ev);

        switch (ev.type)
        {
            case SDL_EVENT_QUIT:
            {
                event_context_t data = {};
                event_fire(EVENT_CODE_APPLICATION_QUIT, 0, data);
                return FALSE;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            {
                b8 pressed = (ev.type == SDL_EVENT_KEY_DOWN);
                keys k = sdl_scancode_to_bs_key(ev.key.scancode);
                if (k != KEYS_MAX_KEYS)
                    input_process_key(k, pressed);
            } break;
            case SDL_EVENT_MOUSE_MOTION:
            {
                InternalState* st = (InternalState*)plat_state->internal_state;
                i16 px = 0, py = 0;
                mouse_logical_to_pixel(st->window, ev.motion.x, ev.motion.y, &px, &py);
                input_process_mouse_move(px, py);
            } break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                b8 pressed = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                buttons mb = BUTTON_MAX_BUTTONS;
                switch (ev.button.button)
                {
                    case SDL_BUTTON_LEFT:   mb = BUTTON_LEFT; break;
                    case SDL_BUTTON_MIDDLE: mb = BUTTON_MIDDLE; break;
                    case SDL_BUTTON_RIGHT:  mb = BUTTON_RIGHT; break;
                    default: break;
                }
                if (mb != BUTTON_MAX_BUTTONS)
                    input_process_button(mb, pressed);
            } break;
            case SDL_EVENT_MOUSE_WHEEL:
            {
                if (ev.wheel.y != 0)
                    input_process_mouse_wheel(ev.wheel.y > 0 ? 1 : -1);
            } break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            {
                event_context_t data = {};
                data.data.u16[0] = (u16)ev.window.data1; // new width
                data.data.u16[1] = (u16)ev.window.data2; // new height
                event_fire(EVENT_CODE_WINDOW_RESIZED, 0, data);
            } break;
            default: break;
        }
    }

    return TRUE;
}

VOID_PTR platform_get_window_handle(PlatformState* plat_state)
{
    InternalState* state = (InternalState*)plat_state->internal_state;
    if (!state) return NULL;
    return (VOID_PTR)state->window;
}

VOID_PTR platform_allocate_virtual_memory_commit(VOID_PTR block, u64 commit_size)
{
#if BS_PLATFORM_WINDOWS
    return VirtualAlloc(block, commit_size, MEM_COMMIT, PAGE_READWRITE);
#else
    (void)block; (void)commit_size;
    return NULL; // TODO: mmap-based implementation for other platforms
#endif
}

VOID_PTR platform_allocate_virtual_memory_reserve(u64 reserve_size)
{
#if BS_PLATFORM_WINDOWS
    return VirtualAlloc(NULL, reserve_size, MEM_RESERVE, PAGE_NOACCESS);
#else
    (void)reserve_size;
    return NULL; // TODO: mmap-based implementation for other platforms
#endif
}

void platform_virtual_free(VOID_PTR block, u64 size)
{
#if BS_PLATFORM_WINDOWS
    (void)size;
    VirtualFree(block, 0, MEM_RELEASE);
#else
    (void)block; (void)size;
#endif
}

VOID_PTR platform_allocate(u64 size, b8 aligned)
{
    (void)aligned;
    return malloc(size);
}

void platform_free(VOID_PTR block, b8 aligned)
{
    (void)aligned;
    free(block);
}

VOID_PTR platform_zero_memory(VOID_PTR block, u64 size)
{
    return memset(block, 0, size);
}

VOID_PTR platform_copy_memory(VOID_PTR dest, const VOID_PTR source, u64 size)
{
    return memcpy(dest, source, size);
}

VOID_PTR platform_set_memory(VOID_PTR dest, i32 value, u64 size)
{
    return memset(dest, value, size);
}

void platform_console_write(const char* message, u8 colour)
{
#if BS_PLATFORM_WINDOWS
    HANDLE console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    // FATAL, ERROR, WARN, INFO, DEBUG, TRACE
    static u8 levels[6] = {64, 4, 6, 2, 1, 8};
    SetConsoleTextAttribute(console_handle, levels[colour]);

    OutputDebugStringA(message);
    u64 length = strlen(message);
    LPDWORD number_written = 0;
    WriteConsoleA(console_handle, message, (DWORD)length, number_written, 0);
#else
    (void)colour;
    fputs(message, stdout);
#endif
}

void platform_console_write_error(const char* message, u8 colour)
{
#if BS_PLATFORM_WINDOWS
    HANDLE console_handle = GetStdHandle(STD_ERROR_HANDLE);
    static u8 levels[6] = {64, 4, 6, 2, 1, 8};
    SetConsoleTextAttribute(console_handle, levels[colour]);

    OutputDebugStringA(message);
    u64 length = strlen(message);
    LPDWORD number_written = 0;
    WriteConsoleA(console_handle, message, (DWORD)length, number_written, 0);
#else
    (void)colour;
    fputs(message, stderr);
#endif
}

real platform_get_absolute_time()
{
    Uint64 now = SDL_GetPerformanceCounter();
    return (real)(now - perf_start) / (real)perf_freq;
}

void platform_sleep(u64 ms)
{
    SDL_Delay((Uint32)ms);
}

// Translation from SDL3 scancodes to the engine's `keys` enum (which uses
// historical Win32 virtual-key values). Returns KEYS_MAX_KEYS for unmapped keys.
static keys sdl_scancode_to_bs_key(SDL_Scancode sc)
{
    switch (sc)
    {
        case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
        case SDL_SCANCODE_RETURN:    return KEY_ENTER;
        case SDL_SCANCODE_TAB:       return KEY_TAB;
        case SDL_SCANCODE_LSHIFT:    return KEY_LSHIFT;
        case SDL_SCANCODE_RSHIFT:    return KEY_RSHIFT;
        case SDL_SCANCODE_LCTRL:     return KEY_LCONTROL;
        case SDL_SCANCODE_RCTRL:     return KEY_RCONTROL;
        case SDL_SCANCODE_LALT:      return KEY_LMENU;
        case SDL_SCANCODE_RALT:      return KEY_RMENU;
        case SDL_SCANCODE_PAUSE:     return KEY_PAUSE;
        case SDL_SCANCODE_CAPSLOCK:  return KEY_CAPITAL;
        case SDL_SCANCODE_ESCAPE:    return KEY_ESCAPE;
        case SDL_SCANCODE_SPACE:     return KEY_SPACE;
        case SDL_SCANCODE_PAGEUP:    return KEY_PRIOR;
        case SDL_SCANCODE_PAGEDOWN:  return KEY_NEXT;
        case SDL_SCANCODE_END:       return KEY_END;
        case SDL_SCANCODE_HOME:      return KEY_HOME;
        case SDL_SCANCODE_LEFT:      return KEY_LEFT;
        case SDL_SCANCODE_UP:        return KEY_UP;
        case SDL_SCANCODE_RIGHT:     return KEY_RIGHT;
        case SDL_SCANCODE_DOWN:      return KEY_DOWN;
        case SDL_SCANCODE_PRINTSCREEN: return KEY_SNAPSHOT;
        case SDL_SCANCODE_INSERT:    return KEY_INSERT;
        case SDL_SCANCODE_DELETE:    return KEY_DELETE;
        case SDL_SCANCODE_HELP:      return KEY_HELP;

        case SDL_SCANCODE_A: return KEY_A;
        case SDL_SCANCODE_B: return KEY_B;
        case SDL_SCANCODE_C: return KEY_C;
        case SDL_SCANCODE_D: return KEY_D;
        case SDL_SCANCODE_E: return KEY_E;
        case SDL_SCANCODE_F: return KEY_F;
        case SDL_SCANCODE_G: return KEY_G;
        case SDL_SCANCODE_H: return KEY_H;
        case SDL_SCANCODE_I: return KEY_I;
        case SDL_SCANCODE_J: return KEY_J;
        case SDL_SCANCODE_K: return KEY_K;
        case SDL_SCANCODE_L: return KEY_L;
        case SDL_SCANCODE_M: return KEY_M;
        case SDL_SCANCODE_N: return KEY_N;
        case SDL_SCANCODE_O: return KEY_O;
        case SDL_SCANCODE_P: return KEY_P;
        case SDL_SCANCODE_Q: return KEY_Q;
        case SDL_SCANCODE_R: return KEY_R;
        case SDL_SCANCODE_S: return KEY_S;
        case SDL_SCANCODE_T: return KEY_T;
        case SDL_SCANCODE_U: return KEY_U;
        case SDL_SCANCODE_V: return KEY_V;
        case SDL_SCANCODE_W: return KEY_W;
        case SDL_SCANCODE_X: return KEY_X;
        case SDL_SCANCODE_Y: return KEY_Y;
        case SDL_SCANCODE_Z: return KEY_Z;

        case SDL_SCANCODE_LGUI: return KEY_LWIN;
        case SDL_SCANCODE_RGUI: return KEY_RWIN;
        case SDL_SCANCODE_APPLICATION: return KEY_APPS;
        case SDL_SCANCODE_SLEEP: return KEY_SLEEP;

        case SDL_SCANCODE_KP_0: return KEY_NUMPAD0;
        case SDL_SCANCODE_KP_1: return KEY_NUMPAD1;
        case SDL_SCANCODE_KP_2: return KEY_NUMPAD2;
        case SDL_SCANCODE_KP_3: return KEY_NUMPAD3;
        case SDL_SCANCODE_KP_4: return KEY_NUMPAD4;
        case SDL_SCANCODE_KP_5: return KEY_NUMPAD5;
        case SDL_SCANCODE_KP_6: return KEY_NUMPAD6;
        case SDL_SCANCODE_KP_7: return KEY_NUMPAD7;
        case SDL_SCANCODE_KP_8: return KEY_NUMPAD8;
        case SDL_SCANCODE_KP_9: return KEY_NUMPAD9;
        case SDL_SCANCODE_KP_MULTIPLY: return KEY_MULTIPLY;
        case SDL_SCANCODE_KP_PLUS:     return KEY_ADD;
        case SDL_SCANCODE_KP_MINUS:    return KEY_SUBTRACT;
        case SDL_SCANCODE_KP_PERIOD:   return KEY_DECIMAL;
        case SDL_SCANCODE_KP_DIVIDE:   return KEY_DIVIDE;
        case SDL_SCANCODE_KP_EQUALS:   return KEY_NUMPAD_EQUAL;

        case SDL_SCANCODE_F1:  return KEY_F1;
        case SDL_SCANCODE_F2:  return KEY_F2;
        case SDL_SCANCODE_F3:  return KEY_F3;
        case SDL_SCANCODE_F4:  return KEY_F4;
        case SDL_SCANCODE_F5:  return KEY_F5;
        case SDL_SCANCODE_F6:  return KEY_F6;
        case SDL_SCANCODE_F7:  return KEY_F7;
        case SDL_SCANCODE_F8:  return KEY_F8;
        case SDL_SCANCODE_F9:  return KEY_F9;
        case SDL_SCANCODE_F10: return KEY_F10;
        case SDL_SCANCODE_F11: return KEY_F11;
        case SDL_SCANCODE_F12: return KEY_F12;
        case SDL_SCANCODE_F13: return KEY_F13;
        case SDL_SCANCODE_F14: return KEY_F14;
        case SDL_SCANCODE_F15: return KEY_F15;
        case SDL_SCANCODE_F16: return KEY_F16;
        case SDL_SCANCODE_F17: return KEY_F17;
        case SDL_SCANCODE_F18: return KEY_F18;
        case SDL_SCANCODE_F19: return KEY_F19;
        case SDL_SCANCODE_F20: return KEY_F20;
        case SDL_SCANCODE_F21: return KEY_F21;
        case SDL_SCANCODE_F22: return KEY_F22;
        case SDL_SCANCODE_F23: return KEY_F23;
        case SDL_SCANCODE_F24: return KEY_F24;

        case SDL_SCANCODE_NUMLOCKCLEAR: return KEY_NUMLOCK;
        case SDL_SCANCODE_SCROLLLOCK:   return KEY_SCROLL;

        case SDL_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
        case SDL_SCANCODE_EQUALS:    return KEY_PLUS;
        case SDL_SCANCODE_COMMA:     return KEY_COMMA;
        case SDL_SCANCODE_MINUS:     return KEY_MINUS;
        case SDL_SCANCODE_PERIOD:    return KEY_PERIOD;
        case SDL_SCANCODE_SLASH:     return KEY_SLASH;
        case SDL_SCANCODE_GRAVE:     return KEY_GRAVE;

        default: return KEYS_MAX_KEYS;
    }
}
