# Dependencies of sr::core, fetched at configure time and pinned to exact versions.
include(FetchContent)

# Tracy profiler client. Zones come from SR_ZONE / SR_PROFILE (sr/core/Trace.h,
# sr/core/Profiler.h); connect with the Tracy profiler of the same version (0.14.1).
# On demand: nothing is recorded or buffered until a profiler connects.
if(SR_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    FetchContent_Declare(tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.14.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(tracy)
endif()
