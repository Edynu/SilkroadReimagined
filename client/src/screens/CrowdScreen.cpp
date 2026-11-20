#include "screens/CrowdScreen.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "sr/core/Log.h"
#include "sr/core/Profiler.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace {

double MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// A stable colour per entity.
void ColourOf(std::uint32_t id, float& r, float& g, float& b) {
    std::uint32_t h = id * 2654435761u;
    h ^= h >> 15;
    const float hue = static_cast<float>(h % 360) / 60.0f;
    const float x = 1.0f - std::abs(std::fmod(hue, 2.0f) - 1.0f);
    const int sector = static_cast<int>(hue);
    const float rgb[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
    r = 0.35f + 0.6f * rgb[sector % 6][0];
    g = 0.35f + 0.6f * rgb[sector % 6][1];
    b = 0.35f + 0.6f * rgb[sector % 6][2];
}

float LerpAngle(float a, float b, float t) {
    constexpr float kTwoPi = 6.28318531f;
    float d = std::fmod(b - a, kTwoPi);
    if (d > 3.14159265f) d -= kTwoPi;
    if (d < -3.14159265f) d += kTwoPi;
    return a + d * t;
}

} // namespace

void CrowdScreen::Enter(ScreenContext& context) {
    const core::Config& config = context.config;
    game::CrowdMessageSource::Settings settings;
    settings.population = static_cast<std::uint32_t>(std::max(0, config.GetInt("characters", 1000)));
    const auto area = static_cast<float>(config.GetDouble("crowd-area", 0.0));
    settings.area = area > 0.0f ? area : 20.0f * std::sqrt(static_cast<float>(std::max(1u, settings.population)));
    settings.burstEveryTicks = static_cast<std::uint64_t>(std::max(0, config.GetInt("burst-every-ticks", 0)));
    settings.burstSize = static_cast<std::uint32_t>(std::max(0, config.GetInt("burst-size", 500)));
    crowd_ = std::make_unique<game::SyntheticCrowd>(7, settings.area);
    network_ = std::make_unique<FakeNetwork>(settings);
    clock_ = game::SimulationClock(config.GetDouble("simulation-rate", game::SimulationClock::kDefaultRate));
    chunks_ = static_cast<std::uint32_t>(std::clamp(config.GetInt("record-chunks", 8), 1, 64));
    applyBudgetMs_ = config.GetDouble("apply-budget-ms", 2.0);
    hashStep_ = static_cast<std::uint64_t>(std::max(1, config.GetInt("hash-step", 300)));
    // Reserve palette memory for the expected peak now: creating GPU memory during play stalls
    // the driver (and with it the render thread's submit), even from a worker thread. Growth
    // remains as a fallback only.
    const std::size_t peak = std::max<std::size_t>(4096, std::size_t{settings.population} * 2 + std::size_t{settings.burstSize} * 3);
    paletteWanted_ = peak * CrowdRig::kBones * CrowdRig::kBytesPerBone / 2;
    camera_.targetX = camera_.targetZ = settings.area * 0.5f;
    camera_.distance = settings.area * 0.9f;
    context.camera.Store(camera_.yaw, camera_.pitch);
}

void CrowdScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    network_.reset(); // stops its thread
    for (PaletteSlot& slot : palettes_) {
        if (slot.buffer) snapshot.releaseBuffers.push_back(slot.buffer);
        if (slot.growing) {
            // A growth still running finishes on its own; wait for it so its buffer is not lost.
            while (!slot.growing->ready.load(std::memory_order_acquire)) std::this_thread::yield();
            if (slot.growing->buffer) snapshot.releaseBuffers.push_back(slot.growing->buffer);
        }
        slot = {};
    }
    context.camera.Clear();
}

