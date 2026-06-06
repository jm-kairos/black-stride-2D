// =====================================================================================
// The single translation unit that compiles the stb_image implementation. stb_image is a
// public-domain single-header PNG/JPG/etc. decoder (vendored at vendor/include/stb_image.h).
//
// The engine builds with -Wall -Werror; stb_image is not warning-clean, so its implementation
// is fenced inside diagnostic-suppression pragmas. Only this .cpp defines STB_IMAGE_IMPLEMENTATION,
// so the decoder body is emitted exactly once and linked into engine.dll. The backend includes
// the plain header (no IMPLEMENTATION macro) wherever it needs to decode pixels.
// =====================================================================================

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wdouble-promotion"

// We only need PNG (sprites/atlases authored as PNG). Trim the others to keep the TU small and
// avoid pulling extra decoders into the DLL. Add formats here if assets demand them.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO   // we feed bytes from SDL_LoadFile; no fopen path needed.
#include "stb_image.h"

#pragma clang diagnostic pop
