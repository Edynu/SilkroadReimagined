// Passives (K6): the conditions on what is worn, the
// named values and what they do to the skills that read them (game/Passives.h), each as an exact
// number; the variance's and the poison's additions (game/Combat.h, game/States.h).

#include <doctest/doctest.h>

#include <initializer_list>
#include <random>

#include "sr/assets/GameData.h"
#include "sr/game/Buffs.h"
#include "sr/game/Combat.h"
#include "sr/game/Passives.h"
#include "sr/game/States.h"

using namespace sr;

namespace {

std::uint32_t T(const char* tag) { return assets::SkillTag(tag); }

assets::SkillRow Skill(std::initializer_list<std::uint32_t> tail) {
    assets::SkillRow s;
    s.id = 1;
    s.params.assign(tail.begin(), tail.end());
    return s;
}

game::SkillModifiers Values(std::initializer_list<std::pair<const char*, int>> values) {
    game::SkillModifiers m;
    for (const auto& [code, v] : values) m.Set(T(code), v);
    return m;
}

} // namespace

TEST_CASE("passives: the conditions on what is worn ('reqi', 'reqn')") {
    game::WornKinds worn;
    worn.weapon = 7; // a European one-handed sword
    worn.shield = 2; // a European shield
    CHECK(game::EquipmentFits(Skill({}), worn));                                     // none: always
    CHECK(game::EquipmentFits(Skill({T("reqi"), 6, 7}), worn));                      // the weapon's sort
    CHECK_FALSE(game::EquipmentFits(Skill({T("reqi"), 6, 8}), worn));
    CHECK(game::EquipmentFits(Skill({T("reqi"), 6, 8, T("reqi"), 6, 7}), worn));     // any one of them
    CHECK(game::EquipmentFits(Skill({T("reqi"), 4, 2}), worn));                      // the shield's
    CHECK_FALSE(game::EquipmentFits(Skill({T("reqi"), 4, 1}), worn));                // a Chinese shield
    CHECK_FALSE(game::EquipmentFits(Skill({T("reqi"), 5, 0}), worn));                // a kind it does not know
    // Armour: sort 0 all six parts of the kind; another sort, something on that part.
    for (std::size_t part = 1; part <= 6; ++part) worn.armour[part] = 9;
    CHECK(game::EquipmentFits(Skill({T("reqi"), 9, 0}), worn));
    worn.armour[4] = 10;
    CHECK_FALSE(game::EquipmentFits(Skill({T("reqi"), 9, 0}), worn));
    CHECK(game::EquipmentFits(Skill({T("reqi"), 10, 4}), worn));
    worn.armour[4] = 0;
    CHECK_FALSE(game::EquipmentFits(Skill({T("reqi"), 9, 4}), worn));
    // 'reqn': every one (the Cleric's: its armour on all six and a rod).
    worn.armour[4] = 9;
    worn.weapon = 15;
    CHECK(game::EquipmentFits(Skill({T("reqi"), 9, 0, T("reqi"), 6, 15, T("reqn")}), worn));
    worn.weapon = 7;
    CHECK_FALSE(game::EquipmentFits(Skill({T("reqi"), 9, 0, T("reqi"), 6, 15, T("reqn")}), worn));
    CHECK(game::EquipmentFits(Skill({T("reqi"), 9, 0, T("reqi"), 6, 15}), worn)); // without it, one is enough
}

