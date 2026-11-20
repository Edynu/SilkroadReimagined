#include "sr/game/Pk.h"

#include <algorithm>
#include <cmath>

namespace sr::game {

std::int32_t PenaltyForMurder(int totalPkAfter) {
    return std::int32_t((std::max(0, totalPkAfter) + 1) / 2) * kPenaltyStep;
}

int DailyForMurder(int killerLevel, int victimLevel) {
    if (victimLevel >= killerLevel) return 1;
    return 1 + 2 * ((killerLevel - victimLevel) / 5);
}

std::int32_t MonsterKillRelief(int monsterLevel, int playerLevel) {
    // The level gap's class into five rows.
    static constexpr std::int32_t kRelief[] = {10, 7, 5, 3, 1};
    const int d = monsterLevel - playerLevel;
    const int row = d >= 4 ? 0 : d >= 1 ? 1 : d >= -3 ? 2 : d >= -6 ? 3 : 4;
    return kRelief[row];
}

std::int32_t DeathRelief(KillRelation relation) {
    switch (relation) {
    case KillRelation::Monster: return kMonsterDeathRelief;
    case KillRelation::GuildWar: return kGuildWarDeathRelief;
    default: return 0;
    }
}

int DeathItemChance(std::int32_t points) {
    if (points <= 0) return 5;
    if (points < 4'000) return 30;
    if (points < 15'000) return 50;
    if (points < 30'000) return 70;
    return 100;
}

std::uint64_t DeathExperienceLoss(const DeathFacts& f) {
    if (f.level <= 10) return 0;
    // The rates as the server keeps them (doubles: 0.0040000002, 0.0199999995, 0.0599999987).
    constexpr double k04 = 0.004000000189989805, k2 = 0.019999999552965164, k6 = 0.05999999865889549;
    double rate = 0.0;
    std::int64_t factor = 0;
    if (f.fortressWar) {
        rate = f.byPlayer ? k04 : k2;
        factor = f.byPlayer ? 4 : 20;
    } else if (f.murderer) {
        rate = f.byPlayer ? k2 : k6;
        factor = f.byPlayer ? 100 : 300;
    } else {
        rate = f.byPlayer ? k04 : k2;
        factor = f.byPlayer ? 20 : 100;
    }
    const std::int64_t share = std::int64_t(double(f.levelExperience) * rate);
    if (share < 1) return 0;
    double cap = double(std::int64_t(f.monsterExperience) * factor);
    if (f.thiefMonster) cap *= 0.5;
    std::int64_t lost = std::min<std::int64_t>(std::int64_t(cap), share);
    lost = std::max<std::int64_t>(lost, 1);
    // Less the reduction's percent of it (0x101).
    const std::int64_t cut = std::int64_t(std::trunc(f.reducePercent / 100.0 * double(lost)));
    if (cut > 0) lost -= cut;
    return lost > 0 ? std::uint64_t(lost) : 0u;
}

int MurdererEquipmentPlace(int roll13, int roll6, bool ammunitionInShield) {
    const int place = std::clamp(roll13, 0, 12);
    if (place == 6 || place == 8 || (place == 7 && ammunitionInShield)) return std::clamp(roll6, 0, 5);
    return place;
}

double PvpLevelGapFactor(int victimLevel, int killerLevel) {
    const int d = victimLevel - killerLevel;
    double f = 1.0;
    if (d > 10) f = 1.0 - double(float(0.02 * double(d - 10)));
    else if (d <= -7) f = 1.0 - double(float(0.15 * double(-d - 3)));
    else if (d <= -4) f = double(0.9f);
    return std::clamp(f, 0.1, 1.0);
}

double PvpPartyFactor(int victimLevel, int killerLevel, bool expSharedParty, bool european) {
    if (victimLevel + 3 <= killerLevel) return 1.0;
    const int d = victimLevel - killerLevel;
    float bonus = 0.0f;
    if (expSharedParty) {
        if (european) bonus = d >= 9 ? 0.9f : d >= 7 ? 0.6f : d >= 5 ? 0.3f : 0.0f;
        else bonus = d >= 9 ? 0.45f : d >= 7 ? 0.3f : d >= 5 ? 0.15f : 0.0f;
    }
    bonus = float(double(bonus) + 1.0);
    const float f = float(double(std::min(d + 3, 13)) * 0.029999999329447746 + double(bonus));
    return std::clamp(double(f), 1.0, 4.0);
}

std::uint64_t PvpExperience(std::uint32_t lowerMonsterExperience, int victimLevel, int killerLevel, double rate, bool expSharedParty,
                            bool european, bool doubled) {
    const float gap = float(PvpLevelGapFactor(victimLevel, killerLevel));
    const float party = float(PvpPartyFactor(victimLevel, killerLevel, expSharedParty, european));
    float v = float(double(gap) * double(float(lowerMonsterExperience)) * double(float(rate)) * double(party));
    if (v == 0.0f) v = 1.0f;
    const std::uint64_t whole = v > 0.0f ? std::uint64_t(std::trunc(double(v))) : 0u;
    return doubled ? whole * 2 : whole;
}

KeeperStep KeeperAfter(int totalPk, std::int64_t secondsLeft, std::int64_t elapsed) {
    KeeperStep s;
    s.totalPk = std::max(0, totalPk);
    if (s.totalPk == 0) {
        s.cleared = true;
        return s;
    }
    std::int64_t left = secondsLeft - std::max<std::int64_t>(0, elapsed);
    while (left <= 0 && s.totalPk > 0) {
        --s.totalPk;
        ++s.taken;
        left += kKeeperSeconds;
    }
    if (s.totalPk == 0) {
        s.cleared = true;
        s.secondsLeft = 0;
        return s;
    }
    s.secondsLeft = left;
    return s;
}

bool NaturalEnemy(const PvpPair& p) {
    if (p.striker.level < kPkLevel || p.target.level < kPkLevel) return false;
    return p.target.hasRecords || p.target.state == PvpState::Murderer || p.hostileJobs || p.guildWar;
}

AttackVerdict AttackCheck(const PvpPair& p) {
    using S = game::proto::SkillUseStatus;
    AttackVerdict v;
    const auto allow = [&] {
        v.allowed = true;
        v.code = S::Ok;
        return v;
    };
    const auto refuse = [&](S code) {
        v.allowed = false;
        v.code = code;
        return v;
    };
    // An event's foes (relation 0x20 Capture the Flag, 0x40 the Battle Arena): before every other rule, the side alone.
    if (p.eventRelation != 0) return allow();
    if (!p.striker.battlefield || !p.target.battlefield) return refuse(S::NotBattlefield);
    if (p.sameParty) return refuse(S::PartyMember);
    if (p.fortressFoes || p.freePvpFoes) return allow();
    if (p.safeTrade != S::Ok) return refuse(p.safeTrade);
    if (p.guildWar) return allow();
    if (p.pkProhibited) return refuse(S::PkProhibited);
    if (p.hostileJobs) return p.jobLock ? refuse(S::JobLock) : allow();
    const bool area = p.mode == AttackMode::Area;
    const bool noNewVictims = p.mode != AttackMode::Chosen; // mode bit 1 (modes 1 and 3)
    if (area && p.striker.state == PvpState::Neutral && p.target.state == PvpState::Assaulter) return refuse(S::Target);
    if (NaturalEnemy(p)) return allow();
    if (p.strikerHolds) return allow();
    if (noNewVictims) return refuse(S::Target);
    if (p.striker.level < kPkLevel) return refuse(S::YourLevelTooLow);
    if (p.target.level < kPkLevel) return refuse(S::TargetLevelTooLow);
    if (p.striker.points >= kPenaltyMost) return refuse(S::TotalPkLimit);
    if (p.striker.dailyPk >= kDailyPkMost) return refuse(S::DailyPkLimit);
    v.pk = true;
    return allow();
}

bool HitMakesRecord(const PvpPair& p) {
    if (p.eventRelation != 0) return false;
    if (p.sameParty) return false;
    if (p.fortressFoes || p.freePvpFoes || p.hostileJobs || p.guildWar) return false;
    if (p.striker.level >= kPkLevel && p.target.level >= kPkLevel && p.target.state == PvpState::Murderer) return false;
    if (NaturalEnemy(p) && !p.target.hasRecords) return false;
    return true;
}

bool KillIsMurder(const PvpPair& p) { return p.eventRelation == 0 && !NaturalEnemy(p); }

} // namespace sr::game
