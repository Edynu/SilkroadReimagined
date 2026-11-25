#include "sr/game/States.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "sr/assets/GameData.h"
#include "sr/game/Combat.h"

namespace sr::game {

namespace {

// Each state's tag, the key its roll keeps its history under (the original's n << 24, no skill id),
// its resistance bucket (-1 an element) and, for an element, its time a level in ms. In the original's roll order.
struct Kind {
    StateKind kind;
    const char* tag;
    int key;
    int bucket;
    int msPerLevel;
};
constexpr Kind kKinds[] = {
    {StateKind::Freeze, "fz", 0x01, -1, 97},    {StateKind::Frostbite, "fb", 0x02, -1, 250}, {StateKind::Shock, "es", 0x03, -1, 500},
    {StateKind::Burn, "bu", 0x04, -1, 750},     {StateKind::Poison, "ps", 0x05, -1, 1000},   {StateKind::Zombie, "zb", 0x06, -1, 750},
    {StateKind::Sleep, "se", 0x07, 0, 0},       {StateKind::Root, "rt", 0x08, 1, 0},         {StateKind::Slow, "sl", 0x09, 2, 0},
    {StateKind::Fear, "fe", 0x0A, 3, 0},        {StateKind::Myopia, "my", 0x0B, 4, 0},       {StateKind::Bleed, "bl", 0x0C, 5, 0},
    {StateKind::Dark, "dn", 0x0E, 6, 0},        {StateKind::Stun, "st", 0x0F, 7, 0},         {StateKind::Disease, "ds", 0x11, 8, 0},
    {StateKind::Confusion, "ca", 0x12, 9, 0},   {StateKind::Impotent, "cssr", 0x13, 12, 0},  {StateKind::Division, "csit", 0x14, 13, 0},
    {StateKind::Decay, "cspd", 0x15, 10, 0},    {StateKind::Weaken, "csmd", 0x16, 11, 0},    {StateKind::Panic, "cshp", 0x17, 14, 0},
    {StateKind::Combustion, "csmp", 0x18, 15, 0}, {StateKind::TimeBomb, "tb", 0x19, 16, 0},
};

const Kind* KindOf(StateKind kind) {
    for (const Kind& k : kKinds) {
        if (k.kind == kind) return &k;
    }
    return nullptr;
}

// An element's place among the resistances: its bit's (freeze, frostbite, shock, burn, poison, zombie;
// the stats 0x1B..0x20 and 0x91..0x96).
int ElementIndex(StateKind kind) { return std::countr_zero(StateBit(kind)); }

constexpr int kTickMs = 2000; // burn, poison, panic, combustion; a bleed without its skill's 'puls'

// The burn's damage by its rate (the tag's third word): the original's table of 141 values, as its server holds it.
constexpr std::uint16_t kBurnRates[] = {
    0, 8, 9, 11, 12, 14, 15, 17, 19, 20, 22, 24, 26, 29, 31, 33,
    36, 39, 41, 44, 47, 50, 54, 57, 61, 64, 68, 72, 76, 81, 85, 90,
    95, 100, 106, 111, 117, 123, 129, 136, 142, 149, 157, 164, 172, 180, 189, 197,
    206, 216, 226, 236, 246, 257, 268, 280, 292, 305, 318, 331, 345, 359, 374, 389,
    405, 422, 439, 457, 475, 494, 513, 533, 554, 576, 598, 621, 645, 670, 695, 721,
    749, 777, 806, 835, 866, 898, 931, 965, 1000, 1036, 1074, 1112, 1152, 1193, 1236, 1280,
    1325, 1371, 1419, 1469, 1520, 1573, 1627, 1683, 1741, 1800, 1862, 1925, 1991, 2058, 2127, 2199,
    2272, 2348, 2427, 2507, 2590, 2676, 2764, 2855, 2949, 3045, 3145, 3247, 3352, 3461, 3573, 3688,
    3806, 3928, 4054, 4183, 4316, 4454, 4595, 4740, 4890, 5044, 5202, 5365, 5533,
};
static_assert(std::size(kBurnRates) == 141);

} // namespace

std::uint32_t StateBit(StateKind kind) {
    switch (kind) {
    case StateKind::Burn: return 0x8;
    case StateKind::Shock: return 0x4;
    default: return 1u << (std::uint8_t(kind) & 31);
    }
}

const char* StateName(StateKind kind) {
    static constexpr const char* kNames[] = {"freeze", "frostbite", "burn",   "shock",    "poison",   "zombie",   "sleep",      "root",
                                             "slow",   "fear",      "myopia", "bleed",    "stone",    "darkness", "stun",       "disease",
                                             "confusion", "decay",  "weaken", "impotent", "division", "panic",    "combustion", "?",
                                             "time bomb"};
    const std::size_t i = std::uint8_t(kind);
    return i < std::size(kNames) ? kNames[i] : "?";
}

int ResistBucket(StateKind kind) {
    const Kind* k = KindOf(kind);
    return k != nullptr ? k->bucket : -1;
}

std::vector<StateGift> StateGiftsOf(const assets::SkillRow& skill) {
    std::vector<StateGift> out;
    for (const Kind& k : kKinds) {
        const assets::SkillParam p = assets::FindSkillParam(skill, assets::SkillTag(k.tag));
        if (!p) continue;
        StateGift g;
        g.kind = k.kind;
        for (std::size_t i = 0; i < g.words.size(); ++i) g.words[i] = p.Int(i);
        out.push_back(g);
    }
    return out;
}

CureSpec CureOf(const assets::SkillRow& skill) {
    CureSpec c;
    if (const assets::SkillParam t = assets::FindSkillParam(skill, assets::SkillTag("curt"))) {
        c.elementMask = t[0];
        c.elementLevel = t.Int(1);
    }
    if (const assets::SkillParam t = assets::FindSkillParam(skill, assets::SkillTag("curl"))) {
        c.curseMask = t[0];
        c.curseChance = t.Int(1);
        c.curseLevel = t.Int(2);
    }
    if (const assets::SkillParam t = assets::FindSkillParam(skill, assets::SkillTag("rcur"))) c.most = std::max(0, t.Int(0));
    return c;
}

StateOdds OddsOf(const StateGift& g, const StateDefence& target, int susceptibility) {
    StateOdds o;
    const Kind* k = KindOf(g.kind);
    if (k == nullptr) return o;
    if (IsElement(g.kind)) {
        // The chance (the tag's second word and the susceptibility); the target's resistance cuts the
        // power into the level, less its grade; the time a level.
        o.chance = g.words[1] + susceptibility;
        const int e = ElementIndex(g.kind);
        const int resist = target.elementResist[std::size_t(e)];
        if (resist >= 100) return o;
        const int level = int(std::trunc(double(100 - resist) / 100.0 * double(std::uint32_t(g.words[0])))) + g.levelBonus -
                          target.elementGrade[std::size_t(e)];
        if (level <= 0) return o;
        o.level = level;
        o.durationMs = std::uint32_t(level) * std::uint32_t(k->msPerLevel);
        return o;
    }
    // The level word reaches targets up to ten times it; over that each level costs 2.5 % of the time
    // (never under half) and 5 % of the chance (never under a tenth). Then the target's resistance: a
    // flat amount (halved for each level the state is above its grade), the susceptibility, a percent.
    const int level = g.words[2];
    const int gap = std::max(0, target.level - 10 * (level + (susceptibility != 0 ? 1 : 0)));
    const double time = double(std::uint32_t(g.words[0]));
    const int chanceWord = g.words[1];
    std::int64_t duration = std::int64_t(std::trunc(time * (1.0 - double(gap) * 2.5 / 100.0)));
    duration = std::max(duration, std::int64_t(std::trunc(time * 0.5)));
    int chance = int(std::trunc(double(float(chanceWord)) * (1.0 - double(gap * 5) / 100.0)));
    chance = std::max(chance, int(std::trunc(double(chanceWord) * double(0.1f))));
    const std::size_t b = std::size_t(k->bucket);
    int flat = target.flat[b];
    if (target.grade[b] != 0) {
        for (int above = level - target.grade[b]; above > 0; --above) flat /= 2;
    }
    chance = chance - flat + susceptibility;
    if (target.percent[b] != 0 && chance > 0) chance = int(std::trunc((1.0 - double(target.percent[b]) / 100.0) * double(chance)));
    o.chance = chance;
    o.level = level;
    // A time bomb's fuse is not its first word's time but one of its bytes, at the roll (RollStates).
    o.durationMs = g.kind == StateKind::TimeBomb ? 0u : std::uint32_t(std::max<std::int64_t>(0, duration));
    return o;
}

std::vector<ActiveState> RollStates(std::span<const StateGift> gifts, const StateDefence& target, int susceptibility, ChanceRolls& rolls,
                                    std::mt19937& random, std::uint32_t source, std::uint32_t skill, int skillPulseMs,
                                    ActiveState::Clock::time_point now) {
    std::vector<ActiveState> out;
    for (const StateGift& g : gifts) {
        const Kind* k = KindOf(g.kind);
        if (k == nullptr) continue;
        // An element rolls before its resistance is looked at (the history moves on an immune target too).
        const StateOdds odds = OddsOf(g, target, susceptibility);
        if (!rolls.Roll(odds.chance, char(k->key), 0, random) || odds.level <= 0) continue;
        ActiveState s;
        s.kind = g.kind;
        s.source = source;
        s.skill = skill;
        s.started = now;
        s.nextTick = now;
        s.level = odds.level;
        s.durationMs = odds.durationMs;
        switch (g.kind) {
        case StateKind::Shock: s.value = g.words[2]; break; // the parry cut, percent
        case StateKind::Burn:
            s.value = BurnRate(g.words[2]);
            s.tickMs = kTickMs;
            break;
        case StateKind::Poison:
            s.value = g.words[2] + g.valueBonus;
            s.tickMs = kTickMs;
            break;
        case StateKind::Myopia:
        case StateKind::Dark:
        case StateKind::Disease:
        case StateKind::Decay:
        case StateKind::Weaken:
        case StateKind::Impotent:
        case StateKind::Division: s.value = g.words[3]; break;
        case StateKind::Bleed:
            s.value = g.words[3];  // a tick's damage
            s.value2 = g.words[4]; // the defences' cut, percent
            s.tickMs = skillPulseMs > 0 ? skillPulseMs : kTickMs;
            break;
        case StateKind::Panic:
        case StateKind::Combustion:
            s.value = g.words[3];  // the most HP / MP's cut, percent
            s.value2 = g.words[4]; // the recovery's cut, percent
            s.value3 = g.words[5]; // a tick's drain, percent
            s.tickMs = s.value3 > 0 ? kTickMs : 0;
            break;
        case StateKind::TimeBomb: {
            // The fuse: one of the first word's non-zero bytes at random, in seconds, no level cut
            // (0x050A0F00: 15, 10 or 5 s); the burst's damage the fourth word.
            std::array<std::uint32_t, 4> fuses{};
            std::size_t n = 0;
            for (int i = 0; i < 4; ++i) {
                if (const std::uint32_t b = (std::uint32_t(g.words[0]) >> (8 * i)) & 0xFF; b != 0) fuses[n++] = b;
            }
            if (n == 0) continue;
            s.durationMs = fuses[std::uniform_int_distribution<std::size_t>(0, n - 1)(random)] * 1000u;
            s.value = g.words[3] + g.valueBonus;
            break;
        }
        default: break;
        }
        out.push_back(s);
    }
    return out;
}

int BurnRate(int rate) { return kBurnRates[std::size_t(std::clamp(rate, 0, int(std::size(kBurnRates)) - 1))]; }

int KnockDownChance(int level, int chance, int targetLevel) {
    if (level + targetLevel <= 0) return 0;
    const float p = float(double(chance) * (double(level) * 2.0 / double(level + targetLevel)));
    return int(std::trunc(std::clamp(p, 0.0f, 75.0f)));
}

Knock RollKnock(const AttackSpec& attack, int targetLevel, bool canFall, bool canPush, ChanceRolls& rolls, std::mt19937& random) {
    if (attack.knockDownChance > 0 && canFall &&
        rolls.Roll(KnockDownChance(attack.knockDownLevel, attack.knockDownChance, targetLevel), char(0x44), attack.skill, random)) {
        return Knock::Down;
    }
    if (attack.knockBackChance > 0 && canPush && rolls.Roll(attack.knockBackChance, char(0x45), attack.skill, random)) return Knock::Back;
    return Knock::None;
}

std::uint32_t KnockDownMs(int recoverMs, int skillDurationMs) {
    const float seconds = float(double(std::uint32_t(recoverMs + skillDurationMs)) / 1000.0 + 0.5);
    return std::uint32_t(std::lround(double(seconds) * 1000.0));
}

// --- The set -------------------------------------------------------------------------------------

bool StateSet::Apply(const ActiveState& state) {
    const std::size_t slot = std::uint8_t(state.kind);
    if (slot >= slots_.size() || state.level <= 0) return false;
    const std::uint32_t bit = StateBit(state.kind);
    if ((mask_ & bit) != 0 && state.level <= slots_[slot].level) return false;
    slots_[slot] = state;
    mask_ |= bit;
    return true;
}

bool StateSet::End(StateKind kind) {
    const std::uint32_t bit = StateBit(kind);
    if ((mask_ & bit) == 0) return false;
    mask_ &= ~bit;
    slots_[std::uint8_t(kind)] = {};
    return true;
}

std::vector<StateKind> StateSet::Damaged(ChanceRolls& ownRolls, std::mt19937& random) {
    std::vector<StateKind> ended;
    if (In(StateKind::TimeBomb)) return ended;
    if (End(StateKind::Sleep)) ended.push_back(StateKind::Sleep);
    if (In(StateKind::Stun) && ownRolls.Roll(25, char(0x10), 0, random) && End(StateKind::Stun)) ended.push_back(StateKind::Stun);
    return ended;
}

bool StateSet::Cure(const CureSpec& cure, ChanceRolls& ownRolls, std::mt19937& random, Clock::time_point now) {
    // The states in either mask, slot by slot; with 'rcur' that many of them, shuffled first.
    std::vector<std::size_t> picked;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const std::uint32_t bit = StateBit(StateKind(i));
        if ((mask_ & bit) != 0 && ((cure.curseMask & bit) != 0 || (cure.elementMask & bit) != 0)) picked.push_back(i);
    }
    if (cure.most > 0) {
        std::shuffle(picked.begin(), picked.end(), random);
        if (picked.size() > std::size_t(cure.most)) picked.resize(std::size_t(cure.most));
    }
    bool changed = false;
    for (const std::size_t i : picked) {
        const StateKind kind = StateKind(i);
        const std::uint32_t bit = StateBit(kind);
        ActiveState& s = slots_[i];
        if ((cure.curseMask & bit) != 0) {
            // A curse: its chance, divided by the levels it is above the cure's.
            int chance = cure.curseChance;
            const int gap = s.level - cure.curseLevel;
            if (gap > 0) chance /= gap;
            if (ownRolls.Roll(chance, char(0x1A), 0, random)) changed = End(kind) || changed;
            continue;
        }
        // An element: its time less the cure level's worth of its step; gone when none is left.
        const Kind* k = KindOf(kind);
        const std::uint32_t cut = k != nullptr ? std::uint32_t(std::max(0, cure.elementLevel)) * std::uint32_t(std::max(0, k->msPerLevel)) : 0u;
        if (cut == 0) continue;
        changed = true;
        s.durationMs = s.durationMs > cut ? s.durationMs - cut : 0u;
        if (now - s.started >= std::chrono::milliseconds(s.durationMs)) End(kind);
    }
    return changed;
}

