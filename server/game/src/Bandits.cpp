#include "sr/server/Bandits.h"

#include <cmath>
#include <numbers>

#include "sr/core/Log.h"

namespace sr::server {

namespace proto = game::proto;

Bandits::Bandits(BanditHost& host, game::BanditTable table, std::uint32_t seed) : host_(host), table_(std::move(table)), random_(seed) {}

void Bandits::SetInterval(double minSeconds, double spanSeconds) {
    minSeconds_ = std::max(0.0, minSeconds);
    spanSeconds_ = std::max(0.0, spanSeconds);
}

Bandits::Clock::time_point Bandits::Next(Clock::time_point now) {
    // 60,000 + rand / 32767 x 60,000 ms.
    const double r = std::uniform_real_distribution<double>(0.0, 1.0)(random_);
    return now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(minSeconds_ + r * spanSeconds_));
}

void Bandits::Register(std::uint32_t player, Clock::time_point now) {
    if (caravans_.contains(player)) return; // one per player
    const auto c = host_.BanditCaravan(player);
    if (!c) return;
    caravans_[player] = Next(now);
    core::LogInfo("bandits: {}'s caravan registered ({} goods)", c->name, c->goods);
}

void Bandits::Tick(Clock::time_point now) {
    for (auto it = caravans_.begin(); it != caravans_.end();) {
        if (now < it->second) {
            ++it;
            continue;
        }
        // Dropped only here: the player, its transport or its goods gone.
        const auto c = host_.BanditCaravan(it->first);
        if (!c) {
            it = caravans_.erase(it);
            continue;
        }
        it->second = Next(now);
        // In a town the wave is skipped and the caravan kept.
        if (!host_.BanditTown(c->at.region)) Wave(it->first);
        ++it;
    }
}

int Bandits::Wave(std::uint32_t player) {
    const auto c = host_.BanditCaravan(player);
    if (!c) return 0;
    const game::JobRules& rules = host_.BanditRules();
    const std::uint32_t value =
        game::TradeValue(rules, c->goods, c->valueLevel, game::CartCapacity(game::CartClassOf(c->transportCodename)));
    const int stars = game::TradeStars(rules, value);
    const auto percent = [&] { return std::uniform_int_distribution<int>(0, 99)(random_); };
    const int count = game::BanditCount(rules, value, c->bargain, percent() < 50, percent() < 2);
    // One tactics row for the wave: k 3 at 50 %, else 0..3.
    const int k = percent() < 50 ? 3 : std::uniform_int_distribution<int>(0, 3)(random_);
    const bool trader = c->jobState == std::uint8_t(game::JobType::Trader);
    const game::BanditKind kind = trader ? game::BanditKind::Thief : game::BanditKind::Hunter;
    const int zone = host_.BanditZone(c->playerRegion);
    int made = 0;
    for (int i = 0; i < count; ++i) {
        const int level = game::BanditLevel(c->level, stars, std::uniform_int_distribution<int>(0, 3)(random_), percent() < 2);
        const std::uint32_t ref = table_.Pick(zone, kind, level, std::uniform_int_distribution<std::uint32_t>()(random_));
        if (ref == 0) continue; // no list here: no bandit
        MonsterNest nest;
        nest.refObjId = ref;
        const float distance = game::BanditSpawnDistance(std::uniform_int_distribution<int>(0, 100)(random_),
                                                         std::uniform_real_distribution<float>(0.0f, 1.0f)(random_));
        const float angle = std::uniform_real_distribution<float>(0.0f, 2.0f * std::numbers::pi_v<float>)(random_);
        const proto::FlatPosition centre = proto::ToFlat(c->at);
        nest.position = proto::FromFlat({centre.x + distance * std::sin(angle), centre.z + distance * std::cos(angle)}, c->at.y);
        nest.position.layer = c->at.layer; // in the player's world
        // On ground the transport reaches in a straight line (ours: the original tries the point on its navigation and
        // loses a bandit there is none for; our navigation's objects have floors inside walls, the user's
        // report 2026-11-14: a thief came up among Jangan's wall's pillars).
        const auto ground = host_.BanditGround(c->at, nest.position);
        if (!ground) continue;
        nest.position = *ground;
        nest.radius = game::kBanditTrace / 1.5f; // led away up to its trace
        nest.spawnRadius = 0.0f;
        nest.maxCount = 1;
        nest.aggressive = true;
        nest.sight = game::kBanditSight;
        nest.respawn = false;
        nest.temporary = true;
        nest.rarity = percent() < game::kBanditChampionPercent ? 1 : 0; // a champion
        nest.bandit = std::uint8_t(kind);
        nest.changeTarget = game::BanditChangeTarget(k);
        nest.leaveMs = game::kBanditLeaveMs;
        if (host_.BanditSpawn(nest)) ++made;
    }
    core::LogInfo("bandits: a wave of {} ({} asked, row {}) at {}'s caravan, {} stars, value {}", made, count, game::BanditTacticsRow(trader, k), c->name,
                  stars, value);
    return made;
}

} // namespace sr::server
