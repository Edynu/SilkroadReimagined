add_library(sr_devui STATIC
    src/ImGuiLayer.cpp
)
add_library(sr::devui ALIAS sr_devui)

target_include_directories(sr_devui PUBLIC include)
target_link_libraries(sr_devui
    PUBLIC imgui sr::rhi sr::platform
    PRIVATE SDL3::SDL3-static sr_compile_options)

sr_add_shader(sr_devui SOURCE shaders/ImGui.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kImGuiVertexShader)
sr_add_shader(sr_devui SOURCE shaders/ImGui.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kImGuiPixelShader)
