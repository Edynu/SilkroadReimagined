# sr::anim - skeletal animation on the CPU: poses and skinning palettes.
add_library(sr_anim STATIC
    src/Pose.cpp
)
add_library(sr::anim ALIAS sr_anim)

target_include_directories(sr_anim PUBLIC include)
target_link_libraries(sr_anim PUBLIC sr::assets glm::glm PRIVATE sr_compile_options)