std::uint32_t CrowdScreen::ApplyMessages(double budgetMs) {
    SR_ZONE("crowd: apply messages");
    const auto start = std::chrono::steady_clock::now();
    std::uint32_t spawns = 0;
    // In arrival order; what does not fit in the budget waits for the next frame.
    while (receivedCursor_ < received_.size()) {
        const game::CrowdMessage& m = received_[receivedCursor_++];
        crowd_->Apply(m);
        if (m.kind == game::CrowdMessage::Kind::Spawn) ++spawns;
        if ((receivedCursor_ & 63) == 0 && MsSince(start) > budgetMs) break;
    }
    if (receivedCursor_ == received_.size()) {
        received_.clear();
        receivedCursor_ = 0;
    }
    return spawns;
}

void CrowdScreen::Simulate(ScreenContext& context) {
    SR_ZONE("crowd: simulate");
    const int steps = clock_.Advance(context.dt);
    for (int i = 0; i < steps; ++i) {
        if (!crowd_->CanStep()) {
            // The tick's messages have not all arrived (or not all fit the budget): the steps stay
            // owed and run when they do, so the simulation never skips a tick.
            starved_ += static_cast<std::uint64_t>(steps - i);
            clock_.Return(steps - i);
            break;
        }
        crowd_->Step(context.jobs, clock_.StepSeconds());
        if (!hashLogged_ && crowd_->Steps() == hashStep_) {
            hashLogged_ = true;
            core::LogInfo("crowd: state hash at step {}: {:016x} ({} entities, {} workers)", hashStep_, crowd_->Hash(),
                          crowd_->Entities().size(), context.jobs.WorkerCount());
        }
    }
    network_->SetConsumedTick(crowd_->Steps());
}

void CrowdScreen::Present(ScreenContext& context, FrameSnapshot& snapshot) {
    SR_ZONE("crowd: present");
    std::span<const game::CrowdEntity> entities = crowd_->Entities();
    constexpr std::size_t kCharacterBytes = CrowdRig::kBones * CrowdRig::kBytesPerBone;
    PaletteSlot& slot = palettes_[context.slot];
    // A finished growth replaces the buffer; the old one goes when frames using it are done.
    if (slot.growing && slot.growing->ready.load(std::memory_order_acquire)) {
        if (slot.buffer) snapshot.releaseBuffers.push_back(slot.buffer);
        slot.buffer = slot.growing->buffer;
        slot.bytes = slot.growing->bytes;
        slot.growing.reset();
    }
    // Grow ahead: past three quarters full, a Streaming job creates one twice as big.
    const std::size_t needed = entities.size() * kCharacterBytes;
    paletteWanted_ = std::max(paletteWanted_, needed);
    if (!slot.growing && (slot.bytes == 0 || needed > slot.bytes * 3 / 4)) {
        auto growth = std::make_shared<PaletteSlot::Growth>();
        growth->bytes = std::max<std::size_t>({slot.bytes * 2, paletteWanted_ * 2, 1024 * kCharacterBytes});
        slot.growing = growth;
        context.jobs.Run(
            [growth, device = &context.device] {
                SR_ZONE("crowd: grow palettes");
                rhi::BufferDesc desc;
                desc.size = growth->bytes;
                desc.usage = rhi::BufferUsage::Storage;
                desc.debugName = "crowd palettes";
                desc.cpuVisible = true;
                growth->buffer = device->CreateBuffer(desc);
                growth->ready.store(true, std::memory_order_release);
            },
            core::JobPriority::Streaming);
    }
    auto* memory = static_cast<std::byte*>(context.device.MappedMemory(slot.buffer));
    if (memory == nullptr) return; // the first buffer is still being made: nothing to show yet
    // Until a bigger buffer arrives, show as many as fit (a frame or two, never a stall).
    entities = entities.first(std::min(entities.size(), slot.bytes / kCharacterBytes));

    // Interpolate between the last two steps and pose every character, on the workers, straight
    // into this slot's GPU memory (the slot's last GPU frame is done: SnapshotQueue waited).
    const float alpha = static_cast<float>(clock_.Alpha());
    const float step = static_cast<float>(clock_.StepSeconds());
    const double now = crowd_->Time() - (1.0 - clock_.Alpha()) * clock_.StepSeconds();
    context.jobs.ParallelFor(entities.size(), 16, [&](std::size_t begin, std::size_t end) {
        SR_ZONE("crowd: pose");
        for (std::size_t i = begin; i < end; ++i) {
            const game::CrowdEntity& e = entities[i];
            CrowdRig::Placement p;
            p.x = e.prevX + (e.x - e.prevX) * alpha;
            p.z = e.prevZ + (e.z - e.prevZ) * alpha;
            p.yaw = LerpAngle(e.prevYaw, e.yaw, alpha);
            p.animTime = std::max(0.0f, e.animTime - (e.speed > 0 ? (1.0f - alpha) * step : 0.0f));
            p.runBlend = std::clamp((e.speed - game::SyntheticCrowd::kWalkSpeed) /
                                        (game::SyntheticCrowd::kRunSpeed - game::SyntheticCrowd::kWalkSpeed), 0.0f, 1.0f);
            p.scale = static_cast<float>(std::clamp((now - e.spawnedAt) / 0.4, 0.0, 1.0));
            ColourOf(e.id, p.r, p.g, p.b);
            rig_.Pose(p, memory + i * CrowdRig::kBones * CrowdRig::kBytesPerBone);
        }
    });

    snapshot.crowd.active = true;
    snapshot.crowd.palettes = slot.buffer;
    snapshot.crowd.bones = static_cast<std::uint32_t>(entities.size()) * CrowdRig::kBones;
    snapshot.crowd.bonesPerCharacter = CrowdRig::kBones;
    snapshot.crowd.chunks = chunks_;
    snapshot.crowd.camera = camera_;
}

