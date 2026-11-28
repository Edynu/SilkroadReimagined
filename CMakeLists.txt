cmake_minimum_required(VERSION 3.29) # CMAKE_LINKER_TYPE (lld)

# The version (the players' client's, its exe's version resource, the package and the launcher's manifest): one
# number here. 0.1.1 was the first beta.
project(SroReimagined
    VERSION 0.1.34
    DESCRIPTION "Clean-room Silkroad Online client with a modern renderer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Do not depend on CMAKE_CXX_FLAGS defaults: some IDE/tool configurations cache
# them empty, which silently drops exception handling (/EHsc) and the Debug
# flags for every C++ target, dependencies included.
if(MSVC)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/EHsc;/GR>"
        "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Debug>>:/Od;/RTC1>"
        "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release,RelWithDebInfo>>:/O2;/Ob2;/DNDEBUG>")
endif()

option(SR_BUILD_TESTS "Build the unit and integration tests" ON)
option(SR_BUILD_TOOLS "Build command-line tools (pk2tool, ...)" ON)

# Shared warning/feature flags for every SroReimagined target. The default compiler is clang++
# (CMakePresets.json "clang"); MSVC's cl (and clang-cl) take the MSVC branch.
add_library(sr_compile_options INTERFACE)
if(MSVC)
    # /bigobj: the largest sources (the game screen) pass the object format's default section count.
    target_compile_options(sr_compile_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /bigobj)
else()
    # -Wno-c2y-extensions: __COUNTER__ (doctest's test macros) counts as an extension under -Wpedantic.
    # -Wno-missing-field-initializers: `VkFoo info{VK_STRUCTURE_TYPE_FOO};` zero-fills the rest on purpose.
    target_compile_options(sr_compile_options INTERFACE
        -Wall -Wextra -Wpedantic -Wno-c2y-extensions -Wno-missing-field-initializers)
endif()
if(WIN32)
    target_compile_definitions(sr_compile_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()

option(SR_BUILD_CLIENT "Build the client (needs Vulkan SDK; fetches SDL3, ImGui, VMA, OpenAL Soft)" ON)
option(SR_BUILD_SERVERS "Build our servers (gateway, login, game; needs the PostgreSQL client library)" ON)
option(SR_DEV_TOOLS "Include the in-game ImGui developer tools in the clients" ON)
# The client as players get it (the client-release preset): a window without a console, its settings, options and log
# next to the exe, the title screen after the logo, the game folder asked for on the first start, its version resource.
option(SR_PLAYER_BUILD "Build the client for players" OFF)
option(SR_TRACY "Tracy profiler zones (on demand; connect with the Tracy 0.14.1 profiler)" ON)
# The game server's developer commands (--dev-commands: teleports, items, experience for any player; the test seeds).
# A public release builds without them (-DSR_DEV_COMMANDS=OFF): its SroGame.exe has no way to turn them on, whatever
# its settings say. The tests and the skill lab need them, so they are left out of such a build.
option(SR_DEV_COMMANDS "Developer commands in the game server (OFF for a public release)" ON)
# The players' bug reports from the game (the HUD's button left of the performance panel, a screenshot with or without the
# interface): the client's button and window, the game server's taking them into the website's support tickets
# (sr_support_tickets, the screenshot in sr_support_files). OFF: neither side has them (the server refuses the messages).
option(SR_BUG_REPORTS "In-game bug reports (client button and window, game server into the support tickets)" ON)
# Ours, experimental: sounds behind walls muffled by a small depth cube around the camera (the render thread draws the world's
# static objects into six 64 x 64 faces and reads them back; the Option window's Beta page turns it on). OFF: none of it.
option(SR_AUDIO_OCCLUSION "Sound occlusion by a depth cube around the listener (experimental, the Beta page's option)" ON)
if(NOT SR_DEV_COMMANDS AND SR_BUILD_TESTS)
    message(STATUS "SR_DEV_COMMANDS is OFF: the tests are not built (they use the developer commands)")
    set(SR_BUILD_TESTS OFF CACHE BOOL "Build the unit and integration tests" FORCE)
endif()

include(cmake/CoreDependencies.cmake)
add_subdirectory(engine/core)
add_subdirectory(engine/assets)
include(cmake/NetDependencies.cmake)
add_subdirectory(engine/net)
include(cmake/UiDependencies.cmake)
add_subdirectory(engine/ui)
add_subdirectory(game)
if(SR_BUILD_SERVERS)
    include(cmake/ServerDependencies.cmake)
    add_subdirectory(server)
endif()

if(SR_BUILD_CLIENT)
    include(cmake/Dependencies.cmake)
    include(cmake/Shaders.cmake)
    add_subdirectory(engine/platform)
    add_subdirectory(engine/rhi)
    add_subdirectory(engine/render)
    add_subdirectory(engine/anim)
    add_subdirectory(engine/gfx)
    add_subdirectory(engine/devui)
    include(cmake/AudioDependencies.cmake)
    add_subdirectory(engine/audio)
    add_subdirectory(client)
endif()

if(SR_BUILD_TOOLS)
    add_subdirectory(tools/pk2tool)
endif()

if(SR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(cmake/DependencyRules.cmake)
sr_check_dependency_rules()
