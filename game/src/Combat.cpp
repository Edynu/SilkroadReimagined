#include "sr/game/Combat.h"

#include <algorithm>
#include <cmath>

#include "sr/assets/GameData.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game {

namespace {

double Clamp(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

// The armour parts' default reinforcements (physical, magical; percent) where nothing is worn, places 0..5 (head, chest,
// shoulders, hands, legs, feet): the original's table indexed by the part's TID4.
constexpr std::array<std::array<double, 2>, 6> kPartDefaults{{{3.2, 5.2}, {4.2, 6.7}, {2.7, 4.3}, {2.5, 3.9}, {3.4, 5.5}, {3.0, 4.9}}};

double LevelAdvantage(int attacker, int defender) {
    return attacker <= defender ? 0.0 : std::min(double(attacker - defender) * 0.03, 0.30);
}

} // namespace

std::uint32_t PlayerVitalFrom(int level, double part) {
    return std::uint32_t(std::max(1.0, std::trunc(std::pow(1.02, double(std::max(1, level) - 1)) * part * 10.0)));
}

std::uint32_t PlayerMaxHp(int level, int strength) { return PlayerVitalFrom(level, double(strength)); }

std::uint32_t PlayerMaxMp(int level, int intelligence) { return PlayerVitalFrom(level, double(intelligence)); }

CombatStats PlayerCombatStats(int level, int strength, int intelligence, const std::vector<WornItem>& worn,
                              const std::vector<assets::MagicOptionRow>* options, BareHands bare) {
    CombatStats s;
    s.level = level;
    s.strength = strength;
    s.intelligence = intelligence;
    s.maxHp = PlayerMaxHp(level, strength);
    s.maxMp = PlayerMaxMp(level, intelligence);
    // The bare hands' (phy, mag), the keeper's floats as they are written (30.6f ...).
    std::array<double, 2> weaponReinforceMin{double(30.6f), double(49.0f)}, weaponReinforceMax{double(34.2f), double(54.8f)};
    if (bare == BareHands::AfterWeapon) {
        weaponReinforceMin = {double(30.6f), double(34.2f)};
        weaponReinforceMax = {double(49.0f), double(54.8f)};
    }
    std::array<std::array<double, 2>, 6> parts = kPartDefaults;
    double shieldPhy = 0, shieldMag = 0;
    for (const WornItem& w : worn) {
        if (w.row == nullptr || w.item == nullptr || w.place > slot::kRingRight) continue;
        if (KindOf(*w.row) == ItemKind::Armour || KindOf(*w.row) == ItemKind::Shield || KindOf(*w.row) == ItemKind::Weapon ||
            KindOf(*w.row) == ItemKind::Accessory) {
            if (w.item->durability == 0 && NewDurability(*w.row) != 0) continue; // broken: no bonuses
        }
        const ItemStats st = ComputeStats(*w.row, *w.item, options, w.setPlus, true); // the server's floats
        switch (KindOf(*w.row)) {
        case ItemKind::Weapon:
            s.phyAttackMin += st.phyAttack.low;
            s.phyAttackMax += st.phyAttack.high;
            s.magAttackMin += st.magAttack.low;
            s.magAttackMax += st.magAttack.high;
            s.weaponMagMin = st.magAttack.low;
            s.weaponMagMax = st.magAttack.high;
            s.weaponPhyMin = st.phyAttack.low;
            s.weaponPhyMax = st.phyAttack.high;
            weaponReinforceMin = {st.phyReinforce.low, st.magReinforce.low};
            weaponReinforceMax = {st.phyReinforce.high, st.magReinforce.high};
            s.hitRate += st.attackRating.low;
            s.critical += st.critical.low;
            s.range = w.row->Stat(94);
            s.weaponKind = w.row->typeId[3];
            break;
        case ItemKind::Shield:
            s.phyDefence += st.phyDefence.low;
            s.magDefence += st.magDefence.low;
            s.block += st.blocking.low;
            shieldPhy = st.phyReinforce.low;
            shieldMag = st.magReinforce.low;
            s.shieldDefence = st.phyDefence.low;
            break;
        case ItemKind::Armour:
            s.phyDefence += st.phyDefence.low;
            s.magDefence += st.magDefence.low;
            s.evasion += st.parry.low;
            if (w.place >= 0 && w.place < 6) parts[std::size_t(w.place)] = {st.phyReinforce.low, st.magReinforce.low};
            break;
        case ItemKind::Accessory:
            s.phyAbsorption += st.phyAbsorption.low;
            s.magAbsorption += st.magAbsorption.low;
            break;
        default: break;
        }
    }
    s.phyAttackMin += double(strength) * weaponReinforceMin[0] / 100.0;
    s.phyAttackMax += double(strength) * weaponReinforceMax[0] / 100.0;
    s.magAttackMin += double(intelligence) * weaponReinforceMin[1] / 100.0;
    s.magAttackMax += double(intelligence) * weaponReinforceMax[1] / 100.0;
    s.phyReinforceMin = weaponReinforceMin[0];
    s.phyReinforceMax = weaponReinforceMax[0];
    s.magReinforceMin = weaponReinforceMin[1];
    s.magReinforceMax = weaponReinforceMax[1];
    double phyParts = shieldPhy, magParts = shieldMag;
    for (const auto& p : parts) {
        phyParts += p[0];
        magParts += p[1];
    }
    s.phyDefence += double(strength) * phyParts / 100.0;
    s.magDefence += double(intelligence) * magParts / 100.0;
    s.phyParts = phyParts;
    s.magParts = magParts;
    // The balance: STR (INT) against what the level expects, at most 1.2 (its constants as the original keeps them,
    // 6/7 and 0.8 in single precision).
    const double offset = double(std::max(1, level) - 1);
    s.phyBalance = Clamp((double(strength) + 16.0 + 2.0 * offset) / ((7.0 * offset + 56.0) * double(6.0f / 7.0f)), 0.0, 1.2);
    s.magBalance = Clamp(double(intelligence) / ((5.0 * offset + 40.0) * double(0.8f)), 0.0, 1.2);
    return s;
}

CombatStats PlayerCombatStats(int level, int strength, int intelligence, const std::vector<WornItem>& worn,
                              const std::vector<assets::MagicOptionRow>* options, const StatSheet& sheet, BareHands bare) {
    if (sheet.Empty()) return PlayerCombatStats(level, strength, intelligence, worn, options, bare);
    const float str = std::max(0.0f, sheet.Value(stat::kStr, float(strength)));
    const float intel = std::max(0.0f, sheet.Value(stat::kInt, float(intelligence)));
    CombatStats s = PlayerCombatStats(level, int(std::trunc(str)), int(std::trunc(intel)), worn, options, bare);
    s.strength = str;
    s.intelligence = intel;
    // The parts' defence from the float STR / INT (the keeper's 0x15 / 0x16 <- STR / INT).
    s.phyDefence += (double(str) - std::trunc(double(str))) * s.phyParts / 100.0;
    s.magDefence += (double(intel) - std::trunc(double(intel))) * s.magParts / 100.0;
    // The attacks from them likewise (0x11..0x14 <- STR / INT).
    const double strFraction = double(str) - std::trunc(double(str)), intFraction = double(intel) - std::trunc(double(intel));
    s.phyAttackMin += strFraction * s.phyReinforceMin / 100.0;
    s.phyAttackMax += strFraction * s.phyReinforceMax / 100.0;
    s.magAttackMin += intFraction * s.magReinforceMin / 100.0;
    s.magAttackMax += intFraction * s.magReinforceMax / 100.0;
    // The Devil's Spirit's HPNA / MPNA: a percent of the STR (INT) part before the level's factor (the keeper's 0x34 <- STR,
    // 0x36 <- 0x34 times 1.02^(level - 1) x 10 as its "percent of" changes; max HP 3 <- 0x36).
    if (sheet.Changes(stat::kHpFromStr)) s.maxHp = PlayerVitalFrom(level, double(sheet.Value(stat::kHpFromStr, std::trunc(str))));
    if (sheet.Changes(stat::kMpFromInt)) s.maxMp = PlayerVitalFrom(level, double(sheet.Value(stat::kMpFromInt, std::trunc(intel))));
    ApplySheet(s, sheet);
    return s;
}

CombatStats MonsterCombatStats(const assets::ObjectRow& row, std::uint8_t rarity) {
    CombatStats s;
    s.level = row.level;
    s.maxHp = std::max<std::uint32_t>(1, proto::MonsterMaxHp(row.maxHp, rarity));
    s.phyDefence = row.phyDefence;
    s.magDefence = row.magDefence;
    s.phyAbsorption = row.phyParry; // characterdata's PAR / MAR (the absorption rates)
    s.magAbsorption = row.magParry;
    s.evasion = std::max(1.0f, row.evasion);
    s.hitRate = std::max(1.0f, row.hitRate);
    s.block = row.block;
    s.critical = row.critical;
    s.bodyRadius = std::max(1.0f, row.bodyRadius * float(row.scalePercent) / 100.0f);
    s.targetClass = rarity & 0x0F; // 3: a unique, the one the attacker's 0x103 counts on
    // Its range and its hits by its population (the high nibble: 1 a party monster) and its grade (the low nibble).
    const int population = rarity >> 4, grade = rarity & 0x0F;
    s.rangeFactor = (population == 1 ? 1.2 : 1.0) * (grade == 6 ? 1.5 : 1.0);
    const double gradeScale = grade == 4 || grade == 5 || grade == 6 ? 1.5 : grade == 7 ? 1.8 : 1.0;
    s.hitScale = std::clamp(population == 1 ? 1.3 : 1.0, 0.1, 10.0) * gradeScale;
    return s;
}

int AttackSpec::Lane() const {
    // The lane its stat slots go by: physical (bit 4) or magical (bit 8), melee (bit 1) or ranged
    // (bit 2); a kind with neither (kind 8: an imbue's force, a damage over time) has none: no lane's damage percent, no
    // block bonus.
    if ((kind & 3) == 0) return -1;
    const int ranged = (kind & 1) == 0 && (kind & 2) != 0 ? 1 : 0;
    return (magical ? 2 : 0) + ranged;
}

ThreatSpec ThreatOf(const assets::SkillRow& skill) {
    ThreatSpec t;
    // `tant` wins over `tnt2`.
    assets::SkillParam raise = assets::FindSkillParam(skill, assets::SkillTag("tant"));
    if (!raise) raise = assets::FindSkillParam(skill, assets::SkillTag("tnt2"));
    if (raise) {
        t.raise = true;
        t.flat = raise.Int(0);
        t.percent = raise.Int(1);
    }
    if (const assets::SkillParam p = assets::FindSkillParam(skill, assets::SkillTag("pwtt"))) t.physicalWord = p.Int(0);
    if (const assets::SkillParam p = assets::FindSkillParam(skill, assets::SkillTag("mwtt"))) t.magicalWord = p.Int(0);
    if (const assets::SkillParam cut = assets::FindSkillParam(skill, assets::SkillTag("dtnt"))) {
        t.cut = true;
        t.cutFlat = cut.Int(0);
        t.cutPercent = cut.Int(1);
    }
    if (const assets::SkillParam p = assets::FindSkillParam(skill, assets::SkillTag("pwdt"))) t.physicalCutWord = p.Int(0);
    if (const assets::SkillParam p = assets::FindSkillParam(skill, assets::SkillTag("mwdt"))) t.magicalCutWord = p.Int(0);
    return t;
}

// The weapon's own attacks (its whites with the plus, not the STR / INT share; no weapon: 0) at their mean, times the STR
// (INT) balance and the word / 100, truncated (physical, magical, which `mwhh`'s heal reads
// too: game::HealFor).
std::int64_t WeaponThreat(const CombatStats& attacker, int word, bool magical) {
    if (word == 0) return 0;
    const double low = magical ? attacker.weaponMagMin : attacker.weaponPhyMin, high = magical ? attacker.weaponMagMax : attacker.weaponPhyMax;
    if (low == 0.0 && high == 0.0) return 0;
    const double half = double(float((low + high) * 0.5));
    const double balance = magical ? attacker.magBalance : attacker.phyBalance;
    return std::int64_t(std::trunc(half * balance * double(word) / 100.0));
}

std::int64_t RaiseThreat(std::int64_t total, std::uint32_t damage, const ThreatSpec& spec, const CombatStats& attacker) {
    std::int64_t t = total + std::int64_t(damage);
    if (spec.raise) t = std::int64_t(std::trunc(double(t) * (1.0 + double(spec.percent) / 100.0))) + spec.flat;
    t += WeaponThreat(attacker, spec.physicalWord, false) + WeaponThreat(attacker, spec.magicalWord, true);
    return t;
}

AttackSpec AttackOf(const assets::SkillRow& skill) {
    AttackSpec a;
    a.skill = skill.id;
    a.formula = skill.attack;
    a.kind = skill.attack ? skill.attackKind : 5;
    a.power = skill.attack && skill.attackPower > 0 ? skill.attackPower : 100; // a non-positive one: an ordinary attack's
    a.flatMin = skill.attackMin;
    a.flatMax = std::max(skill.attackMin, skill.attackMax);
    a.magical = (a.kind & 8) != 0 && (a.kind & 4) == 0; // magic, an imbue's force
    a.hits = std::max(1, skill.hits);
    if (const assets::SkillParam att = assets::FindSkillParam(skill, assets::SkillTag("att"))) a.imbuePercent = std::max(0, att.Int(4));
    if (const assets::SkillParam cr = assets::FindSkillParam(skill, assets::SkillTag("cr"))) {
        a.criticalFlat = cr.Int(0);
        a.criticalPercent = cr.Int(1);
    }
    if (const assets::SkillParam fixed = assets::FindSkillParam(skill, assets::SkillTag("pdmg"))) a.fixedDamage = std::max(0, fixed.Int(0));
    if (const assets::SkillParam steal = assets::FindSkillParam(skill, assets::SkillTag("lfst"))) a.lifeSteal = std::max(0, steal.Int(0));
    if (const assets::SkillParam drain = assets::FindSkillParam(skill, assets::SkillTag("dmgt"))) {
        a.drainHp = std::max(0, drain.Int(0));
        a.drainMp = std::max(0, drain.Int(1));
    }
    if (const assets::SkillParam down = assets::FindSkillParam(skill, assets::SkillTag("da"))) a.downPercent = std::max(0, down.Int(0));
    if (const assets::SkillParam against = assets::FindSkillParam(skill, assets::SkillTag("atca"))) {
        a.againstMask = against[0];
        a.againstPercent = against.Int(1);
    }
    if (const assets::SkillParam ko = assets::FindSkillParam(skill, assets::SkillTag("ko"))) {
        a.knockDownLevel = ko.Int(0);
        a.knockDownChance = ko.Int(1);
    }
    if (const assets::SkillParam kb = assets::FindSkillParam(skill, assets::SkillTag("kb"))) {
        a.knockBackChance = kb.Int(0);
        a.knockBackDistance = kb.Int(1);
    }
    return a;
}

bool ChanceRolls::Roll(int chance, char kind, std::uint32_t skill, std::mt19937& random) {
    if (chance <= 0) return false;
    if (chance >= 100) return true;
    // rand() % 101, as the original rolls it.
    const int r = std::uniform_int_distribution<int>(0, 32767)(random) % 101;
    const std::uint32_t key = std::uint32_t(std::uint8_t(kind)) << 24 | (skill & 0xFFFFFFu);
    auto it = std::find_if(history_.begin(), history_.end(), [&](const auto& h) { return h.first == key; });
    if (it == history_.end()) {
        const bool success = r <= chance;
        history_.push_back({key, success ? 2 * chance - 100 : 2 * chance});
        return success;
    }
    if (r > it->second) {
        it->second += chance;
        return false;
    }
    it->second += chance - 100;
    return true;
}

namespace {

// One part of a hit, the physical or the magical one by the attack's kind: the variance's point, the defences,
// the skill's power and the factors; a whole number.
// `takenLane`: the lane of the target's factor (an imbue's part: its own physical / magical with the main skill's melee /
// ranged; -1 none).
std::uint32_t FormulaPart(const CombatStats& attacker, const CombatStats& defender, const AttackSpec& attack, bool critical, bool ignoreDefence,
                          bool attackerPlayer, std::mt19937& random, int takenLane) {
    const auto unit = [&] { return double(std::uniform_int_distribution<int>(0, 32767)(random)) / 32767.0; }; // rand() / 32767
    const int lane = attack.Lane();
    // The variance: the range (the skill's flat attack added, then 1 % a level of its mastery), the
    // point's centre from the hit rate over the parry rate and the level gap (10..90), moved by the
    // least of three rolls up or down.
    // A monster's ends times its range factor, held to 0..999,999.
    const double scale = 1.0 + double(std::max(0, attack.mastery)) / 100.0;
    const auto end = [&](double stat, int flat) { return Clamp((stat + double(flat)) * attacker.rangeFactor, 0.0, 999999.0) * scale; };
    const double low = end(attack.magical ? attacker.magAttackMin : attacker.phyAttackMin, attack.flatMin);
    const double high = std::max(low, end(attack.magical ? attacker.magAttackMax : attacker.phyAttackMax, attack.flatMax));
    const double advantage = LevelAdvantage(attacker.level, defender.level);
    const double centre = defender.evasion <= 0.0 ? 90.0 : Clamp((attacker.hitRate / defender.evasion * 0.5 + advantage) * 100.0, 10.0, 90.0);
    // A passive's hit rate adds to the roll before it is held to 0..100, a passive's attack percent
    // times the point.
    const double spread = std::min({100.0, unit() * 100.0, unit() * 100.0, unit() * 100.0});
    const bool up = (std::uniform_int_distribution<int>(0, 32767)(random) & 1) != 0;
    const double percentile = Clamp((up ? centre + spread : centre - spread) + double(attack.spreadBonus), 0.0, 100.0);
    const double point = (low + (high - low) * percentile / 100.0) * attack.pointFactor;
    // Mitigated: divided by 1 + the absorption / 100, less the defence; times the skill's power.
    const double absorption = attack.magical ? defender.magAbsorption : defender.phyAbsorption;
    const double defence = attack.magical ? defender.magDefence : defender.phyDefence;
    double damage = ignoreDefence ? point : std::max(0.0, point / (1.0 + absorption / 100.0) - defence);
    damage = damage * double(attack.power) / 100.0;
    // The buffs' and passives' factors: its lane's damage, a class bonus, what the target takes in it.
    const double classPart = defender.targetClass == 3 ? attacker.classBonus / 100.0 * damage : 0.0;
    damage = damage * (1.0 + (lane >= 0 ? attacker.damagePercent[std::size_t(lane)] : 0.0) / 100.0) + classPart;
    if (takenLane >= 0 && defender.damageTaken[std::size_t(takenLane)] != 0.0) damage *= defender.damageTaken[std::size_t(takenLane)];
    // Berserk, a critical's double (magic never criticals), the level above the target, a player's balance.
    double times = attacker.berserk ? attacker.berserkBonus + 2.0 : 1.0;
    if (critical && !attack.magical) times *= 2.0;
    damage *= times;
    damage *= 1.0 + advantage;
    if (attackerPlayer) damage *= attack.magical ? attacker.magBalance : attacker.phyBalance;
    // A hit under 5 % of the point: a small random one, 1 to 10 % of the point.
    if (damage < point * 0.05) {
        double ceiling = point * 10.0 / 100.0;
        if (ceiling <= 0.0) ceiling = 1.0;
        damage = 1.0 + (ceiling - 1.0) * unit();
    }
    const std::size_t part = attack.magical ? 1 : 0;
    if (attacker.finalFactor[part] != 0.0) damage *= attacker.finalFactor[part];
    if (defender.finalTakenFactor[part] != 0.0) damage *= defender.finalTakenFactor[part];
    return std::uint32_t(std::trunc(Clamp(damage, 0.0, 16777215.0)));
}

// What a target's wall takes of one part of a hit (the original's wall formulas, physical and
// magical): the attack's power rolled in its range (ours: evenly), divided by 1 + the wall's absorption / 100 less
// its defence (ignoring the defence: the power), times the skill's power and the lane's damage; under 5 % of the power
// 1 to 10 % of it; a physical critical's double; the attacker's final factor and the holder's taken one.
std::uint32_t WallPart(const CombatStats& attacker, const CombatStats& holder, const AttackSpec& attack, const WallSpec& wall, bool critical,
                       bool ignoreDefence, std::mt19937& random) {
    const auto unit = [&] { return double(std::uniform_int_distribution<int>(0, 32767)(random)) / 32767.0; };
    const int lane = attack.Lane();
    const double scale = 1.0 + double(std::max(0, attack.mastery)) / 100.0;
    const auto end = [&](double stat, int flat) { return Clamp((stat + double(flat)) * attacker.rangeFactor, 0.0, 999999.0) * scale; };
    const double low = end(attack.magical ? attacker.magAttackMin : attacker.phyAttackMin, attack.flatMin);
    const double high = std::max(low, end(attack.magical ? attacker.magAttackMax : attacker.phyAttackMax, attack.flatMax));
    const double power = low + (high - low) * unit();
    double damage = ignoreDefence ? power : std::max(0.0, power / (1.0 + double(wall.absorption) / 100.0) - double(wall.defence));
    damage = damage * double(attack.power) / 100.0 * (1.0 + (lane >= 0 ? attacker.damagePercent[std::size_t(lane)] : 0.0) / 100.0);
    if (damage < power * 0.05) {
        const double most = std::max(1.0, std::trunc(power * 10.0 / 100.0));
        damage = 1.0 + unit() * (most - 1.0);
    }
    if (critical && !attack.magical) damage *= 2.0;
    const std::size_t part = attack.magical ? 1 : 0;
    if (attacker.finalFactor[part] != 0.0) damage *= attacker.finalFactor[part];
    if (holder.finalTakenFactor[part] != 0.0) damage *= holder.finalTakenFactor[part];
    return std::uint32_t(std::trunc(Clamp(damage, 0.0, 16777215.0)));
}

// An amount less 2.5 % a level the target is above the attacker, and its tenth (the least it falls
// to).
std::uint32_t LevelReduced(int amount, int attackerLevel, int defenderLevel, double& least) {
    least = std::trunc(double(amount) * 0.1);
    const int gap = defenderLevel - attackerLevel;
    if (gap <= 0) return std::uint32_t(std::max(0, amount));
    return std::uint32_t(std::max(0.0, std::trunc(double(amount) * (1.0 - double(gap) * 2.5 / 100.0))));
}

} // namespace

std::uint32_t FixedDamage(int damage, int attackerLevel, int defenderLevel) {
    // The damage (a passive's modifier joins it: K6), less the level gap, held between a tenth and all of it.
    if (damage <= 0) return 0;
    double least = 0.0;
    const std::uint32_t reduced = LevelReduced(damage, attackerLevel, defenderLevel, least);
    return std::uint32_t(Clamp(double(reduced), least, double(damage)));
}

HitResult RollHit(const CombatStats& attacker, const CombatStats& defender, const AttackSpec& attack, const HitRoll& who, std::mt19937& random,
                  const AttackSpec* imbue) {
    // The original's hit: the outcome and its rolls, the formula's
    // parts, an imbue's share, the skill's own damage, the target's share.
    const auto roll = [&](int chance, char kind) {
        if (who.rolls != nullptr) return who.rolls->Roll(chance, kind, attack.skill, random);
        return chance > 0 && (chance >= 100 || std::uniform_int_distribution<int>(0, 100)(random) <= chance);
    };
    HitResult hit;
    bool ignoreDefence = false;
    // The rolls belong to a skill with an 'att' (one of its own damage alone is neither critical nor
    // blocked): a physical critical (its rate with the skill's cr, against the target's resistance),
    // ignoring a monster's defence (a player's hit), then blocked (the target's block rate and its
    // lane bonus, against the attacker's block ignore).
    if (attack.formula) {
        if ((attack.kind & 4) != 0) {
            const int base = int(std::trunc(attacker.critical));
            const int chance = base + attack.criticalFlat + int(std::trunc(double(base) * double(attack.criticalPercent) / 100.0));
            hit.critical = roll(int(std::trunc(double(chance) / (1.0 + defender.criticalResist / 100.0))), 'C');
        }
        ignoreDefence = who.attackerPlayer && !who.defenderPlayer && roll(int(std::trunc(attacker.ignoreDefenceChance)), 'M');
        // A wall on the target (the original's hit outcome): before the block, for a plain 'att' (not a life
        // steal nor one of fixed damage) and while it has HP, it takes its parts of the hit (each by its own formula, an
        // imbue's at the skill's share); covering every part the hit has (the physical, the magical or an imbue's),
        // the holder is untouched.
        if (who.wall != nullptr) {
            const WallSpec& wall = *who.wall;
            const bool withImbue = imbue != nullptr && attack.imbuePercent > 0;
            const bool coversPhysical = attack.magical || (wall.kinds & 4) != 0;
            const bool coversMagical = (!attack.magical && !withImbue) || (wall.kinds & 8) != 0;
            if (attack.lifeSteal <= 0 && attack.fixedDamage <= 0 && wall.hpLeft > 0) {
                const auto take = [&](const AttackSpec& part) {
                    const bool covered = (part.magical ? wall.kinds & 8 : wall.kinds & 4) != 0;
                    return covered ? WallPart(attacker, defender, part, wall, hit.critical, ignoreDefence, random) : 0u;
                };
                std::uint64_t taken = take(attack);
                if (withImbue) taken += std::uint64_t(std::trunc(double(take(*imbue)) * double(attack.imbuePercent) / 100.0));
                hit.wallTaken = std::uint32_t(std::min<std::uint64_t>(taken, 0xFFFFFFFFu));
                if (coversPhysical && coversMagical) {
                    hit.wallOnly = true;
                    return hit;
                }
            }
        }
        // The block rate and its lane's bonus each truncated to a byte, summed as one, then over the attacker's ignore.
        const int lane = attack.Lane();
        const int bonus = lane >= 0 ? int(std::trunc(defender.blockBonus[std::size_t(lane)])) & 0xFF : 0;
        const int rate = ((int(std::trunc(defender.block)) & 0xFF) + bonus) & 0xFF;
        const double blockRate = double(rate) / (1.0 + attacker.blockIgnore / 100.0);
        if (roll(int(std::trunc(blockRate)), 'B')) {
            hit.blocked = true;
            return hit;
        }
    }
    // The formula's part (a down attack's percent of it on a target that is down); an
    // imbue's force (its own part, the same percent) joins it at the skill's share of it.
    const auto down = [&](std::uint32_t part) {
        if (!who.defenderDown || attack.downPercent <= 0) return std::int64_t(part);
        return std::int64_t(std::trunc(double(std::uint64_t(part) * std::uint64_t(attack.downPercent)) / 100.0));
    };
    // On a target an imbue's jump added, the physical parts are 0. A wall's kinds zero its parts of the holder's.
    const auto walled = [&](const AttackSpec& part) {
        return who.wall != nullptr && ((part.magical ? who.wall->kinds & 8 : who.wall->kinds & 4) != 0);
    };
    std::int64_t damage = attack.formula ? down(FormulaPart(attacker, defender, attack, hit.critical, ignoreDefence, who.attackerPlayer, random, attack.Lane())) : 0;
    if ((who.magicalOnly && !attack.magical) || walled(attack)) damage = 0;
    hit.magical = attack.formula && attack.magical && damage > 0;
    if (imbue != nullptr && attack.formula && attack.imbuePercent > 0) {
        // The target's factor for the imbue's part: the imbue's physical / magical, the main skill's melee / ranged.
        const int mainLane = attack.Lane();
        const int imbueLane = mainLane < 0 ? -1 : (imbue->magical ? 2 : 0) + (mainLane & 1);
        std::int64_t force = down(FormulaPart(attacker, defender, *imbue, hit.critical, ignoreDefence, who.attackerPlayer, random, imbueLane));
        if ((who.magicalOnly && !imbue->magical) || walled(*imbue)) force = 0;
        hit.magical = hit.magical || (imbue->magical && force > 0);
        damage += std::int64_t(std::trunc(double(force) * double(attack.imbuePercent) / 100.0));
    }
    const int share = std::clamp(who.share, 0, 255);
    if (attack.lifeSteal > 0) {
        // A life steal is the hit: its amount less the level gap, at least a tenth, no more
        // than the target has, at the target's share.
        double least = 0.0;
        const double reduced = double(LevelReduced(attack.lifeSteal, attacker.level, defender.level, least));
        if (who.defenderHp != 0 && (reduced > double(who.defenderHp) || least > double(who.defenderHp))) {
            damage = who.defenderHp;
        } else {
            damage = std::int64_t(std::trunc(std::max(reduced, least) * double(share) / 100.0));
        }
    } else {
        // The skill's own damage; against a state its percent more; then the target's share.
        damage += FixedDamage(attack.fixedDamage, attacker.level, defender.level);
        if ((attack.againstMask & who.defenderStates) != 0) {
            damage = std::int64_t(std::trunc((double(std::uint32_t(attack.againstPercent)) / 100.0 + 1.0) * double(damage)));
        }
        damage = std::int64_t(std::trunc(double(damage) * double(share) / 100.0));
    }
    // A monster's grade (its population's and its grade's scale).
    if (attacker.hitScale != 1.0) damage = std::int64_t(std::trunc(double(damage) * attacker.hitScale));
    hit.damage = std::uint32_t(std::clamp<std::int64_t>(damage, 1, 16777215));
    return hit;
}

} // namespace sr::game
