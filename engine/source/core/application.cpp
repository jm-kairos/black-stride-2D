#include "application.h"
#include "game_types.h"

#include "logger.h"
#include "platform/platform.h"
#include "renderer/renderer.h"
#include "core/memory/bs_memory.h"
#include "core/memory/arena.h"
#include "core/event.h"
#include "core/input.h"

// Singleton design for the state of the application struct.

struct ApplicationState{
    Game* game_inst;
    b8 is_running;
    b8 is_suspended;
    PlatformState platform;
    i16 width;
    i16 height;
    real last_time;
};

// Safety check if there is more than one instance of application state.
static b8 initialized = FALSE;

// Private to this CPP file.
static ApplicationState app_state;

b8 application_on_event(u16 code, VOID_PTR sender, VOID_PTR listener_inst, event_context_t context);
b8 application_on_key(u16 code, VOID_PTR sender, VOID_PTR listener_inst, event_context_t context);
b8 application_on_resized(u16 code, VOID_PTR sender, VOID_PTR listener_inst, event_context_t context);

b8 application_init(Game* game_inst){
    if (initialized)
    {
        BS_LOG_ERROR("application_init called more than once !");
        return FALSE;
    }

    app_state.game_inst = game_inst;

    // Seed the window dimensions from the requested config size. The platform
    // resize event will keep these current once the window is live.
    app_state.width  = (i16)game_inst->app_config.start_width;
    app_state.height = (i16)game_inst->app_config.start_height;
    app_state.last_time = 0;

    // Initialize subsystems
    logger_initialize();
    input_initialize();

    // TODO: remove this
    BS_LOG_FATAL("A test message: %f", 3.14f);
    BS_LOG_ERROR("A test message: %f", 3.14f);
    BS_LOG_WARN("A test message: %f", 3.14f);
    BS_LOG_INFO("A test message: %f", 3.14f);
    BS_LOG_DEBUG("A test message: %f", 3.14f);    
    BS_LOG_TRACE("A test message: %f", 3.14f); 

    app_state.is_running = TRUE;
    // State the application enters in when the window is, for example, minimized.
    app_state.is_suspended = FALSE;

    if (!event_initialize())
    {
        BS_LOG_ERROR("event subsystem failed to initialize !");
        return FALSE;
    }
    
    event_register(EVENT_CODE_APPLICATION_QUIT, 0, application_on_event);
    event_register(EVENT_CODE_KEY_PRESSED, 0, application_on_key);
    event_register(EVENT_CODE_KEY_RELEASED, 0, application_on_key);
    event_register(EVENT_CODE_WINDOW_RESIZED, 0, application_on_resized);

    if (!platform_initialize(&app_state.platform, 
        game_inst->app_config.name, 
        game_inst->app_config.start_pos_x, 
        game_inst->app_config.start_pos_y, 
        game_inst->app_config.start_width, 
        game_inst->app_config.start_height)){

        BS_LOG_ERROR("platform_initialize failed !");
        return FALSE;
    }

    // Bring up the renderer after the window exists but before the game's init, so the
    // game can create GPU resources during its own initialization.
    if (!renderer_initialize(game_inst->app_config.name, &app_state.platform))
    {
        BS_LOG_FATAL("renderer_initialize failed !");
        return FALSE;
    }

    if (!app_state.game_inst->init(app_state.game_inst))
    {
        BS_LOG_FATAL("Game failed to initialized.");
        return FALSE;
    }

    app_state.game_inst->on_resize(
        app_state.game_inst,
        app_state.width,
        app_state.height
    );

    initialized = TRUE;

    return TRUE;
}

