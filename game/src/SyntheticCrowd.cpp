#include "sr/game/SyntheticCrowd.h"

#include <algorithm>
#include <cmath>

#include "sr/core/FramePacer.h"
#include "sr/core/Trace.h"

namespace sr::game {

namespace {

// [0, 1) from the generator's bits, identical on every platform (unlike the std distributions).
float Unit(std::mt19937& rng) {
    return static_cast<float>(rng() >> 8) * (1.0f / 16777216.0f);
}

} // namespace

SyntheticCrowd::SyntheticCrowd(std::uint32_t seed, float area) : rng_(seed), area_(area) {}

void SyntheticCrowd::Apply(const CrowdMessage& message) {
    switch (message.kind) {
    case CrowdMessage::Kind::Spawn: {
        if (indexOf_.contains(message.entity)) return;
        CrowdEntity e;
        e.id = message.entity;
        e.x = e.prevX = message.x;
        e.z = e.prevZ = message.z;
        e.destX = Unit(rng_) * area_;
        e.destZ = Unit(rng_) * area_;
        e.speed = Unit(rng_) < 0.3f ? kRunSpeed : kWalkSpeed;
        e.yaw = e.prevYaw = std::atan2(e.destX - e.x, e.destZ - e.z);
        e.spawnedAt = time_;
        indexOf_[e.id] = static_cast<std::uint32_t>(entities_.size());
        entities_.push_back(e);
        break;
    }
    case CrowdMessage::Kind::Despawn:
        Remove(message.entity);
        break;
    case CrowdMessage::Kind::MoveTo: {
        const auto it = indexOf_.find(message.entity);
        if (it == indexOf_.end()) return;
        CrowdEntity& e = entities_[it->second];
        e.destX = message.x;
        e.destZ = message.z;
        e.speed = kWalkSpeed;
        break;
    }
    case CrowdMessage::Kind::TickEnd:
        completeTick_ = std::max(completeTick_, message.tick);
        break;
    }
}

void SyntheticCrowd::Remove(std::uint32_t id) {
    const auto it = indexOf_.find(id);
    if (it == indexOf_.end()) return;
    const std::uint32_t index = it->second;
    indexOf_.erase(it);
    if (index + 1 != entities_.size()) {
        entities_[index] = entities_.back();
        indexOf_[entities_[index].id] = index;
    }
    entities_.pop_back();
}

void SyntheticCrowd::Step(core::JobSystem& jobs, double stepSeconds) {
    SR_ZONE("crowd: step");
    const float dt = static_cast<float>(stepSeconds);

    // Update (parallel): each job writes only its own entities.
    commands_.Begin(jobs);
    jobs.ParallelFor(entities_.size(), 128, [&](std::size_t begin, std::size_t end) {
        SR_ZONE("crowd: update entities");
        for (std::size_t i = begin; i < end; ++i) {
            CrowdEntity& e = entities_[i];
            e.prevX = e.x;
            e.prevZ = e.z;
            e.prevYaw = e.yaw;
            if (e.speed <= 0.0f) continue;
            const float dx = e.destX - e.x;
            const float dz = e.destZ - e.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            const float travel = e.speed * dt;
            e.animTime += dt;
            if (distance <= travel) {
                e.x = e.destX;
                e.z = e.destZ;
                e.speed = 0.0f;
                commands_.Add(e.id, Arrived{});
            } else {
                e.x += dx / distance * travel;
                e.z += dz / distance * travel;
                e.yaw = std::atan2(dx, dz);
            }
        }
    });

    // Commit (serial, by entity id): the shared random stream is drawn in the same order
    // whatever thread updated which entity.
    for (const auto& entry : commands_.Merge()) {
        const auto it = indexOf_.find(entry.entity);
        if (it == indexOf_.end()) continue;
        CrowdEntity& e = entities_[it->second];
        e.destX = Unit(rng_) * area_;
        e.destZ = Unit(rng_) * area_;
        e.speed = Unit(rng_) < 0.3f ? kRunSpeed : kWalkSpeed;
    }
    ++steps_;
    time_ += stepSeconds;
}

std::uint64_t SyntheticCrowd::Hash() const {
    core::Hash64 hash;
    hash.Value(steps_);
    for (const CrowdEntity& e : entities_) {
        hash.Value(e.id);
        hash.Value(e.x);
        hash.Value(e.z);
        hash.Value(e.yaw);
        hash.Value(e.destX);
        hash.Value(e.destZ);
        hash.Value(e.speed);
        hash.Value(e.animTime);
    }
    return hash.Result();
}

CrowdMessageSource::CrowdMessageSource(const Settings& settings) : settings_(settings), rng_(settings.seed) {}

void CrowdMessageSource::Spawn(std::uint64_t tick, std::vector<CrowdMessage>& out, std::vector<std::uint32_t>* group) {
    CrowdMessage m;
    m.kind = CrowdMessage::Kind::Spawn;
    m.tick = tick;
    m.entity = nextId_++;
    m.x = Unit(rng_) * settings_.area;
    m.z = Unit(rng_) * settings_.area;
    out.push_back(m);
    if (group != nullptr) {
        group->push_back(m.entity);
    } else {
        alive_.push_back(m.entity);
    }
}

void CrowdMessageSource::Generate(std::uint64_t tick, std::vector<CrowdMessage>& out) {
    if (tick == 1) {
        for (std::uint32_t i = 0; i < settings_.population; ++i) Spawn(tick, out, nullptr);
    }
    // A trickle: two residents are sent somewhere; every 10 ticks one leaves and one arrives.
    for (int i = 0; i < 2 && !alive_.empty(); ++i) {
        CrowdMessage m;
        m.kind = CrowdMessage::Kind::MoveTo;
        m.tick = tick;
        m.entity = alive_[rng_() % alive_.size()];
        m.x = Unit(rng_) * settings_.area;
        m.z = Unit(rng_) * settings_.area;
        out.push_back(m);
    }
    if (tick % 10 == 0 && !alive_.empty()) {
        const std::size_t index = rng_() % alive_.size();
        out.push_back({CrowdMessage::Kind::Despawn, alive_[index], tick, 0, 0});
        alive_[index] = alive_.back();
        alive_.pop_back();
        Spawn(tick, out, nullptr);
    }
    // Bursts: a crowd arrives at once (a teleport into town), and leaves later.
    const bool scheduled = settings_.burstEveryTicks != 0 && tick % settings_.burstEveryTicks == 0;
    if (scheduled || burstRequested_) {
        burstRequested_ = false;
        std::vector<std::uint32_t> group;
        for (std::uint32_t i = 0; i < settings_.burstSize; ++i) Spawn(tick, out, &group);
        leaving_.push_back({tick + settings_.burstLifeTicks, std::move(group)});
    }
    for (auto it = leaving_.begin(); it != leaving_.end();) {
        if (it->first == tick) {
            for (std::uint32_t id : it->second) out.push_back({CrowdMessage::Kind::Despawn, id, tick, 0, 0});
            it = leaving_.erase(it);
        } else {
            ++it;
        }
    }
    out.push_back({CrowdMessage::Kind::TickEnd, 0, tick, 0, 0});
}

} // namespace sr::game
