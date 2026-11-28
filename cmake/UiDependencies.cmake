# Game UI dependencies, fetched at configure time and pinned. Separate from
# Dependencies.cmake because the tests build sr::ui without the client.
include(FetchContent)

# stb: stb_truetype (the game UI's fonts) and stb_image_write (screenshots).
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(stb)

add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# FreeType: the interface's letters as the original's (hinted by the font's own instructions, the classic TrueType
# interpreter, rasterised as 1-bit glyphs, as GDI's GetGlyphOutline GGO_BITMAP gives them). Static, no optional codecs.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(SR_SAVED_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG VER-2-14-1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(freetype)
set(BUILD_SHARED_LIBS ${SR_SAVED_SHARED_LIBS})
