#pragma once

// The crowd benchmark: a synthetic crowd fed by a stand-in network
// thread, simulated at a fixed rate with the phase discipline, posed by jobs straight into GPU
// memory, drawn by the render thread in parallel chunks. Right-drag turns the camera, the wheel
// zooms, B (or the button) sends a burst of spawns.

#include <array>
#include <memory>
#include <vector>

#include "bench/CrowdRig.h"
#include "bench/FakeNetwork.h"
#include "screens/Screen.h"
#include "sr/game/SimulationClock.h"
#include "sr/game/SyntheticCrowd.h"

namespace sr::client {

class CrowdScreen final : public Screen {
public:
    const char* Name() const override { return "crowd"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    // Applies received messages in order for at most `budgetMs`; returns the spawns applied.
    std::uint32_t ApplyMessages(double budgetMs);
    void Simulate(ScreenContext& context);
    void Present(ScreenContext& context, FrameSnapshot& snapshot);
    void DrawUi(ScreenContext& context);

    std::unique_ptr<game::SyntheticCrowd> crowd_;
    std::unique_ptr<FakeNetwork> network_;
    game::SimulationClock clock_;
    CrowdRig rig_;
    // Per snapshot slot: the bone palette buffer, and a bigger one being created on a Streaming
    // job when the crowd grows towards the current one's size (never on the frame's path).
    struct PaletteSlot {
        rhi::BufferHandle buffer;
        std::size_t bytes = 0;
        struct Growth {
            std::atomic<bool> ready{false};
            rhi::BufferHandle buffer;
            std::size_t bytes = 0;
        };
        std::shared_ptr<Growth> growing;
    };
    std::array<PaletteSlot, SnapshotQueue::kSlots> palettes_{};
    std::size_t paletteWanted_ = 0; // bytes the crowd may soon need
    std::vector<game::CrowdMessage> received_;
    std::size_t receivedCursor_ = 0;
    OrbitCamera camera_;
    std::uint32_t chunks_ = 8;
    double applyBudgetMs_ = 2.0;
    std::uint64_t hashStep_ = 300;
    bool hashLogged_ = false;
    // Last frame's numbers, for the window.
    double applyMs_ = 0, stepMs_ = 0, poseMs_ = 0;
    std::uint32_t lastSpawns_ = 0;
    std::uint64_t starved_ = 0; // steps the clock wanted that had no complete tick yet
};

} // namespace sr::client
