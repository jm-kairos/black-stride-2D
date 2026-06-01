#include "platform.h"

// Windows platform layer.
#if IBX_PLATFORM_WINDOWS

#include "core/logger.h"
#include "core/input.h"
#include "core/event.h"

#include <windows.h>
#include <windowsx.h> // param input extraction
#include <stdlib.h>

#include <iostream>

#include "renderer/vulkan/vulkan_types.h"
#include "renderer/vulkan/vulkan_platform.h"

#include <vulkan/vulkan_win32.h>

typedef struct internal_state{
    HINSTANCE h_instance;
    HWND hwnd;
    VkSurfaceKHR surface;
} internal_state;

static real clock_frequency;
static LARGE_INTEGER start_time;

LRESULT CALLBACK win32_process_message(HWND hwnd, u32 msg, WPARAM w_param, LPARAM l_param);

b8 platform_initialize(
    platform_state* plat_state,
    const char* app_name,
    i32 x,
    i32 y,
    i32 width,
    i32 height)
{
    plat_state->internal_state = malloc(sizeof(internal_state));
    internal_state* state = (internal_state*)plat_state->internal_state;
    
    state->h_instance = GetModuleHandleA(0); // "Give me a handle to the applicationn that is currently executing this code."
        
    // Setup and register window class.
    HICON icon = LoadIcon(state->h_instance, IDI_APPLICATION);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));

    wc.style = CS_DBLCLKS; // Get double-clicks.
    wc.lpfnWndProc = win32_process_message;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = state->h_instance;
    wc.hIcon = icon;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); // NULL; // Manage the cursos manually
    wc.hbrBackground = NULL;
    wc.lpszClassName = "ibx_window_class";
    
    if (!RegisterClassA(&wc))
    {
        MessageBoxA(0, "Window registration failed !", "Error", MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }
    
    u32 client_x = x;
    u32 client_y = y;
    u32 client_width = width;
    u32 client_height = height;

    u32 window_x = client_x;
    u32 window_y = client_y;
    u32 window_width = client_width;
    u32 window_height = client_height;

    u32 window_style = WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION;
    u32 window_ex_style = WS_EX_APPWINDOW;

    window_style |= WS_MAXIMIZEBOX;
    window_style |= WS_MINIMIZEBOX;
    window_style |= WS_THICKFRAME;

    // Get the size of the border.
    RECT border_rect = {0, 0, 0, 0}; // How different the client area is from the outer window.
    AdjustWindowRectEx(&border_rect, window_style, 0, window_ex_style);

    // In this case the border rect is negative.
    window_x += border_rect.left;
    window_y += border_rect.top;

    window_width += border_rect.right - border_rect.left;
    window_height += border_rect.bottom - border_rect.top; 

    HWND handle = CreateWindowExA(
        window_ex_style, "ibx_window_class", app_name,
        window_style, window_x, window_y, window_width, window_height,
        0, 0, state->h_instance, 0);

    if (handle == 0)
    {
        MessageBoxA(0, "Window creation failed !", "Error", MB_ICONEXCLAMATION | MB_OK);

        IBX_LOG_FATAL("Window creation failed !");
        return FALSE;
    }else{
        state->hwnd = handle;
    }

    b32 should_activate = 1; // TODO: if the window should not accept input, this should be false 
    i32 show_window_command_flags = should_activate ? SW_SHOW : SW_SHOWNOACTIVATE;

    ShowWindow(state->hwnd, show_window_command_flags);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency); // Gives the clock frequency for the specific processor.
     
    clock_frequency = 1.0 / (real)frequency.QuadPart;
    QueryPerformanceCounter(&start_time); // Gives a snapshot of current time.

    return TRUE;
}

void platform_terminate(platform_state* plat_state){
    internal_state* state = (internal_state*)plat_state->internal_state;
    if (state->hwnd)
    {
        DestroyWindow(state->hwnd);
        state->hwnd = NULL;
    }  
}

b8 platform_pump_messages(platform_state* plat_state){
    MSG message; 
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    return TRUE;
}

VOID_PTR platform_allocate_virtual_memory_commit(VOID_PTR block, u64 commit_size){
    return VirtualAlloc(block, commit_size, MEM_COMMIT, PAGE_READWRITE);
}

VOID_PTR platform_allocate_virtual_memory_reserve(u64 reserve_size){
    return VirtualAlloc(NULL, reserve_size, MEM_RESERVE, PAGE_NOACCESS);
}

void platform_virtual_free(VOID_PTR block, u64 size)
{
    // If lpAddress is the base address returned by VirtualAlloc and dwSize is 0 (zero), the function decommits the entire region that is allocated by VirtualAlloc. 
    // After that, the entire region is in the reserved state.
    VirtualFree(block, size, MEM_RELEASE);
}

VOID_PTR platform_allocate(u64 size, b8 aligned){
    return malloc(size);
}

void platform_free(VOID_PTR block, b8 aligned){
    return free(block);
}