void StateSet::Clear() {
    slots_ = {};
    mask_ = 0;
}

std::vector<StateKind> StateSet::Expire(Clock::time_point now) {
    std::vector<StateKind> ended;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const StateKind kind = StateKind(i);
        if ((mask_ & StateBit(kind)) == 0 || now <= slots_[i].Ends()) continue;
        End(kind);
        ended.push_back(kind);
    }
    return ended;
}

std::vector<StateTick> StateSet::Ticks(Clock::time_point now, std::uint32_t hp, std::uint32_t mp, double magAbsorption) {
    std::vector<StateTick> out;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        ActiveState& s = slots_[i];
        if ((mask_ & StateBit(s.kind)) == 0 || s.kind != StateKind(i) || s.tickMs <= 0 || now < s.nextTick || now > s.Ends()) continue;
        s.nextTick = now + std::chrono::milliseconds(s.tickMs);
        StateTick t{s.kind, s.source};
        switch (s.kind) {
        case StateKind::Burn: {
            // The rate over 1 + the magical absorption / 100.
            const float damage = float(double(s.value) / (magAbsorption / 100.0 + 1.0));
            if (damage <= 0.0f) continue;
            t.hp = std::uint32_t(std::trunc(damage));
            break;
        }
        case StateKind::Poison: {
            // Never the last point.
            const std::int64_t left = std::int64_t(hp) - s.value;
            t.hp = std::uint32_t(std::int64_t(hp) - (left > 0 ? left : 1));
            break;
        }
        case StateKind::Bleed: t.hp = std::uint32_t(std::max(0, s.value)); break;
        case StateKind::Panic: t.hp = std::uint32_t(std::trunc(double(hp) * double(s.value3) / 100.0)); break;
        case StateKind::Combustion: t.mp = std::uint32_t(std::trunc(double(mp) * double(s.value3) / 100.0)); break;
        default: continue;
        }
        if (t.hp == 0 && t.mp == 0) continue;
        out.push_back(t);
    }
    return out;
}

