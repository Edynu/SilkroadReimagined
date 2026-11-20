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
