#pragma once

// The guild's mercenaries (docs/re/65-special-items.md 10.1): a guild's master summons soldiers from a scroll that fight
// beside him for their minutes, and the guild buys abilities that raise them. The original's game server holds all of
// it, but 1.188's data never lets it run: every mercenary scroll carries the requirement type 10, which the item use's
// gate does not know, so the scroll is refused 2 before the summon. Ours builds it behind the shard's switch
// guild-mercenaries (off by default: off, the scrolls are refused as 1.188's and no ability is sold); on, the type 10 is
// read as the guild's level (inferred: its ReqLevel1 3 is the summon's own guild level).

#include <cstdint>
#include <optional>
#include <string>

#include "sr/assets/GameData.h"
#include "sr/game/Combat.h"

namespace sr::game {

// The abilities (`_Guild.MercenaryAttr`'s bits), the soldier rows of the guild's NPC in this order; the client offers
// the first four and the release of them all (0), never the resistance.
inline constexpr std::uint8_t kMercDefence = 0x01, kMercAttack = 0x02, kMercHit = 0x04, kMercHealth = 0x08, kMercResistance = 0x10;
inline constexpr std::uint8_t kMercAbilityBits = kMercDefence | kMercAttack | kMercHit | kMercHealth | kMercResistance;
// At most two at once (the client's words and check; the server's "most" check before its job).
inline constexpr int kMercMostAbilities = 2;
// The ability rows' guild level (the client opens them from 4; the server refuses below, 0x4C52).
inline constexpr int kMercAbilityGuildLevel = 4;
// What a mercenary's ability adds to each of its parameters (CGObjCOS_Mercenary_ApplyGuildAttributes: 35.0).
inline constexpr double kMercAbilityAmount = 35.0;

// An ability's price, flat (the game server's and the shard manager's one table): each of the five 500,000 gold and 50,000
// guild points; 0 (the release of all) 500,000 and 10,000 (its row's text says 100,000; the servers take 10,000); any
// other value 100,000,000 of both.
struct MercenaryPrice {
    std::uint64_t gold = 0;
    std::uint32_t points = 0;
};
MercenaryPrice MercenaryPriceOf(std::uint8_t ability);
// The abilities set in `bits`.
int MercenaryAbilityCount(std::uint8_t bits);

// The purchase's answers, in the original's order: in a guild (0x4C0D), its level 4 or more (0x4C52), its master
// (0x4C1E), the gold (0x4C0C), the guild's points (0x4C32), the ability not set already (0x4C54), not two already (2).
inline constexpr std::uint16_t kMercNoGuild = 0x4C0D, kMercGuildLevel = 0x4C52, kMercNotMaster = 0x4C1E, kMercGold = 0x4C0C,
                               kMercPoints = 0x4C32, kMercAbilityTaken = 0x4C54, kMercAbilitiesFull = 2;
// The purchase's answer from the guild's state (0: it may be bought): `held` the abilities the guild has, `ability` the
// one asked (0: release them all), the gold and points it has.
std::uint16_t MercenaryPurchaseCheck(bool inGuild, int guildLevel, bool master, std::uint64_t gold, std::uint32_t points, std::uint8_t held,
                                     std::uint8_t ability);

// The summon's answers (the original's SummonMercenary, in its order where ours has the state): a teleport under way
// (0x1869), a thief (0x18AD) or a trader or hunter (0x189D), free PvP (0x18C8), its guild of level 3 or more and the
// scroll's character known (5), its master (0x18BC), none out already (0x18C9), its wait (0x18BF), the race (0x18D2),
// a row for the summoner's level (0x1811).
inline constexpr std::uint16_t kMercTravelling = 0x1869, kMercThief = 0x18AD, kMercJob = 0x189D, kMercFreePvp = 0x18C8, kMercCannot = 5,
                               kMercSummonNotMaster = 0x18BC, kMercOut = 0x18C9, kMercWaiting = 0x18BF, kMercRace = 0x18D2, kMercNoRow = 0x1811;
inline constexpr int kMercSummonGuildLevel = 3;
// The wait after a summon (_TimedJob 2 / 4, "PENALTY_TYPE_MERCENARY_SUMMON_DISABLE"): 1,200 s.
inline constexpr std::uint8_t kMercWaitCategory = 2;
inline constexpr std::uint32_t kMercWaitJob = 4;
inline constexpr int kMercWaitSeconds = 1200;

// A mercenary scroll (itemdata TID 3/3/12/1).
bool IsMercenaryScroll(const assets::ItemRow& row);
// How many soldiers a scroll brings for a guild of `level` (the original's tables by level 0..5: {0, 0, 0, 1, 3, 6}; for a
// union's leading guild {0, 0, 0, 1, 5, 10}).
int MercenaryCount(int guildLevel, bool unionLeader);
// The scroll's soldier for a summoner of `level`: its Desc1's group (COS_GUILD_*_SOLDIERn_001) at that level's row (_LLL);
// none from level 140 on (the original takes a level below its group's 140 rows only: 0x1811).
std::optional<std::string> MercenaryCodename(const assets::ItemRow& scroll, int level);
// How long its soldiers stay (Param1, minutes; 20 in 1.188).
int MercenaryMinutes(const assets::ItemRow& scroll);

// A soldier's numbers with its guild's abilities (35 added to each parameter an ability names): defence the damage
// absorption of every lane (0x84..0x87; ours: kept, not read by our combat yet, as a 'dar' buff's), attack the damage of
// every lane (0x80..0x83: + 35 %), hit the hit rate and parry (+ 35 %), health the most HP (and its recovery, + 35 %);
// resistance nothing (the original's code has no branch for it).
CombatStats WithMercenaryAbilities(CombatStats base, std::uint8_t bits);

} // namespace sr::game