const ActiveState* StateSet::Find(StateKind kind) const { return In(kind) ? &slots_[std::uint8_t(kind)] : nullptr; }

std::vector<ActiveState> StateSet::All() const {
    std::vector<ActiveState> out;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if ((mask_ & StateBit(StateKind(i))) != 0) out.push_back(slots_[i]);
    }
    return out;
}

std::uint32_t RecoveryCut(const StateSet& states, std::uint32_t amount, bool hp) {
    const ActiveState* s = states.Find(hp ? StateKind::Panic : StateKind::Combustion);
    if (s == nullptr || s->value2 <= 0 || amount == 0) return amount;
    return std::uint32_t(std::max(0.0, std::trunc(double(amount) * (1.0 - double(s->value2) / 100.0))));
}

bool StateSet::Locked() const { return In(StateKind::Freeze) || In(StateKind::Sleep) || In(StateKind::Stun) || In(StateKind::Stone); }
bool StateSet::Rooted() const { return Locked() || In(StateKind::Root); }

bool StateSet::Idles(std::uint32_t target) const {
    if (In(StateKind::Confusion)) return true;
    const ActiveState* fear = Find(StateKind::Fear);
    return fear != nullptr && fear->source == target;
}

int StateSet::SpeedPercent() const { return In(StateKind::Frostbite) ? 50 : In(StateKind::Slow) ? 75 : 100; }
int StateSet::AttackTimePercent() const { return game::AttackTimePercent(mask_); }
int AttackTimePercent(std::uint32_t mask) {
    return (mask & StateBit(StateKind::Frostbite)) != 0 ? 200 : (mask & StateBit(StateKind::Slow)) != 0 ? 125 : 100;
}

