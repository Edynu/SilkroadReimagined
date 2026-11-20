add_library(sr_core STATIC
    src/MappedFile.cpp
    src/Path.cpp
    src/Crc32.cpp
    src/Text.cpp
    src/ThreadPool.cpp
    src/Options.cpp
    src/Profiler.cpp
    src/Jobs.cpp
    src/Log.cpp
    src/Config.cpp
    src/Trace.cpp
    src/FramePacer.cpp
    src/RangeAllocator.cpp
    src/AssetTable.cpp
)
add_library(sr::core ALIAS sr_core)

target_include_directories(sr_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(sr_core PUBLIC Threads::Threads PRIVATE sr_compile_options)
if(SR_TRACY)
    target_link_libraries(sr_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(sr_core PUBLIC SR_TRACY=1)
endif()
