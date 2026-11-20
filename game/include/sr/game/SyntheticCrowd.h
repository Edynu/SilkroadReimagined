#pragma once

// A stand-in world for the threading benchmark and tests: entities
// that walk between points, spawned, moved and despawned by server-like messages grouped in
// ticks, and updated with the phase discipline. It has the shape of the real simulation (C6)
// without its game rules.

#include <cstdint>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

#include "sr/core/Jobs.h"
#include "sr/game/PhaseCommands.h"

namespace sr::game {

struct CrowdMessage {
    enum class Kind : std::uint8_t { Spawn, Despawn, MoveTo, TickEnd };
    Kind kind = Kind::TickEnd;
    std::uint32_t entity = 0;
    std::uint64_t tick = 0; // the server tick the message belongs to (1-based)
    float x = 0, z = 0;     // Spawn: position; MoveTo: destination
};

struct CrowdEntity {
    std::uint32_t id = 0;
    float prevX = 0, prevZ = 0, prevYaw = 0; // the state one step earlier, for interpolation
    float x = 0, z = 0, yaw = 0;
    float destX = 0, destZ = 0;
    float speed = 0;          // units a second; 0 standing
    float animTime = 0;       // seconds into its walk cycle
    double spawnedAt = 0;     // simulation time of its spawn (presentation fades it in)
};

class SyntheticCrowd {
public:
    static constexpr float kWalkSpeed = 16.0f;
    static constexpr float kRunSpeed = 48.0f;

    explicit SyntheticCrowd(std::uint32_t seed = 7, float area = 600.0f);

    // Applies one message (serial). TickEnd marks its tick complete.
    void Apply(const CrowdMessage& message);
    // The newest tick whose messages have all been applied; Step() may run up to it.
    std::uint64_t CompleteTick() const { return completeTick_; }
    bool CanStep() const { return completeTick_ > steps_; }

    // One fixed step (tick Steps() + 1): every entity updates in parallel and writes only itself;
    // the commands it produced are then applied in entity order.
    void Step(core::JobSystem& jobs, double stepSeconds);

    std::uint64_t Steps() const { return steps_; }
    double Time() const { return time_; }
    std::span<const CrowdEntity> Entities() const { return entities_; }
    // Over every entity's state in storage order; equal across thread counts (R4).
    std::uint64_t Hash() const;

private:
    struct Arrived {}; // an entity reached its destination: the commit gives it a new one

    void Remove(std::uint32_t id);

    std::vector<CrowdEntity> entities_;
    std::unordered_map<std::uint32_t, std::uint32_t> indexOf_;
    PhaseCommands<Arrived> commands_;
    std::mt19937 rng_;
    float area_;
    std::uint64_t completeTick_ = 0;
    std::uint64_t steps_ = 0;
    double time_ = 0;
};

// The messages a server would send for the crowd, tick by tick and deterministically: the
// initial population at tick 1, a trickle of moves, despawns and spawns, and bursts of spawns.
class CrowdMessageSource {
public:
    struct Settings {
        std::uint32_t population = 1000;
        std::uint32_t seed = 1;
        float area = 600.0f;
        std::uint64_t burstEveryTicks = 0; // 0: no scheduled bursts
        std::uint32_t burstSize = 500;
        std::uint64_t burstLifeTicks = 150; // a burst's entities leave again after this
    };
    explicit CrowdMessageSource(const Settings& settings);

    // Appends every message of `tick` (call with 1, 2, 3, ...), ending with TickEnd.
    void Generate(std::uint64_t tick, std::vector<CrowdMessage>& out);
    // A burst at the next generated tick (interactive; not deterministic).
    void RequestBurst() { burstRequested_ = true; }

private:
    void Spawn(std::uint64_t tick, std::vector<CrowdMessage>& out, std::vector<std::uint32_t>* group);

    Settings settings_;
    std::mt19937 rng_;
    std::vector<std::uint32_t> alive_;
    std::vector<std::pair<std::uint64_t, std::vector<std::uint32_t>>> leaving_; // tick, burst ids
    std::uint32_t nextId_ = 1;
    bool burstRequested_ = false;
};

} // namespace sr::game
