add_library(sr_render STATIC
    src/Camera.cpp
    src/TextureCache.cpp
    src/ModelRenderer.cpp
    src/ParticleRenderer.cpp
    src/PostProcess.cpp
    src/Pose.cpp
    src/SkyRenderer.cpp
    src/TerrainRenderer.cpp
    src/UiRenderer.cpp
)
add_library(sr::render ALIAS sr_render)

target_include_directories(sr_render PUBLIC include)
target_link_libraries(sr_render PUBLIC sr::rhi sr::assets sr::ui glm::glm PRIVATE sr_compile_options)

sr_add_shader(sr_render SOURCE shaders/Mesh.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kMeshVertexShader)
sr_add_shader(sr_render SOURCE shaders/Mesh.hlsl ENTRY VSSkinned PROFILE vs_6_0 NAME kSkinnedVertexShader)
sr_add_shader(sr_render SOURCE shaders/Mesh.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kMeshPixelShader)
sr_add_shader(sr_render SOURCE shaders/Mesh.hlsl ENTRY PSShadow PROFILE ps_6_0 NAME kMeshShadowShader)
sr_add_shader(sr_render SOURCE shaders/Particle.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kParticleVertexShader)
sr_add_shader(sr_render SOURCE shaders/Particle.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kParticlePixelShader)
sr_add_shader(sr_render SOURCE shaders/Terrain.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kTerrainVertexShader)
sr_add_shader(sr_render SOURCE shaders/Terrain.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kTerrainPixelShader)
sr_add_shader(sr_render SOURCE shaders/Terrain.hlsl ENTRY PSShadow PROFILE ps_6_0 NAME kTerrainShadowShader)
sr_add_shader(sr_render SOURCE shaders/Ui.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kUiVertexShader)
sr_add_shader(sr_render SOURCE shaders/Ui.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kUiPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kFullscreenVertexShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSFxaa PROFILE ps_6_0 NAME kFxaaPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSSsao PROFILE ps_6_0 NAME kSsaoPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSAoBlur PROFILE ps_6_0 NAME kAoBlurPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSBloomDown PROFILE ps_6_0 NAME kBloomDownPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSBloomBlur PROFILE ps_6_0 NAME kBloomBlurPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSDualDown PROFILE ps_6_0 NAME kDualDownPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSDualUp PROFILE ps_6_0 NAME kDualUpPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSShafts PROFILE ps_6_0 NAME kShaftsPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSCompose PROFILE ps_6_0 NAME kComposePixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSFlareVisibility PROFILE ps_6_0 NAME kFlareVisibilityPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSCopy PROFILE ps_6_0 NAME kCopyPixelShader)
sr_add_shader(sr_render SOURCE shaders/Post.hlsl ENTRY PSDepthCopy PROFILE ps_6_0 NAME kDepthCopyPixelShader)
sr_add_shader(sr_render SOURCE shaders/Atmosphere.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kAtmosphereVertexShader)
sr_add_shader(sr_render SOURCE shaders/Atmosphere.hlsl ENTRY PSFog PROFILE ps_6_0 NAME kFogPixelShader)
sr_add_shader(sr_render SOURCE shaders/Atmosphere.hlsl ENTRY PSClouds PROFILE ps_6_0 NAME kCloudsPixelShader)
sr_add_shader(sr_render SOURCE shaders/Flare.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kFlareVertexShader)
sr_add_shader(sr_render SOURCE shaders/Flare.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kFlarePixelShader)
sr_add_shader(sr_render SOURCE shaders/Sky.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kSkyVertexShader)
sr_add_shader(sr_render SOURCE shaders/Sky.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kSkyPixelShader)
