// stb_vorbis's implementation (public domain), compiled here once, its warnings off. Decode.cpp
// sees only its declarations.

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <stb_vorbis.c>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
