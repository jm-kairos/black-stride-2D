#pragma once

#include "defines.h"

// FATAL and ERROR will always be reported, no matter what
#define LOG_WARN_ENABLED 1
#define LOG_INFO_ENABLED 1
#define LOG_DEBUG_ENABLED 1
#define LOG_TRACE_ENABLED 1

// Disable DEBUG nad TRACE logging for release builds.
#if IBXRELEASE == 1
#define LOG_DEBUG_ENABLED 0
#define LOG_TRACE_ENABLED 0
#endif

typedef enum ELogLevel {
    LOG_LEVEL_FATAL = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_INFO = 3,
    LOG_LEVEL_DEBUG = 4,
    LOG_LEVEL_TRACE = 5
} ELogLevel;

b8 logger_initialize();
void logger_terminate();

ibx__api__ void logger_output(ELogLevel level, const char* message, ...);

// Logs a FATAL level message.
#define IBX_LOG_FATAL(message, ...) logger_output(LOG_LEVEL_FATAL, message, ##__VA_ARGS__);

#ifndef IBX_LOG_ERROR
// Logs a ERROR level message.
#define IBX_LOG_ERROR(message, ...) logger_output(LOG_LEVEL_ERROR, message, ##__VA_ARGS__);
#endif

#if LOG_WARN_ENABLED == 1
// Logs a WARN level message.
#define IBX_LOG_WARN(message, ...) logger_output(LOG_LEVEL_WARN, message, ##__VA_ARGS__);
#else
// Still compiles, but compiles to nothing when LOG_WARN_ENABLED != 1.
#define IBX_LOG_WARN(message, ...)
#endif

#if LOG_INFO_ENABLED == 1
// Logs a INFO level message.
#define IBX_LOG_INFO(message, ...) logger_output(LOG_LEVEL_INFO, message, ##__VA_ARGS__);
#else
// Still compiles, but compiles to nothing when LOG_INFO_ENABLED != 1.
#define IBX_LOG_INFO(message, ...)
#endif

#if LOG_DEBUG_ENABLED == 1
// Logs a DEBUG level message.
#define IBX_LOG_DEBUG(message, ...) logger_output(LOG_LEVEL_DEBUG, message, ##__VA_ARGS__);
#else
// Still compiles, but compiles to nothing when LOG_DEBUG_ENABLED != 1.
#define IBX_LOG_DEBUG(message, ...)
#endif

#if LOG_TRACE_ENABLED == 1
// Logs a TRACE level message.
#define IBX_LOG_TRACE(message, ...) logger_output(LOG_LEVEL_TRACE, message, ##__VA_ARGS__);
#else
// Still compiles, but compiles to nothing when LOG_TRACE_ENABLED != 1.
#define IBX_LOG_TRACE(message, ...)
#endif

