#pragma once

// The item mall's timed services: the gold, silver and skill time tickets (3/3/13/4 and
// /5) and the premium gold time package (3/3/13/14), as the original keeps them in `_TimedJob` and runs them, and the
// item mall's other rules that need no server state (the expansions' limits, the Devil's Spirit's awakening). Both
// sides read them; the server keeps the state. Headless.
//
// A ticket's record (the original's `_TimedJob` fields, the getters): TimeToKeep = (seconds
// into the current day << 8) | days left; Data1 the start (Unix seconds); Data2 the bonus seconds (yesterday's unused
// << 16 | today's); Data3 the time spent paused (ours: in seconds; the original's inferred in days); Data4 the pause flag;
// Data5 (ours) when the pause began. Time runs on the wall clock, offline too: each day that passed moves today's
// unused bonus to yesterday's and gives a new day's (the older is lost); the bonus is spent only in battle.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sr/assets/GameData.h"

namespace sr::game {

// The `_TimedJob` categories the mall's items use.
constexpr std::uint8_t kTimedGoldSilver = 3, kTimedSkill = 9, kTimedPremium = 10;
// Ours: the bonus scrolls' runs, one category a key (experience, skill points, skill experience, berserk time; the
// original keeps them all in its category 10 by key, which our layout gives the premium).
constexpr std::uint8_t kTimedBonusFirst = 116, kTimedBonusCount = 4;
constexpr bool IsBonusCategory(std::uint8_t category) { return category >= kTimedBonusFirst && category < kTimedBonusFirst + kTimedBonusCount; }
constexpr std::int64_t kSecondsPerDay = 86400;

// Which category a ticket goes in (0: not a ticket): 3/3/13/4 gold or silver, 3/3/13/5 the skill ones.
std::uint8_t TimedCategoryOf(const assets::ItemRow& row);
bool IsPremiumPackage(const assets::ItemRow& row); // 3/3/13/14

struct TimedService {
    std::uint8_t category = 0;
    std::uint32_t item = 0;        // the ticket's RefObjID (the job's JobID)
    std::int64_t start = 0;        // Data1: when it began (Unix seconds; its days counted from here, the pauses added)
    int days = 0;                  // the whole time in days (Param1 / 86,400)
    int daysLeft = 0;              // TimeToKeep's low byte
    int secondsIntoDay = 0;        // TimeToKeep's upper bits
    std::uint32_t bonusToday = 0;  // Data2's low half: seconds of bonus left today
    std::uint32_t bonusYesterday = 0;
    std::uint32_t dailyBonus = 0;  // Param3 / 1,000 (10,800: 3 hours)
    int expPercent = 0, skillPercent = 0; // Param4, Param5
    bool paused = false;           // Data4
    std::int64_t pausedAt = 0;     // Data5 (ours)
    std::int64_t pausedSeconds = 0; // all the paused time before the running pause (Data3)
    double spendCarry = 0;         // battle time not yet a whole second (not kept)

    // A new one from its ticket's row (P1 the time, P3 the day's bonus in ms, P4 / P5 the bonuses), begun `now`.
    static TimedService Begin(const assets::ItemRow& row, std::uint8_t category, std::int64_t now);
    // The original's record, and back (the row of the ticket gives the bonuses again).
    std::int32_t TimeToKeep() const;
    std::int32_t Data2() const { return std::int32_t(std::min<std::uint32_t>(bonusYesterday, 0xFFFF) << 16 | std::min<std::uint32_t>(bonusToday, 0xFFFF)); }
    static TimedService FromRecord(std::uint8_t category, std::uint32_t item, std::int32_t timeToKeep, std::int32_t data1, std::int32_t data2,
                                   std::int32_t data3, std::int32_t data4, std::int32_t data5, const assets::ItemRow* row);

    // The day's turn up to `now` (online and at a load alike): the days that passed counted off, the bonus moved on.
    // False when its time is over (it ends).
    bool Advance(std::int64_t now);
    // `seconds` of battle: the bonus spent, yesterday's first (none while paused).
    void SpendBattle(double seconds);
    bool BonusOn() const { return !paused && bonusToday + bonusYesterday > 0; }
    // Its time left in all (seconds).
    std::uint32_t SecondsLeft(std::int64_t now) const;
    std::uint32_t Bonus() const { return std::min<std::uint32_t>(bonusYesterday, 0xFFFF) << 16 | std::min<std::uint32_t>(bonusToday, 0xFFFF); }
    void Pause(bool on, std::int64_t now);
};

// The premium gold time's parts (the package's Param2..4 texts: `[TAG:args],...`).
struct PremiumPart {
    std::string tag;               // UIU1, UIL1, USU1, BFI1, UQL1
    std::string target;            // the item, skill or quest's codename
    std::vector<std::string> args; // the rest ("A", the period, the count)
    std::int64_t period = 0;       // UIL1 / BFI1 / UQL1: seconds
    int count = 0;                 // UIL1 / UQL1: the uses in a period
};
std::vector<PremiumPart> PremiumPartsOf(const assets::ItemRow& row);

// The premium package as a character keeps it (category 10: JobID the package, Data1 the start, TimeToKeep its seconds;
// ours: Data2..4 the daily uses taken of its first three UIL1 parts, Data5 when their period began, Data6 the booth,
// Data7 the finishes of its UQL1 quest).
struct PremiumState {
    std::uint32_t item = 0;
    std::int64_t start = 0;
    std::int64_t seconds = 0;          // the package's whole time (Param1)
    std::int64_t periodStart = 0;      // the daily counts' 24 h began then
    std::array<int, 3> used{};         // by UIL1 part, in the package's order
    int questUsed = 0;                 // its UQL1 quest finished this many times (of the part's count, in the package's time)
    bool Running(std::int64_t now) const { return item != 0 && now < start + seconds; }
    std::int64_t SecondsLeft(std::int64_t now) const { return item == 0 ? 0 : std::max<std::int64_t>(0, start + seconds - now); }
    // The 24 h turned (ours: from the activation, the item's "24 hours from the moment the item is used").
    void Advance(std::int64_t now, std::int64_t period = kSecondsPerDay);
};

// The expansions (3/3/13/17): P1 the places, P2 0 the bag / 1 the storage. The bag's
// limit 77 (45 + 32), 99 with one of the bag quests done or under way; the storage's 210 (150 + 30 + 30).
constexpr int kBagExpansionLimit = 77, kBagExpansionQuestLimit = 99, kStorageExpansionLimit = 210;
constexpr std::array<std::string_view, 6> kBagQuests = {"QSP_CH_EXINVENTORY_1", "QSP_EU_EXINVENTORY_1", "QSP_WC_EXINVENTORY_2",
                                                        "QSP_CA_EXINVENTORY_2", "QSP_KT_EXINVENTORY_3", "QSP_RM_EXINVENTORY_4"};
bool IsExpansion(const assets::ItemRow& row);

// The Devil's Spirit (3/1/14/1): awake while its data word (the awakening's end, Unix seconds) is after `now`.
bool DevilSpiritAwake(std::uint32_t data, std::int64_t now);

} // namespace sr::game
