include(FetchContent)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
# doctest's CMakeLists still declares cmake_minimum_required < 3.5, which CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM)

add_executable(sr_tests
    main.cpp
    CoreTests.cpp
    BlowfishTests.cpp
    TextureTests.cpp
    ModelTests.cpp
    WorldGridTests.cpp
    UiLayoutTests.cpp
    IntroScriptTests.cpp
    GameDataTableTests.cpp
    EffectTests.cpp
    CooldownTests.cpp
    CombatTests.cpp
    BuffTests.cpp
    PassiveTests.cpp
    HealTests.cpp
    PkTests.cpp
    FlagViewTests.cpp
    ArenaViewTests.cpp
    OptionsTests.cpp
    EnvironmentTests.cpp
    UiTests.cpp
    InterfaceTests.cpp
    ChatTests.cpp
    InventoryTests.cpp
    EquipmentTests.cpp
    HudTests.cpp
    UiWarmupTests.cpp
    PortraitTests.cpp
    Pk2ArchiveTests.cpp
    FileSystemTests.cpp
    GameDataTests.cpp
    JobsTests.cpp
    ConfigTests.cpp
    SimulationClockTests.cpp
    NavMeshTests.cpp
    NavDataTests.cpp
    DungeonDataTests.cpp
    SyntheticCrowdTests.cpp
    StreamingTests.cpp
    ProtocolTests.cpp
    NetTests.cpp
)
target_link_libraries(sr_tests PRIVATE sr::assets sr::net sr::ui sr::game httplib::httplib asio doctest::doctest sr_compile_options)
sr_copy_openssl_runtime(sr_tests)
# The client's HUD manager and windows, tested headless (they need only sr::ui and sr::game).
set(SR_HUD_SOURCES Hud HudChat HudGuild HudGuildBoxes HudCommunity HudAcademy HudOpenMarket HudInventory HudMinimap HudNotices HudPerformance HudNpc HudPlayer HudTarget HudOptions HudSkills HudSkillTooltip HudUnderBar HudWindows HudParty HudAutoPotion HudGuildMaster HudGuildWar HudFortress HudActions HudFortressMap HudFortressWork HudMapView HudWorldMap HudExchange HudStall HudFreePvp HudQuest HudInstance HudFlagWorld HudArena HudCos HudAlchemy HudItemOptions HudJobs HudTrade HudMagicPop HudMall HudServices HudTemple)
list(TRANSFORM SR_HUD_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/client/src/gui/)
list(TRANSFORM SR_HUD_SOURCES APPEND .cpp)
target_sources(sr_tests PRIVATE ${SR_HUD_SOURCES} ${PROJECT_SOURCE_DIR}/client/src/gui/UiWarmupList.cpp)
# The bug report's window (the root's SR_BUG_REPORTS), as the client builds it.
if(SR_BUG_REPORTS)
    target_sources(sr_tests PRIVATE ${PROJECT_SOURCE_DIR}/client/src/gui/HudBugReport.cpp)
    target_compile_definitions(sr_tests PRIVATE SR_BUG_REPORTS=1)
endif()
target_include_directories(sr_tests PRIVATE ${PROJECT_SOURCE_DIR}/client/src)
# The servers, run in-process (their database tests need SR_DB_URL).
if(TARGET sr::login)
    target_sources(sr_tests PRIVATE ShardTests.cpp CrossServerTests.cpp CryptoTests.cpp ServerTests.cpp LobbyTests.cpp WorldTests.cpp GuildTests.cpp ImportTests.cpp SkillTests.cpp StateTests.cpp PartyTests.cpp AutoPotionTests.cpp FortressTests.cpp DungeonTravelTests.cpp ActionTests.cpp ExchangeTests.cpp CosTests.cpp StallTests.cpp QuestTests.cpp InstanceTests.cpp RocTests.cpp QinTombTests.cpp WebTests.cpp UniqueTests.cpp WebAdminTests.cpp WebPlayerTests.cpp AlchemyTests.cpp JobTests.cpp MallTests.cpp MagicPopTests.cpp MallItemTests.cpp MallServiceTests.cpp MurderTests.cpp FlagWorldTests.cpp SkillLabTests.cpp ItemLabTests.cpp ItemUseTests.cpp QuestItemUseTests.cpp QuestCaptureTests.cpp QuestTrialTests.cpp QuestFamiliesEuropeTests.cpp QuestFamiliesAsiaTests.cpp QuestTownsTests.cpp GuildMercenaryTests.cpp SetTests.cpp SpiritTests.cpp DropBlueTests.cpp PetItemTests.cpp FuzzTests.cpp SqlServerBackupTests.cpp GameLogTests.cpp)
    target_link_libraries(sr_tests PRIVATE sr::gateway sr::login sr::gameserver sr::web sr::skilllab nlohmann_json::nlohmann_json)
    sr_copy_postgresql_runtime(sr_tests)
endif()
# Header-only render helpers (Frustum.h), when GLM is there (it comes with the client);
# sr_render itself needs a GPU device.
if(TARGET glm::glm)
    target_sources(sr_tests PRIVATE RenderTests.cpp)
    target_link_libraries(sr_tests PRIVATE glm::glm)
    target_include_directories(sr_tests PRIVATE ${PROJECT_SOURCE_DIR}/engine/render/include)
endif()
# The rewrite's renderer: the parts that need no GPU (the render graph's plan, the terrain build).
if(TARGET sr::gfx)
    target_sources(sr_tests PRIVATE RenderGraphTests.cpp TerrainBuildTests.cpp EffectSystemTests.cpp)
    target_link_libraries(sr_tests PRIVATE sr::gfx)
endif()
# The sound files and tables (sr::audio_data comes with the client; the tests need no OpenAL).
if(TARGET sr::audio_data)
    target_sources(sr_tests PRIVATE SoundTests.cpp)
    target_link_libraries(sr_tests PRIVATE sr::audio_data)
    target_compile_definitions(sr_tests PRIVATE SR_TEST_AUDIO=1)
endif()

add_test(NAME sr_tests COMMAND sr_tests)
