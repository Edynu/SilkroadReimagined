add_library(sr_platform STATIC
    src/Window.cpp
)
add_library(sr::platform ALIAS sr_platform)

target_include_directories(sr_platform PUBLIC include)
target_link_libraries(sr_platform PUBLIC sr::ui)
target_link_libraries(sr_platform PRIVATE SDL3::SDL3-static sr_compile_options)
