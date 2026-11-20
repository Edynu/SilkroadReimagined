# sr::game - the simulation: world, entities, movement, combat,
# items, skills, quests, social, and the adapter from server messages to game events. It never
# depends on rendering, audio, UI or the window (checked by cmake/DependencyRules.cmake), so it
# runs headless in tests and could run on a server.
add_library(sr_game STATIC
    src/SimulationClock.cpp
    src/Berserk.cpp
    src/GameClock.cpp
    src/NavMesh.cpp
    src/NavPath.cpp
    src/NavLoader.cpp
    src/NavDungeon.cpp
    src/Weather.cpp
    src/Loading.cpp
    src/SyntheticCrowd.cpp
    src/Protocol.cpp
    src/CharacterRules.cpp
    src/TitleSession.cpp
    src/WorldRecording.cpp
    src/Guild.cpp
    src/Community.cpp
    src/Exchange.cpp
    src/Stall.cpp
    src/Cos.cpp
    src/Fortress.cpp
    src/Jobs.cpp
    src/Pk.cpp
    src/SkillBench.cpp
    src/WorldView.cpp
    src/Inventory.cpp
    src/Items.cpp
    src/Combat.cpp
    src/States.cpp
    src/Stats.cpp
    src/Buffs.cpp
    src/Passives.cpp
    src/Heals.cpp
    src/Skills.cpp
    src/SkillText.cpp
    src/SkillCatalogue.cpp
    src/ItemCatalogue.cpp
    src/SpecialItems.cpp
    src/QuestItems.cpp
    src/BetaQuests.cpp
    src/Sets.cpp
    src/Spirit.cpp
    src/Storage.cpp
    src/PlayerState.cpp
    src/Party.cpp
    src/Potions.cpp
    src/Places.cpp
    src/FlagWorld.cpp
    src/Arena.cpp
    src/Academy.cpp
    src/AcademyView.cpp src/OpenMarketView.cpp
    src/FlagView.cpp
    src/ArenaView.cpp
    src/Quest.cpp
    src/QuestView.cpp
    src/Alchemy.cpp
    src/MallServices.cpp
    src/Mall.cpp
    src/MagicPop.cpp
    src/TimedServices.cpp
)
add_library(sr::game ALIAS sr_game)

target_include_directories(sr_game PUBLIC include)
target_link_libraries(sr_game PUBLIC sr::core sr::assets sr::net PRIVATE sr_compile_options)
