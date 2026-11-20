# Audio: OpenAL Soft (the maintained implementation of the OpenAL API), pinned. LGPL-2.1, so it
# is built and shipped as a shared library (OpenAL32.dll), which executables copy next to them.
include(FetchContent)

set(LIBTYPE SHARED)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_NO_CONFIG_UTIL ON CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_UTILS OFF CACHE BOOL "" FORCE)
# With clang and the MSVC standard library, OpenAL Soft's [[clang::nonblocking]] annotations do
# not compile; the patch turns them off (see the patch file).
FetchContent_Declare(openal-soft
    GIT_REPOSITORY https://github.com/kcat/openal-soft.git
    GIT_TAG 1.25.2
    GIT_SHALLOW TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/openal-soft-nonblocking.cmake
    UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(openal-soft)
