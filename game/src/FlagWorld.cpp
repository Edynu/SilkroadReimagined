#include "sr/game/FlagWorld.h"

#include <algorithm>
#include <functional>
#include <ctime>

namespace sr::game {

namespace flag {

std::string_view MonsterOfGroup(int group) {
    switch (group) {
    case 0:
    case 1: return "MOB_EV_BEAR_25";
    case 2: return "MOB_EV_FOX_45";
    case 3: return "MOB_EV_RAVEN_60";
    case 4: return "MOB_EV_PENGUIN_75";
    case 5: return "MOB_EV_KANGAROO_85";
    default: return "MOB_EV_JINN_95";
    }
}

int GroupOfLevel(int level) {
    if (level <= 19) return 0;
    if (level <= 35) return 1;
    if (level <= 50) return 2;
    if (level <= 65) return 3;
    if (level <= 80) return 4;
    if (level <= 90) return 5;
    return 6;
}

int MinPlayersOfGroup(int group) { return group <= 2 ? 4 : 8; }

} // namespace flag

FlagCode RegistrationRefusal(const FlagEntrant& e) {
    if (e.maxLevel < flag::kMinLevel) return FlagCode::Level;
    if (!e.alive || e.interacting || e.jobSuit || e.freePvp || e.guildWar || !e.inTown || e.questBlock) return FlagCode::Conditions;
    return FlagCode::Done;
}

FlagPlan PlanMatches(std::vector<FlagEntry> entries, int minPlayers, int teamMost, const std::function<int()>& coin) {
    FlagPlan plan;
    std::stable_sort(entries.begin(), entries.end(), [](const FlagEntry& a, const FlagEntry& b) {
        return a.level != b.level ? a.level < b.level : a.order < b.order;
    });
    const std::size_t perMatch = std::size_t(std::max(1, teamMost) * 2);
    std::vector<std::vector<const FlagEntry*>> chunks;
    for (const FlagEntry& e : entries) {
        if (flag::GroupOfLevel(e.level) == 0) {
            plan.cancelled.push_back(e.character);
            continue;
        }
        if (chunks.empty() || chunks.back().size() == perMatch) chunks.emplace_back();
        chunks.back().push_back(&e);
    }
    // The last match of an odd count: its last (the highest) out.
    if (!chunks.empty() && chunks.back().size() % 2 != 0) {
        plan.cancelled.push_back(chunks.back().back()->character);
        chunks.back().pop_back();
        if (chunks.back().empty()) chunks.pop_back();
    }
    for (const auto& chunk : chunks) {
        int group = 0;
        for (const FlagEntry* e : chunk) group = std::max(group, flag::GroupOfLevel(e->level));
        const int fewest = minPlayers > 0 ? minPlayers : flag::MinPlayersOfGroup(group);
        if (int(chunk.size()) < fewest) {
            for (const FlagEntry* e : chunk) plan.cancelled.push_back(e->character);
            continue;
        }
        FlagMatchPlan m;
        m.group = group;
        // t, !t, !t, t, t, ...: the team flips after each one that matched the one before (SM).
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

// --- The flag ------------------------------------------------------------------------------------------------

void FlagMatch::Reset() {
    flapped_[0] = true;
    flapped_[1] = flapped_[2] = false;
    for (Action& a : actions_) a = {};
    carrier_ = 0;
    carrierTeam_ = 0;
    carrierSince_ = 0;
    dropped_ = false;
}

std::optional<FlagCode> FlagMatch::SelectPole(int pole, std::uint32_t actor, std::uint8_t team, std::int64_t now, std::vector<Event>& out) {
    if (pole < 0 || pole > 2 || actor == 0 || team > 1) return std::nullopt;
    for (const Action& a : actions_) {
        if (a.actor == actor) return std::nullopt; // one action at a time for him
    }
    Action& slot = actions_[pole];
    if (carrier_ == 0) {
        // A take: a pole holding the flag; the white one anyone's, a team's pole only the other team's.
        if (dropped_ || !flapped_[pole]) return std::nullopt;
        if (pole != 0 && flag::OwnPole(team) == pole) return std::nullopt;
        if (slot.actor != 0) return FlagCode::TakeBusy;
        slot = {actor, team, false, now + flag::kActionMs};
    } else {
        // A place: the carrier, at his own empty pole.
        if (actor != carrier_ || pole != flag::OwnPole(team) || flapped_[pole]) return std::nullopt;
        if (slot.actor != 0) return FlagCode::PlaceBusy;
        slot = {actor, team, true, now + flag::kActionMs};
    }
    out.push_back({Event::Kind::Started, actor, team, pole});
    return FlagCode::Done;
}

bool FlagMatch::PickUp(std::uint32_t actor, std::uint8_t team, std::int64_t now, std::vector<Event>& out) {
    if (!dropped_ || carrier_ != 0 || actor == 0) return false;
    dropped_ = false;
    carrier_ = actor;
    carrierTeam_ = team;
    carrierSince_ = now;
    out.push_back({Event::Kind::Carrier, actor, team, -1});
    return true;
}

void FlagMatch::Drop(std::vector<Event>& out) {
    out.push_back({Event::Kind::Dropped, carrier_, carrierTeam_, -1});
    carrier_ = 0;
    dropped_ = true;
    for (Action& a : actions_) a = {}; // every pole's slot reset
}

void FlagMatch::Gone(std::uint32_t actor, std::vector<Event>& out) {
    if (actor == 0) return;
    for (Action& a : actions_) {
        if (a.actor == actor) a = {};
    }
    if (carrier_ == actor) Drop(out);
}

void FlagMatch::Tick(std::int64_t now, std::vector<Event>& out) {
    for (int pole = 0; pole < 3; ++pole) {
        Action& a = actions_[pole];
        if (a.actor == 0 || now < a.until) continue;
        const Action done = a;
        a = {};
        if (!done.place) {
            if (!flapped_[pole] || carrier_ != 0) continue;
            flapped_[pole] = false;
            carrier_ = done.actor;
            carrierTeam_ = done.team;
            carrierSince_ = now;
            out.push_back({Event::Kind::Carrier, done.actor, done.team, pole});
        } else {
            if (carrier_ != done.actor || flapped_[pole]) continue;
            flapped_[pole] = true;
            carrier_ = 0;
            out.push_back({Event::Kind::Placed, done.actor, done.team, pole});
        }
    }
    if (carrier_ != 0 && now >= carrierSince_ + flag::kCarryMs) Drop(out);
}

std::optional<std::uint8_t> FlagMatch::Winner() const {
    if (flapped_[0]) return std::nullopt;
    if (flapped_[1]) return std::uint8_t(0);
    if (flapped_[2]) return std::uint8_t(1);
    return std::nullopt;
}

// --- The schedule ----------------------------------------------------------------------------------------------

FlagCycle FlagCycleFrom(std::int64_t open, const FlagSchedule& s) {
    FlagCycle c;
    c.open = open;
    c.start = open + s.registerSeconds;
    c.standby5 = std::max(open, c.start - 300);
    c.standby1 = std::max(open, c.start - 60);
    c.end = c.start + s.playSeconds;
    c.stop = c.end + s.winnersSeconds;
    return c;
}

std::optional<FlagCycle> NextFlagCycle(std::int64_t now, const FlagSchedule& s) {
    if (s.cycle) {
        const std::int64_t period = std::max<std::int64_t>(1, s.registerSeconds + s.playSeconds + s.winnersSeconds);
        const std::int64_t base = now - ((now % period) + period) % period;
        return FlagCycleFrom(base, s);
    }
    if (s.times.empty()) return std::nullopt;
    // The plays of yesterday, today and tomorrow in the server's local time: the first whose stop is still ahead.
    const std::time_t t = std::time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    std::optional<FlagCycle> best;
    for (int day = -1; day <= 1; ++day) {
        for (const int minutes : s.times) {
            std::tm when = local;
            when.tm_mday += day;
            when.tm_hour = minutes / 60;
            when.tm_min = minutes % 60;
            when.tm_sec = 0;
            when.tm_isdst = -1;
            const std::int64_t start = std::int64_t(std::mktime(&when));
            const FlagCycle c = FlagCycleFrom(start - s.registerSeconds, s);
            if (c.stop <= now) continue;
            if (!best || c.open < best->open) best = c;
        }
    }
    return best;
}

// --- So-Ok's swap ------------------------------------------------------------------------------------------------

namespace {

constexpr SwapPrize kIcePrizes[] = {
    {"ITEM_ETC_ARCHEMY_MAGICSTONE_LUCK_09", 1, 1.0f},          {"ITEM_ETC_ARCHEMY_MAGICSTONE_LUCK_08", 1, 5.0f},
    {"ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B", 1, 1.0f},   {"ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_SHIELD_B", 1, 5.0f},
    {"ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ARMOR_B", 1, 3.0f},    {"ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ACCESSARY_B", 1, 5.0f},
    {"ITEM_ETC_E051123_EVATION_SCROLL", 1, 7.0f},              {"ITEM_ETC_E051123_HIT_SCROLL", 1, 7.0f},
    {"ITEM_ETC_E051123_SPEED_UP_SCROLL", 1, 7.0f},             {"ITEM_ETC_E051123_AGILITY_SCROLL", 1, 7.0f},
    {"ITEM_ETC_E051123_HP500_SCROLL", 1, 10.0f},               {"ITEM_ETC_E051123_MP500_SCROLL", 1, 10.0f},
    {"ITEM_ETC_HP_SPOTION_01", 10, 13.0f},                     {"ITEM_ETC_MP_SPOTION_01", 10, 13.0f},
    {"ITEM_ETC_ALL_SPOTION_01", 10, 6.0f},
};
constexpr SwapPrize kTicketPrizes[] = {
    {"ITEM_ETC_ARCHEMY_MAGICSTONE_LUCK_09", 1, 0.3f},          {"ITEM_ETC_ARCHEMY_MAGICSTONE_LUCK_08", 1, 0.7f},
    {"ITEM_ETC_ARCHEMY_MAGICSTONE_LUCK_07", 1, 5.0f},          {"ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_WEAPON_B", 1, 1.0f},
    {"ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_SHIELD_B", 1, 5.0f},   {"ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ARMOR_B", 1, 3.0f},
    {"ITEM_ETC_ARCHEMY_REINFORCE_RECIPE_ACCESSARY_B", 1, 5.0f}, {"ITEM_ETC_E051123_EVATION_SCROLL", 1, 7.0f},
    {"ITEM_ETC_E051123_HIT_SCROLL", 1, 7.0f},                  {"ITEM_ETC_E051123_SPEED_UP_SCROLL", 1, 7.0f},
    {"ITEM_ETC_E051123_AGILITY_SCROLL", 1, 7.0f},              {"ITEM_ETC_E051123_HP500_SCROLL", 1, 10.0f},
    {"ITEM_ETC_E051123_MP500_SCROLL", 1, 10.0f},               {"ITEM_ETC_HP_SPOTION_01", 10, 13.0f},
    {"ITEM_ETC_MP_SPOTION_01", 10, 13.0f},                     {"ITEM_ETC_ALL_SPOTION_01", 10, 6.0f},
};

const SwapQuest kIce{10145, "QEV_CH_EVENT_KISAENG_081201", "SN_QEV_CH_EVENT_KISAENG_081201", flag::kTrophy, kIcePrizes};
const SwapQuest kTickets{10077, "QEV_CH_EVENT_KISAENG_070919", "SN_QEV_CH_EVENT_KISAENG_070919", "ITEM_ETC_E070919_TROPHY", kTicketPrizes};

} // namespace

const SwapQuest& SwapIceTrophies() { return kIce; }
const SwapQuest& SwapTickets() { return kTickets; }
const SwapQuest* SwapQuestById(std::uint16_t id) {
    if (id == kIce.id) return &kIce;
    if (id == kTickets.id) return &kTickets;
    return nullptr;
}

std::size_t DrawPrize(std::span<const SwapPrize> prizes, float r) {
    float sum = 0;
    for (std::size_t i = 0; i < prizes.size(); ++i) {
        sum += prizes[i].weight;
        if (sum >= r) return i;
    }
    return 0;
}

} // namespace sr::game
