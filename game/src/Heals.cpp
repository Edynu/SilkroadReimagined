#include "sr/game/Heals.h"
#include "sr/game/Pk.h"

#include <algorithm>
#include <cmath>

#include "sr/assets/GameData.h"

namespace sr::game {

HealSpec HealOf(const assets::SkillRow& skill, int healBonus) {
    HealSpec h;
    const assets::SkillParam heal = assets::FindSkillParam(skill, assets::SkillTag("heal"));
    if (!heal) return h;
    h.hp = heal.Int(0);
    h.hpPercent = heal.Int(1) + healBonus; // the passive's to both percents
    h.mp = heal.Int(2);
    h.mpPercent = heal.Int(3) + healBonus;
    h.ofMostHp = static_cast<bool>(assets::FindSkillParam(skill, assets::SkillTag("eshp")));
    if (const assets::SkillParam w = assets::FindSkillParam(skill, assets::SkillTag("mwhh"))) h.weaponHp = w.Int(0);
    if (const assets::SkillParam w = assets::FindSkillParam(skill, assets::SkillTag("mwmh"))) h.weaponMp = w.Int(0);
    return h;
}

HealAmount HealFor(const HealSpec& heal, int sharePercent, const CombatStats& healer, const CombatStats& target) {
    const auto whole = [](double v) { return std::trunc(v); };
    // A transfer's share of the flat amounts (the percents stay).
    double hp = heal.hp, mp = heal.mp;
    if (sharePercent != 100) {
        hp = whole(double(sharePercent) / 100.0 * double(heal.hp));
        mp = whole(double(sharePercent) / 100.0 * double(heal.mp));
    }
    // Its percents: of the amount itself, or (eshp) of the target's most HP (its virtual 0x110; ours:
    // read as the most HP). A percent with no amount is of the most (ours, from the data: the socket stones'
    // heal, 'heal 0 60 0 0', heals 60 % of the HP).
    if (heal.hpPercent != 0) {
        hp = heal.ofMostHp || heal.hp == 0 ? whole(double(target.maxHp) * double(heal.hpPercent) / 100.0) : hp + whole(hp * double(heal.hpPercent) / 100.0);
    }
    if (heal.mpPercent != 0) {
        mp = heal.mp == 0 ? whole(double(target.maxMp) * double(heal.mpPercent) / 100.0) : mp + whole(mp * double(heal.mpPercent) / 100.0);
    }
    // The target's percents more, then the weapon's bonus.
    hp = whole((double(target.healReceived[0]) / 100.0 + 1.0) * hp);
    mp = whole((double(target.healReceived[1]) / 100.0 + 1.0) * mp);
    const auto weapon = [&](int percent) {
        if (percent == 0 || (healer.weaponMagMin == 0.0 && healer.weaponMagMax == 0.0)) return 0.0;
        const double half = double(float((healer.weaponMagMax + healer.weaponMagMin) * 0.5));
        return whole(half * healer.magBalance * double(percent) / 100.0);
    };
    hp += weapon(heal.weaponHp);
    mp += weapon(heal.weaponMp);
    return {std::uint32_t(std::max(0.0, hp)), std::uint32_t(std::max(0.0, mp))};
}

std::uint64_t DeathPenalty(int level, std::uint64_t levelExperience, std::uint32_t monsterExperience, double reducePercent, bool byPlayer, bool fortressWar) {
    DeathFacts f;
    f.level = level;
    f.levelExperience = levelExperience;
    f.monsterExperience = monsterExperience;
    f.reducePercent = reducePercent;
    f.byPlayer = byPlayer;
    f.fortressWar = fortressWar;
    return DeathExperienceLoss(f);
}

ReviveSpec ReviveOf(const assets::SkillRow& skill) {
    ReviveSpec r;
    const assets::SkillParam resu = assets::FindSkillParam(skill, assets::SkillTag("resu"));
    if (!resu) return r;
    r.maxLevel = resu.Int(0);
    r.experiencePercent = resu.Int(1);
    if (const assets::SkillParam heal = assets::FindSkillParam(skill, assets::SkillTag("heal"))) {
        r.hp = heal.Int(0);
        r.hpPercent = heal.Int(1);
        r.mp = heal.Int(2);
        r.mpPercent = heal.Int(3);
    }
    return r;
}

Revival RevivalFor(const ReviveSpec& revive, std::uint32_t maxHp, std::uint32_t maxMp, std::uint64_t lost, bool murderer) {
    Revival out;
    // The amount and its percent of the most (the percent's part cut to a whole number).
    out.hp = std::uint32_t(std::max<std::int64_t>(0, revive.hp + std::int64_t(std::trunc(double(maxHp) * double(revive.hpPercent) / 100.0))));
    out.mp = std::uint32_t(std::max<std::int64_t>(0, revive.mp + std::int64_t(std::trunc(double(maxMp) * double(revive.mpPercent) / 100.0))));
    // The percent (a float) of what the death took, up when anything is left over.
    // The product is kept as a float, as the remainder is.
    float percent = float(double(revive.experiencePercent) / 100.0);
    if (murderer) percent = float(double(percent) * 0.5); // a murderer raised: half
    const float exact = float(double(lost) * double(percent));
    std::uint64_t whole = std::uint64_t(std::trunc(double(exact)));
    if (float(double(exact) - double(whole)) > 9.999999974752427e-07f) ++whole;
    out.experience = whole;
    return out;
}

} // namespace sr::game
