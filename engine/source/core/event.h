#pragma once

#include "defines.h"

/* Event context explained:
* In event_context_t we can send a variety of different values.
* Set one of the element in union data and that is what gets processed on the other side.
* All is allocated on the stack.
*/

typedef struct event_context_t {
    // 128 bytes
    union{
        i64 i64[2];
        u64 u64[2];
        f64 f64[2];

        i32 i32[2];
        u32 u32[2];
        f32 f32[2];

        i16 i16[8];
        u16 u16[8];

        i8 i8[16];
        u8 u8[16];

        char c[16];
    } data;
} event_context;

// We have a sender and a listener to an event.
// We dont want a direct reference to either one of the them.
// So we use a function pointer to provide an interface the event subsystem can use 
// to make a call to a specific listener and pass it the event. 
typedef b8 (*PFN_on_event)(u16 code,
    VOID_PTR sender, 
    VOID_PTR listener,
    event_context_t data);

b8 event_initialize();
void event_terminate();

ibx__api__ b8 event_register(u16 code, VOID_PTR listener, PFN_on_event on_event);
ibx__api__ b8 event_unregister(u16 code, VOID_PTR listener, PFN_on_event on_event);
ibx__api__ b8 event_fire(u16 code, VOID_PTR listener, event_context context);

// System internal event codes. Application should use codes beyond 255.
typedef enum ESystemEventCode {
    EVENT_CODE_APPLICATION_QUIT = 0x01,
    EVENT_CODE_KEY_PRESSED = 0x02,
    EVENT_CODE_KEY_RELEASED = 0x03,
    EVENT_CODE_BUTTON_PRESSED = 0x04,
    EVENT_CODE_BUTTON_RELEASED = 0x05,
    EVENT_CODE_MOUSE_MOVED = 0x06,
    EVENT_CODE_MOUSE_WHEEL = 0x07
} ESystemEventCode;

