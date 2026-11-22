# SroReimagined.exe - the rewritten client. Grows phase by phase; the
# playable reference until it catches up is prototype/ (SroPrototype.exe).
add_executable(SroReimagined
    src/main.cpp
    src/App.cpp
    src/Screenshot.cpp
    src/Cursors.cpp
    src/frame/SnapshotQueue.cpp
    src/frame/FrameRenderer.cpp
    src/frame/VideoWriter.cpp
    src/frame/FrameStats.cpp
    src/frame/SceneRenderer.cpp
    src/frame/Portraits.cpp
    src/screens/ScreenMachine.cpp
    src/screens/LogoScreen.cpp
    src/screens/CrowdScreen.cpp
    src/screens/ViewerScreen.cpp
    src/screens/AssetCheckScreen.cpp
    src/screens/SceneBenchScreen.cpp
    src/screens/WorldScreen.cpp
    src/screens/WorldCrowd.cpp
    src/screens/WorldMenu.cpp
    src/screens/DungeonScreen.cpp
    src/screens/GalleryScreen.cpp
    src/world/WorldStreamer.cpp
    src/world/DungeonScene.cpp
    src/world/WeatherEffect.cpp
    src/world/LoadingScreen.cpp
    src/world/StageEffects.cpp
    src/gui/Hud.cpp
    src/gui/HudMinimap.cpp
    src/gui/HudNpc.cpp
    src/gui/HudChat.cpp
    src/gui/HudGuild.cpp
    src/gui/HudJobs.cpp
    src/gui/HudTrade.cpp
    src/gui/HudMagicPop.cpp
    src/gui/HudMall.cpp
    src/gui/HudServices.cpp
    src/gui/HudGuildBoxes.cpp
    src/gui/HudAcademy.cpp
    src/gui/HudOpenMarket.cpp
    src/gui/HudCommunity.cpp
    src/gui/HudExchange.cpp
    src/gui/HudStall.cpp
    src/gui/HudQuest.cpp
    src/gui/HudFreePvp.cpp
    src/gui/HudAlchemy.cpp
    src/gui/HudItemOptions.cpp
    src/play/PlayScene.cpp
    src/gui/HudTemple.cpp
    src/play/PlayStall.cpp
    src/play/PlayServices.cpp
    src/play/PlayCos.cpp
    src/play/PlayMallItems.cpp
    src/gui/HudCos.cpp
    src/play/PlayQuest.cpp
    src/play/PlayInstance.cpp
    src/play/PlayFlagWorld.cpp
    src/play/PlayArena.cpp
    src/play/PlayAlchemy.cpp
    src/play/PlayMagicPop.cpp
    src/play/PlayMall.cpp
    src/gui/HudInstance.cpp
    src/gui/HudFlagWorld.cpp
    src/gui/HudArena.cpp
    src/gui/HudGuildMaster.cpp
    src/gui/HudGuildWar.cpp
    src/gui/HudFortress.cpp
    src/gui/HudActions.cpp
    src/gui/HudFortressMap.cpp
    src/gui/HudFortressWork.cpp
    src/gui/HudMapView.cpp
    src/gui/HudWorldMap.cpp
    src/play/PlayGuild.cpp
    src/play/PlayAcademy.cpp
    src/play/PlayOpenMarket.cpp
    src/play/PlayCommunity.cpp
    src/play/PlayExchange.cpp
    src/play/PlayFortress.cpp
    src/play/PlayActions.cpp
    src/play/PlayPk.cpp
    src/gui/HudInventory.cpp
    src/gui/HudNotices.cpp
    src/gui/HudPerformance.cpp
    src/gui/HudPlayer.cpp
    src/gui/HudTarget.cpp
    src/gui/HudUnderBar.cpp
    src/gui/HudSkills.cpp
    src/gui/HudSkillTooltip.cpp
    src/gui/HudOptions.cpp
    src/gui/HudWindows.cpp
    src/gui/HudParty.cpp
    src/gui/HudAutoPotion.cpp
    src/gui/InterfaceHost.cpp
    src/gui/UiData.cpp
    src/gui/UiWarmup.cpp
    src/gui/UiWarmupList.cpp
    src/title/UiKit.cpp
    src/title/TitleScreen.cpp
    src/title/CharactersScreen.cpp
    src/title/CreateScreen.cpp
    src/play/PlayScreen.cpp
    src/play/PlayPortraits.cpp
    src/audio/GameAudio.cpp
    src/play/PlayEffects.cpp
    src/play/PlayParty.cpp
    src/play/PlayAutoPotion.cpp
    src/play/PlayRoute.cpp
    src/bench/CrowdRig.cpp
    src/bench/CrowdRenderer.cpp
    src/bench/FakeNetwork.cpp
)
target_link_libraries(SroReimagined PRIVATE
    sr::core sr::assets sr::game sr::audio sr::rhi sr::gfx sr::anim sr::platform sr::devui glm::glm stb sr_compile_options)
