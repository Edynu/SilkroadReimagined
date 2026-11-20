#include "sr/server/SummonEvents.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "sr/core/Log.h"
#include "sr/game/QuestItems.h"

namespace sr::server {

namespace proto = game::proto;

SummonEvents::SummonEvents(SummonHost& host, std::uint32_t seed) : host_(host), random_(seed) {}

proto::WorldPosition SummonEvents::Around(const proto::WorldPosition& at, float near, float far) {
    // A bearing over the whole turn and a distance between the two (rand / 32767 each).
    const float angle = std::uniform_real_distribution<float>(0.0f, 2.0f * std::numbers::pi_v<float>)(random_);
    const float distance = near + std::uniform_real_distribution<float>(0.0f, 1.0f)(random_) * (far - near);
    const proto::FlatPosition centre = proto::ToFlat(at);
    proto::WorldPosition p = proto::FromFlat({centre.x + distance * std::sin(angle), centre.z + distance * std::cos(angle)}, at.y);
    p.layer = at.layer;
    return p;
}

// The handler's checks in its order, each saying its words and returning 0.
bool SummonEvents::Refused(std::uint32_t entity, const SummonHost::User& u, bool pandora) {
    if (u.town) {
        host_.SummonSay(entity, std::string(pandora ? game::kPandoraInTown : game::kSummonInTown));
        return true;
    }
    if (u.level < game::kSummonEventLevel) {
        host_.SummonSay(entity, std::string(game::kSummonLevelLow));
        return true;
    }
    if (u.arena) {
        host_.SummonSay(entity, std::string(game::kSummonInArena));
        return true;
    }
    if (u.level < game::kSummonLowLevel) { // never reached by these two (30 first); kept as the handler has it
        host_.SummonSay(entity, std::string(game::kSummonUnderTen));
        return true;
    }
    return false;
}

bool SummonEvents::UseItem(std::uint32_t entity, const std::string& item, Clock::time_point now) {
    const bool guild = item == game::kGuildSupportItem, pandora = item == game::kPandoraItem;
    if (!guild && !pandora) return false; // the events' filters take their own item alone
    const auto u = host_.SummonUser(entity);
    if (!u || Refused(entity, *u, pandora)) return false;
    if (guild) GuildSupport(entity, *u);
    else Pandora(entity, *u, now);
    return true;
}

// QEV_GUILD_SUPPORT (its spawn): in a party as many as its members (the words _L),
// alone one (0729_03); each the level's monster, 20..100 away.
void SummonEvents::GuildSupport(std::uint32_t entity, const SummonHost::User& u) {
    const int count = u.party > 0 ? u.party : 1;
    host_.SummonSay(entity, std::string(u.party > 0 ? game::kSummonParty : game::kSummonOne));
    const std::string_view monster = game::GuildSupportMonster(u.level);
    int made = 0;
    for (int i = 0; i < count && !monster.empty(); ++i) {
        if (host_.SummonSpawn(monster, u.position, Around(u.position, game::kSummonNear, game::kSummonFar), 0.0f, false)) ++made;
    }
    core::LogInfo("summons: {} reads the party summon scroll: {} x{} ({} asked)", u.name, monster, made, count);
}

// QEV_PANDORA_BOX_100LV: the box by the user (within 20), the words _M, its daemon.
void SummonEvents::Pandora(std::uint32_t entity, const SummonHost::User& u, Clock::time_point now) {
    host_.SummonSay(entity, std::string(game::kPandoraOpened));
    const auto box = host_.SummonSpawn(game::kPandoraBox, u.position, u.position, 20.0f, true);
    if (!box) {
        core::LogInfo("summons: {} opens a Pandora box: no room for it", u.name);
        return; // the original's spawn failed: the daemon is never made, the item still used
    }
    Box b;
    b.owner = entity;
    b.box = *box;
    b.place = u.position;
    b.monsters = game::PandoraMonsters(u.level);
    b.next = now + std::chrono::seconds(game::kPandoraPeriodSeconds);
    boxes_.push_back(std::move(b));
    core::LogInfo("summons: {} opens a Pandora box (level {}: {} .. {})", u.name, u.level, boxes_.back().monsters.empty() ? "" : boxes_.back().monsters.front(),
                  boxes_.back().monsters.empty() ? "" : boxes_.back().monsters.back());
}

void SummonEvents::End(Box& b) {
    for (const std::size_t n : b.spawned) host_.SummonRemove(n);
    host_.SummonRemove(b.box);
    b.spawned.clear();
}

// The daemon's runs (the spawner's run): the first sets up, the
// last (240th) ends it; between, with none of its monsters standing, a wave of four. Its user gone or its box dead, it ends.
void SummonEvents::Tick(Clock::time_point now) {
    for (Box& b : boxes_) {
        while (b.runs < game::kPandoraRuns && now >= b.next) {
            b.next += std::chrono::seconds(game::kPandoraPeriodSeconds);
            ++b.runs;
            const auto user = host_.SummonUser(b.owner);
            if (!user || host_.SummonStanding(b.box) == 0 || b.runs >= game::kPandoraRuns) {
                b.runs = game::kPandoraRuns;
                break;
            }
            if (b.runs == 1) continue; // the first run only binds the box
            std::erase_if(b.spawned, [&](std::size_t n) {
                if (host_.SummonStanding(n) > 0) return false;
                host_.SummonRemove(n);
                return true;
            });
            if (!b.spawned.empty() || b.monsters.empty()) continue; // its monsters still stand (the box's count)
            for (int i = 0; i < game::kPandoraWave; ++i) {
                // A random one of the ten (the spawner's random flag set).
                const std::string_view m = b.monsters[std::uniform_int_distribution<std::size_t>(0, b.monsters.size() - 1)(random_)];
                const proto::WorldPosition at = Around(b.place, game::kPandoraRing, game::kPandoraRing);
                if (const auto n = host_.SummonSpawn(m, b.place, at, 0.0f, true)) b.spawned.push_back(*n);
            }
            // Then the box casts its skill (the spawner bound to the box).
            host_.SummonCast(b.box, game::kPandoraCast);
            ++b.casts;
        }
    }
    std::erase_if(boxes_, [&](Box& b) {
        if (b.runs < game::kPandoraRuns) return false;
        End(b);
        core::LogInfo("summons: a Pandora box ends");
        return true;
    });
}

std::vector<SummonEvents::BoxView> SummonEvents::BoxViews() const {
    std::vector<BoxView> out;
    for (const Box& b : boxes_) out.push_back({b.owner, b.box, b.runs, b.casts, b.spawned});
    return out;
}

} // namespace sr::server
