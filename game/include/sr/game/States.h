#pragma once

// States from hits: the original's abnormal states
// (freeze, stun, burn, the curses, ...): which a skill's hit gives, at what chance, level and time,
// what each does to its target while it lasts, and what ends it, as the original game server decides.
// Headless: the server keeps a StateSet per monster (players' later) and tells
// the client the states (their kinds, time left and levels), which it shows.

#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "sr/game/Combat.h"

namespace sr::assets {
struct SkillRow;
}

namespace sr::game {

// The original's record kinds (its slots); the client's mask bit is StateBit.
enum class StateKind : std::uint8_t {
    Freeze = 0,
    Frostbite = 1,
    Burn = 2,
    Shock = 3,
    Poison = 4,
    Zombie = 5,
    Sleep = 6,
    Root = 7,
    Slow = 8,
    Fear = 9,
    Myopia = 10,
    Bleed = 11,
    Stone = 12,
    Dark = 13,
    Stun = 14,
    Disease = 15,
    Confusion = 16,
    Decay = 17,
    Weaken = 18,
    Impotent = 19,
    Division = 20,
    Panic = 21,
    Combustion = 22,
    TimeBomb = 24,
};
inline constexpr int kStateSlots = 32;

// The kind's bit in the state mask: 1 << kind, burn and shock swapped.
std::uint32_t StateBit(StateKind kind);
// The elements (freeze .. zombie): their level is the tag's power cut by the target's resistance.
constexpr bool IsElement(StateKind kind) { return std::uint8_t(kind) <= 5; }
const char* StateName(StateKind kind);
// The resistance bucket of a kind that is not an element (0..16: sleep, root, slow, fear, myopia,
// bleed, darkness, stun, disease, confusion, decay, weaken, impotent, division, panic, combustion,
// time bomb); -1 for the elements and stone.
int ResistBucket(StateKind kind);
inline constexpr int kResistBuckets = 17;

// A state one of a skill's tags gives: its kind and the tag's words.
struct StateGift {
    StateKind kind = StateKind::Freeze;
    std::array<int, 6> words{};
    // A passive's additions (the Rogue's poison, K6): to the
    // poison's level after the resistance's cut, and to a tick's damage.
    int levelBonus = 0, valueBonus = 0;
};
// The states a skill's hits give, in the order the original rolls them.
std::vector<StateGift> StateGiftsOf(const assets::SkillRow& skill);

// What a cure takes away (K7): the element states of 'curt' mask level (their
// time less the level's worth of each one's step), the others of 'curl' mask chance level (each at
// its chance, divided by how many levels it is above the cure's), at most 'rcur' of them (0: all)
// picked at random.
struct CureSpec {
    std::uint32_t elementMask = 0;
    int elementLevel = 0;
    std::uint32_t curseMask = 0;
    int curseChance = 0, curseLevel = 0;
    int most = 0;
    explicit operator bool() const { return elementMask != 0 || curseMask != 0; }
};
CureSpec CureOf(const assets::SkillRow& skill);

// The target as the roll sees it (a monster's from its reference: its level and Resist columns).
struct StateDefence {
    int level = 1;
    std::array<int, 6> elementResist{}; // percent by element (freeze .. zombie): 100 or more, never
    std::array<int, 6> elementGrade{};  // taken off an element's level (players' items; 0 for monsters)
    // By bucket: the percent off the chance (a monster's Resist column), and a flat amount off it
    // that halves for each level the state is above the grade (players' items and passives).
    std::array<int, kResistBuckets> percent{}, flat{}, grade{};
};

// A state as rolled, and as kept while it lasts.
struct ActiveState {
    using Clock = std::chrono::steady_clock;
    StateKind kind = StateKind::Freeze;
    int level = 0;                // an element's level, the others' level word (the replacing rule)
    std::uint32_t durationMs = 0;
    std::uint32_t source = 0;     // the attacker's entity
    std::uint32_t skill = 0;
    // The kind's numbers: a tick's damage or percent (burn, poison, bleed, panic, combustion), the
    // stat change (shock's parry, darkness's hit rate, bleed's defence, myopia's reach, disease's
    // susceptibility, the curses' amounts), panic's and combustion's other two.
    int value = 0, value2 = 0, value3 = 0;
    int tickMs = 0;               // 0: it does not tick
    Clock::time_point started, nextTick;
    Clock::time_point Ends() const { return started + std::chrono::milliseconds(durationMs); }
};

// A state's odds on a target: the chance its roll takes, and when it takes
// its level and time. An element: the chance is the tag's and the susceptibility, the level the power
// cut by the resistance less the grade (0: it never takes, whatever the roll). The others: the chance
// and time cut by the level gap, the chance by the resistance; the level the tag's.
struct StateOdds {
    int chance = 0;
    int level = 0;
    std::uint32_t durationMs = 0;
};
StateOdds OddsOf(const StateGift& gift, const StateDefence& target, int susceptibility);

// One skill's states rolled on one target after a hit, through the
// attacker's chance history: those that took, in order. `susceptibility` is the target's (disease's
// stat 0xA9); `skillPulseMs` the skill's 'puls' (a bleed's period; 0: 2 s).
std::vector<ActiveState> RollStates(std::span<const StateGift> gifts, const StateDefence& target, int susceptibility, ChanceRolls& rolls,
                                    std::mt19937& random, std::uint32_t source, std::uint32_t skill, int skillPulseMs,
                                    ActiveState::Clock::time_point now);

// The burn's damage for its rate (the tag's third word; the original's table, rates 0..140, a higher one taken as 140),
// before the target's magical absorption.
int BurnRate(int rate);

// A knock-down's chance: chance x 2 x level / (level + the target's level),
// 0..75, a whole number.
int KnockDownChance(int level, int chance, int targetLevel);

// After a hit that was not blocked, and its states: a knock-down ('ko') on a target that may
// fall (its reference's Knockdown bit 0, not down already), else a knock-back ('kb') on one that may be
// pushed (bit 1), through the attacker's chance history (keys 0x44 / 0x45 and the skill).
enum class Knock : std::uint8_t { None = 0, Down = 1, Back = 2 };
Knock RollKnock(const AttackSpec& attack, int targetLevel, bool canFall, bool canPush, ChanceRolls& rolls, std::mt19937& random);
// How far a knock pushes (world units): a knock-down 20, a knock-back its distance word; how long the
// target cannot act: down (its KO_RecoverTime + the skill's action time) / 1000 + 0.5 s, back 2 s.
inline constexpr float kKnockDownDistance = 20.0f;
// A knock-back's slide: 250 units a second to its end (the 1.188 client's SYSTEM_KNOCKBACK presentation, kind
// 5).
inline constexpr float kKnockBackSpeed = 250.0f;
std::uint32_t KnockDownMs(int recoverMs, int skillDurationMs);
inline constexpr std::uint32_t kKnockBackMs = 2000;

// What a tick does (a state's damage over time).
struct StateTick {
    StateKind kind = StateKind::Burn;
    std::uint32_t source = 0;
    std::uint32_t hp = 0, mp = 0; // lost
    std::uint32_t skill = 0;      // the state's skill (a time bomb's burst shows its S_RETURN)
};

// The states one entity is in (GCharAbnormalState): one slot a kind.
class StateSet;
// A recovery (a potion, its steps, a heal, natural recovery, a life steal, a rising) as its taker gets it: cut by its
// parameters 0x8F (HP: panic's recovery cut) and 0x90 (MP: combustion's), trunc(amount x (1 - cut / 100)), at least 0.
std::uint32_t RecoveryCut(const StateSet& states, std::uint32_t amount, bool hp);

class StateSet {
public:
    using Clock = ActiveState::Clock;

