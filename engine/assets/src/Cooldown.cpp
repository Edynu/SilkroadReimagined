#include "sr/assets/Cooldown.h"

#include <algorithm>

namespace sr::assets {

int PotionCategory(std::uint16_t typeWord) {
    const int sub = typeWord >> 11;
    const int family = typeWord & 0x7FE;
    if (family == 0xEC) { // TID1 3, TID2 3, TID3 1: potions
        if (sub >= 1 && sub <= 3) return sub;
        switch (sub) {
        case 4: return 4;
        case 5: return 5;
        case 7: return 6;
        case 9: return 7;
        default: return 0;
        }
    }
    if (family == 0x16C) { // TID3 2: pills
        switch (sub) {
        case 1: return 13;
        case 6: return 14;
        case 7: return 15;
        default: return 0;
        }
    }
    return 0;
}

int PotionCooldownMs(int category, const ItemRow& item) {
    if (category >= 1 && category <= 3) {
        const bool percentage = item.params[1] != 0 || item.params[3] != 0;
        if (percentage) return 4000;
        return item.country == 1 ? 15000 : 1000;
    }
    return category == 13 ? 20000 : 1000;
}

int SkillCooldownMs(const SkillRow& skill) {
    // The authored CoolTime alone: the client's reuse manager starts the skill's (or its
    // group's) row with it when the server accepts the cast; the action's own time is the
    // cast's, not the slot's.
    return skill.cooldownMs;
}

void CooldownBoard::Start(const CooldownStart& start, double nowMs) {
    if (start.durationMs <= 0) return;
    Prune(nowMs);
    std::erase_if(rows_, [&](const Row& r) {
        if (r.start.kind != start.kind) return false;
        switch (start.kind) {
        case CooldownStart::Kind::ItemLane: return r.start.category == start.category;
        case CooldownStart::Kind::ItemAuthored:
            return start.group != 0 ? r.start.group == start.group : r.start.group == 0 && r.start.id == start.id;
        case CooldownStart::Kind::Skill: return r.start.id == start.id || (start.group != 0 && r.start.group == start.group);
        }
        return false;
    });
    rows_.push_back({start, {nowMs, static_cast<double>(start.durationMs)}});
}

const CooldownWait* CooldownBoard::Item(std::uint16_t typeWord, std::uint32_t refObjId, int group, double nowMs) const {
    const int category = PotionCategory(typeWord);
    for (const Row& r : rows_) {
        if (nowMs >= r.wait.EndMs()) continue;
        if (category != 0) {
            if (r.start.kind == CooldownStart::Kind::ItemLane && r.start.category == category) return &r.wait;
        } else if (r.start.kind == CooldownStart::Kind::ItemAuthored &&
                   (group != 0 ? r.start.group == group : r.start.group == 0 && r.start.id == refObjId)) {
            return &r.wait;
        }
    }
    return nullptr;
}

const CooldownWait* CooldownBoard::Skill(std::uint32_t skillId, int group, double nowMs) const {
    for (const Row& r : rows_) {
        if (r.start.kind != CooldownStart::Kind::Skill) continue;
        if (nowMs >= r.wait.EndMs() + 500.0) continue;
        if (r.start.id == skillId || (group != 0 && r.start.group == group)) return &r.wait;
    }
    return nullptr;
}

void CooldownBoard::Prune(double nowMs) {
    std::erase_if(rows_, [&](const Row& r) { return nowMs >= r.wait.EndMs() + 500.0; });
}

bool SweepFor(const CooldownWait& wait, double nowMs, CooldownSweep& out) {
    const double end = wait.EndMs();
    if (wait.durationMs <= 0 || nowMs >= end + 500.0) return false;
    const bool active = nowMs < end;
    const double ratio = std::min(1.0, active ? (end - nowMs) / wait.durationMs : (end + 500.0 - nowMs) / 500.0);
    const int count = active ? 239 : 15;
    out.flash = !active;
    out.columns = active ? 16 : 4;
    out.frame = std::clamp(static_cast<int>(static_cast<float>(count) - static_cast<float>(ratio) * static_cast<float>(count)), 0, count);
    return true;
}

bool CountFor(const CooldownWait& wait, double nowMs, CooldownCount& out) {
    const double left = wait.EndMs() - nowMs;
    if (left <= 0.0) return false;
    const int seconds = static_cast<int>(left / 1000.0);
    out.tier = seconds > 3600 ? 2 : seconds > 60 ? 1 : 0;
    out.value = seconds > 3600 ? seconds / 3600 : seconds > 60 ? seconds / 60 : seconds;
    return true;
}

} // namespace sr::assets
