#include "sr/game/Arena.h"

#include <algorithm>

namespace sr::game {

namespace arena {

int CoinsFor(Mode mode, std::uint8_t outcome) {
    const bool big = mode == Mode::Guild || mode == Mode::Job;
    if (outcome == 1) return big ? 7 : 5;
    return big ? 2 : 1;
}

std::string_view ModeName(Mode m) {
    switch (m) {
    case Mode::Random: return "UIIT_STT_ARENA_RANDOMMATCH";
    case Mode::Party: return "UIIT_STT_ARENA_PARTY";
    case Mode::Guild: return "UIIT_STT_ARENA_GUILD";
    case Mode::Job: return "UIIT_STT_ARENA_JOB";
    default: return "";
    }
}

} // namespace arena

ArenaCode RegistrationRefusal(arena::Mode mode, const ArenaEntrant& e) {
    using M = arena::Mode;
    if (e.registered) return ArenaCode::Already;
    if (e.maxLevel < arena::kMinLevel) return ArenaCode::Level;
    if (!e.open) return ArenaCode::NotNow;
    if (mode == M::Party && !e.partyLeader) return ArenaCode::NotLeader;
    if (mode == M::Guild && !e.guildMaster) return ArenaCode::NotMaster;
    const FlagEntrant& c = e.conditions;
    if (!c.alive || c.interacting || c.freePvp || c.guildWar || !c.inTown || c.questBlock || e.riding) return ArenaCode::Conditions;
    const bool suit = e.jobState >= 1 && e.jobState <= 3;
    if ((mode == M::Job) != suit) return ArenaCode::JobSuit;
    return ArenaCode::Done;
}

ArenaPlan PlanArena(arena::Mode mode, std::vector<ArenaUnit> units, int minPlayers, const std::function<int()>& coin) {
    using M = arena::Mode;
    ArenaPlan plan;
    if (mode == M::Random) {
        // The shard manager's random matches: as the flag world's, the group by the average level.
        std::vector<FlagEntry> entries;
        for (const ArenaUnit& u : units)
            if (!u.characters.empty()) entries.push_back({u.characters.front(), u.level, u.order});
        std::stable_sort(entries.begin(), entries.end(), [](const FlagEntry& a, const FlagEntry& b) {
            return a.level != b.level ? a.level < b.level : a.order < b.order;
        });
        std::vector<std::vector<const FlagEntry*>> chunks;
        for (const FlagEntry& e : entries) {
            if (flag::GroupOfLevel(e.level) == 0) {
                plan.cancelled.push_back(e.character);
                continue;
            }
            if (chunks.empty() || chunks.back().size() == 16) chunks.emplace_back();
            chunks.back().push_back(&e);
        }
        if (!chunks.empty() && chunks.back().size() % 2 != 0) {
            plan.cancelled.push_back(chunks.back().back()->character);
            chunks.back().pop_back();
            if (chunks.back().empty()) chunks.pop_back();
        }
        for (const auto& chunk : chunks) {
            int total = 0;
            for (const FlagEntry* e : chunk) total += e->level;
            const int group = flag::GroupOfLevel(total / int(chunk.size()));
            const int fewest = minPlayers > 0 ? minPlayers : flag::MinPlayersOfGroup(group);
            if (int(chunk.size()) < fewest) {
                for (const FlagEntry* e : chunk) plan.cancelled.push_back(e->character);
                continue;
            }
            ArenaMatchPlan m;
            m.group = group;
            m.size = 8;
            std::uint8_t team = std::uint8_t(coin ? (coin() & 1) : 0), previous = team;
            for (const FlagEntry* e : chunk) {
                m.players.emplace_back(e->character, team);
                if (team == previous) team = previous == 0 ? 1 : 0;
                else previous = team;
            }
            plan.matches.push_back(std::move(m));
        }
        return plan;
    }
    const auto coinOf = [&] { return std::uint8_t(coin ? (coin() & 1) : 0); };
    if (mode == M::Party || mode == M::Guild) {
        std::vector<ArenaUnit> list;
        for (ArenaUnit& u : units)
            if (!u.characters.empty()) list.push_back(std::move(u));
        const auto leaderOf = [](const ArenaUnit& u) { return u.leaderLevel > 0 ? u.leaderLevel : u.level; };
        std::stable_sort(list.begin(), list.end(), [&](const ArenaUnit& a, const ArenaUnit& b) {
            return leaderOf(a) != leaderOf(b) ? leaderOf(a) < leaderOf(b) : a.order < b.order;
        });
        std::size_t i = 0;
        for (; i + 1 < list.size(); i += 2) {
            const ArenaUnit& a = list[i];
            const ArenaUnit& b = list[i + 1];
            ArenaMatchPlan m;
            m.size = mode == M::Guild && a.characters.size() + b.characters.size() > 16 ? 16 : 8;
            const std::uint8_t first = coinOf();
            int sum = 0, count = 0;
            for (int t = 0; t < 2; ++t) {
                const ArenaUnit& u = t == 0 ? a : b;
                const std::uint8_t team = t == 0 ? first : std::uint8_t(first ^ 1);
                for (std::size_t k = 0; k < u.characters.size() && k < m.size; ++k) m.players.emplace_back(u.characters[k], team);
                sum += u.levelSum > 0 ? u.levelSum : u.level * int(u.characters.size());
                count += int(u.characters.size());
            }
            m.group = flag::GroupOfLevel(count > 0 ? sum / count : 1);
            plan.matches.push_back(std::move(m));
        }
        if (i < list.size())
            for (std::uint32_t c : list[i].characters) plan.cancelled.push_back(c);
        return plan;
    }
    if (mode == M::Job) {
        // Traders and hunters (side 0), thieves (side 1), each by level, the lowest first.
        std::vector<const ArenaUnit*> sides[2];
        for (const ArenaUnit& u : units) {
            if (u.characters.empty()) continue;
            if (flag::GroupOfLevel(u.level) == 0) {
                plan.cancelled.push_back(u.characters.front());
                continue;
            }
            sides[u.job == 2 ? 1 : 0].push_back(&u);
        }
        for (auto& side : sides) {
            std::stable_sort(side.begin(), side.end(), [](const ArenaUnit* a, const ArenaUnit* b) {
                return a->level != b->level ? a->level < b->level : a->order < b->order;
            });
        }
        std::size_t used[2] = {0, 0};
        int n = 32;
        while (n >= 2) {
            const std::size_t left0 = sides[0].size() - used[0], left1 = sides[1].size() - used[1];
            if (n == 2 && left0 > 0 && sides[0][used[0]]->level > 50) break;
            if (left0 < std::size_t(n) || left1 < std::size_t(n)) {
                n /= 2;
                continue;
            }
            ArenaMatchPlan m;
            m.size = std::uint8_t(n >= 32 ? 32 : n >= 16 ? 16 : 8);
            m.group = flag::GroupOfLevel(std::max(sides[0][used[0]]->level, sides[1][used[1]]->level));
            const std::uint8_t first = coinOf();
            for (int k = 0; k < n; ++k) {
                m.players.emplace_back(sides[0][used[0] + std::size_t(k)]->characters.front(), first);
                m.players.emplace_back(sides[1][used[1] + std::size_t(k)]->characters.front(), std::uint8_t(first ^ 1));
            }
            used[0] += std::size_t(n);
            used[1] += std::size_t(n);
            plan.matches.push_back(std::move(m));
        }
        for (int t = 0; t < 2; ++t)
            for (std::size_t k = used[t]; k < sides[t].size(); ++k) plan.cancelled.push_back(sides[t][k]->characters.front());
        return plan;
    }
    return plan;
}

// --- The flag kind's play ---------------------------------------------------------------------------------------------

void ArenaFlags::Reset(const std::vector<int>& colours) {
    poles_.clear();
    for (const int c : colours) poles_.push_back({c, c == 0, {}});
    carriers_.clear();
    dropped_.clear();
    nextFlag_ = 1;
}

std::optional<FlagCode> ArenaFlags::SelectPole(int pole, std::uint32_t actor, std::uint8_t team, std::int64_t now, std::vector<Event>& out) {
    if (pole < 0 || std::size_t(pole) >= poles_.size() || actor == 0 || team > 1) return std::nullopt;
    for (const Pole& p : poles_) {
        if (p.action.actor == actor) return std::nullopt; // one action at a time for him
    }
    Pole& p = poles_[std::size_t(pole)];
    const std::uint8_t owner = PoleTeam(p.colour);
    if (!Carrying(actor)) {
        // A take: a pole showing a flag that is not his team's (a white one, or the other team's).
        if (!p.flapped || owner == team) return std::nullopt;
        if (p.action.actor != 0) return FlagCode::TakeBusy;
        p.action = {actor, team, false, now + flag::kActionMs};
    } else {
        // A place: the carrier, at an empty pole of his team.
        if (p.flapped || owner != team) return std::nullopt;
        if (p.action.actor != 0) return FlagCode::PlaceBusy;
        p.action = {actor, team, true, now + flag::kActionMs};
    }
    out.push_back({Event::Kind::Started, actor, team, pole, 0});
    return FlagCode::Done;
}

bool ArenaFlags::PickUp(std::uint32_t flag, std::uint32_t actor, std::uint8_t team, std::int64_t now, std::vector<Event>& out) {
    if (actor == 0 || !dropped_.contains(flag) || Carrying(actor)) return false;
    dropped_.erase(flag);
    carriers_[actor] = {team, now};
    out.push_back({Event::Kind::Carrier, actor, team, -1, flag});
    return true;
}

void ArenaFlags::Drop(std::uint32_t actor, std::vector<Event>& out) {
    const auto it = carriers_.find(actor);
    if (it == carriers_.end()) return;
    const std::uint8_t team = it->second.team;
    carriers_.erase(it);
    for (Pole& p : poles_) {
        if (p.action.actor == actor) p.action = {}; // his action on a pole, if any
    }
    const std::uint32_t flag = nextFlag_++;
    dropped_.insert(flag);
    out.push_back({Event::Kind::Dropped, actor, team, -1, flag});
}

void ArenaFlags::Gone(std::uint32_t actor, std::vector<Event>& out) {
    if (actor == 0) return;
    for (Pole& p : poles_) {
        if (p.action.actor == actor) p.action = {};
    }
    Drop(actor, out);
}

void ArenaFlags::Tick(std::int64_t now, std::vector<Event>& out) {
    for (std::size_t i = 0; i < poles_.size(); ++i) {
        Pole& p = poles_[i];
        if (p.action.actor == 0 || now < p.action.until) continue;
        const Action done = p.action;
        p.action = {};
        if (!done.place) {
            if (!p.flapped || Carrying(done.actor)) continue;
            p.flapped = false;
            carriers_[done.actor] = {done.team, now};
            out.push_back({Event::Kind::Carrier, done.actor, done.team, int(i), 0});
        } else {
            if (!Carrying(done.actor) || p.flapped) continue;
            p.flapped = true;
            carriers_.erase(done.actor);
            out.push_back({Event::Kind::Placed, done.actor, done.team, int(i), 0});
        }
    }
    std::vector<std::uint32_t> late;
    for (const auto& [actor, c] : carriers_) {
        if (now >= c.since + flag::kCarryMs) late.push_back(actor);
    }
    for (const std::uint32_t actor : late) Drop(actor, out);
}

std::vector<std::uint32_t> ArenaFlags::Carriers() const {
    std::vector<std::uint32_t> out;
    for (const auto& [actor, c] : carriers_) out.push_back(actor);
    return out;
}

std::uint32_t ArenaFlags::FlagsOn(std::uint8_t team) const {
    std::uint32_t n = 0;
    for (const Pole& p : poles_) n += p.flapped && PoleTeam(p.colour) == team ? 1u : 0u;
    return n;
}

} // namespace sr::game
