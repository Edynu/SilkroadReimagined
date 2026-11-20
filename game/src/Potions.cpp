#include "sr/game/Potions.h"

#include <algorithm>
#include <cmath>

#include "sr/assets/GameData.h"
#include "sr/game/Cos.h"

namespace sr::game {

namespace {
int Param(const assets::ItemRow& item, int n) { return std::max(0, item.params[std::size_t(n - 1)]); } // -1: none
bool Recovery3(const assets::ItemRow& item, int tid3) { return item.typeId[0] == 3 && item.typeId[1] == 3 && item.typeId[2] == tid3; }
} // namespace

bool IsPotion(const assets::ItemRow& item) { return Recovery3(item, 1); }
bool IsCure(const assets::ItemRow& item) { return Recovery3(item, 2); }

ReuseLock ReuseLockOf(const assets::ItemRow& item, bool european) {
    if (IsPotion(item)) {
        std::uint32_t bit = 0;
        switch (item.typeId[3]) {
        case 1: bit = 0x01; break;
        case 2: bit = 0x02; break;
        case 3: bit = 0x04; break;
        case 4: bit = 0x08; break;
        case 5: bit = 0x10; break;
        case 7: bit = 0x20; break;
        default: return {};
        }
        // The pets' (used on a COS): 1.1 s whatever the kind.
        if (bit >= 0x08) return {bit, 1100};
        const bool flat = Param(item, 1) != 0 || Param(item, 3) != 0;
        return {bit, !flat ? 4100 : european ? 15100 : 1100};
    }
    if (IsCure(item)) return item.typeId[3] == 1 ? ReuseLock{0x100, 20100} : ReuseLock{0x40, 1100};
    return {};
}

Recovery RecoveryOf(const assets::ItemRow& item, int level, double strength, double intelligence, std::uint32_t maxHp, std::uint32_t maxMp) {
    Recovery r;
    const int hp = Param(item, 1), mp = Param(item, 3);
    if (hp != 0 || mp != 0) {
        r.flat = true;
        const double grow = std::pow(1.02, double(std::max(1, level) - 1));
        r.hp = std::uint32_t(std::trunc(double(hp) * grow * (1.0 + strength / 416.0)));
        r.mp = std::uint32_t(std::trunc(double(mp) * grow * (intelligence / 416.0 + 1.0)));
        return r;
    }
    r.hp = std::uint32_t(std::trunc(double(maxHp) * double(Param(item, 2)) / 100.0));
    r.mp = std::uint32_t(std::trunc(double(maxMp) * double(Param(item, 4)) / 100.0));
    return r;
}

std::uint32_t PotionStep(std::uint32_t amount) { return std::clamp<std::uint32_t>(amount / std::uint32_t(kPotionSteps), 1u, 1'000'000u); }

CureSpec PillCureOf(const assets::ItemRow& item) {
    CureSpec c;
    if (!IsCure(item)) return c;
    if (item.typeId[3] == 6 || item.typeId[3] == 7) {
        for (int k = 0; k <= int(StateKind::Zombie); ++k) c.elementMask |= StateBit(StateKind(k));
        c.elementLevel = Param(item, 1);
    } else if (item.typeId[3] == 8) {
        // The mall's bag: the server takes it as the universal pill (ApplyRecoveryOrCure
        // every TID4 but 1), its own Param1..6 the six states' levels, each read as 16 bits
        // (movzx): the bags' -1 in Param1 and Param2 (65,535) end freeze
        // and frostbite outright, their Param3..6 are 0. The pills Desc1 and Desc2 name are not read. The data's levels
        // are equal where they are not 0 (one level for the mask).
        for (int k = 0; k <= int(StateKind::Zombie); ++k) {
            const int level = int(std::uint16_t(item.params[std::size_t(k)]));
            if (level == 0) continue;
            c.elementMask |= StateBit(StateKind(k));
            c.elementLevel = std::max(c.elementLevel, level);
        }
    } else if (item.typeId[3] == 1) {
        c.curseMask = std::uint32_t(item.params[0]);
        c.curseLevel = Param(item, 2);
        c.curseChance = Param(item, 3);
        c.most = 1;
    }
    return c;
}

void AutoPotion::Clamp() {
    for (int r = 0; r < kRows; ++r) {
        Entry& e = rows[std::size_t(r)];
        e.percent = r == kAbnormal ? std::uint8_t(std::min<int>(e.percent, 100)) : std::uint8_t(std::clamp<int>(e.percent, 1, 100));
    }
    const int ms = std::clamp<int>(delayMs, kDelayMinMs, kDelayMaxMs);
    delayMs = std::uint16_t((ms + kDelayStepMs / 2) / kDelayStepMs * kDelayStepMs);
}

std::uint16_t EncodeAutoPotionWord(const AutoPotion::Entry& entry, int quickSlot) {
    int word = (entry.percent & 0x7F) << 8;
    if (quickSlot > 0) {
        // Slot 1..40 as the bar's page and key: 10 is page 1 key 10 (the 0 key).
        int page = quickSlot / 10 + 1, key = quickSlot % 10;
        if (key == 0) {
            key = 10;
            page = quickSlot / 10;
        }
        word |= (page & 0xF) << 4 | (key & 0xF);
    }
    if (entry.enabled) word |= 0x8000;
    return std::uint16_t(word);
}

AutoPotion::Entry DecodeAutoPotionWord(std::uint16_t word, int* quickSlot) {
    AutoPotion::Entry e;
    e.enabled = (word & 0x8000) != 0;
    e.percent = std::uint8_t((word >> 8) & 0x7F);
    if (quickSlot != nullptr) {
        const int page = (word >> 4) & 0xF, key = word & 0xF;
        *quickSlot = page == 0 && key == 0 ? 0 : std::min(40, (page - 1) * 10 + key);
    }
    return e;
}

std::uint8_t EncodeAutoPotionDelay(bool enabled, int ms) {
    const int tenths = std::clamp(ms / 100, 0, 0x7F);
    return std::uint8_t((enabled ? 0x80 : 0) | tenths);
}

void DecodeAutoPotionDelay(std::uint8_t byte, bool& enabled, int& ms) {
    enabled = (byte & 0x80) != 0;
    ms = (byte & 0x7F) * 100;
}

std::uint32_t AutoPotionThreshold(std::uint32_t most, int percent) {
    return std::uint32_t(std::trunc(double(most) * double(std::clamp(percent, 0, 100)) / 100.0));
}

proto::AutoPotionSettings ToMessage(const AutoPotion& settings) {
    proto::AutoPotionSettings m;
    proto::AutoPotionRow* rows[AutoPotion::kRows] = {&m.hp, &m.mp, &m.abnormal, &m.vehicle, &m.attackPet};
    for (int r = 0; r < AutoPotion::kRows; ++r) {
        const AutoPotion::Entry& e = settings.rows[std::size_t(r)];
        *rows[r] = {e.enabled, e.percent, e.item};
    }
    m.delayEnabled = settings.delayEnabled;
    m.delayMs = settings.delayMs;
    return m;
}

AutoPotion FromMessage(const proto::AutoPotionSettings& message) {
    AutoPotion s;
    const proto::AutoPotionRow* rows[AutoPotion::kRows] = {&message.hp, &message.mp, &message.abnormal, &message.vehicle, &message.attackPet};
    for (int r = 0; r < AutoPotion::kRows; ++r) s.rows[std::size_t(r)] = {rows[r]->enabled, rows[r]->percent, rows[r]->item};
    s.delayEnabled = message.delayEnabled;
    s.delayMs = message.delayMs;
    return s;
}

bool AutoPotionFits(int row, const assets::ItemRow& item) {
    const int kind = item.typeId[3];
    switch (row) {
    case AutoPotion::kHp: return IsPotion(item) && (kind == 1 || kind == 3);
    case AutoPotion::kMp: return IsPotion(item) && (kind == 2 || kind == 3);
    case AutoPotion::kAbnormal: return IsCure(item) && (kind == 1 || kind == 6);
    case AutoPotion::kVehicle:
    case AutoPotion::kAttackPet: return IsPetHpPotion(item);
    default: return false;
    }
}

} // namespace sr::game
