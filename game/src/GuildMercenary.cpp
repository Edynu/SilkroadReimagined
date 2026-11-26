#include "sr/game/GuildMercenary.h"

#include <algorithm>
#include <array>
#include <bit>

#include "sr/game/Cos.h"

namespace sr::game {

MercenaryPrice MercenaryPriceOf(std::uint8_t ability) {
    switch (ability) {
    case 0: return {500'000, 10'000};
    case kMercDefence:
    case kMercAttack:
    case kMercHit:
    case kMercHealth:
    case kMercResistance: return {500'000, 50'000};
    default: return {100'000'000, 100'000'000};
    }
}

int MercenaryAbilityCount(std::uint8_t bits) { return std::popcount(std::uint8_t(bits & kMercAbilityBits)); }

std::uint16_t MercenaryPurchaseCheck(bool inGuild, int guildLevel, bool master, std::uint64_t gold, std::uint32_t points, std::uint8_t held,
                                     std::uint8_t ability) {
    if (!inGuild) return kMercNoGuild;
    if (guildLevel < kMercAbilityGuildLevel) return kMercGuildLevel;
    if (!master) return kMercNotMaster;
    const MercenaryPrice price = MercenaryPriceOf(ability);
    if (gold < price.gold) return kMercGold;
    if (points < price.points) return kMercPoints;
    // The release of none held, or an unknown value, goes no further (the original's 2).
    if (ability == 0) return held != 0 ? 0 : kMercAbilitiesFull;
    if (std::popcount(ability) != 1 || (ability & ~kMercAbilityBits) != 0) return kMercAbilitiesFull;
    if ((held & ability) != 0) return kMercAbilityTaken;
    if (MercenaryAbilityCount(held) >= kMercMostAbilities) return kMercAbilitiesFull;
    return 0;
}

bool IsMercenaryScroll(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 12, 1}; }

int MercenaryCount(int guildLevel, bool unionLeader) {
    // The original's two tables (by the guild's level, 0..5).
    static constexpr std::array<int, 6> kGuild{0, 0, 0, 1, 3, 6}, kUnionLeader{0, 0, 0, 1, 5, 10};
    if (guildLevel < 0) return 0;
    const std::size_t i = std::size_t(std::min(guildLevel, 5));
    return unionLeader ? kUnionLeader[i] : kGuild[i];
}

std::optional<std::string> MercenaryCodename(const assets::ItemRow& scroll, int level) {
    const std::string group = PetCodename(scroll); // its Desc1, COS_GUILD_*_SOLDIERn_001
    if (group.empty() || group == "xxx" || level < 1 || level >= 140) return std::nullopt;
    std::string row = PetCodenameAtLevel(group, level);
    if (row.empty()) return std::nullopt;
    return row;
}

int MercenaryMinutes(const assets::ItemRow& scroll) { return std::max(1, scroll.params[0]); }

CombatStats WithMercenaryAbilities(CombatStats s, std::uint8_t bits) {
    // Defence (0x84..0x87, the damage absorption of every lane): our combat reads no such lanes yet (a 'dar' buff's
    // neither); kept out here until it does.
    if ((bits & kMercAttack) != 0) {
        for (double& d : s.damagePercent) d += kMercAbilityAmount;
    }
    if ((bits & kMercHit) != 0) {
        s.hitRate *= 1.0 + kMercAbilityAmount / 100.0;
        s.evasion *= 1.0 + kMercAbilityAmount / 100.0;
    }
    if ((bits & kMercHealth) != 0) s.maxHp = std::uint32_t(double(s.maxHp) * (1.0 + kMercAbilityAmount / 100.0));
    return s;
}

} // namespace sr::game
