#include "sr/game/Cos.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "sr/assets/GameData.h"
#include "sr/game/TimedServices.h"

namespace sr::game {

CosKind CosKindOf(const assets::ObjectRow& row) {
    if (row.typeId[0] != 1 || row.typeId[1] != 2 || row.typeId[2] != 3) return CosKind::None;
    return row.typeId[3] >= 1 && row.typeId[3] <= 5 ? CosKind(row.typeId[3]) : CosKind::None;
}

bool IsStableNpc(std::string_view codename) {
    for (const std::string_view part : {"NPC_CH_HORSE", "NPC_WC_HORSE", "NPC_KT_HORSE", "NPC_EU_HORSE", "NPC_CA_HORSE", "NPC_SD_M_AREA_HORSE"}) {
        if (codename.find(part) != std::string_view::npos) return true;
    }
    return false;
}

bool IsCosScroll(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 3, 2}; }

CosKind PetOfItem(const assets::ItemRow& row) {
    if (row.typeId[0] != 3 || row.typeId[1] != 2 || row.typeId[2] != 1) return CosKind::None;
    return row.typeId[3] == 1 ? CosKind::AttackPet : row.typeId[3] == 2 ? CosKind::GrabPet : CosKind::None;
}

std::string PetCodename(const assets::ItemRow& row) {
    std::string_view name = row.paramTexts[0];
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) name.remove_suffix(1);
    return std::string(name);
}

int PetRentalMinutes(const assets::ItemRow& row) { return PetOfItem(row) == CosKind::GrabPet && row.params[0] > 0 ? row.params[0] : 0; }

PetGrowth GrowPet(int level, std::uint64_t experience, std::uint64_t gained, int ownerLevel, std::span<const std::uint64_t> expToNext) {
    PetGrowth g{level, experience + gained, 0};
    const auto need = [&](int l) -> std::uint64_t { return l > 0 && std::size_t(l) < expToNext.size() ? expToNext[std::size_t(l)] : 0; };
    while (need(g.level) > 0 && g.experience >= need(g.level)) {
        if (g.level >= ownerLevel || g.level >= kAttackPetGrowthLimit || g.level >= kAttackPetMaxLevel) {
            g.experience = need(g.level); // full, and no further
            break;
        }
        g.experience -= need(g.level);
        ++g.level;
        ++g.levelsGained;
    }
    return g;
}

std::string PetCodenameAtLevel(std::string_view codename, int level) {
    const std::size_t underscore = codename.rfind('_');
    if (underscore == std::string_view::npos || codename.size() - underscore != 4) return {};
    for (std::size_t i = underscore + 1; i < codename.size(); ++i) {
        if (codename[i] < '0' || codename[i] > '9') return {};
    }
    char digits[8];
    std::snprintf(digits, sizeof digits, "%03d", std::clamp(level, 0, 999));
    return std::string(codename.substr(0, underscore + 1)) + digits;
}

double PetHgpPerSecond(const assets::ObjectRow& row) { return row.except4 > 0 ? 100.0 / (double(row.except4) * 60.0) : 0.0; }

bool IsPetHgpPotion(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 1, 9}; }

int PetHgpPotionAmount(const assets::ItemRow& row) { return std::clamp(row.params[0], 0, 100) * kCosHgpFull / 100; }

bool IsPetHpPotion(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 1, 4}; }

std::uint64_t PetDeathPenalty(std::uint64_t levelExperience, std::uint32_t monsterExperience, bool byPlayer, bool murderer) {
    // The rates as the server keeps them (doubles: 0.0199999995, 0.0040000002, 0.0599999987)
    // and the cap's factors (x 20, x 300, x 100).
    constexpr double k2 = 0.019999999552965164, k04 = 0.004000000189989805, k6 = 0.05999999865889549;
    const double rate = byPlayer ? (murderer ? k2 : k04) : (murderer ? k6 : k2);
    const std::int64_t factor = byPlayer ? (murderer ? 100 : 20) : (murderer ? 300 : 100);
    const std::int64_t half = std::int64_t(double(levelExperience) * rate) / 2;
    if (half < 1) return 0;
    const std::int64_t cap = std::max<std::int64_t>(1, std::int64_t(monsterExperience) * factor);
    return std::uint64_t(std::min(cap, half));
}

