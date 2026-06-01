#include "math_utils.h"

namespace ibx_math
{
    u32 clamp(u32 d, u32 min, u32 max) {
        const u32 t = d < min ? min : d;
        return (t > max ? max : t);
    }
}