    // Takes a rolled state: into its empty slot, or replacing one of a lower
    // level; refused (false) when the one there is as strong or stronger.
    bool Apply(const ActiveState& state);
    // Ends a state now (a wake-up, a cure); false when it was not in it.
    bool End(StateKind kind);
    // The entity took a hit's damage: unless it carries a time bomb, a sleep
    // ends and a stun one time in four, by the entity's own chance history. The states it
    // ended. (Magical damage ends a root before the hit lands, End(Root).)
    std::vector<StateKind> Damaged(ChanceRolls& ownRolls, std::mt19937& random);
    void Clear();
    // A cure on the entity, its rolls the entity's own (key 0x1A); true when a state ended or lost time.
    bool Cure(const CureSpec& cure, ChanceRolls& ownRolls, std::mt19937& random, Clock::time_point now);
    // The states whose time has passed end (returned), then the ticks due by `now` (burn, poison:
    // every 2 s from the start; bleed every its period; panic and combustion: their drains), against
    // the entity's HP and MP now and its magical absorption (burn), most HP and MP (the drains).
    // Poison never takes the last HP point.
    std::vector<StateKind> Expire(Clock::time_point now);
    std::vector<StateTick> Ticks(Clock::time_point now, std::uint32_t hp, std::uint32_t mp, double magAbsorption);

    std::uint32_t Mask() const { return mask_; }
    bool In(StateKind kind) const { return (mask_ & StateBit(kind)) != 0; }
    const ActiveState* Find(StateKind kind) const;
    std::vector<ActiveState> All() const;

    // What they do to the entity.
    bool Locked() const;     // frozen, asleep or stunned: no move, no action
    bool Rooted() const;     // cannot move (it may act)
    bool Idles(std::uint32_t target) const; // confused, or feared by its target: a monster drops its fight
    int SpeedPercent() const;      // walking and running: frostbite 50, slow 75, else 100
    int AttackTimePercent() const; // the time between its attacks: frostbite 200, slow 125, else 100
    int ReachPercent() const;      // its attacks' reach: myopia's cut
    int Susceptibility() const;    // disease's: added to every state's chance on it
    // Its states' changes to its numbers, into a sheet beside its buffs' (the keeper sums them all;
    // each state its own source, kStateSource + its kind): shock's parry (a percent of it), darkness's
    // hit rate and bleed's defences (percents off), decay's and weaken's defence (flat, never past 70 %
    // of `base`'s), impotent's and division's final factors.
    static constexpr std::uint32_t kStateSource = 0xFF000000u;
    void AddChanges(StatSheet& sheet, const CombatStats& base) const;
    // Its numbers with its states' changes alone.
    void Modify(CombatStats& stats) const;

private:
    std::array<ActiveState, kStateSlots> slots_{};
    std::uint32_t mask_ = 0;
};

} // namespace sr::game