PetTreatment PetTreatmentOf(const assets::ItemRow& row) {
    if (row.typeId[0] != 3 || row.typeId[1] != 3) return PetTreatment::None;
    if (row.typeId[2] == 1 && row.typeId[3] == 6) return PetTreatment::Revival;
    if (row.typeId[2] == 13 && row.typeId[3] == 12) return PetTreatment::Extension;
    return PetTreatment::None;
}

bool IsPetMallItem(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 13, 15}; }

PetItemJobs PetItemJobsOf(const assets::ItemRow& row) {
    PetItemJobs jobs;
    if (!IsPetMallItem(row)) return jobs;
    jobs.attackPet = (row.params[5] & 1) != 0;
    jobs.grabPet = (row.params[5] & 2) != 0;
    // Param2's text, as the premium's parts are written: [TAG:target,args...].
    for (const PremiumPart& part : PremiumPartsOf(row)) {
        if (part.tag == "PSU1" && jobs.skill.empty()) {
            jobs.skill = part.target;
            jobs.skillByLevel = !part.args.empty() && std::atoi(part.args[0].c_str()) != 0;
        } else if (part.tag == "BCS1" && part.target == "INC_INVENTORY_SIZE" && part.args.size() >= 3) {
            jobs.bagPlaces = std::max(0, std::atoi(part.args[2].c_str())); // A, the seconds, the places
        }
    }
    return jobs;
}

std::string PetSkillCodename(const PetItemJobs& jobs, int petLevel) {
    if (!jobs.skillByLevel || jobs.skill.empty()) return jobs.skill;
    char digits[8];
    std::snprintf(digits, sizeof digits, "%03d", std::clamp(petLevel, 0, 255)); // the level as a byte
    return jobs.skill + "_" + digits;
}

int PetWatchMinutes(int count, std::int64_t rentSecondsLeft) {
    const std::int64_t minutes = std::max<std::int64_t>(0, rentSecondsLeft / 60);
    return int(std::min<std::int64_t>(std::max(0, count), minutes));
}

PetStats PetStatsOf(const assets::ObjectRow& row, std::span<const assets::SkillRow* const> skills, int hgp) {
    PetStats s;
    s.hungry = hgp <= kCosHungry;
    const int shift = s.hungry ? 1 : 0;
    bool phy = false, mag = false;
    for (const assets::SkillRow* skill : skills) {
        if (skill == nullptr || !skill->attack) continue;
        if (skill->attackKind == 10) {
            if (mag) continue;
            s.magMin = skill->attackMin >> shift;
            s.magMax = skill->attackMax >> shift;
            mag = true;
        } else {
            if (phy) continue;
            s.phyMin = skill->attackMin >> shift;
            s.phyMax = skill->attackMax >> shift;
            phy = true;
        }
        if (phy && mag) break;
    }
    s.phyDefence = int(row.phyDefence) / (1 << shift);
    s.magDefence = int(row.magDefence) / (1 << shift);
    s.hitRate = int(row.hitRate) / (1 << shift);
    s.parry = int(row.evasion) / (1 << shift);
    return s;
}

std::optional<std::string> CosOfScroll(const assets::ItemRow& row, int level) {
    if (!IsCosScroll(row)) return std::nullopt;
    std::string_view name = row.paramTexts[0];
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) name.remove_suffix(1);
    if (name.empty()) return std::nullopt;
    // The level ladder, when there is one: the highest step reached.
    std::string_view ladder = row.paramTexts[1];
    int step = -1;
    bool any = false;
    while (!ladder.empty()) {
        while (!ladder.empty() && (ladder.front() == ' ' || ladder.front() == ',')) ladder.remove_prefix(1);
        int value = 0;
        const auto [end, ec] = std::from_chars(ladder.data(), ladder.data() + ladder.size(), value);
        if (ec != std::errc{}) break;
        any = true;
        if (value <= level && value > step) step = value;
        ladder.remove_prefix(std::size_t(end - ladder.data()));
    }
    if (!any) return std::string(name);
    if (step < 0) return std::nullopt;
    return std::string(name) + "_" + std::to_string(step);
}

} // namespace sr::game
