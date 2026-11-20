# sr::gfx - the rewrite's renderer. C2: streaming:
# the geometry pool, the asset store, the GPU scene and the model pass of the viewer. C3 adds the
# render graph and the GPU-driven passes.
add_library(sr_gfx STATIC
    src/GeometryPool.cpp
    src/AssetStore.cpp
    src/GpuScene.cpp
    src/ModelPass.cpp
    src/RenderGraph.cpp
    src/MeshTable.cpp
    src/WorldPass.cpp
    src/TerrainBuild.cpp
    src/TerrainPass.cpp
    src/FrameConstants.cpp
    src/Environment.cpp
    src/SkyPass.cpp
    src/WeatherPass.cpp
    src/PostPass.cpp
    src/ModernPass.cpp
    src/UiPass.cpp
    src/Effects.cpp
    src/EffectPass.cpp
    src/PortraitPass.cpp
)
add_library(sr::gfx ALIAS sr_gfx)

target_include_directories(sr_gfx PUBLIC include)
target_link_libraries(sr_gfx PUBLIC sr::core sr::rhi sr::assets glm::glm PRIVATE sr_compile_options)

sr_add_shader(sr_gfx SOURCE shaders/Model.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kModelVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Model.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kModelPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Cull.hlsl ENTRY CSMain PROFILE cs_6_0 NAME kCullComputeShader)
sr_add_shader(sr_gfx SOURCE shaders/Skin.hlsl ENTRY CSMain PROFILE cs_6_0 NAME kSkinComputeShader)
sr_add_shader(sr_gfx SOURCE shaders/World.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kWorldVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/World.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kWorldPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/World.hlsl ENTRY PSBlended PROFILE ps_6_0 NAME kWorldBlendedPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/World.hlsl ENTRY VSDepth PROFILE vs_6_0 NAME kWorldDepthVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/World.hlsl ENTRY VSDepthOnly PROFILE vs_6_0 NAME kWorldDepthOnlyVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/World.hlsl ENTRY PSDepth PROFILE ps_6_0 NAME kWorldDepthPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Terrain.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kTerrainVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Terrain.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kTerrainPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Terrain.hlsl ENTRY VSShadow PROFILE vs_6_0 NAME kTerrainShadowVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Terrain.hlsl ENTRY PSShadow PROFILE ps_6_0 NAME kTerrainShadowPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Sky.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kSkyVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Sky.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kSkyPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Weather.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kWeatherVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Weather.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kWeatherPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Terrain.hlsl ENTRY VSWater PROFILE vs_6_0 NAME kWaterVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Terrain.hlsl ENTRY PSWater PROFILE ps_6_0 NAME kWaterPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Terrain.hlsl ENTRY VSWaterModern PROFILE vs_6_0 NAME kWaterModernVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Terrain.hlsl ENTRY PSWaterModern PROFILE ps_6_0 NAME kWaterModernPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Terrain.hlsl ENTRY PSIceModern PROFILE ps_6_0 NAME kIceModernPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Post.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kPostVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Post.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kPostPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Flare.hlsl ENTRY VSFlare PROFILE vs_6_0 NAME kPostFlareVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Flare.hlsl ENTRY PSFlare PROFILE ps_6_0 NAME kPostFlarePixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Modern.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kModernVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Modern.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kModernPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Modern.hlsl ENTRY PSDepthCopy PROFILE ps_6_0 NAME kDepthCopyPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/GameUi.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kGameUiVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/GameUi.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kGameUiPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Effect.hlsl ENTRY VSSprite PROFILE vs_6_0 NAME kEffectSpriteVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Effect.hlsl ENTRY VSMesh PROFILE vs_6_0 NAME kEffectMeshVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Effect.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kEffectPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Effect.hlsl ENTRY VSRibbon PROFILE vs_6_0 NAME kEffectRibbonVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Effect.hlsl ENTRY PSRibbon PROFILE ps_6_0 NAME kEffectRibbonPixelShader)
sr_add_shader(sr_gfx SOURCE shaders/Portrait.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kPortraitVertexShader)
sr_add_shader(sr_gfx SOURCE shaders/Portrait.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kPortraitPixelShader)
