#include "core/event.h"

#include "core/memory/ibx_memory.h"

#include "containers/vector.h"
#include "containers/array.h"

struct event_registered_t {
    VOID_PTR listener;
    PFN_on_event callback;
};

struct event_code_entry_t {
    Vector(event_registered_t) events;
};

#define MAX_MESSAGE_CODES 10000

struct event_system_state_t
{
    // Lookup table for event codes. 
    Array(event_code_entry_t, MAX_MESSAGE_CODES) registered;
};

// Internal state of the event system.
static b8 initialized = FALSE;
static event_system_state_t state;

b8 event_initialize()
{
    if (initialized == TRUE)
    {
        return FALSE;
    }

    state = {};
    initialized = TRUE;

    return TRUE;
}

void event_terminate()
{

}

b8 event_register(u16 code, VOID_PTR listener, PFN_on_event on_event)
{
    if (initialized == FALSE)
    {
        return FALSE;
    }

    // Dont register the same listener more than once.
    size_t registered_count = state.registered[code].events.size();
    for (size_t i = 0; i < registered_count; ++i)
    {
        if (state.registered[code].events[i].listener == listener)
        {
            // TODO: A registered listener is attempting to register again.
            return FALSE;
        }
    }

    event_registered_t event;
    event.listener = listener;
    event.callback = on_event;
    state.registered[code].events.push_back(event);

    return TRUE;
}

b8 event_unregister(u16 code, VOID_PTR listener, PFN_on_event on_event)
{
    if (initialized == FALSE)
    {
        return FALSE;
    }

    size_t registered_count = state.registered[code].events.size();
    for (size_t i = 0; i < registered_count; ++i)
    {
        event_registered_t e = state.registered[code].events[i];
        if (e.listener == listener && e.callback == on_event)
        {
            // WARN: vector "erase" method is O(n).
            state.registered[code].events.erase(state.registered[code].events.begin() + i);
            return TRUE;
        }
    }

    // Not found.
    return FALSE;
}

b8 event_fire(u16 code, VOID_PTR sender, event_context context)
{
    if (initialized == FALSE)
    {
        return FALSE;
    }

    size_t registered_count = state.registered[code].events.size();
    for (size_t i = 0; i < registered_count; ++i)
    {
        event_registered_t e = state.registered[code].events[i];
        if (e.callback(code, sender, e.listener, context))
        {
            // Message has been handled, do not send to other listeners.
            return TRUE;
        }
    }

    // Not found.
    return FALSE;
}
