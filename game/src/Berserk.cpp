#include "sr/game/Berserk.h"

#include <algorithm>

namespace sr::game {

BerserkGain BerserkPointGain(int playerLevel, int monsterLevel, std::uint8_t rarity, int chanceFactor) {
    BerserkGain g;
    double percent = 8.0;
    // the monster more than 3 levels below the player.
    if (monsterLevel - playerLevel < -3) percent = std::clamp(8.0 - 0.25 * double(playerLevel - monsterLevel - 5), 0.25, 8.0);
    g.chance = int(percent * 100.0);
    switch (rarity) {
    case 3: // a unique (and kind 8): all five, always
    case 8:
        g.points = kBerserkPoints;
        g.chance = 10'000'000;
        break;
    case 4: g.chance *= 5; break; // a giant
    case 6: g.chance *= 2; break; // an elite
    default: break;
    }
    if (chanceFactor != 0) g.chance = g.chance * chanceFactor / 100;
    return g;
}

HwanLevel HwanLevelOf(const std::vector<HwanLevel>& table, int level) {
    HwanLevel best;
    for (const HwanLevel& h : table) {
        if (h.level <= level && h.level >= best.level) best = h;
    }
    return best;
}

} // namespace sr::game