TEST_CASE("passives: the named values and what they do to a skill that reads them (getv)") {
    game::SkillModifiers m = Values({{"E1SA", 40}, {"WIMD", 17}, {"WIRU", 30}, {"CBRA", 10}});
    m.Set(T("E1SA"), 50); // a later one takes its place
    CHECK(m.Find(T("E1SA")) == 50);
    CHECK_FALSE(m.Find(T("DGAT")));
    // A physical skill: the first attack percent it names decides; one it names without a value: none.
    CHECK(game::BoostsOf(Skill({T("getv"), T("E1SA")}), false, m).pointFactor == doctest::Approx(1.5));
    CHECK(game::BoostsOf(Skill({T("getv"), T("DGAT"), T("getv"), T("E1SA")}), false, m).pointFactor == 1.0);
    // A magical one: each element's percent in turn; the hit rates add.
    const game::SkillModifiers magic = Values({{"COAT", 20}, {"FIAT", 50}, {"DGHR", 9}, {"E2AH", 13}});
    const assets::SkillRow both = Skill({T("getv"), T("COAT"), T("getv"), T("FIAT"), T("getv"), T("DGHR"), T("getv"), T("E2AH")});
    CHECK(game::BoostsOf(both, true, magic).pointFactor == doctest::Approx(1.2 * 1.5));
    CHECK(game::BoostsOf(both, true, magic).spread == 22);
    CHECK(game::BoostsOf(both, false, magic).spread == 9); // the physical one: the first it names
    // Its reach, its MP (each cut a whole number), a toggle's MP.
    const assets::SkillRow wizard = Skill({T("getv"), T("WIMD"), T("getv"), T("WIRU")});
    CHECK(game::BoostsOf(wizard, true, m).range == 30);
    CHECK(game::CutMana(1000, game::BoostsOf(wizard, true, m).manaCuts) == 830);
    CHECK(game::CutMana(1000, {10, 10}) == 810);
    CHECK(game::CutMana(333, {17}) == 276); // 276.39
    // Its effect's time: the milliseconds named first, else the percent of its 'dura'.
    const game::SkillModifiers times = Values({{"RPBU", 3000}, {"DTDR", 80}});
    CHECK(game::ExtendedMs(6000, game::BoostsOf(Skill({T("getv"), T("RPBU")}), false, times)) == 9000u);
    CHECK(game::ExtendedMs(12000, game::BoostsOf(Skill({T("getv"), T("DTDR")}), true, times)) == 21600u);
    CHECK(game::ExtendedMs(12000, game::BoostsOf(Skill({T("getv"), T("STDU"), T("getv"), T("DTDR")}), true, times)) == 12000u);
    // The poison's, its own damage, a life steal, the blesses.
    const game::SkillModifiers rogue = Values({{"RPTU", 30}, {"RPDU", 19}, {"SAAA", 28}, {"BSHP", 21}, {"HLFS", 15}, {"HLSM", 142}});
    const game::SkillBoosts b = game::BoostsOf(
        Skill({T("getv"), T("RPTU"), T("getv"), T("RPDU"), T("getv"), T("SAAA"), T("getv"), T("BSHP"), T("getv"), T("HLFS"), T("getv"), T("HLSM")}), false, rogue);
    CHECK(b.poisonLevel == 30);
    CHECK(b.poisonDamage == 19);
    CHECK(b.fixedDamage == 28);
    CHECK(b.lifeSteal == 21);
    CHECK(b.bonus.strength == 15.0f);
    CHECK(b.bonus.magDefence == 142.0f);
    // A skill that reads nothing, or a player of no values: nothing.
    CHECK(game::BoostsOf(Skill({T("dura"), 5000}), false, m).pointFactor == 1.0);
    CHECK(game::BoostsOf(wizard, true, {}).manaCuts.empty());
}

TEST_CASE("passives: a bless's bonus joins its buff before the cap") {
    const assets::SkillRow str = Skill({T("stri"), 20, 30, T("defp"), 100, 50, 0});
    game::BuffBasis basis{100.0f, 80.0f, 1000.0f, 500.0f, 0.0f};
    game::BuffBonus bonus;
    bonus.strength = 15.0f;
    bonus.phyDefence = 89.0f;
    const std::vector<game::StatChange> c = game::BuffChanges(str, basis, bonus);
    CHECK(c[0].value == 30.0f); // 20 + 15 = 35, at most 30 % of 100
    CHECK(c[1].value == 189.0f);
    CHECK(c[2].value == 50.0f);
}

TEST_CASE("passives: the variance's additions and the poison's") {
    game::CombatStats attacker;
    attacker.level = 10;
    attacker.phyAttackMin = 1000;
    attacker.phyAttackMax = 2000;
    attacker.hitRate = 100;
    game::CombatStats defender;
    defender.level = 10;
    defender.phyAbsorption = 100;
    defender.phyDefence = 100;
    defender.evasion = 100;
    game::AttackSpec a;
    a.kind = 5;
    // A roll moved past the top: the point is the most, 2000 / 2 - 100; times the attack percent's 1.5.
    a.spreadBonus = 200;
    std::mt19937 random(5);
    CHECK(game::RollHit(attacker, defender, a, {}, random).damage == 900u);
    a.pointFactor = 1.5;
    CHECK(game::RollHit(attacker, defender, a, {}, random).damage == 1400u);
    // The poison: its level after the resistance's cut, plus the passive's, less the grade; a second each.
    game::StateGift poison;
    poison.kind = game::StateKind::Poison;
    poison.words = {100, 100, 25};
    game::StateDefence target;
    target.elementResist[4] = 50;
    CHECK(game::OddsOf(poison, target, 0).level == 50);
    poison.levelBonus = 30;
    CHECK(game::OddsOf(poison, target, 0).level == 80);
    CHECK(game::OddsOf(poison, target, 0).durationMs == 80'000u);
}
