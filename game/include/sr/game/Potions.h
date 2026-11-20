#pragma once

// The recovery items and the auto potion. Headless, shared by the game
// server (which uses them) and the client (which shows them).
//
// - A potion (TID 3/3/1) or a cure (3/3/2) used: the reuse lock of its kind, the HP and MP it gives,
//   flat ones over five steps, a pill's cure.
// - The auto potion: the HP, MP and abnormal-state rows the player sets (on, a percent, the potion
//   dropped in the row's slot: ours, in place of the original's quick slot), the delay between uses,
//   the original's words kept in _ClientConfig.
// - Ours beyond the original (2026-11-13, the user's ask): two more rows, the vehicle's (a ride or a
//   transport) and the attack pet's health, each healed with the pets' HP recovery kit (TID 3/3/1/4).

#include <array>
#include <cstdint>

#include "sr/game/States.h"
#include "sr/game/WorldProtocol.h"

namespace sr::assets {
struct ItemRow;
}

namespace sr::game {

// --- The recovery items --------------------------------------------------------------------------

// A potion (TID 3/3/1: TID4 1 HP, 2 MP, 3 both; 4, 5, 9 the pets') or a cure (3/3/2: TID4 1 the
// purification pill, 6 the universal pill, 7 the pets').
bool IsPotion(const assets::ItemRow& item);
bool IsCure(const assets::ItemRow& item);

// The lock a use puts on its kind (a locked kind refused with 0x185B): its bit in the player's mask (HP 0x01, MP 0x02,
// both 0x04, the pets' HP 0x08 and MP 0x10, TID4 7 0x20; the purification pill 0x100, the other cures 0x40) and how
// long: a percent potion 4.1 s, a flat one 1.1 s for a Chinese
// character and 15.1 s for a European, the purification pill 20.1 s, the other cures 1.1 s; the pets'
// potions (TID4 4, 5, 7, used on a COS) 1.1 s whatever they are. Kinds of
// different bits never lock each other (an HP and an MP potion may be drunk back to back). Bit 0: no lock.
struct ReuseLock {
    std::uint32_t bit = 0;
    int ms = 0;
};
ReuseLock ReuseLockOf(const assets::ItemRow& item, bool european);

// What a potion gives: with a flat amount
// (Param1 HP or Param3 MP above 0) HP = Param1 x 1.02^(level - 1) x (1 + STR / 416) and MP = Param3 x
// 1.02^(level - 1) x (1 + INT / 416), each cut to a whole number, the percents ignored; else HP =
// most HP x Param2 / 100 and MP = most MP x Param4 / 100.
struct Recovery {
    std::uint32_t hp = 0, mp = 0;
    bool flat = false;
};
Recovery RecoveryOf(const assets::ItemRow& item, int level, double strength, double intelligence, std::uint32_t maxHp, std::uint32_t maxMp);

// A flat potion's steps: a fifth of the amount each (1 .. 1,000,000), one at once and four more, one a second (from the
// player's state tick; ours: the period inferred as 1 s).
inline constexpr int kPotionSteps = 5;
std::uint32_t PotionStep(std::uint32_t amount);

// A pill's cure: the universal pill (TID4 6, 7)
// takes Param1 levels' worth of time off each element state (freeze .. zombie; its six params are
// one power a state, equal in every row); the purification pill (TID4 1) ends one state of its
// Param1 mask at random, at Param3's chance divided by the levels it is above Param2; the mall's bag
// (TID4 8) as the universal pill with its own six params (freeze and frostbite outright).
CureSpec PillCureOf(const assets::ItemRow& item);

// --- The auto potion -------------------------------------------------------------------------------

struct AutoPotion {
    // The original's three rows, then ours: the vehicle's HP (a ride or a transport out) and the attack pet's.
    enum Row : int { kHp = 0, kMp = 1, kAbnormal = 2, kOriginalRows = 3, kVehicle = 3, kAttackPet = 4, kRows = 5 };
    struct Entry {
        bool enabled = false;
        std::uint8_t percent = 50; // 1..100 (the abnormal row has none)
        std::uint32_t item = 0;    // the dropped item's itemdata id (0: none)
        bool operator==(const Entry&) const = default;
    };
    // The original's defaults: HP and MP off at 50 %, the abnormal row off, the
    // delay on at 1 s; ours off at 50 %.
    std::array<Entry, kRows> rows{Entry{false, 50, 0}, Entry{false, 50, 0}, Entry{false, 0, 0}, Entry{false, 50, 0}, Entry{false, 50, 0}};
    bool delayEnabled = true;
    std::uint16_t delayMs = 1000; // 500 .. 9500 in steps of 500 (the window's spin)
    bool operator==(const AutoPotion&) const = default;

    // Its values within the original's ranges (a percent 1..100, the delay 500..9500 by 500).
    void Clamp();
    // How often a row that holds is tried again (the original's timer): the delay
    // when it is on, else 500 ms.
    int RepeatMs() const { return delayEnabled ? delayMs : 500; }
};
inline constexpr int kDelayMinMs = 500, kDelayMaxMs = 9500, kDelayStepMs = 500;

// The original's word for a row (_ClientConfig types 11, 12, 13; writes it,
// reads it): bit 15 on, bits 8..14 the percent, bits 4..7 the quick bar's page (1..4) and 0..3 its
// key (1..10) of the quick slot 1..40 it named, 0 / 0 for none. Ours names no quick slot (the item
// is ours, in sr_auto_potion), so `quickSlot` is 0 unless a caller has one.
std::uint16_t EncodeAutoPotionWord(const AutoPotion::Entry& entry, int quickSlot = 0);
AutoPotion::Entry DecodeAutoPotionWord(std::uint16_t word, int* quickSlot = nullptr);
// The delay's byte (type 14): bit 7 on, bits 0..6 the delay in tenths of a second.
std::uint8_t EncodeAutoPotionDelay(bool enabled, int ms);
void DecodeAutoPotionDelay(std::uint8_t byte, bool& enabled, int& ms);

// A row holds at or under its most x percent / 100, cut to a whole number (the HP row is tested with <=).
std::uint32_t AutoPotionThreshold(std::uint32_t most, int percent);

// The settings as the protocol carries them (proto::AutoPotionSettings), and back.
proto::AutoPotionSettings ToMessage(const AutoPotion& settings);
AutoPotion FromMessage(const proto::AutoPotionSettings& message);

// What may be dropped in a row: the HP row a potion that gives HP (TID4 1 or 3), the MP row one that
// gives MP (2 or 3), the abnormal row a player's pill (TID 3/3/2/1 or 6), the vehicle's and the attack
// pet's rows the pets' HP recovery kit (3/3/1/4). Ours (the original took any quick slot's item).
bool AutoPotionFits(int row, const assets::ItemRow& item);

} // namespace sr::game
