#pragma once

#include "defines.h"

#define BS_ASSERTIONS_ENABLED

#ifdef BS_ASSERTIONS_ENABLED
// If we are using the VS compiler
#if _MSC_VER
// Clang on windows does in fact support some of the MS extensions
// which is why we need to take this into account
#include <intrin.h>
#define debugBreak() __debugbreak() // If called, halts the application
#else
#define debugBreak() __builtin_trap() // gcc uses this
#endif

/*
    expression : string representation of the code that was running that caused the assertion failure.
    message: given message to contextualize the assertion - optional behaviour.
    file: name of the code file where the assertion was raised.
    line: line within the file, where the assertion was raised.
*/
bs__api__ void report_assertion_failure(const char* expression, const char* message, const char* file, i32 line);

#define BS_ASSERT(expr)                                            \
    {                                                               \
        if(expr){                                                   \
                                                                    \
        }else{                                                      \
            report_assertion_failure(#expr, "", __FILE__, __LINE__);\
            debugBreak();                                           \
        }                                                           \
    }          
#define BS_ASSERT_MSG(expr, message)                                    \
    {                                                                    \
        if(expr){                                                        \
                                                                         \
        }else{                                                           \
            report_assertion_failure(#expr, message, __FILE__, __LINE__);\
            debugBreak();                                                \
        }                                                                \
    }   

#ifdef BS_DEBUG
#define BS_ASSERT_DEBUG(expr)                                           \
    {                                                                    \
        if(expr){                                                        \
                                                                         \
        }else{                                                           \
            report_assertion_failure(#expr, message, __FILE__, __LINE__);\
            debugBreak();                                                \
        }                                                                \
    } 
#else
#define BS_ASSERT_DEBUG(expr) // Does nothing
#endif

#else
#define BS_ASSERT(expr)
#define BS_ASSERT_MSG(expr)
#define BS_ASSERT_DEBUG(expr)
#endif