void CrowdScreen::DrawUi(ScreenContext& context) {
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 376, 16}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({360, 0}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Crowd")) {
        ImGui::Text("characters %zu (%zu bones)", crowd_->Entities().size(), crowd_->Entities().size() * CrowdRig::kBones);
        ImGui::Text("simulation: step %llu at %.0f Hz, %llu starved", static_cast<unsigned long long>(crowd_->Steps()),
                    clock_.Rate(), static_cast<unsigned long long>(starved_));
        ImGui::Text("apply %.2f ms (%u spawns), steps %.2f ms, pose %.2f ms", applyMs_, lastSpawns_, stepMs_, poseMs_);
        ImGui::Text("pending messages %zu", received_.size() - receivedCursor_);
        int chunks = static_cast<int>(chunks_);
        if (ImGui::SliderInt("record chunks", &chunks, 1, 64)) chunks_ = static_cast<std::uint32_t>(chunks);
        if (ImGui::Button("Burst of spawns (B)")) network_->RequestBurst();
        ImGui::SameLine();
        if (ImGui::Button("Log state hash")) {
            core::LogInfo("crowd: state hash at step {}: {:016x} ({} workers)", crowd_->Steps(), crowd_->Hash(),
                          context.jobs.WorkerCount());
        }
    }
    ImGui::End();
}

void CrowdScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    SR_PROFILE("crowd: update");
    // Input: right-drag turns, the wheel zooms (unless the tools want the mouse).
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camera_.yaw -= io.MouseDelta.x * 0.005f;
            camera_.pitch = std::clamp(camera_.pitch + io.MouseDelta.y * 0.005f, 0.05f, 1.5f);
        }
        camera_.distance = std::clamp(camera_.distance * std::pow(0.9f, io.MouseWheel), 50.0f, 20000.0f);
    }
    if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_B, false)) network_->RequestBurst();
    context.camera.Store(camera_.yaw, camera_.pitch);

    auto start = std::chrono::steady_clock::now();
    network_->Take(received_);
    lastSpawns_ = ApplyMessages(applyBudgetMs_);
    applyMs_ = MsSince(start);
    snapshot.burst = lastSpawns_ >= 100;

    start = std::chrono::steady_clock::now();
    Simulate(context);
    stepMs_ = MsSince(start);

    start = std::chrono::steady_clock::now();
    Present(context, snapshot);
    poseMs_ = MsSince(start);

    if (context.toolsVisible) DrawUi(context);
}

} // namespace sr::client
