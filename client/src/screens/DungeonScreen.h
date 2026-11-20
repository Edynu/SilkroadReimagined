#pragma once

// A dungeon as a test scene. `--dungeon N`
// picks it by region id (0x8001 the Donwhang cave, the default; 0x8009 the event map) or by its
// dungeoninfo.txt id (1, 9); its .dof (world/DungeonScene) and its navigation (game::NavLoader) are
// read on jobs. The camera starts over `--block N` (default: the first entrance block) or at
// `--camera x,y,z,yaw,pitch` in the dungeon's own space; right mouse drag looks around, WASD / QE fly
// (Shift faster).
//
// Drawn as the original draws a dungeon (DungeonScene): the current block (here the one whose floor
// is under the camera; the original takes the player's) and every block in its visible list. The
// original's loading screen (its picture for the dungeon) stays up until the first view's models are
// in (C4e).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "screens/Screen.h"
#include "world/DungeonScene.h"
#include "world/LoadingScreen.h"
#include "sr/game/NavLoader.h"
#include "sr/game/NavMesh.h"

namespace sr::client {

class DungeonScreen final : public Screen {
public:
    const char* Name() const override { return "dungeon"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    void Start();
    void Fly(ScreenContext& context);
    DungeonScene::Handles Handles();

    DungeonScene scene_;
    // The dungeon's navigation (the NavLoader loads a dungeon's region as a game::NavDungeon).
    std::unique_ptr<game::NavLoader> navLoader_;
    game::NavWorld nav_;
    bool started_ = false, reported_ = false;
    std::uint16_t region_ = 0x8001;
    int startBlock_ = -1;

    OrbitCamera camera_;
    bool cameraPlaced_ = false;
    std::vector<std::uint32_t> freeHandles_;
    std::uint32_t nextHandle_ = 0;
    bool glow_ = true;
    // The loading screen while the start block's models load (entering a dungeon is a teleport).
    LoadingScreen loading_;
    ui::DrawList loadingUi_;
};

} // namespace sr::client
