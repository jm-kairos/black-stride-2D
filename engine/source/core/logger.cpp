#include "logger.h"
#include "asserts.h"
#include "platform/platform.h"

// TODO: temporary.
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

void report_assertion_failure(const char* expression, const char* message, const char* file, i32 line){
    logger_output(LOG_LEVEL_FATAL, "Assertion Failure: %s, message: '%s', in file: %s, line: %d\n", expression, message, file, line);
}

b8 logger_initialize()
{
    // TODO:  create log file.
    return TRUE;
}

void logger_terminate(){
    // TODO: terminate logging/write queued entries.
    // Since the logging content is going to be written in batches,
    // this function will make sure that the accumulated content up to the termination of the app
    // is flushed. ("Write to file before shutdown.")

}

void logger_output(ELogLevel level, const char* message, ...){
    // TODO: aim is to keep the number of file writes down, since writing to disk is generally slow.
    // So the goal is to output content is batches.

    const char* level_strings[6] = 
    {
        "[FATAL]: ", 
        "[ERROR]: ", 
        "[WARN]: ", 
        "[INFO]: ", 
        "[DEBUG]: ", 
        "[TRACE]: "
    };

    b8 const is_error = level < LOG_LEVEL_WARN;

    const i32 msg_length = 32000;
    char out_message[msg_length];
    memset(out_message, 0, sizeof(out_message));

    // Format message in a string.
    // TODO: compiler specific workaround, study this issue later.
    __builtin_va_list arg_ptr;
    va_start(arg_ptr, message);
    vsnprintf(out_message, msg_length, message, arg_ptr);
    va_end(arg_ptr);

    // Pre-apend level of the message into a separate buffer to avoid
    // overlapping source/destination undefined behavior in sprintf.
    char final_message[msg_length];
    snprintf(final_message, sizeof(final_message), "%s%s\n", level_strings[level], out_message);

    // Print the message.
    // TODO: this is platform specific.

    // TODO: output it to a file, later.

    // Platform-specific output  
    if (is_error)
    {
        platform_console_write_error(final_message, level);
    }else{
        platform_console_write(final_message, level);
    }
    
}