#pragma once

// The special items' rules: the kinds the item lab listed as unhandled, as the original game server has them, shared by
// our server and the item catalogue. Headless.
//
// - The bag's other uses: fireworks (3/3/6, consumed, their show on everyone's screen), the berserk potion (3/3/1/8, the
//   gauge full), the experience and skill experience scrolls (3/3/3/10, a timed bonus by its key), the generic COOLTIME
//   wait an item's data may carry.
// - The uses the original's server refuses, by kind, with its code (the stat resets, the storage ticket, the
//   reinforcement scrolls, the indulgence, the skill points and plus scrolls, the arena ticket, the pet naming scroll):
//   OriginalUseRefusal.

#include <cstdint>

namespace sr::assets {
struct ItemRow;
}

namespace sr::game {

// An item's COOLTIME pair (itemdata: a param's text "COOLTIME:0x000000C9" beside the wait in ms): every item of the group
// waits with the one used (the wait, the group,
// refused 0x185B while it runs). None: ms 0.
struct ItemCoolTime {
    std::uint32_t group = 0;
    std::uint32_t ms = 0;
};
ItemCoolTime CoolTimeOf(const assets::ItemRow& row);
constexpr std::uint16_t kCoolTimeRefused = 0x185B;
// The key a COOLTIME's wait is kept by: its group, every item of the group waiting with the one used; a group of 0 is
// the item's own wait (the original's check keys group 0 by the item: a fortress manual, a hwan potion and a quest item
// never wait for each other).
constexpr std::uint32_t kItemCoolTimeKey = 0x80000000u;
constexpr std::uint32_t CoolTimeKey(std::uint32_t refObjId, std::uint32_t group) { return group != 0 ? group : kItemCoolTimeKey | refObjId; }

// A firework (3/3/6 but TID4 2, the fortress's shock bomb): asks only that the user
// is in the world (0x186B otherwise) and succeeds: the item is taken and the use told to those around (0x305C), whose
// clients play the effect list named after the item.
bool IsFirework(const assets::ItemRow& row);

// The berserk potion (3/3/1/8): refused 0x18CE in berserk,
// else the gauge gets five points (with 5): full. No reuse lock of
// the potions' kinds; its COOLTIME, when it has one, as any item's.
bool IsBerserkPotion(const assets::ItemRow& row);
constexpr std::uint16_t kBerserkPotionRefused = 0x18CE;
constexpr std::uint8_t kBerserkPotionPoints = 5;

// The bonus scrolls (3/3/3/10): Param1 seconds, Param2 the key (a four-letter code), Param3 the value. A
// timed job of the original's category 10 keyed by Param2 (with 10), its end on the wall
// clock (now + Param1); a second of the same key is refused 0x1894 while one runs.
// What a key gives: the timed job's keeper puts the value on a parameter by its table: 'peru' 0x100 the experience of
// kills +value %,
// 'psru' and 'seru' both 0x102 their skill experience +value %, '\0pht' 0x3C the berserk mode's own time +value seconds.
enum class BonusKind : std::uint8_t { None = 0, Experience, SkillExperience, BerserkTime };
constexpr std::uint32_t kBonusExperienceKey = 0x70657275;      // 'peru' -> 0x100
constexpr std::uint32_t kBonusSkillPointsKey = 0x70737275;     // 'psru' -> 0x102
constexpr std::uint32_t kBonusSkillExperienceKey = 0x73657275; // 'seru' -> 0x102
constexpr std::uint32_t kBonusBerserkTimeKey = 0x706874;       // '\0pht' -> 0x3C
struct BonusScroll {
    BonusKind kind = BonusKind::None;
    std::uint32_t key = 0;
    int value = 0;   // a percent, or the berserk time's seconds
    int seconds = 0; // how long it runs
    explicit operator bool() const { return kind != BonusKind::None && seconds > 0; }
};
BonusScroll BonusScrollOf(const assets::ItemRow& row);
constexpr std::uint16_t kBonusRunning = 0x1894;

// The uses the original's server refuses (0: it takes the item), with the code its 0xB04C answer carries:
// - 3/3/13/10 and /11 (the long-range storage ticket, the reinforcement scrolls): 2;
// - 3/3/13/13 (the stat resets, ITEM_MALL_HWAN, the auto potion ticket, the friend summoning scroll): 2;
// - 3/3/13/0 and /18 (a skill restoration potion through the item use, the indulgence): 2 (the code left as it began);
// - 3/3/3/12 and /14 (the skill points scroll, the option level scrolls): 0x183E (no case of its own);
// - 3/3/12/4 (the arena ticket) and 3/3/16 (the pets' naming and skill scrolls): 0x183E;
// - before them all, a requirement type the use's gate does not know: 2 (RequirementTypeKnown): the guild's
//   15 mercenary scrolls (3/3/12/1, type 10), so the original never summons a mercenary.
// The skill and mastery withdrawal still takes its three potions (WithdrawModeOf) through its own message.
std::uint16_t OriginalUseRefusal(const assets::ItemRow& row);
// A requirement type (_RefObjCommon.ReqLevelType1..4) the original's requirement gate knows.
bool RequirementTypeKnown(int type);

// What used quest and event items go through (3/3/9 usable, 3/3/15): the quests' own uses of their items, or the timed
// world event's for items flagged so, refused 2 when none takes it; taken, the item is used up (no work of its own).
bool IsQuestUseItem(const assets::ItemRow& row);
constexpr std::uint16_t kNoQuestTakesIt = 2;

} // namespace sr::game