b8 application_run(){

    ARENA_PTR frame_arena = arena_initialize(1024 * 1024 * 64); // 64MB frame arena
    
    BS_LOG_INFO(bs_memory_get_memory_usage_string());

    app_state.last_time = platform_get_absolute_time();

    while (app_state.is_running){

        arena_reset(frame_arena);

        // Compute real frame delta time.
        real current_time = platform_get_absolute_time();
        real delta = current_time - app_state.last_time;
        app_state.last_time = current_time;
        f32 dt = (f32)delta;

        if(!platform_pump_messages(&app_state.platform)){
            app_state.is_running = FALSE;
        }

        if (!app_state.is_suspended)
        {
            if (!app_state.game_inst->update(app_state.game_inst, dt))
            {
                BS_LOG_FATAL("Game update failed, shutting down.");
                app_state.is_running = FALSE;
                break;
            }

            // Only render if the renderer accepted the frame. A skipped begin_frame
            // (e.g. window minimized) means we must not call render or end_frame.
            if (renderer_begin_frame(dt))
            {
                f64 t0 = platform_get_absolute_time();
                if (!app_state.game_inst->render(app_state.game_inst, dt))
                {
                    BS_LOG_FATAL("Game render failed, shutting down.");
                    app_state.is_running = FALSE;
                    break;
                }
                f64 t1 = platform_get_absolute_time();
                renderer_end_frame(dt);
                f64 t2 = platform_get_absolute_time();
                f32 render_ms = (f32)((t1 - t0) * 1000.0);
                f32 present_ms = (f32)((t2 - t1) * 1000.0);
                if (render_ms > 100.0f || present_ms > 100.0f)
                {
                    BS_LOG_WARN("SLOW FRAME: render=%.1fms present=%.1fms", render_ms, present_ms);
                }
            }

            input_update(delta);
        }
        
    }

    event_unregister(EVENT_CODE_APPLICATION_QUIT, 0, application_on_event);
    event_unregister(EVENT_CODE_KEY_PRESSED, 0, application_on_key);
    event_unregister(EVENT_CODE_KEY_RELEASED, 0, application_on_key);
    event_unregister(EVENT_CODE_WINDOW_RESIZED, 0, application_on_resized);

    event_terminate();

    input_terminate();

    arena_terminate(frame_arena);
    
    app_state.is_running = FALSE; 

    renderer_shutdown();

    platform_terminate(&app_state.platform);

    return TRUE;
}

b8 application_on_event(u16 code, VOID_PTR sender, VOID_PTR listener_inst, event_context_t context){
    switch (code)
    {
        case EVENT_CODE_APPLICATION_QUIT:{
            BS_LOG_INFO("EVENT_CODE_APPLICATION_QUIT received, shutting down."); 
            app_state.is_running = FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

b8 application_on_key(u16 code, VOID_PTR sender, VOID_PTR listener_inst, event_context_t context){
    if (code == EVENT_CODE_KEY_PRESSED)
    {
        u16 key_code = context.data.u16[0];
        if (key_code == KEY_ESCAPE)
        {
            event_context data = {};
            event_fire(EVENT_CODE_APPLICATION_QUIT, 0, data);

            return TRUE;
        } else if (key_code == KEY_A){
            BS_LOG_DEBUG("Explicit - A key pressed !");

        }else{
            BS_LOG_DEBUG("'%c' key pressed in window. ", key_code);
        }
    }
    else if (code == EVENT_CODE_KEY_RELEASED){
        u16 key_code = context.data.u16[0];
        if (key_code == KEY_B)
        {
            BS_LOG_DEBUG("Explicit - B key released !");
        }else{
            BS_LOG_DEBUG("'%c' key released in window. ", key_code);
        }
    }
    return FALSE;
}

b8 application_on_resized(u16 code, VOID_PTR sender, VOID_PTR listener_inst, event_context_t context){
    if (code == EVENT_CODE_WINDOW_RESIZED)
    {
        u16 width  = context.data.u16[0];
        u16 height = context.data.u16[1];

        // Only react to actual changes.
        if (width != (u16)app_state.width || height != (u16)app_state.height)
        {
            app_state.width  = (i16)width;
            app_state.height = (i16)height;

            BS_LOG_DEBUG("Window resized: %u x %u", width, height);

            // A zero-size window means it was minimized. Suspend the app until restored.
            if (width == 0 || height == 0)
            {
                BS_LOG_INFO("Window minimized, suspending application.");
                app_state.is_suspended = TRUE;
                return TRUE;
            }
            else
            {
                if (app_state.is_suspended)
                {
                    BS_LOG_INFO("Window restored, resuming application.");
                    app_state.is_suspended = FALSE;
                }
                app_state.game_inst->on_resize(app_state.game_inst, width, height);
                renderer_on_resize(width, height);
            }
        }
    }
    // Allow other listeners (e.g. the renderer) to also handle this event.
    return FALSE;
}