int StateSet::ReachPercent() const {
    const ActiveState* m = Find(StateKind::Myopia);
    return m != nullptr ? std::clamp(100 - m->value, 0, 100) : 100;
}

int StateSet::Susceptibility() const {
    const ActiveState* d = Find(StateKind::Disease);
    return d != nullptr ? d->value : 0;
}

void StateSet::AddChanges(StatSheet& sheet, const CombatStats& base) const {
    const auto add = [&](StateKind kind, std::uint16_t stat, std::uint8_t how, double value) {
        sheet.Add(kStateSource | std::uint8_t(kind), {stat, how, float(value)});
    };
    // The callbacks' keeper writes.
    if (const ActiveState* s = Find(StateKind::Shock)) add(s->kind, stat::kParry, 3, std::clamp(100 - s->value, 0, 100));
    if (const ActiveState* s = Find(StateKind::Dark)) add(s->kind, stat::kHitRate, 1, -s->value);
    if (const ActiveState* s = Find(StateKind::Bleed)) {
        add(s->kind, stat::kPhyDefence, 1, -s->value2);
        add(s->kind, stat::kMagDefence, 1, -s->value2);
    }
    // Decay and weaken: a flat cut, never past 70 % of the defence (0.3 kept).
    const auto cut = [](double defence, int amount) {
        const double v = std::trunc(defence);
        return -std::min(double(amount), std::trunc(v - v * 0.3));
    };
    if (const ActiveState* s = Find(StateKind::Decay)) add(s->kind, stat::kPhyDefence, 0, cut(base.phyDefence, s->value));
    if (const ActiveState* s = Find(StateKind::Weaken)) add(s->kind, stat::kMagDefence, 0, cut(base.magDefence, s->value));
    if (const ActiveState* s = Find(StateKind::Impotent)) {
        add(s->kind, stat::kFinalFactor, 2, -s->value);
        add(s->kind, std::uint16_t(stat::kFinalFactor + 1), 2, -s->value);
    }
    if (const ActiveState* s = Find(StateKind::Division)) {
        add(s->kind, stat::kFinalTaken, 2, s->value);
        add(s->kind, std::uint16_t(stat::kFinalTaken + 1), 2, s->value);
    }
}

void StateSet::Modify(CombatStats& stats) const {
    StatSheet sheet;
    AddChanges(sheet, stats);
    ApplySheet(stats, sheet);
}

} // namespace sr::game