target_include_directories(SroReimagined PRIVATE src)
# Its connections to our servers (sr::net through sr::game) need the OpenSSL DLLs.
sr_copy_openssl_runtime(SroReimagined)

sr_add_shader(SroReimagined SOURCE shaders/Crowd.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kCrowdVertexShader)
sr_add_shader(SroReimagined SOURCE shaders/Crowd.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kCrowdPixelShader)
sr_add_shader(SroReimagined SOURCE shaders/Blit.hlsl ENTRY VSMain PROFILE vs_6_0 NAME kBlitVertexShader)
sr_add_shader(SroReimagined SOURCE shaders/Blit.hlsl ENTRY PSMain PROFILE ps_6_0 NAME kBlitPixelShader)
sr_add_shader(SroReimagined SOURCE shaders/DepthProbe.hlsl ENTRY CSMain PROFILE cs_6_0 NAME kDepthProbeComputeShader)

if(SR_DEV_TOOLS)
    target_sources(SroReimagined PRIVATE src/devtools/EngineWindow.cpp)
    target_compile_definitions(SroReimagined PRIVATE SR_DEV_TOOLS=1)
endif()

# The players' build (the client-release preset): a window program without a console, started beside its exe
# (src/PlayerStart.h), with its version resource and application manifest.
# Its version: the project's (the root CMakeLists.txt), the one place it is set.
set(SR_CLIENT_VERSION "${PROJECT_VERSION}")
# The shard a players' build plays on, built into the exe (res/PlayerServer.h.in): its gateway, the servers'
# certificate (the .pem the servers made, its certificate alone) and the name it was made for.
set(SR_PLAYER_GATEWAY "" CACHE STRING "The players' build: the shard's gateway, host:port")
set(SR_PLAYER_CERTIFICATE "" CACHE FILEPATH "The players' build: the servers' certificate (server.pem)")
set(SR_PLAYER_SERVER_NAME "localhost" CACHE STRING "The players' build: the name the certificate was made for (shard.cfg tls-name)")
if(SR_PLAYER_BUILD)
    if(SR_PLAYER_GATEWAY STREQUAL "" OR NOT SR_PLAYER_GATEWAY MATCHES ":[0-9]+$")
        message(FATAL_ERROR "A players' build needs its shard's gateway: -DSR_PLAYER_GATEWAY=host:port")
    endif()
    if(NOT EXISTS "${SR_PLAYER_CERTIFICATE}")
        message(FATAL_ERROR "A players' build needs its shard's certificate: -DSR_PLAYER_CERTIFICATE=path\\server.pem")
    endif()
    file(READ "${SR_PLAYER_CERTIFICATE}" SR_PLAYER_CERTIFICATE_PEM)
    if(SR_PLAYER_CERTIFICATE_PEM MATCHES "PRIVATE KEY")
        message(FATAL_ERROR "${SR_PLAYER_CERTIFICATE} holds a private key: give the certificate alone (server.pem, never server.key)")
    endif()
    if(NOT SR_PLAYER_CERTIFICATE_PEM MATCHES "BEGIN CERTIFICATE")
        message(FATAL_ERROR "${SR_PLAYER_CERTIFICATE} is not a PEM certificate")
    endif()
    configure_file(res/PlayerServer.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/PlayerServer.h @ONLY)
    target_include_directories(SroReimagined PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_sources(SroReimagined PRIVATE src/PlayerStart.cpp)
    target_compile_definitions(SroReimagined PRIVATE SR_PLAYER_BUILD=1)
    if(WIN32)
        string(REPLACE "." ";" sr_version_parts "${SR_CLIENT_VERSION}")
        list(GET sr_version_parts 0 SR_VERSION_MAJOR)
        list(GET sr_version_parts 1 SR_VERSION_MINOR)
        list(GET sr_version_parts 2 SR_VERSION_PATCH)
        configure_file(res/SroReimagined.rc.in ${CMAKE_CURRENT_BINARY_DIR}/SroReimagined.rc @ONLY)
        target_sources(SroReimagined PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/SroReimagined.rc res/SroReimagined.manifest)
        # The resource compiler does not see its icon as an input: a new icon must rebuild it.
        set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/SroReimagined.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/SroReimagined.ico)
        set_target_properties(SroReimagined PROPERTIES WIN32_EXECUTABLE TRUE)
        # A window program that still starts at main().
        if(MSVC)
            target_link_options(SroReimagined PRIVATE /ENTRY:mainCRTStartup)
        else()
            target_link_options(SroReimagined PRIVATE -Xlinker /entry:mainCRTStartup)
        endif()
        target_link_libraries(SroReimagined PRIVATE ole32 shell32)
    endif()
endif()

# Shared libraries the client needs at run time (OpenAL32.dll) go next to it.
add_custom_command(TARGET SroReimagined POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:SroReimagined> $<TARGET_FILE_DIR:SroReimagined>
    COMMAND_EXPAND_LISTS)

set_target_properties(SroReimagined PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
