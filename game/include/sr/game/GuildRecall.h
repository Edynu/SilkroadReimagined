#pragma once

// The guild recall scroll (ITEM_ETC_GUILD_RECALL, TID 3/3/12/3): the original game server's rules, numbers and codes,
// shared by our server and the item catalogue. Headless.
//
// The use, in the original's order, after the generic gates (alive 0x1889, ...):
// - a guild-points item (the reference's AssocFile1 "GP:1000"; the points are its price at the guild shop, not spent
//   by the use): its user a guild's master, else 0x18CB;
// - in a guild, else 0x4C0D (never reached past the master's gate);
// - its world a shared one (_RefGame_World's Type, inferred), else 0x18C1: an instance, the arena, the Qin-shi floors;
// - its guild's master (0x18BC; never reached) and its guild at level Param3 (5) or more, else 0x18BC;
// - standing in a town region (the region's `_RefRegion.IsBattleField` 0), else 0x18C2 ("Guild member recall
//   scroll can only be used at towns."), but not the thieves' village (region 0x60B6, Town_Thief), 0x18C7;
// - its own wait not running (timed job category 2, job 3), else 0x185B; then the wait
//   made for Param1 / 1,000 seconds (3,600 s, with the item's id);
// - the members asked over the shard (the relay 0x7030 through the shard
//   manager with the master's world, region and place and Param2 / 1,000 seconds (30) to answer); the use succeeds (1),
//   the item is spent (log event 0x51).
//
// A member (the 0x7030 at its game server): not asked while blocked by a quest (operation mask
// 0x20000) or with a teleport under way. Its client shows "Guild master has recalled guild
// members. Agree to recall?" for the 30 s (UIIT_MSG_GUILD_RECALL_ANSWER). A yes (0x7031) is refused 0x18C1 (0xB031
// {2, 0x18C1}: UIIT_MSG_GUILD_RECALL_ERROR) when blocked by a quest, dead, a
// teleport under way, a murderer (PvP state 2), a transport out or busy (motion state
// neither 0 nor 1: an exchange, a stall, an NPC's talk); else the shard manager takes it to the master's place.
//
// Ours: the same checks and codes; the shard's bus in place of the shard manager (Guilds). The
// original's member answer carried the destination back from the client and was taken at any time, asked or not (a
// forged 0x7031 would teleport anyone anywhere): ours keeps each member's ask on its server and takes a yes only to the
// place asked, within its time.

#include <cstdint>

#include "sr/assets/GameData.h"

namespace sr::game {

// The codes (the item use's answer, the original's 0xB04C; the member's 0xB031).
constexpr std::uint16_t kRecallNotMaster = 0x18CB;   // a guild-points item: not its guild's master (or in none)
constexpr std::uint16_t kRecallNoGuild = 0x4C0D;     // in no guild (never reached past kRecallNotMaster)
constexpr std::uint16_t kRecallWorld = 0x18C1;       // not in a shared world; a member's yes refused
constexpr std::uint16_t kRecallGuildLevel = 0x18BC;  // the guild below Param3's level
constexpr std::uint16_t kRecallNotTown = 0x18C2;     // not in a town region
constexpr std::uint16_t kRecallThievesTown = 0x18C7; // in the thieves' village
constexpr std::uint16_t kRecallWaiting = 0x185B;     // its own wait runs
constexpr std::uint16_t kThievesVillageRegion = 0x60B6; // _RefRegion: Thief Village / Town_Thief (182, 96)
// The wait's timed job (_TimedJob category 2, job 3).
constexpr std::uint8_t kRecallWaitCategory = 2;
constexpr std::uint32_t kRecallWaitJob = 3;

struct GuildRecallItem {
    std::uint32_t waitSeconds = 0;   // Param1 ms / 1,000 (3,600)
    std::uint32_t answerSeconds = 0; // Param2 ms / 1,000 (30)
    int guildLevel = 0;              // Param3 (5)
};
inline bool IsGuildRecall(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 12, 3}; }
inline GuildRecallItem GuildRecallOf(const assets::ItemRow& row) {
    GuildRecallItem r;
    r.waitSeconds = std::uint32_t(row.params[0] > 0 ? row.params[0] / 1000 : 0);
    r.answerSeconds = std::uint32_t(row.params[1] > 0 ? row.params[1] / 1000 : 0);
    r.guildLevel = row.params[2];
    return r;
}

} // namespace sr::game
