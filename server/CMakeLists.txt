# Our servers: the gateway, the login server and (C5d) the game server.
# They link the same game/ as the client and never rendering, audio, UI or the window
# (cmake/DependencyRules.cmake).

# sr::server - what the servers share: PostgreSQL, the schema, accounts, game-server reports,
# tickets, password hashing, the server loop's pieces, the command line.
add_library(sr_server STATIC
    common/src/Database.cpp
    common/src/Schema.cpp
    common/src/Crypto.cpp
    common/src/Store.cpp
    common/src/SupportStore.cpp
    common/src/ClientGate.cpp
    common/src/AdminStore.cpp
    common/src/GuildStore.cpp
    common/src/ShardLayout.cpp
    common/src/CommunityStore.cpp
    common/src/AcademyStore.cpp
    common/src/FortressStore.cpp
    common/src/QuestScript.cpp
    common/src/OriginalQuests.cpp
    common/src/OriginalQuestsAsia.cpp
    common/src/OriginalQuestsEurope.cpp
    common/src/OriginalQuestsTowns.cpp
    common/src/QuestStore.cpp
    common/src/InstanceStore.cpp
    common/src/EventStore.cpp
    common/src/JobStore.cpp
    common/src/SilkStore.cpp
    common/src/ServiceStore.cpp
    common/src/Import.cpp
    common/src/SqlServerBackup.cpp
    common/src/Service.cpp
    common/src/Program.cpp
)
add_library(sr::server ALIAS sr_server)
target_include_directories(sr_server PUBLIC common/include)
target_link_libraries(sr_server PUBLIC sr::core sr::net sr::game PRIVATE PostgreSQL::PostgreSQL OpenSSL::Crypto sr_compile_options)

# The gateway: a library (the tests run it in-process) and SroGateway.exe.
add_library(sr_gateway STATIC gateway/src/GatewayServer.cpp)
add_library(sr::gateway ALIAS sr_gateway)
target_include_directories(sr_gateway PUBLIC gateway/include)
target_link_libraries(sr_gateway PUBLIC sr::server PRIVATE sr_compile_options)

add_executable(SroGateway gateway/src/main.cpp)
target_link_libraries(SroGateway PRIVATE sr::gateway sr_compile_options)
sr_copy_openssl_runtime(SroGateway)
sr_copy_postgresql_runtime(SroGateway)

# The login server.
add_library(sr_login STATIC login/src/LoginServer.cpp)
add_library(sr::login ALIAS sr_login)
target_include_directories(sr_login PUBLIC login/include)
target_link_libraries(sr_login PUBLIC sr::server PRIVATE sr_compile_options)

add_executable(SroLogin login/src/main.cpp)
target_link_libraries(SroLogin PRIVATE sr::login sr_compile_options)
sr_copy_openssl_runtime(SroLogin)
sr_copy_postgresql_runtime(SroLogin)

# The front machine's program: the gateway, the login server and the shard's control in one process.
add_executable(SroControl control/src/main.cpp)
target_link_libraries(SroControl PRIVATE sr::gateway sr::login sr_compile_options)
sr_copy_openssl_runtime(SroControl)
sr_copy_postgresql_runtime(SroControl)

# The website's API: a library (the tests run it in-process) and SroWeb.exe.
add_library(sr_web STATIC web/src/WebServer.cpp web/src/WebAdmin.cpp web/src/WebPlayers.cpp web/src/WebDev.cpp web/src/WebSupport.cpp)
add_library(sr::web ALIAS sr_web)
target_include_directories(sr_web PUBLIC web/include)
target_link_libraries(sr_web PUBLIC sr::server PRIVATE httplib::httplib nlohmann_json::nlohmann_json sr_compile_options)

add_executable(SroWeb web/src/main.cpp)
target_link_libraries(SroWeb PRIVATE sr::web sr_compile_options)
sr_copy_openssl_runtime(SroWeb)
sr_copy_postgresql_runtime(SroWeb)

# The game server (C5d: the lobby; the world from C6).
add_library(sr_gameserver STATIC game/src/GameServer.cpp game/src/Guilds.cpp game/src/World.cpp game/src/Monsters.cpp game/src/Skills.cpp game/src/Drops.cpp game/src/ReferenceData.cpp game/src/DatabaseText.cpp
    game/src/Parties.cpp game/src/Fortress.cpp game/src/Community.cpp game/src/Academies.cpp game/src/OpenMarket.cpp game/src/Exchanges.cpp game/src/Coses.cpp game/src/Alchemy.cpp game/src/Stalls.cpp game/src/Quests.cpp game/src/Leagues.cpp game/src/Mall.cpp game/src/MagicPop.cpp game/src/Trades.cpp game/src/Bandits.cpp
    game/src/Instances.cpp game/src/InstanceData.cpp game/src/FlagWorld.cpp game/src/Arena.cpp game/src/SummonEvents.cpp
    game/src/Roc.cpp game/src/Temples.cpp)
add_library(sr::gameserver ALIAS sr_gameserver)
target_include_directories(sr_gameserver PUBLIC game/include)
target_link_libraries(sr_gameserver PUBLIC sr::server sr::game sr::assets PRIVATE sr_compile_options)
if(SR_DEV_COMMANDS)
    target_compile_definitions(sr_gameserver PUBLIC SR_DEV_COMMANDS=1)
endif()
if(SR_BUG_REPORTS)
    target_compile_definitions(sr_gameserver PRIVATE SR_BUG_REPORTS=1)
endif()

add_executable(SroGame game/src/main.cpp)
target_link_libraries(SroGame PRIVATE sr::gameserver sr_compile_options)
sr_copy_openssl_runtime(SroGame)
sr_copy_postgresql_runtime(SroGame)

# The bot benchmark (C6f): bots enter one game server's world and walk.
add_executable(SroBots bots/src/main.cpp)
target_link_libraries(SroBots PRIVATE sr::server sr::game sr::assets sr_compile_options)
sr_copy_openssl_runtime(SroBots)
sr_copy_postgresql_runtime(SroBots)

# The database tool: the original server's export imported, our layout made.
add_executable(SroDb dbtool/src/main.cpp)
target_link_libraries(SroDb PRIVATE sr::server sr_compile_options)
sr_copy_openssl_runtime(SroDb)
sr_copy_postgresql_runtime(SroDb)

# A server's one-time setup (docs/SERVER_SETUP.md 5): the original's shard backup and quest script into the database.
add_executable(SroSetup setup/src/main.cpp)
target_link_libraries(SroSetup PRIVATE sr::server sr::assets sr_compile_options)
sr_copy_openssl_runtime(SroSetup)
sr_copy_postgresql_runtime(SroSetup)

# The skill lab: its list into the database, then its exam; a library (the tests run it) and
# SroSkillLab.exe. It needs the developer commands (its exam's setup).
if(SR_DEV_COMMANDS)
add_library(sr_skilllab STATIC skilllab/src/SkillLab.cpp skilllab/src/SkillExam.cpp skilllab/src/ItemLab.cpp skilllab/src/ItemExam.cpp)
add_library(sr::skilllab ALIAS sr_skilllab)
target_include_directories(sr_skilllab PUBLIC skilllab/include)
target_link_libraries(sr_skilllab PUBLIC sr::server sr::gameserver sr::game sr::assets PRIVATE nlohmann_json::nlohmann_json sr_compile_options)

add_executable(SroSkillLab skilllab/src/main.cpp)
target_link_libraries(SroSkillLab PRIVATE sr::skilllab sr_compile_options)
sr_copy_openssl_runtime(SroSkillLab)
sr_copy_postgresql_runtime(SroSkillLab)
endif()
