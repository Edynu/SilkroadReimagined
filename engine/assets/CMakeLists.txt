add_library(sr_assets STATIC
    src/Blowfish.cpp
    src/Pk2Archive.cpp
    src/FileSystem.cpp
    src/Texture.cpp
    src/Effect.cpp
    src/Model.cpp
    src/Animation.cpp
    src/Terrain.cpp
    src/NavMesh.cpp
    src/Dungeon.cpp
    src/WorldGrid.cpp
    src/UiLayout.cpp
    src/UiLayout2dt.cpp
    src/IntroScript.cpp
    src/Dress.cpp
    src/GameData.cpp
    src/Outfit.cpp
    src/Shop.cpp
    src/Gacha.cpp
    src/ExeCursors.cpp
    src/SkillAnimation.cpp
    src/ItemGlow.cpp
    src/CollectionBook.cpp
    src/Cooldown.cpp
    src/Environment.cpp
    src/WorldMap.cpp
    src/GameGuide.cpp
    src/RegionInfo.cpp
    src/StallNetworkData.cpp
)
add_library(sr::assets ALIAS sr_assets)

target_include_directories(sr_assets PUBLIC include)
target_link_libraries(sr_assets PUBLIC sr::core PRIVATE sr_compile_options)
