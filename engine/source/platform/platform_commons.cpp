#include "platform_commons.h"

#include "defines.h"

#if BS_PLATFORM_WINDOWS
#include <windows.h>
#endif

// Dynamically get the OS page size once.
const unsigned long PAGE_SIZE = []() {
#if BS_PLATFORM_WINDOWS
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return sysInfo.dwPageSize;
#else
    // TODO: implement for other platforms
#endif
}();