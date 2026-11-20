#pragma once

// Reuse waits: what the quickslots and the bag draw as a sweep over an icon. Three kinds of
// clock, as the original keeps them:
//  - potion lanes: recovery potions by their TID4 (1 HP, 2 MP, 3 vigour), the cure potions and
//    pills (categories 4..7, 13..15) are shared by every item of the lane, whatever its
//    stack or reference; a use answer starts the lane's clock;
//  - an item's own authored wait (itemdata "COOLTIME:0xGG"): shared by the items of one group,
//    or by one reference when it has no group; started by the same answer;
//  - skills: a cast the server accepted starts the skill's wait (CoolTime, skilldata col 14),
//    shared by the skills of one group.
// An item's wait is gone when it ends; a skill's slot flashes for another 500 ms.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "sr/assets/GameData.h"

namespace sr::assets {

// The lane of an item by its packed type word (TID1..TID4 as ItemTypeWord packs them); 0 when
// it has none: 1..3 recovery, 4..7 and 13..15 cures and pills.
int PotionCategory(std::uint16_t typeWord);

// How long a use of this potion keeps its lane shut (ms): recovery potions 1 s (European ones
// 15 s, percentage ones 4 s), the universal pill 20 s, the other lanes 1 s.
int PotionCooldownMs(int category, const ItemRow& item);

// How long an accepted cast keeps the skill shut: its CoolTime, or the whole action (casting +
// recovery) when that is longer.
int SkillCooldownMs(const SkillRow& skill);

// A wait that was started (reported by whoever saw the answer; the board gives it a clock).
struct CooldownStart {
    enum class Kind { ItemLane, ItemAuthored, Skill };
    Kind kind = Kind::ItemLane;
    int category = 0;         // ItemLane: the lane
    int group = 0;            // ItemAuthored / Skill: the group (0: none)
    std::uint32_t id = 0;     // ItemAuthored: the item's reference; Skill: the skill
    int durationMs = 0;
};

// A running wait as the HUD draws it.
struct CooldownWait {
    double startMs = 0, durationMs = 0;
    double EndMs() const { return startMs + durationMs; }
    // 1 when it has just started, down to 0 at its end.
    double Remaining(double nowMs) const { return durationMs > 0 ? std::max(0.0, EndMs() - nowMs) / durationMs : 0.0; }
};

class CooldownBoard {
public:
    void Start(const CooldownStart& start, double nowMs);
    // The running wait of an item (nullptr: none).
    const CooldownWait* Item(std::uint16_t typeWord, std::uint32_t refObjId, int group, double nowMs) const;
    // The wait of a skill, or its group's; kept 500 ms after its end for the slot's flash.
    const CooldownWait* Skill(std::uint32_t skillId, int group, double nowMs) const;
    void Prune(double nowMs);
    void Clear() { rows_.clear(); }
    std::size_t size() const { return rows_.size(); }

private:
    struct Row {
        CooldownStart start;
        CooldownWait wait;
    };
    std::vector<Row> rows_;
};

// The sweep a slot shows for a wait: while it runs frame `239 - ratio * 239` of a 16 x 16
// atlas (skill_delay), after its end (skills) 16 frames of a 4 x 4 atlas (skill_charge) over
// 500 ms. `frame` is the atlas cell; `flash` selects the second atlas; false when nothing shows.
struct CooldownSweep {
    bool flash = false;
    int frame = 0, columns = 16;
};
bool SweepFor(const CooldownWait& wait, double nowMs, CooldownSweep& out);

// The countdown shown over a quickslot: the digits of the seconds left (minutes beyond a
// minute, hours beyond an hour; `tier` 0 seconds, 1 minutes, 2 hours).
struct CooldownCount {
    int value = 0;
    int tier = 0;
};
bool CountFor(const CooldownWait& wait, double nowMs, CooldownCount& out);

} // namespace sr::assets