VOID_PTR platform_zero_memory(VOID_PTR block, u64 size){
    return memset(block, 0, size);
}

VOID_PTR platform_copy_memory(VOID_PTR dest, const VOID_PTR source, u64 size){
    return memcpy(dest, source, size);
}

VOID_PTR platform_set_memory(VOID_PTR dest, i32 value, u64 size){
    return memset(dest, value, size);
}

void platform_console_write(const char* message, u8 colour){
    HANDLE console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    // FATAL, ERROR, WARN, INFO, DEBUG, TRACE
    static u8 levels[6] = {64, 4, 6, 2, 1, 8};
    SetConsoleTextAttribute(console_handle, levels[colour]);

    OutputDebugStringA(message);
    u64 length = strlen(message);
    LPDWORD number_written = 0;
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), message, (DWORD)length, number_written, 0);
}

void platform_console_write_error(const char* message, u8 colour){
    HANDLE console_handle = GetStdHandle(STD_ERROR_HANDLE);
    // FATAL, ERROR, WARN, INFO, DEBUG, TRACE
    static u8 levels[6] = {64, 4, 6, 2, 1, 8};
    SetConsoleTextAttribute(console_handle, levels[colour]);

    OutputDebugStringA(message);
    u64 length = strlen(message);
    LPDWORD number_written = 0;
    WriteConsoleA(GetStdHandle(STD_ERROR_HANDLE), message, (DWORD)length, number_written, 0);
}

// Get time in seconds stored as a 64-bit floating point number.
real platform_get_absolute_time(){
    LARGE_INTEGER now_time;
    QueryPerformanceCounter(&now_time);
    return (real)now_time.QuadPart * clock_frequency;
}

void platform_sleep(u64 ms){
    Sleep(ms);
}

void vulkan_platform_get_required_extension_names(Vector(const char*)& ext_names_vector){
    ext_names_vector.push_back( "VK_KHR_win32_surface" );
}

b8 vulkan_platform_create_surface(platform_state * plat_state, __VulkanContext * context){

    internal_state* state = (internal_state*)(plat_state->internal_state);

    VkWin32SurfaceCreateInfoKHR surface_info;
    surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surface_info.pNext = 0;
    surface_info.flags = 0;
    surface_info.hwnd = state->hwnd;
    surface_info.hinstance = state->h_instance;

    if(vkCreateWin32SurfaceKHR(
        context->instance, 
        &surface_info, 
        context->allocator, 
        &context->surface) != VK_SUCCESS){
        IBX_LOG_FATAL("Unable to create a Vulkan Win32 Surface object.")
        return FALSE;
    }

    // Store a copy of the created surface in Internal State.
    state->surface = context->surface;

    return TRUE;
}

LRESULT CALLBACK win32_process_message(HWND hwnd, u32 msg, WPARAM w_param, LPARAM l_param){
    using namespace std;
    switch (msg)
    {
        case WM_ERASEBKGND:
            // Notify the OS that erasing will be handled by the application to present flicker.
            return 1;
        case WM_CLOSE:{
            event_context_t data = {};
            event_fire(EVENT_CODE_APPLICATION_QUIT, 0, data);
            return TRUE;
        }
        case WM_DESTROY:{
            PostQuitMessage(0);
            return 0;
        }
        case WM_SIZE:{
            // Get the updated size.
            // RECT r;
            // GetClientRect(hwnd, &r);
            // u32 width = r.right - r.left;
            // u32 height = r.bottom - r.top;´

            // TODO: fire event for window resize 
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:{
            b8 pressed = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            keys key = static_cast<keys>(w_param); 
            input_process_key(key, pressed);

        } break;
        case WM_MOUSEMOVE:{
            // Mouse move
            i32 x_position = GET_X_LPARAM(l_param);
            i32 y_position = GET_Y_LPARAM(l_param);

            // Pass over to the input subsystem
            input_process_mouse_move(x_position, y_position);
        } break;
        case WM_MOUSEWHEEL:{
            i32 z_delta = GET_WHEEL_DELTA_WPARAM(w_param);
            if (z_delta != 0)
            {
                // Flatten the inpute to an OS-independent (-1,1)
                z_delta = (z_delta < 0) ? -1 : 1;
                input_process_mouse_wheel(z_delta);
            }

        } break;
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP: {
            b8 pressed = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
            // TODO: input processing
            buttons mouse_button = BUTTON_MAX_BUTTONS;
            switch (msg)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
                mouse_button = BUTTON_LEFT;
                break;
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
                mouse_button = BUTTON_MIDDLE;
                break;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
                mouse_button = BUTTON_RIGHT;
                break;
            default:
                break;
            }

            if (mouse_button != BUTTON_MAX_BUTTONS)
                input_process_button(mouse_button, pressed);
            
        } break;
    }

    return DefWindowProcA(hwnd, msg, w_param, l_param);
}

#endif // IBX_PLATFORM_WINDOWS