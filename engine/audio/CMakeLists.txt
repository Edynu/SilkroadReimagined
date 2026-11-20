# The sound files and tables (no OpenAL: the tests decode the whole game with it), and the sound
# engine over OpenAL Soft on top.
add_library(sr_audio_data STATIC
    src/Decode.cpp
    src/StbVorbis.cpp
    src/SoundTables.cpp
)
add_library(sr::audio_data ALIAS sr_audio_data)
target_include_directories(sr_audio_data PUBLIC include)
target_link_libraries(sr_audio_data PUBLIC sr::core sr::assets PRIVATE stb sr_compile_options)
# The Vorbis decoder optimised in every configuration: a debug build streams the music with it too.
if(NOT MSVC)
    set_source_files_properties(src/StbVorbis.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()

add_library(sr_audio STATIC
    src/Device.cpp
    src/SoundSystem.cpp
)
add_library(sr::audio ALIAS sr_audio)

target_include_directories(sr_audio PUBLIC include)
# OpenAL Soft is LGPL: linked as a DLL (OpenAL32.dll next to the executable), never statically.
target_link_libraries(sr_audio PUBLIC sr::core sr::audio_data PRIVATE sr::assets OpenAL::OpenAL sr_compile_options)
