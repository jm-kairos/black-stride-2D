#include "application.h"
#include "game_types.h"

#include "logger.h"
#include "platform/platform.h"
#include "core/memory/bs_memory.h"
#include "core/memory/arena.h"
#include "core/event.h"
#include "core/input.h"

// Singleton design for the state of the application struct.

typedef struct application_state{
    game* game_inst;
    b8 is_running;
    b8 is_suspended;
    platform_state platform;
    i16 width;
    i16 height;
    real last_time;
} application_state;

// Safety check if there is more than one instance of application state.
static b8 initialized = FALSE;

// Private to this CPP file.
static application_state app_state;

b8 application_on_event(u16 code, VOID_PTR sender, VOID_PTR listener_inst, event_context_t context);
b8 application_on_key(u16 code, VOID_PTR sender, VOID_PTR listener_inst, event_context_t context);

b8 application_init(game* game_inst){
    if (initialized)
    {
        BS_LOG_ERROR("application_init called more than once !");
        return FALSE;
    }

    app_state.game_inst = game_inst;

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

    if (!platform_initialize(&app_state.platform, 
        game_inst->app_config.name, 
        game_inst->app_config.start_pos_x, 
        game_inst->app_config.start_pos_y, 
        game_inst->app_config.start_width, 
        game_inst->app_config.start_height)){

        BS_LOG_ERROR("platform_initialize failed !");
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

    while (app_state.is_running){

        arena_reset(frame_arena);

        if(!platform_pump_messages(&app_state.platform)){
            app_state.is_running = FALSE;
        }

        if (!app_state.is_suspended)
        {
            // if (!app_state.game_inst->update(app_state.game_inst, (f32)0))
            // {
            //     BS_LOG_FATAL("Game update failed, shutting down.");
            //     app_state.is_running = FALSE;
            //     break;
            // }
            // 
            // if (!app_state.game_inst->render(app_state.game_inst, (f32)0))
            // {
            //     BS_LOG_FATAL("Game render failed, shutting down.");
            //     app_state.is_running = FALSE;
            //     break;
            // }

            input_update((real)0);
        }
        
    }

    event_unregister(EVENT_CODE_APPLICATION_QUIT, 0, application_on_event);
    event_unregister(EVENT_CODE_KEY_PRESSED, 0, application_on_key);
    event_unregister(EVENT_CODE_BUTTON_RELEASED, 0, application_on_key);

    event_terminate();

    input_terminate();

    arena_terminate(frame_arena);
    
    app_state.is_running = FALSE; 

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