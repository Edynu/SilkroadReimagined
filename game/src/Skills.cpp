#include "sr/game/Skills.h"

#include <algorithm>

#include "sr/assets/GameData.h"

namespace sr::game {

int SkillBook::MasteryLevel(std::uint32_t mastery) const {
    const auto it = masteries.find(mastery);
    return it == masteries.end() ? 0 : it->second;
}

int SkillBook::GroupLevel(std::uint32_t group) const {
    const auto it = groups.find(group);
    return it == groups.end() ? 0 : it->second;
}

int SkillBook::MasteryTotal() const {
    int total = 0;
    for (const auto& [id, level] : masteries) total += level;
    return total;
}

bool SkillBook::Knows(const assets::SkillRow& skill) const {
    return GroupLevel(skill.group) >= skill.level || std::find(itemSkills.begin(), itemSkills.end(), skill.id) != itemSkills.end();
}

bool SkillTraits::Any() const {
    return passive || attack || imbue || buff || debuff || toggle || heal || cure || resurrect || harmony || taunt || stealth || detect ||
           teleport || transform || summon || trap || link || reflect;
}

SkillTraits TraitsOf(const assets::SkillRow& skill) {
    using assets::SkillTag;
    SkillTraits t;
    bool hits = false, damage = false, duration = false, givesStates = false;
    assets::ForEachSkillParam(skill, [&](std::uint32_t code, std::span<const std::uint32_t>) {
        const auto is = [&](std::initializer_list<const char*> tags) {
            for (const char* tag : tags) {
                if (code == SkillTag(tag)) return true;
            }
            return false;
        };
        if (code == SkillTag("att")) hits = true;
        if (code == SkillTag("pdmg") || code == SkillTag("lfst")) damage = true; // its own damage, a life steal
        if (code == SkillTag("efr")) t.area = true;
        if (code == SkillTag("dura")) duration = true;
        if (is({"fz", "fb", "es", "bu", "ps", "se", "rt", "sl", "fe", "my", "bl", "dn", "st", "ds", "ca", "cssr", "csit", "cspd", "csmd", "cshp",
                "csmp", "tb", "kb", "ko", "stns"})) {
            givesStates = true;
        }
        t.toggle = t.toggle || code == SkillTag("onff");
        t.heal = t.heal || code == SkillTag("heal");
        t.cure = t.cure || is({"cura", "curt", "curl"});
        t.resurrect = t.resurrect || code == SkillTag("resu");
        t.harmony = t.harmony || code == SkillTag("pola");
        t.taunt = t.taunt || is({"tant", "tnt2", "dtnt"});
        t.stealth = t.stealth || code == SkillTag("hide");
        t.detect = t.detect || is({"dtt", "dttp"});
        t.teleport = t.teleport || is({"tele", "tel2", "tel3", "tel4"});
        t.transform = t.transform || is({"msch", "mcap"});
        t.summon = t.summon || code == SkillTag("summ");
        t.trap = t.trap || code == SkillTag("trap");
        t.link = t.link || code == SkillTag("lnks");
        t.reflect = t.reflect || is({"lkdr", "lkag", "odar", "pwtt", "mwtt", "pwdt", "mwdt", "mwhh", "mwmh", "mwhs"});
    });
    t.passive = skill.activity == 0;
    t.chain = skill.chainNext != 0;
    t.imbue = IsImbue(skill);
    t.attack = !t.passive && !t.imbue && !t.trap && (hits || damage);
    t.states = givesStates;
    const bool enemy = skill.targetEnemyMonster || skill.targetEnemyPlayer;
    t.buff = !t.passive && duration && !t.attack && !t.imbue && !t.trap;
    // States without an attack: on an enemy, or around the caster (a poison cloud: no target).
    t.debuff = !t.passive && !t.attack && givesStates && (enemy || t.area);
    if (skill.targetDeadBody || t.resurrect) {
        t.target = SkillTraits::Target::DeadBody;
    } else if (enemy && (t.attack || t.debuff || skill.targetRequired)) {
        t.target = SkillTraits::Target::Enemy;
    } else if (skill.targetParty && !skill.targetRequired) {
        t.target = SkillTraits::Target::Party;
    } else if ((skill.targetAlly || skill.targetParty) && skill.targetRequired) {
        t.target = SkillTraits::Target::Ally;
    } else if (!skill.targetRequired && t.area && !t.buff) {
        t.target = SkillTraits::Target::Area;
    }
    return t;
}

bool IsImbue(const assets::SkillRow& skill) { return skill.activity == 1 && skill.attack && skill.hasDuration; }

std::vector<std::uint32_t> SkillBook::Skills(const assets::SkillTable& table) const {
    std::vector<std::uint32_t> out;
    for (const auto& [group, level] : groups) {
        if (const assets::SkillRow* row = table.GroupLevel(group, level)) out.push_back(row->id);
    }
    return out;
}

int MasteryAllowance(int country, int level) { return country == 0 ? 330 : std::min(240, 2 * level); }

bool MasteryOfRace(const assets::SkillTable& table, std::uint32_t mastery, int country) {
    if (table.Mastery(mastery) == nullptr) return false;
    for (const assets::SkillRow* row : table.Board(mastery)) {
        if (row->race == country || row->race == 3) return true;
    }
    return false;
}

std::uint32_t MasteryCost(const std::vector<std::uint32_t>& masteryCosts, int current) {
    if (current <= 0) return 0; // the first level comes with the mastery
    return std::size_t(current) < masteryCosts.size() ? masteryCosts[std::size_t(current)] : ~0u;
}

SkillCheck CanRaiseMastery(const SkillBook& book, const assets::SkillTable& table, const std::vector<std::uint32_t>& masteryCosts,
                           std::uint32_t mastery, int country, int level, std::uint32_t skillPoints) {
    if (!MasteryOfRace(table, mastery, country)) return table.Mastery(mastery) == nullptr ? SkillCheck::Unknown : SkillCheck::Race;
    const int current = book.MasteryLevel(mastery);
    if (current >= level) return SkillCheck::MasteryLevel;
    if (book.MasteryTotal() + 1 > MasteryAllowance(country, level)) return SkillCheck::MasteryTotal;
    const std::uint32_t cost = MasteryCost(masteryCosts, current);
    if (cost == ~0u) return SkillCheck::Unknown;
    if (skillPoints < cost) return SkillCheck::NoSkillPoints;
    return SkillCheck::Ok;
}

SkillCheck CanLearnSkill(const SkillBook& book, const assets::SkillTable& table, const assets::SkillRow& skill, int country,
                         std::uint32_t skillPoints, int strength, int intelligence) {
    if (skill.spCost <= 0 || skill.group == 0) return SkillCheck::Unknown; // not a trainable row
    if (skill.race != country && skill.race != 3) return SkillCheck::Race;
    const int known = book.GroupLevel(skill.group);
    if (known >= skill.level) return SkillCheck::Learned;
    if (known + 1 != skill.level) return SkillCheck::Unknown; // one level at a time
    for (std::size_t i = 0; i < skill.masteries.size(); ++i) {
        if (skill.masteries[i] != 0 && book.MasteryLevel(skill.masteries[i]) < skill.masteryLevels[i]) return SkillCheck::MasteryLevel;
    }
    for (std::size_t i = 0; i < skill.prerequisiteGroups.size(); ++i) {
        if (skill.prerequisiteGroups[i] != 0 && book.GroupLevel(skill.prerequisiteGroups[i]) < skill.prerequisiteLevels[i]) {
            return SkillCheck::Prerequisite;
        }
    }
    (void)table;
    if (strength < skill.requiredStr) return SkillCheck::Strength;
    if (intelligence < skill.requiredInt) return SkillCheck::Intelligence;
    if (skillPoints < std::uint32_t(skill.spCost)) return SkillCheck::NoSkillPoints;
    return SkillCheck::Ok;
}

} // namespace sr::game
