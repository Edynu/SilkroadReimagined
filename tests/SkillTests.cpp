// The players' skills on the server, skill by skill: the skill unit
// (server/game Skills.h) driven by a test host instead of the game server, so every skill of the
// game data can be cast without the network.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <tuple>
#include <set>
#include <string>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/game/Alchemy.h"
#include "sr/game/SkillBench.h"
#include "sr/game/SkillCatalogue.h"
#include "sr/game/Skills.h"
#include "sr/game/States.h"
#include "sr/server/Skills.h"

namespace {

using namespace sr;
namespace proto = game::proto;
using Clock = std::chrono::steady_clock;

// The world as the skills see it: a flat field in one region, monsters that never die of a test's
// hits unless asked, a quiver that never runs out; every message kept.
class TestHost : public server::SkillHost {
public:
    struct Body {
        proto::WorldPosition position;
    };
    struct Mob {
        game::CombatStats stats;
        std::uint32_t hp = 10'000'000;
        bool alive = true;
        std::uint64_t damage = 0;
        int hits = 0;
        // States (K3): its defence (its level; no resistance unless a test gives it), the states it is
        // in, those it took (every one, in order), its knocks; whether it may fall or be pushed.
        game::StateDefence defence;
        game::StateSet states;
        std::vector<game::ActiveState> took;
        int knockedDown = 0, knockedBack = 0;
        std::vector<std::uint32_t> debuffs; // the buffs' changes put on it (their skills)
        std::int64_t threat = 0;            // the threat its hits gave (their sum) and whose the last
        std::uint32_t threatFrom = 0;
        bool down = false, canFall = true, canPush = true;
        game::ChanceRolls rolls;
    };
    std::map<std::uint32_t, Body> bodies;
    std::map<std::uint32_t, Mob> mobs;
    struct Told {
        bool observers = false;
        std::uint32_t entity = 0;
        server::SkillMessage message;
    };
    std::vector<Told> told;
    int kills = 0, arrows = 0, vitalsChanges = 0;
    std::map<std::uint32_t, int> statsChanged;

    std::optional<proto::WorldPosition> Position(std::uint32_t entity) const override {
        const auto it = bodies.find(entity);
        if (it == bodies.end()) return std::nullopt;
        return it->second.position;
    }
    bool walking = false; // the caster is on its way (an action's stop counts in `stops`)
    int stops = 0;
    bool Moving(std::uint32_t) const override { return walking; }
    void MoveTo(std::uint32_t entity, const proto::WorldPosition& to) override { bodies[entity].position = to; ++stops; } // walks arrive at once
    const game::CombatStats* Monster(std::uint32_t entity) const override {
        const auto it = mobs.find(entity);
        return it != mobs.end() && it->second.alive ? &it->second.stats : nullptr;
    }
    std::uint32_t MonsterHp(std::uint32_t entity) const override {
        const auto it = mobs.find(entity);
        return it != mobs.end() && it->second.alive ? it->second.hp : 0u;
    }
    std::vector<std::uint32_t> MonstersNear(const proto::WorldPosition& at, float radius) const override {
        std::vector<std::uint32_t> out;
        const proto::FlatPosition c = proto::ToFlat(at);
        for (const auto& [id, m] : mobs) {
            const auto b = bodies.find(id);
            if (!m.alive || b == bodies.end()) continue;
            const proto::FlatPosition p = proto::ToFlat(b->second.position);
            if (std::hypot(p.x - c.x, p.z - c.z) <= radius) out.push_back(id);
        }
        return out; // a map: by id already
    }
    std::vector<std::uint32_t> PlayersNear(const proto::WorldPosition& at, float radius) const override {
        std::vector<std::uint32_t> out;
        const proto::FlatPosition c = proto::ToFlat(at);
        for (const auto& [id, b] : bodies) {
            if (mobs.contains(id) && !enemyPlayers.contains(id)) continue;
            const proto::FlatPosition p = proto::ToFlat(b.position);
            if (std::hypot(p.x - c.x, p.z - c.z) <= radius) out.push_back(id);
        }
        return out;
    }
    std::optional<server::Monsters::Target> MonsterTarget(std::uint32_t entity) const override {
        const auto it = mobs.find(entity);
        if (it == mobs.end() || !it->second.alive) return std::nullopt;
        server::Monsters::Target t;
        t.defence = it->second.defence;
        t.defence.level = it->second.stats.level;
        t.states = it->second.states.Mask();
        t.susceptibility = it->second.states.Susceptibility();
        t.down = it->second.down;
        t.canFall = it->second.canFall && !it->second.down;
        t.canPush = it->second.canPush;
        t.recoverMs = 3000;
        return t;
    }
    std::vector<proto::StateRecord> MonsterStates(std::uint32_t entity, Clock::time_point) const override {
        std::vector<proto::StateRecord> out;
        if (const auto it = mobs.find(entity); it != mobs.end()) {
            for (const game::ActiveState& a : it->second.states.All()) out.push_back({std::uint8_t(a.kind), std::uint8_t(a.level), a.durationMs});
        }
        return out;
    }
    server::Monsters::Damaged HitMonster(std::uint32_t monster, std::uint32_t attacker, std::uint32_t damage, Clock::time_point,
                                         const server::Monsters::Affliction* affliction) override {
        server::Monsters::Damaged d;
        auto it = mobs.find(monster);
        if (it == mobs.end() || !it->second.alive) return d;
        Mob& m = it->second;
        d.hit = true;
        if (affliction != nullptr && affliction->magical) d.statesChanged = m.states.End(game::StateKind::Root) || d.statesChanged;
        m.hp = damage >= m.hp ? 0 : m.hp - damage;
        m.damage += damage;
        ++m.hits;
        d.hp = m.hp;
        if (m.hp == 0) {
            m.alive = false;
            d.killed = true;
            d.damageBy.push_back({attacker, m.damage});
            return d;
        }
        if (damage > 0 && !m.states.Damaged(m.rolls, random).empty()) d.statesChanged = true;
        if (affliction != nullptr) {
            if (affliction->threatGiven) {
                m.threat += affliction->threat;
                m.threatFrom = attacker;
            }
            for (const game::ActiveState& s : affliction->states) {
                m.took.push_back(s);
                d.statesChanged = m.states.Apply(s) || d.statesChanged;
            }
            if (affliction->debuff != nullptr) m.debuffs.push_back(affliction->debuff->id);
            if (affliction->knock == game::Knock::Down) ++m.knockedDown;
            if (affliction->knock == game::Knock::Back) ++m.knockedBack;
            d.knock = affliction->knock;
            d.knockMs = affliction->knockMs;
        }
        return d;
    }
    std::mt19937 random{11};
    void Killed(std::uint32_t, const server::Monsters::Damaged&) override { ++kills; }
    int TakeAmmunition(std::uint32_t, int count) override {
        arrows += count;
        return count;
    }
    void VitalsChanged(std::uint32_t) override { ++vitalsChanges; }
    void StatsChanged(std::uint32_t entity) override { ++statsChanged[entity]; }
    // Enemy players (PvP: a war, free PvP): each a Mob answering the monster calls as a target, as the game server's
    // fighting players do; the caster's enemies, players (a test sets them).
    std::set<std::uint32_t> enemyPlayers;
    bool Hostile(std::uint32_t, std::uint32_t target) const override { return enemyPlayers.contains(target); }
    bool IsPlayer(std::uint32_t entity) const override { return enemyPlayers.contains(entity); }
    // The duplicate's bars on its target: in a suit or free battle, a murderer.
    std::set<std::uint32_t> suited, murderers;
    bool DuplicateBarred(std::uint32_t target, bool murderer) const override { return murderer ? murderers.contains(target) : suited.contains(target); }
    // Other players as buff holders (a test sets them).
    std::function<std::optional<server::SkillCaster>(std::uint32_t)> players;
    std::optional<server::SkillCaster> Player(std::uint32_t entity) override { return players ? players(entity) : std::nullopt; }
    // A party (the links' divide), threats alone (a guard's share), the monsters' link copies (K12).
    std::vector<std::uint32_t> party;
    std::vector<std::uint32_t> PartyOf(std::uint32_t) const override { return party; }
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::int64_t>> threats;
    void AddThreat(std::uint32_t monster, std::uint32_t from, std::int64_t threat) override { threats.push_back({monster, from, threat}); }
    std::map<std::uint32_t, std::vector<std::uint32_t>> monsterBuffs;
    void MonsterBuffs(std::uint32_t monster, std::vector<std::uint32_t> skills) override { monsterBuffs[monster] = std::move(skills); }
    // Bodies of dead monsters, items dropped (owner, item, data) and looks changed (K12's transforms).
    std::map<std::uint32_t, server::Monsters::Corpse> corpses;
    std::optional<server::Monsters::Corpse> MonsterCorpse(std::uint32_t entity) const override {
        const auto it = corpses.find(entity);
        return it != corpses.end() ? std::optional<server::Monsters::Corpse>(it->second) : std::nullopt;
    }
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> dropped;
    void DropItem(std::uint32_t owner, std::uint32_t item, std::uint32_t data) override { dropped.push_back({owner, item, data}); }
    std::vector<std::uint32_t> looks;
    void LookChanged(std::uint32_t player) override { looks.push_back(player); }
    // Resurrections offered (K9): to whom, which skill.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> revivals;
    void OfferRevival(std::uint32_t, std::uint32_t target, const assets::SkillRow& skill) override { revivals.push_back({target, skill.id}); }
    // Pick-ups that waited in the action list, their turn come: the item.
    std::vector<std::uint32_t> pickedUp;
    void PickUpWaited(std::uint32_t, std::uint32_t item) override { pickedUp.push_back(item); }
    void ToCaster(std::uint32_t caster, const server::SkillMessage& m) override { told.push_back({false, caster, m}); }
    void ToObservers(std::uint32_t entity, const server::SkillMessage& m) override { told.push_back({true, entity, m}); }
    // Skill objects (K10): made with ids from 5000 (the skill, where), and those taken away.
    std::map<std::uint32_t, std::pair<std::uint32_t, proto::WorldPosition>> objects;
    std::vector<std::uint32_t> removedObjects;
    std::uint32_t SpawnObject(std::uint32_t, const assets::SkillRow& skill, const proto::WorldPosition& at) override {
        const std::uint32_t id = 5000 + std::uint32_t(objects.size() + removedObjects.size());
        objects[id] = {skill.id, at};
        return id;
    }
    void RemoveObject(std::uint32_t id) override {
        objects.erase(id);
        removedObjects.push_back(id);
    }
    // A blink: straight there on the flat field, at most `most` away.
    std::optional<proto::WorldPosition> Blink(std::uint32_t entity, const proto::WorldPosition& to, float most) override {
        auto it = bodies.find(entity);
        if (it == bodies.end()) return std::nullopt;
        const proto::FlatPosition a = proto::ToFlat(it->second.position), b = proto::ToFlat(to);
        const double d = std::hypot(b.x - a.x, b.z - a.z), k = d > double(most) ? double(most) / d : 1.0;
        it->second.position = proto::FromFlat({a.x + (b.x - a.x) * k, a.z + (b.z - a.z) * k}, to.y);
        return it->second.position;
    }

    template <typename T>
    std::vector<T> All() const {
        std::vector<T> out;
        for (const Told& t : told) {
            if (const T* m = std::get_if<T>(&t.message)) out.push_back(*m);
        }
        return out;
    }
};

// A player with one skill learned (its masteries high enough), the weapon it takes in hand, and its
// fight: as the game server keeps them.
struct TestPlayer {
    std::uint32_t entity = 1;
    game::SkillBook book;
    game::CombatStats stats;
    std::uint32_t hp = 50'000, mp = 1'000'000;
    server::SkillState state;
    // The weapon it holds is the one its numbers say (the skills' conditions read it from what it wears).
    server::SkillCaster Caster() {
        state.worn.weapon = stats.weaponKind;
        return {entity, book, stats, hp, mp, false, {}, state};
    }
};

// What a skill's conditions ask to be worn besides the weapon (K6, game::EquipmentFits): the shield
// of the sort it names, the armour of the kind on the parts it names (all six for sort 0).
void WearFor(TestPlayer& p, const assets::SkillRow& skill) {
    std::vector<int> weapons;
    assets::ForEachSkillParam(skill, [&](std::uint32_t code, std::span<const std::uint32_t> args) {
        if (code != assets::SkillTag("reqi") || args.size() < 2) return;
        const int kind = int(args[0]), sort = int(args[1]);
        if (kind == 6) weapons.push_back(sort);
        if (kind == 4) p.state.worn.shield = sort;
        if ((kind >= 1 && kind <= 3) || (kind >= 9 && kind <= 11)) {
            for (int part = 1; part <= 6; ++part) {
                if (sort == 0 || sort == part) p.state.worn.armour[std::size_t(part)] = kind;
            }
        }
    });
    // A weapon it names where its weapon columns name none (the Warrior's frenzy: a sword, a
    // two-handed sword or an axe): the first one.
    const bool columns = skill.weaponKinds[0] != 255 || skill.weaponKinds[1] != 255;
    if (!columns && !weapons.empty() && std::find(weapons.begin(), weapons.end(), p.stats.weaponKind) == weapons.end()) {
        p.stats.weaponKind = weapons[0];
    }
}

game::CombatStats StrongPlayer(int weaponKind) {
    game::CombatStats s;
    s.level = 100;
    s.phyAttackMin = 900;
    s.phyAttackMax = 1100;
    s.magAttackMin = 900;
    s.magAttackMax = 1100;
    s.hitRate = 500;
    s.maxHp = 50'000;
    s.maxMp = 1'000'000;
    s.weaponKind = weaponKind;
    // Its weapon's own attacks (the threat's 'pwtt' / 'mwtt' read them, game::WeaponThreat).
    s.weaponPhyMin = 300;
    s.weaponPhyMax = 400;
    s.weaponMagMin = 300;
    s.weaponMagMax = 400;
    return s;
}

game::CombatStats TrainingDummy() {
    game::CombatStats s;
    s.level = 1;
    s.maxHp = 10'000'000;
    s.evasion = 0;
    s.bodyRadius = 5;
    return s;
}

const char* StatusName(proto::SkillUseStatus s) {
    static constexpr const char* kNames[] = {"ok", "unknown", "weapon", "mana", "cooling", "target", "not yet", "dead", "ammunition", "untouchable"};
    const auto i = std::size_t(s);
    return i < std::size(kNames) ? kNames[i] : "?";
}

struct Cast {
    proto::SkillUseStatus status = proto::SkillUseStatus::Ok;
    bool answered = false;
    int hits = 0;
    std::uint64_t damage = 0;
    std::uint32_t manaSpent = 0;
    bool cooldown = false;
    bool swung = false;
};

// One cast of `skill` by a player holding `weaponKind` on a training dummy 30 units away, then
// `seconds` of the fight in 50 ms ticks.
Cast CastOnDummy(const assets::SkillTable& table, const assets::SkillRow& skill, int weaponKind, double seconds = 12.0) {
    TestHost host;
    server::Skills skills(host, &table, 7);
    TestPlayer p;
    p.stats = StrongPlayer(weaponKind);
    WearFor(p, skill);
    p.book.groups[skill.group] = skill.level;
    for (std::size_t i = 0; i < skill.masteries.size(); ++i) {
        if (skill.masteries[i] != 0) p.book.masteries[skill.masteries[i]] = std::max(1, skill.masteryLevels[i]);
    }
    constexpr std::uint32_t kDummy = 1000;
    host.bodies[p.entity].position = {proto::WorldPosition{std::uint16_t(98 << 8 | 168), 900.0f, 0.0f, 900.0f}};
    host.bodies[kDummy].position = {proto::WorldPosition{std::uint16_t(98 << 8 | 168), 930.0f, 0.0f, 900.0f}};
    host.mobs[kDummy].stats = TrainingDummy();
    const Clock::time_point t0 = Clock::now();
    server::SkillCaster caster = p.Caster();
    const std::uint32_t mpBefore = p.mp;
    skills.Use(caster, {skill.id, kDummy}, t0);
    Cast out;
    for (const auto& a : host.All<proto::SkillUseAnswer>()) {
        if (a.skill == skill.id && !out.answered) {
            out.answered = true;
            out.status = a.status;
        }
    }
    for (double t = 0.0; t < seconds; t += 0.05) {
        server::SkillCaster c = p.Caster();
        skills.Fight(c, t0 + std::chrono::milliseconds(int(t * 1000.0)));
    }
    out.hits = host.mobs[kDummy].hits;
    out.damage = host.mobs[kDummy].damage;
    out.manaSpent = mpBefore - p.mp;
    for (const auto& c : host.All<proto::Cooldown>()) out.cooldown = out.cooldown || c.id == skill.id;
    for (const auto& s : host.All<proto::EntitySwing>()) out.swung = out.swung || s.skill == skill.id;
    return out;
}

const char* GameDir() { return std::getenv("SR_GAME_DIR"); }

} // namespace

TEST_CASE("the skill unit: a known attack skill walks into reach, spends its mana, starts its wait and hits (game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // Bicheon's Smash, a sword's (or a blade's) skill of one target.
    const assets::SkillRow* smash = table.Find(std::string_view("SKILL_CH_SWORD_SMASH_A_01"));
    REQUIRE(smash != nullptr);
    const Cast cast = CastOnDummy(table, *smash, smash->weaponKinds[0]);
    CHECK(cast.answered);
    CHECK(cast.status == proto::SkillUseStatus::Ok);
    CHECK(cast.swung);
    CHECK(cast.hits >= 1);
    CHECK(cast.damage > 0);
    CHECK(cast.manaSpent == server::Skills::Mana(StrongPlayer(2), *smash));
    CHECK(cast.cooldown == (smash->cooldownMs > 0));
    // The wrong weapon in hand: refused, nothing spent.
    const Cast wrong = CastOnDummy(table, *smash, 6);
    CHECK(wrong.status == proto::SkillUseStatus::Weapon);
    CHECK(wrong.manaSpent == 0);
    CHECK(wrong.hits == 0);
}

TEST_CASE("every attack skill group cast once on a training dummy (game files; K1)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // Each board group's first level, as a player learns it, with the weapon it takes.
    std::map<std::string, int> outcome;
    std::map<std::string, std::vector<std::string>> examples;
    int groups = 0, weaponRefused = 0, weaponChecked = 0;
    for (const assets::MasteryRow& m : table.masteries()) {
        for (const assets::SkillRow* s : table.Board(m.id)) {
            if (!s->inService) continue;
            const game::SkillTraits t = game::TraitsOf(*s);
            if (!t.attack || t.passive) continue;
            ++groups;
            const int weapon = s->weaponKinds[0] != 255 ? s->weaponKinds[0] : 1;
            const Cast cast = CastOnDummy(table, *s, weapon);
            std::string kind = !cast.answered ? "no answer" : cast.status != proto::SkillUseStatus::Ok ? std::string("refused: ") + StatusName(cast.status)
                               : cast.hits == 0                                                  ? "no hit"
                                                                                                 : "hit";
            ++outcome[kind];
            if (examples[kind].size() < 4) examples[kind].push_back(s->codename);
            // A skill that asks for a weapon is refused with another (a bare hand).
            if (s->weaponKinds[0] != 255) {
                ++weaponChecked;
                weaponRefused += CastOnDummy(table, *s, 1, 0.1).status == proto::SkillUseStatus::Weapon ? 1 : 0;
            }
        }
    }
    MESSAGE(groups << " attack groups cast");
    for (const auto& [kind, n] : outcome) {
        std::string list;
        for (const std::string& e : examples[kind]) list += " " + e;
        MESSAGE("  " << kind << ": " << n << " (" << list << " )");
    }
    CHECK(groups > 150);
    CHECK(weaponRefused == weaponChecked);
    CHECK(outcome["hit"] > 0);
}

TEST_CASE("the client's skill bench: every active board skill, in time order, each with a weapon it takes (game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    assets::ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    game::SkillBench bench;
    REQUIRE_MESSAGE(game::MakeSkillBench(skills, items, characters, "all", bench, error), error);
    MESSAGE(bench.steps.size() << " skills, " << bench.recording.Duration() << " s");
    CHECK(bench.steps.size() > 400);
    for (std::size_t i = 1; i < bench.recording.messages.size(); ++i) {
        REQUIRE(bench.recording.messages[i - 1].time <= bench.recording.messages[i].time);
    }
    int checked = 0;
    for (const game::SkillBenchStep& step : bench.steps) {
        const assets::SkillRow* s = skills.Find(step.skill);
        REQUIRE(s != nullptr);
        CHECK(step.until > step.at);
        if (s->weaponKinds[0] == 255) continue;
        const assets::ItemRow* w = items.Find(step.weapon);
        REQUIRE_MESSAGE(w != nullptr, step.codename);
        CHECK_MESSAGE((w->typeId[3] == s->weaponKinds[0] || w->typeId[3] == s->weaponKinds[1]), step.codename);
        ++checked;
    }
    CHECK(checked > 200);
    // A filter by mastery: Bicheon's skills only.
    game::SkillBench bicheon;
    REQUIRE_MESSAGE(game::MakeSkillBench(skills, items, characters, "UIIT_STT_MASTERY_VI", bicheon, error), error);
    CHECK(bicheon.steps.size() > 20);
    CHECK(bicheon.steps.size() < bench.steps.size());
}

namespace {

// A field of training dummies: the selected one 30 in front of the caster, others around it and
// around the caster at known distances (ids in the order an area takes them).
struct Field {
    TestHost host;
    TestPlayer player;
    static constexpr std::uint32_t kSelected = 1000;
    std::vector<std::uint32_t> others;
    Field() {
        const std::uint16_t region = std::uint16_t(98 << 8 | 168);
        host.bodies[player.entity].position = {region, 900.0f, 0.0f, 900.0f};
        const auto add = [&](std::uint32_t id, float x, float z) {
            host.bodies[id].position = {region, x, 0.0f, z};
            host.mobs[id].stats = TrainingDummy();
        };
        add(kSelected, 930.0f, 900.0f);
        // Around the selected one and the caster: 8 to 700 away.
        const std::pair<float, float> offsets[] = {{938, 900}, {930, 915}, {955, 900}, {930, 960}, {990, 900}, {930, 1060},
                                                   {1050, 900}, {930, 1200}, {1230, 900}, {930, 1600}, {870, 900}, {900, 840}};
        std::uint32_t id = 1001;
        for (const auto& [x, z] : offsets) {
            add(id, x, z);
            others.push_back(id++);
        }
    }
};

// Who an area should take, worked out from its 'efr' apart from the skill unit: within the caster's
// body (5), the range and the dummy's body (5) of its centre, by id, the selected one counted in the cap.
std::vector<std::uint32_t> ExpectedTargets(Field& f, const assets::SkillRow& row, double weaponRange) {
    std::vector<std::uint32_t> out{Field::kSelected};
    const assets::SkillParam area = assets::FindSkillParam(row, assets::SkillTag("efr"), 1);
    if (!area || (area[5] & 0x08) == 0) return out;
    const int shape = area.Int(1);
    const double range = double(area[2]);
    const auto full = [&] { return area[3] != 0 && out.size() >= area[3]; };
    const proto::FlatPosition me = proto::ToFlat(f.host.bodies[f.player.entity].position);
    const proto::FlatPosition target = proto::ToFlat(f.host.bodies[Field::kSelected].position);
    const auto at = [&](std::uint32_t id) { return proto::ToFlat(f.host.bodies[id].position); };
    if (shape == 1 || shape == 2) {
        const proto::FlatPosition c = shape == 1 ? me : target;
        for (const std::uint32_t id : f.others) {
            if (full()) break;
            if (std::hypot(at(id).x - c.x, at(id).z - c.z) <= 5.0 + range + 5.0) out.push_back(id);
        }
    } else if (shape == 3 || shape == 4) {
        // A line from the caster: its distance from the line by projection (its angle's sine).
        double dx = target.x - me.x, dz = target.z - me.z;
        if (shape == 3) {
            const double d = std::hypot(dx, dz), length = row.range > 0 ? double(row.range) : weaponRange;
            dx = dx / d * length;
            dz = dz / d * length;
        }
        const double length = std::hypot(dx, dz);
        for (const std::uint32_t id : f.others) {
            if (full()) break;
            const double rx = at(id).x - me.x, rz = at(id).z - me.z, d = std::hypot(rx, rz);
            const double along = length > 0.0 ? (rx * dx + rz * dz) / length : 0.0, across = std::sqrt(std::max(0.0, d * d - along * along));
            if (d <= length + 5.0 + 5.0 && across <= 5.0 + range) out.push_back(id);
        }
    } else if (shape == 6) {
        std::set<std::uint32_t> taken;
        proto::FlatPosition last = target;
        while (!full()) {
            std::uint32_t best = 0;
            double nearest = 1e9;
            for (const std::uint32_t id : f.others) {
                const double d = std::hypot(at(id).x - last.x, at(id).z - last.z);
                if (!taken.contains(id) && d <= range && d < nearest) {
                    nearest = d;
                    best = id;
                }
            }
            if (best == 0) break;
            taken.insert(best);
            out.push_back(best);
            last = at(best);
        }
    }
    return out;
}

} // namespace

TEST_CASE("every attack skill group on a field of dummies: its area's targets, its chain's hits (game files; K2)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    std::map<std::string, int> outcome;
    std::map<std::string, std::vector<std::string>> examples;
    int groups = 0, areas = 0, chains = 0;
    for (const assets::MasteryRow& m : table.masteries()) {
        for (const assets::SkillRow* s : table.Board(m.id)) {
            if (!s->inService) continue;
            const game::SkillTraits t = game::TraitsOf(*s);
            if (!t.attack || t.passive) continue;
            ++groups;
            Field f;
            server::Skills skills(f.host, &table, 7);
            f.player.stats = StrongPlayer(s->weaponKinds[0] != 255 ? s->weaponKinds[0] : 1);
            WearFor(f.player, *s);
            f.player.stats.range = 20.0f; // a weapon's reach (a line skill of no range of its own takes it)
            f.player.book.groups[s->group] = s->level;
            for (std::size_t i = 0; i < s->masteries.size(); ++i) {
                if (s->masteries[i] != 0) f.player.book.masteries[s->masteries[i]] = std::max(1, s->masteryLevels[i]);
            }
            const Clock::time_point t0 = Clock::now();
            server::SkillCaster c0 = f.player.Caster();
            skills.Use(c0, {s->id, Field::kSelected}, t0);
            for (double at = 0.0; at < 15.0; at += 0.05) {
                server::SkillCaster c = f.player.Caster();
                skills.Fight(c, t0 + std::chrono::milliseconds(int(at * 1000.0)));
                // The skill's swing out: no basic attacks after it (its hits still land).
                if (!f.host.All<proto::EntitySwing>().empty()) f.player.state.attackTarget = 0;
            }
            // The rows of its chain, their hits on the selected target, the area of each.
            int expectedHits = 0;
            std::set<std::uint32_t> expected;
            bool unfinished = false;
            for (const assets::SkillRow* row = s; row != nullptr; row = row->chainNext != 0 ? table.Find(row->chainNext) : nullptr) {
                expectedHits += std::max(1, game::AttackOf(*row).hits);
                for (const std::uint32_t id : ExpectedTargets(f, *row, f.player.stats.range)) expected.insert(id);
                if (const assets::SkillParam a = assets::FindSkillParam(*row, assets::SkillTag("efr"), 1); a && (a.Int(1) == 5 || a.Int(1) == 7)) unfinished = true;
            }
            std::set<std::uint32_t> struck;
            for (const auto& [id, mob] : f.host.mobs) {
                if (mob.hits > 0) struck.insert(id);
            }
            areas += expected.size() > 1 ? 1 : 0;
            chains += s->chainNext != 0 ? 1 : 0;
            const bool pulses = static_cast<bool>(assets::FindSkillParam(*s, assets::SkillTag("efr"), 2));
            const bool dot = assets::FindSkillParam(*s, assets::SkillTag("lks2")) && assets::FindSkillParam(*s, assets::SkillTag("puls"));
            std::string kind;
            if (pulses) {
                kind = "a pulsing area (its own test)";
            } else if (dot) {
                kind = "a DOT's link strikes (the links test)";
            } else if (unfinished) {
                kind = "area of another shape (to come)";
            } else if (f.host.mobs[Field::kSelected].hits != expectedHits) {
                kind = "wrong hits on the selected target";
            } else if (struck != expected) {
                kind = "wrong targets";
            } else {
                kind = "right";
            }
            ++outcome[kind];
            if (examples[kind].size() < 4) {
                examples[kind].push_back(s->codename + " (" + std::to_string(f.host.mobs[Field::kSelected].hits) + "/" + std::to_string(expectedHits) + " hits, " +
                                         std::to_string(struck.size()) + "/" + std::to_string(expected.size()) + " targets)");
            }
        }
    }
    MESSAGE(groups << " attack groups; " << areas << " strike more than the selected target, " << chains << " are chains");
    for (const auto& [kind, n] : outcome) {
        std::string list;
        for (const std::string& e : examples[kind]) list += " " + e;
        MESSAGE("  " << kind << ": " << n << " (" << list << " )");
    }
    CHECK(outcome["wrong hits on the selected target"] == 0);
    CHECK(outcome["wrong targets"] == 0);
    CHECK(outcome["right"] > 100);
}

namespace {

// A player with `codename` learned (its masteries high enough) and the weapon it takes, on the field.
const assets::SkillRow* Learn(Field& f, const assets::SkillTable& table, std::string_view codename) {
    const assets::SkillRow* s = table.Find(codename);
    if (s == nullptr) return nullptr;
    f.player.stats = StrongPlayer(s->weaponKinds[0] != 255 ? s->weaponKinds[0] : 1);
    WearFor(f.player, *s);
    f.player.book.groups[s->group] = s->level;
    for (std::size_t i = 0; i < s->masteries.size(); ++i) {
        if (s->masteries[i] != 0) f.player.book.masteries[s->masteries[i]] = std::max(1, s->masteryLevels[i]);
    }
    return s;
}

// A cast's release: the caster's update at it (a skill with a preparing or a casting puts its buff on, pays its mana and
// strikes its first row then); when it was (`at` for an instant one).
Clock::time_point Release(Field& f, server::Skills& skills, Clock::time_point at) {
    if (!f.player.state.casting) return at;
    const Clock::time_point release = f.player.state.casting->release;
    server::SkillCaster c = f.player.Caster();
    skills.Fight(c, release);
    return release;
}

// The fight for `seconds` in 50 ms ticks from t0, no basic attacks after the skill's swing.
void RunFight(Field& f, server::Skills& skills, Clock::time_point t0, double seconds) {
    for (double at = 0.0; at < seconds; at += 0.05) {
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t0 + std::chrono::milliseconds(int(at * 1000.0)));
        if (!f.host.All<proto::EntitySwing>().empty()) f.player.state.attackTarget = 0;
    }
}

} // namespace

TEST_CASE("the skills of their own damage: soul chaos pulses around the caster, MP absorb and life drain give back (game files; K2)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);

    SUBCASE("soul chaos: a buff on the caster; every pulse its area around the caster, the nearest by id first, at most its cap") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* chaos = Learn(f, table, "SKILL_EU_WARLOCK_SOULA_CHAOS_A_01");
        REQUIRE(chaos != nullptr);
        const assets::SkillParam area = assets::FindSkillParam(*chaos, assets::SkillTag("efr"), 2);
        const assets::SkillParam pulse = assets::FindSkillParam(*chaos, assets::SkillTag("puls"));
        const assets::SkillParam fixed = assets::FindSkillParam(*chaos, assets::SkillTag("pdmg"));
        REQUIRE(area);
        REQUIRE(pulse);
        REQUIRE(fixed);
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {chaos->id, 0}, t0);
        REQUIRE(f.host.All<proto::BuffAdded>().size() == 1);
        RunFight(f, skills, t0, 15.0);
        // Who a pulse takes: the dummies within the caster's body, the range and their body of it,
        // by id, at most the cap (no selected target: the cap is all of them).
        const proto::FlatPosition me = proto::ToFlat(f.host.bodies[f.player.entity].position);
        std::vector<std::uint32_t> inside;
        std::vector<std::uint32_t> all{Field::kSelected};
        all.insert(all.end(), f.others.begin(), f.others.end());
        std::sort(all.begin(), all.end());
        for (const std::uint32_t id : all) {
            const proto::FlatPosition p = proto::ToFlat(f.host.bodies[id].position);
            if (inside.size() < area[3] && std::hypot(p.x - me.x, p.z - me.z) <= 5.0 + double(area[2]) + 5.0) inside.push_back(id);
        }
        REQUIRE(inside.size() >= 2);
        const int pulses = (15000 - 1) / pulse.Int(0); // in 15 s (the buff lasts its 'dura')
        int percent = 100 - area.Int(4);
        for (const std::uint32_t id : inside) {
            const TestHost::Mob& mob = f.host.mobs[id];
            CHECK_MESSAGE(mob.hits == pulses, id);
            // Each pulse its own damage at its share (the first 100 - falloff), the dummy's level below.
            CHECK_MESSAGE(mob.damage == std::uint64_t(pulses) * std::uint64_t(fixed.Int(0) * percent / 100), id);
            percent = percent * (100 - area.Int(4)) / 100;
        }
        int struck = 0;
        for (const auto& [id, mob] : f.host.mobs) struck += mob.hits > 0 ? 1 : 0;
        CHECK(struck == int(inside.size()));
    }

    SUBCASE("MP absorb: its own damage on the target, all of it back as the caster's MP") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* absorb = Learn(f, table, "SKILL_EU_BARD_FORGETA_MPABSORB_A_01");
        REQUIRE(absorb != nullptr);
        f.player.mp = 1000;
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {absorb->id, Field::kSelected}, t0);
        const std::uint32_t afterCost = f.player.mp;
        RunFight(f, skills, t0, 6.0);
        const TestHost::Mob& mob = f.host.mobs[Field::kSelected];
        CHECK(mob.hits == 1);
        CHECK(mob.damage == std::uint64_t(assets::FindSkillParam(*absorb, assets::SkillTag("pdmg")).Int(0)));
        CHECK(f.player.mp == afterCost - server::Skills::Mana(f.player.stats, *absorb) + mob.damage);
    }

    SUBCASE("life drain: HP taken from the target, the caster's own") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* drain = Learn(f, table, "SKILL_EU_WARLOCK_BLOODA_LIFEDRAIN_A_01");
        REQUIRE(drain != nullptr);
        f.player.hp = 1000;
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {drain->id, Field::kSelected}, t0);
        RunFight(f, skills, t0, 6.0);
        const TestHost::Mob& mob = f.host.mobs[Field::kSelected];
        CHECK(mob.hits == 1);
        CHECK(mob.damage == std::uint64_t(assets::FindSkillParam(*drain, assets::SkillTag("lfst")).Int(0)));
        CHECK(f.player.hp == 1000 + mob.damage);
    }
}

TEST_CASE("every state-giving skill group gives each of its states at its level and time (game files; K3)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // Each group's highest rank among the players' skills in service that give states (their own
    // rows or their chain's), cast as a player would: an attack or a curse on the selected dummy, the
    // states around the caster at once. Imbues, buffs, traps and fields come with K4, K5, K12.
    std::map<std::uint32_t, const assets::SkillRow*> groups;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || s.activity == 0 || s.boardRow == 255 || (s.race != 0 && s.race != 1 && s.race != 3)) continue;
        if (!s.codename.starts_with("SKILL_CH_") && !s.codename.starts_with("SKILL_EU_")) continue;
        const game::SkillTraits t = game::TraitsOf(s);
        if (!t.states || (!t.attack && !t.debuff) || t.trap || s.hasDuration) continue;
        const assets::SkillRow*& top = groups[s.group];
        if (top == nullptr || s.level > top->level) top = &s;
    }
    // A step a level of each element.
    const auto step = [](game::StateKind k) {
        switch (k) {
        case game::StateKind::Freeze: return 97u;
        case game::StateKind::Frostbite: return 250u;
        case game::StateKind::Shock: return 500u;
        case game::StateKind::Poison: return 1000u;
        default: return 750u;
        }
    };
    int checked = 0, states = 0, knocksOnly = 0;
    std::map<std::string, int> byKind;
    std::vector<std::string> wrong;
    for (const auto& [group, s] : groups) {
        // What it gives: every row of its chain.
        std::vector<game::StateGift> expected;
        for (const assets::SkillRow* row = s; row != nullptr; row = row->chainNext != 0 ? table.Find(row->chainNext) : nullptr) {
            for (const game::StateGift& g : game::StateGiftsOf(*row)) expected.push_back(g);
            if (expected.size() > 64) break;
        }
        if (expected.empty()) {
            ++knocksOnly; // its states are knocks only (rolled at most 75 %: the rules' tests)
            continue;
        }
        Field f;
        server::Skills skills(f.host, &table, 7);
        f.player.stats = StrongPlayer(s->weaponKinds[0] != 255 ? s->weaponKinds[0] : 1);
        WearFor(f.player, *s);
        f.player.book.groups[s->group] = s->level;
        for (std::size_t i = 0; i < s->masteries.size(); ++i) {
            if (s->masteries[i] != 0) f.player.book.masteries[s->masteries[i]] = std::max(1, s->masteryLevels[i]);
        }
        // One for enemy players alone (the Wizard's Mana Dry in the 1.188 rows: refused on a monster): the
        // dummies are enemy players.
        if (s->targetEnemyPlayer && !s->targetEnemyMonster) {
            for (const auto& [id, mob] : f.host.mobs) f.host.enemyPlayers.insert(id);
        }
        // Every dummy diseased past 100: each state's roll is certain (the susceptibility, 0xA9).
        for (auto& [id, mob] : f.host.mobs) {
            game::ActiveState d;
            d.kind = game::StateKind::Disease;
            d.level = 1;
            d.value = 100;
            d.durationMs = 100'000'000;
            mob.states.Apply(d);
        }
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {s->id, Field::kSelected}, t0);
        for (double at = 0.0; at < 8.0; at += 0.05) {
            server::SkillCaster c = f.player.Caster();
            skills.Fight(c, t0 + std::chrono::milliseconds(int(at * 1000.0)));
            if (!f.host.All<proto::EntitySwing>().empty()) f.player.state.attackTarget = 0;
        }
        // The first dummy it gave states to: each of the chain's states once, at its level and time
        // on a level-1 target without resistance (an element: its power, a step a level; the others:
        // their level word and their whole time).
        const TestHost::Mob* struck = nullptr;
        for (const auto& [id, mob] : f.host.mobs) {
            if (!mob.took.empty() && (struck == nullptr || id == Field::kSelected)) struck = &mob;
        }
        ++checked;
        if (struck == nullptr) {
            wrong.push_back(s->codename + " (no state given)");
            continue;
        }
        for (const game::StateGift& g : expected) {
            const int level = game::IsElement(g.kind) ? g.words[0] : g.words[2];
            const std::uint32_t ms = game::IsElement(g.kind) ? std::uint32_t(g.words[0]) * step(g.kind) : std::uint32_t(g.words[0]);
            // A time bomb's fuse: one of its first word's non-zero bytes, in seconds.
            const auto fuse = [&](std::uint32_t d) {
                for (int i = 0; i < 4; ++i) {
                    const std::uint32_t b = (std::uint32_t(g.words[0]) >> (8 * i)) & 0xFF;
                    if (b != 0 && d == b * 1000u) return true;
                }
                return false;
            };
            const bool found = std::any_of(struck->took.begin(), struck->took.end(), [&](const game::ActiveState& a) {
                return a.kind == g.kind && a.level == level && (g.kind == game::StateKind::TimeBomb ? fuse(a.durationMs) : a.durationMs == ms);
            });
            ++states;
            ++byKind[game::StateName(g.kind)];
            if (!found) wrong.push_back(s->codename + " " + game::StateName(g.kind));
        }
    }
    std::string kinds;
    for (const auto& [k, n] : byKind) kinds += " " + k + " " + std::to_string(n);
    MESSAGE(checked << " groups give states (and " << knocksOnly << " only knock); " << states << " states checked:" << kinds);
    std::string list;
    for (std::size_t i = 0; i < wrong.size() && i < 12; ++i) list += " " + wrong[i];
    CHECK_MESSAGE(wrong.empty(), wrong.size() << " wrong:" << list);
    CHECK(checked >= 88);
}

TEST_CASE("every buff group changes exactly its numbers while it lasts, gives way to its own higher rank, and shows on the board (game files; K4)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // Each buff group's highest rank (on the board, in service; imbues are K5's) that a player casts on
    // itself, and its lowest.
    std::map<std::uint32_t, std::pair<const assets::SkillRow*, const assets::SkillRow*>> groups;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || s.activity == 0 || s.boardRow == 255 || game::IsImbue(s)) continue;
        if (!s.codename.starts_with("SKILL_CH_") && !s.codename.starts_with("SKILL_EU_")) continue;
        const game::SkillTraits t = game::TraitsOf(s);
        if (!(t.buff || t.toggle) || t.attack || t.trap || (s.targetRequired && !s.targetSelf)) continue;
        // A skill object's buff (Harmony's circle, the poison field) comes from its object (K10's case);
        // a detection's ('dttp') goes on those around, not on its caster (the stealth test, K12).
        if (assets::FindSkillParam(s, assets::SkillTag("efr"), 3) || assets::FindSkillParam(s, assets::SkillTag("dttp"))) continue;
        // A bard's dance needs one of its songs playing ('reqc' 0x20: its own test, U3-04).
        if ((assets::FindSkillParam(s, assets::SkillTag("reqc")).Int(0) & 0x20) != 0) continue;
        auto& [top, bottom] = groups[s.group];
        if (top == nullptr || s.level > top->level) top = &s;
        if (bottom == nullptr || s.level < bottom->level) bottom = &s;
    }
    int checked = 0, toggles = 0, refusedLower = 0, changing = 0;
    std::vector<std::string> wrong;
    for (const auto& [group, pair] : groups) {
        const auto [top, bottom] = pair;
        Field f;
        server::Skills skills(f.host, &table, 7);
        f.player.stats = StrongPlayer(top->weaponKinds[0] != 255 ? top->weaponKinds[0] : 1);
        WearFor(f.player, *top);
        f.player.stats.strength = 200;
        f.player.stats.intelligence = 200;
        f.player.book.groups[top->group] = top->level;
        for (std::size_t i = 0; i < top->masteries.size(); ++i) {
            if (top->masteries[i] != 0) f.player.book.masteries[top->masteries[i]] = std::max(1, top->masteryLevels[i]);
        }
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {top->id, 0}, t0);
        Release(f, skills, t0);
        ++checked;
        const auto buffs = f.host.All<proto::BuffAdded>();
        if (buffs.empty() || buffs.back().skill != top->id || f.player.state.buffs.size() != 1) {
            wrong.push_back(top->codename + " (not on the board)");
            continue;
        }
        // Exactly its changes, on the numbers it had as it started, under its token; its holder's
        // numbers made again.
        const server::SkillState::Buff& b = f.player.state.buffs[0];
        const std::vector<game::StatChange> expected = game::BuffChanges(*top, server::Skills::BasisOf(f.player.stats));
        const game::StatSheet sheet = f.player.state.Sheet();
        bool same = b.changes.size() == expected.size() && f.host.statsChanged[f.player.entity] == 1;
        for (std::size_t i = 0; same && i < expected.size(); ++i) {
            same = b.changes[i].stat == expected[i].stat && b.changes[i].kind == expected[i].kind && b.changes[i].value == expected[i].value;
        }
        for (const game::StatSheet::Entry& e : sheet.Entries()) same = same && e.source == b.token;
        if (!same) wrong.push_back(top->codename + " (its changes)");
        changing += expected.empty() ? 0 : 1;
        // A lower rank of its group is refused while it runs (when the group has one; a toggle's group
        // turns the toggle off instead, below).
        if (bottom != top && b.upkeepMs == 0) {
            f.player.state.skillCooling.clear();
            f.player.state.groupCooling.clear();
            server::SkillCaster c1 = f.player.Caster();
            f.player.state.busyUntil = {}; // the first's hold over (the action list's: a press during it waits behind it)
            skills.Use(c1, {bottom->id, 0}, t0 + std::chrono::milliseconds(1));
            const auto answers = f.host.All<proto::SkillUseAnswer>();
            if (!answers.empty() && answers.back().status == proto::SkillUseStatus::Overlap && f.player.state.buffs.size() == 1) {
                ++refusedLower;
            } else if (!answers.empty() && answers.back().status != proto::SkillUseStatus::Cooling) {
                wrong.push_back(top->codename + " (a lower rank not refused)");
            }
        }
        // Its end: a toggle's MP every period until the MP runs out; a timed one at its time.
        if (b.upkeepMs > 0) {
            ++toggles;
            const std::uint32_t cost = b.upkeepMp;
            const int period = b.upkeepMs;
            // Used again it turns off; on again, its MP every period until the MP runs out.
            server::SkillCaster off = f.player.Caster();
            skills.Use(off, {top->id, 0}, t0 + std::chrono::milliseconds(2));
            if (!f.player.state.buffs.empty()) wrong.push_back(top->codename + " (its toggle not turned off)");
            f.player.state.skillCooling.clear();
            f.player.state.groupCooling.clear();
            server::SkillCaster on = f.player.Caster();
            f.player.state.busyUntil = {};
            skills.Use(on, {top->id, 0}, t0);
            const Clock::time_point onAt = Release(f, skills, t0);
            if (f.player.state.buffs.size() != 1) {
                wrong.push_back(top->codename + " (its toggle not on again)");
                continue;
            }
            f.player.mp = cost * 2 + cost / 2;
            for (int k = 1; k <= 3; ++k) {
                server::SkillCaster c = f.player.Caster();
                skills.ExpireBuffs(c, onAt + std::chrono::milliseconds(period * k + 1));
            }
            if (!f.player.state.buffs.empty() || f.player.mp != cost / 2) wrong.push_back(top->codename + " (its toggle's MP)");
        } else {
            const auto until = b.until;
            server::SkillCaster c = f.player.Caster();
            skills.ExpireBuffs(c, until - std::chrono::milliseconds(1));
            const bool kept = f.player.state.buffs.size() == 1;
            skills.ExpireBuffs(c, until);
            if (!kept || !f.player.state.buffs.empty() || f.host.All<proto::BuffRemoved>().empty() || !f.player.state.Sheet().Empty()) {
                wrong.push_back(top->codename + " (its end)");
            }
        }
    }
    MESSAGE(checked << " buff groups cast on oneself (" << changing << " change numbers, " << toggles << " toggles); " << refusedLower
                    << " refuse their lower rank while running");
    std::string list;
    for (std::size_t i = 0; i < wrong.size() && i < 12; ++i) list += " " + wrong[i];
    CHECK_MESSAGE(wrong.empty(), wrong.size() << " wrong:" << list);
    CHECK(checked > 100);
}

TEST_CASE("which buffs outlive the logout: the timed jobs ('cbuf': the mall's, events', quests'), not the players' skills (game files; K4)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    int players = 0, kept = 0;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || !s.hasDuration) continue;
        if (s.codename.starts_with("SKILL_CH_") || s.codename.starts_with("SKILL_EU_")) {
            ++players;
            CHECK_MESSAGE(!server::Skills::OutlivesLogout(s), s.codename);
        } else if (server::Skills::OutlivesLogout(s)) {
            ++kept;
        }
    }
    MESSAGE(players << " player skills with a time (none kept), " << kept << " others kept as timed jobs");
    CHECK(players > 1000);
    CHECK(kept > 50);
    // The mall's speed scroll and the Beginner Scroll of Movement (ITEM_ETC_SPEED_UP_BASIC) both.
    for (const char* name : {"SKILL_MALL_MOVE_SPEED_UP_50_01", "SKILL_ETC_SPEED_UP_BASIC_01"}) {
        const assets::SkillRow* speed = table.Find(std::string_view(name));
        REQUIRE_MESSAGE(speed != nullptr, name);
        CHECK_MESSAGE(server::Skills::OutlivesLogout(*speed), name);
    }
}

TEST_CASE("a buff cast on another player, and a buff's cuts on an enemy (game files; K4)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);

    SUBCASE("a buff on an ally: the other player holds it, the caster pays") {
        // A buff that asks for an ally as its target (in service, on the board).
        const assets::SkillRow* bless = nullptr;
        for (const auto& [id, s] : table.rows()) {
            const game::SkillTraits t = game::TraitsOf(s);
            if (s.inService && s.boardRow != 255 && t.buff && s.targetRequired && s.targetAlly && !game::BuffChanges(s, {}).empty() &&
                (bless == nullptr || s.id < bless->id)) {
                bless = &s;
            }
        }
        REQUIRE(bless != nullptr);
        MESSAGE("the buff: " << bless->codename);
        Field f;
        server::Skills skills(f.host, &table, 7);
        Learn(f, table, bless->codename);
        TestPlayer other;
        other.entity = 7;
        other.stats = StrongPlayer(1);
        f.host.bodies[other.entity].position = f.host.bodies[f.player.entity].position;
        f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
            if (entity == other.entity) return other.Caster();
            return std::nullopt;
        };
        const std::uint32_t mpBefore = f.player.mp;
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {bless->id, other.entity}, Clock::now());
        CHECK(other.state.buffs.size() == 1);
        CHECK(f.player.state.buffs.empty());
        CHECK(f.player.mp == mpBefore - server::Skills::Mana(f.player.stats, *bless));
        CHECK(f.host.statsChanged[other.entity] == 1);
        bool told = false;
        for (const TestHost::Told& t : f.host.told) told = told || (std::holds_alternative<proto::BuffAdded>(t.message) && t.entity == other.entity);
        CHECK(told);
    }

    SUBCASE("a buff's cuts on an enemy: the Chinese water's cancel lands as a curse of its time") {
        const assets::SkillRow* cancel = table.Find(std::string_view("SKILL_CH_WATER_CANCEL_A_01"));
        REQUIRE(cancel != nullptr);
        Field f;
        server::Skills skills(f.host, &table, 7);
        Learn(f, table, cancel->codename);
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {cancel->id, Field::kSelected}, t0);
        RunFight(f, skills, t0, 6.0);
        const TestHost::Mob& mob = f.host.mobs[Field::kSelected];
        CHECK(mob.debuffs == std::vector<std::uint32_t>{cancel->id});
        CHECK(mob.damage == 0u); // no damage of its own
    }
}

namespace {

// Every dummy of the field diseased past 100: each state's roll is certain (the susceptibility, 0xA9).
void Susceptible(Field& f) {
    for (auto& [id, mob] : f.host.mobs) {
        game::ActiveState d;
        d.kind = game::StateKind::Disease;
        d.level = 1;
        d.value = 100;
        d.durationMs = 100'000'000;
        mob.states.Apply(d);
    }
}

// The basic attacks' fight on the selected dummy from `from` for `seconds`, in 50 ms ticks, then a
// second more for the last swing's hits to land.
void Swing(Field& f, server::Skills& skills, Clock::time_point t0, double from, double seconds) {
    server::SkillCaster c0 = f.player.Caster();
    skills.Attack(c0, Field::kSelected, t0 + std::chrono::milliseconds(int(from * 1000.0)));
    for (double at = from; at < from + seconds + 1.0; at += 0.05) {
        if (at >= from + seconds) f.player.state.attackTarget = 0;
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t0 + std::chrono::milliseconds(int(at * 1000.0)));
    }
    f.player.state.StopAttacking();
}

} // namespace

TEST_CASE("every imbue: its force and its states join each hit while it lasts, one imbue at a time (game files; K5)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // The players' imbue groups on the boards (Activity 1, a time, an 'att'),
    // each by its highest rank; the other skills of the magical lane's 'att' are not imbues.
    std::map<std::uint32_t, const assets::SkillRow*> groups;
    std::set<std::string> notImbues;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || s.boardRow == 255 || (!s.codename.starts_with("SKILL_CH_") && !s.codename.starts_with("SKILL_EU_"))) continue;
        if (game::IsImbue(s)) {
            CHECK(game::TraitsOf(s).imbue);
            CHECK_FALSE(game::TraitsOf(s).attack);
            const assets::SkillRow*& top = groups[s.group];
            if (top == nullptr || s.level > top->level) top = &s;
        } else if (s.attack && s.attackKind == 8 && s.activity != 0) {
            CHECK_FALSE(game::TraitsOf(s).imbue);
            notImbues.insert(s.codename.substr(0, s.codename.rfind('_')));
        }
    }
    MESSAGE(groups.size() << " imbue groups; not imbues though of the magical lane: " << notImbues.size() << " groups");
    CHECK(groups.size() == 17);
    CHECK(notImbues.contains("SKILL_EU_WARLOCK_DOTA_BURN_A"));
    CHECK(notImbues.contains("SKILL_EU_BARD_BATTLAA_EXPLOSION_A"));

    int checked = 0, jumps = 0;
    std::vector<std::string> wrong;
    for (const auto& [group, imbue] : groups) {
        ++checked;
        // Its holder: a Chinese with a sword (any weapon takes a Chinese imbue), a Rogue with a dagger.
        const int weapon = imbue->weaponKinds[0] != 255 ? (imbue->weaponKinds[1] != 255 ? imbue->weaponKinds[1] : imbue->weaponKinds[0]) : 2;
        // The same swings without the imbue first: what it adds.
        std::uint64_t plain = 0;
        {
            Field f;
            server::Skills skills(f.host, &table, 7);
            f.player.stats = StrongPlayer(weapon);
            Swing(f, skills, Clock::now(), 0.0, 1.5);
            plain = f.host.mobs[Field::kSelected].damage;
        }
        Field f;
        server::Skills skills(f.host, &table, 7);
        Learn(f, table, imbue->codename);
        f.player.stats = StrongPlayer(weapon);
        WearFor(f.player, *imbue);
        Susceptible(f);
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {imbue->id, 0}, t0);
        if (f.player.state.buffs.size() != 1 || skills.Imbue(f.player.state, t0) != imbue) {
            wrong.push_back(imbue->codename + " (not running)");
            continue;
        }
        const auto until = f.player.state.buffs[0].until;
        // While it lasts: every hit carries it (its force at the basic attack's share, more damage
        // than without), its states given at their level on the struck.
        Swing(f, skills, t0, 0.1, 1.5);
        const auto hits = f.host.All<proto::EntityDamaged>();
        const TestHost::Mob& mob = f.host.mobs[Field::kSelected];
        const bool allCarry = !hits.empty() && std::all_of(hits.begin(), hits.end(), [&](const proto::EntityDamaged& d) { return d.imbue == imbue->id; });
        if (!allCarry) wrong.push_back(imbue->codename + " (a hit without it)");
        const bool force = imbue->attackPower > 0;
        if (force && mob.damage <= plain) wrong.push_back(imbue->codename + " (no force: " + std::to_string(mob.damage) + " vs " + std::to_string(plain) + ")");
        for (const game::StateGift& g : game::StateGiftsOf(*imbue)) {
            const int level = game::IsElement(g.kind) ? g.words[0] : g.words[2];
            const bool given = std::any_of(mob.took.begin(), mob.took.end(), [&](const game::ActiveState& a) { return a.kind == g.kind && a.level == level; });
            if (!given) wrong.push_back(imbue->codename + " (its " + game::StateName(g.kind) + ")");
        }
        // Its area (the lightning's): the nearest other dummy (8 away) struck too, at its share.
        const bool area = static_cast<bool>(assets::FindSkillParam(*imbue, assets::SkillTag("efr"), 1));
        const bool jumped = f.host.mobs[f.others[0]].hits > 0;
        if (jumped != area) wrong.push_back(imbue->codename + (area ? " (no jump)" : " (a jump of no area)"));
        jumps += jumped ? 1 : 0;
        // One at a time: another group's imbue is refused while it runs.
        const auto other = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first != group; });
        if (other != groups.end()) {
            f.player.book.groups[other->second->group] = other->second->level;
            f.player.state.skillCooling.clear();
            f.player.state.groupCooling.clear();
            server::SkillCaster c1 = f.player.Caster();
            skills.Use(c1, {other->second->id, 0}, t0 + std::chrono::milliseconds(2700));
            const auto answers = f.host.All<proto::SkillUseAnswer>();
            if (answers.empty() || answers.back().status != proto::SkillUseStatus::Overlap ||
                skills.Imbue(f.player.state, t0 + std::chrono::milliseconds(2700)) != imbue) {
                wrong.push_back(imbue->codename + " (another imbue beside it)");
            }
        }
        // Its end: hits after it carry nothing.
        server::SkillCaster c2 = f.player.Caster();
        skills.ExpireBuffs(c2, until);
        f.host.told.clear();
        const double after = std::chrono::duration<double>(until - t0).count() + 0.1;
        Swing(f, skills, t0, after, 1.5);
        const auto later = f.host.All<proto::EntityDamaged>();
        if (later.empty() || std::any_of(later.begin(), later.end(), [](const proto::EntityDamaged& d) { return d.imbue != 0; })) {
            wrong.push_back(imbue->codename + " (outlived its time)");
        }
    }
    MESSAGE(checked << " imbue groups cast and struck with; " << jumps << " jump to a second target");
    std::string list;
    for (std::size_t i = 0; i < wrong.size() && i < 12; ++i) list += " " + wrong[i];
    CHECK_MESSAGE(wrong.empty(), wrong.size() << " wrong:" << list);
    CHECK(checked == 17);
    CHECK(jumps == 5);
}

TEST_CASE("an imbue is cast on the move, a buff of activity 2 stops its caster (game files; B)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    const assets::SkillRow* imbue = table.Find(std::string_view("SKILL_CH_COLD_GIGONGTA_A_01"));
    REQUIRE(imbue != nullptr);
    CHECK(imbue->activity == 1);
    // A buff on oneself that is an action.
    const assets::SkillRow* action = nullptr;
    for (const auto& [id, s] : table.rows()) {
        if (s.inService && s.boardRow != 255 && s.activity == 2 && !s.targetRequired && s.hasDuration && s.codename.starts_with("SKILL_CH_") &&
            game::TraitsOf(s).buff && s.mpCost >= 0 && s.weaponKinds[0] == 255 && s.weaponKinds[1] == 255 && !assets::FindSkillParam(s, assets::SkillTag("efr"), 3)) {
            action = &s;
            break;
        }
    }
    REQUIRE(action != nullptr);
    const auto cast = [&](const assets::SkillRow& skill) {
        Field f;
        server::Skills skills(f.host, &table, 7);
        Learn(f, table, skill.codename);
        f.player.stats = StrongPlayer(2);
        WearFor(f.player, skill);
        f.host.walking = true;
        server::SkillCaster c = f.player.Caster();
        const Clock::time_point t = Clock::now();
        skills.Use(c, {skill.id, 0}, t);
        Release(f, skills, t);
        CHECK(f.player.state.buffs.size() == 1);
        return f.host.stops;
    };
    CHECK(cast(*imbue) == 0);
    CHECK(cast(*action) == 1);
}

TEST_CASE("an imbue goes with the swing it began: a chain's later rows keep it after it ends (game files; K5)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // A Chinese sword chain of several rows, each taking a share of an imbue.
    const assets::SkillRow* chain = nullptr;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || s.boardRow == 255 || s.chainNext == 0 || !s.codename.starts_with("SKILL_CH_SWORD_") || game::AttackOf(s).imbuePercent <= 0) continue;
        if (chain == nullptr || s.id < chain->id) chain = &s;
    }
    REQUIRE(chain != nullptr);
    const assets::SkillRow* fire = table.Find(std::string_view("SKILL_CH_FIRE_GIGONGTA_A_01"));
    REQUIRE(fire != nullptr);
    Field f;
    server::Skills skills(f.host, &table, 7);
    Learn(f, table, chain->codename);
    f.player.book.groups[fire->group] = fire->level;
    const Clock::time_point t0 = Clock::now();
    server::SkillCaster c0 = f.player.Caster();
    skills.Use(c0, {fire->id, 0}, t0);
    REQUIRE(f.player.state.buffs.size() == 1);
    server::SkillCaster c1 = f.player.Caster();
    skills.Use(c1, {chain->id, Field::kSelected}, t0);
    // Its swing begins (walked into reach first), then the imbue is ended (its time cut short) before
    // the rows' hits land.
    for (int tick = 0; tick < 80 && f.player.state.pendingHits.empty(); ++tick) {
        server::SkillCaster c2 = f.player.Caster();
        skills.Fight(c2, t0 + std::chrono::milliseconds(tick * 50));
    }
    REQUIRE_FALSE(f.player.state.pendingHits.empty());
    server::SkillCaster c3 = f.player.Caster();
    skills.ExpireBuffs(c3, f.player.state.buffs[0].until);
    REQUIRE(f.player.state.buffs.empty());
    RunFight(f, skills, t0, 8.0);
    const auto hits = f.host.All<proto::EntityDamaged>();
    MESSAGE(chain->codename << ": " << hits.size() << " hits");
    CHECK(hits.size() >= 2);
    for (const proto::EntityDamaged& d : hits) CHECK(d.imbue == fire->id);
}

TEST_CASE("every passive group's numbers are in the stats when learned (and what it asks worn), out when not (game files; K6)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // Each player passive group's highest rank in service.
    std::map<std::uint32_t, const assets::SkillRow*> groups;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || s.activity != 0 || (!s.codename.starts_with("SKILL_CH_") && !s.codename.starts_with("SKILL_EU_"))) continue;
        const assets::SkillRow*& top = groups[s.group];
        if (top == nullptr || s.level > top->level) top = &s;
    }
    const game::CombatStats plain = StrongPlayer(1);
    const game::BuffBasis basis = server::Skills::BasisOf(plain);
    int checked = 0, conditional = 0, changing = 0, valued = 0;
    std::vector<std::string> wrong;
    for (const auto& [group, s] : groups) {
        ++checked;
        TestPlayer p;
        p.book.groups[s->group] = s->level;
        // What it asks worn: its weapon, its shield, its armour.
        bool asks = false;
        assets::ForEachSkillParam(*s, [&](std::uint32_t code, std::span<const std::uint32_t> args) {
            if (code == assets::SkillTag("reqi") && args.size() >= 2 && args[0] == 6 && p.state.worn.weapon == 0) p.state.worn.weapon = int(args[1]);
            asks = asks || code == assets::SkillTag("reqi");
        });
        WearFor(p, *s);
        conditional += asks ? 1 : 0;
        // Learned and worn: its changes in the sheet, on the numbers without it, its own source; its values.
        const game::Passives on = game::PassivesOf(p.book, table, p.state.worn, basis);
        const std::vector<game::StatChange> expected = game::BuffChanges(*s, basis);
        if (on.entries.size() != 1 || !on.entries[0].active || on.entries[0].changes.size() != expected.size()) {
            wrong.push_back(s->codename + " (not running)");
            continue;
        }
        game::StatSheet sheet;
        on.AddTo(sheet);
        for (const game::StatChange& c : expected) {
            const auto changes = sheet.Changes(c.stat);
            if (changes == 0) wrong.push_back(s->codename + " (a change not in the sheet)");
        }
        changing += expected.empty() ? 0 : 1;
        int values = 0;
        assets::ForEachSkillParam(*s, [&](std::uint32_t code, std::span<const std::uint32_t> args) {
            if (code != assets::SkillTag("setv") || args.size() < 2) return;
            ++values;
            if (on.modifiers.Find(args[0]) != int(args[1])) wrong.push_back(s->codename + " (a value)");
        });
        valued += values > 0 ? 1 : 0;
        // The numbers a player has with it: those it changes differ from the plain ones.
        const game::CombatStats with = game::PlayerCombatStats(100, 100, 100, {}, nullptr, sheet);
        const game::CombatStats without = game::PlayerCombatStats(100, 100, 100, {}, nullptr);
        for (const game::StatChange& c : expected) {
            const bool shown = (c.stat == game::stat::kMaxHp && with.maxHp != without.maxHp) || (c.stat == game::stat::kMaxMp && with.maxMp != without.maxMp) ||
                               (c.stat == game::stat::kPhyDefence && with.phyDefence != without.phyDefence) ||
                               (c.stat == game::stat::kMagDefence && with.magDefence != without.magDefence) ||
                               (c.stat == game::stat::kHitRate && with.hitRate != without.hitRate) || (c.stat == game::stat::kParry && with.evasion != without.evasion) ||
                               (c.stat == game::stat::kCritical && with.critical != without.critical);
            const bool counted = c.stat == game::stat::kMaxHp || c.stat == game::stat::kMaxMp || c.stat == game::stat::kPhyDefence ||
                                 c.stat == game::stat::kMagDefence || c.stat == game::stat::kHitRate || c.stat == game::stat::kParry || c.stat == game::stat::kCritical;
            if (counted && c.value != 0.0f && !shown) wrong.push_back(s->codename + " (not in its numbers)");
        }
        // What it asks not worn: in the book but not running, nothing in the sheet, no values.
        if (asks) {
            const game::Passives off = game::PassivesOf(p.book, table, {}, basis);
            game::StatSheet none;
            off.AddTo(none);
            if (off.entries.size() != 1 || off.entries[0].active || !none.Empty() || !off.modifiers.Empty()) wrong.push_back(s->codename + " (ran unworn)");
        }
        // Not learned: nothing.
        if (!game::PassivesOf({}, table, p.state.worn, basis).entries.empty()) wrong.push_back(s->codename + " (ran unlearned)");
    }
    MESSAGE(checked << " passive groups: " << changing << " change numbers, " << valued << " give named values, " << conditional << " ask for something worn");
    std::string list;
    for (std::size_t i = 0; i < wrong.size() && i < 12; ++i) list += " " + wrong[i];
    CHECK_MESSAGE(wrong.empty(), wrong.size() << " wrong:" << list);
    CHECK(checked == 50);
}

TEST_CASE("every named value reaches the skills that read it (game files; K6)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // Every value a player passive gives (its highest rank), and every skill in service that reads it.
    game::SkillModifiers all;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || s.activity != 0 || !s.codename.starts_with("SKILL_EU_")) continue;
        assets::ForEachSkillParam(s, [&](std::uint32_t code, std::span<const std::uint32_t> args) {
            if (code == assets::SkillTag("setv") && args.size() >= 2 && int(args[1]) > all.Find(args[0]).value_or(0)) all.Set(args[0], int(args[1]));
        });
    }
    // The values of other steps: stealth's speed and damage, the traps', the Bard's areas (K12).
    const std::set<std::string> later = {"STSP", "DGAA", "TRAA", "MUER", "DSER", "MUCR", "DSCR"};
    std::map<std::string, int> readers, reached;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || s.activity == 0) continue;
        const bool magical = game::AttackOf(s).magical;
        assets::ForEachSkillParam(s, [&](std::uint32_t code, std::span<const std::uint32_t> args) {
            if (code != assets::SkillTag("getv") || args.empty() || !all.Find(args[0])) return;
            std::string name(4, ' ');
            for (int i = 0; i < 4; ++i) name[std::size_t(i)] = char((args[0] >> (24 - 8 * i)) & 0xFF);
            ++readers[name];
            game::SkillModifiers one;
            one.Set(args[0], *all.Find(args[0]));
            const game::SkillBoosts b = game::BoostsOf(s, magical, one);
            const game::SkillBoosts none = game::BoostsOf(s, magical, {});
            const bool moved = b.spread != none.spread || b.pointFactor != none.pointFactor || b.fixedDamage != none.fixedDamage ||
                               b.lifeSteal != none.lifeSteal || b.range != none.range || b.manaCuts != none.manaCuts || b.upkeepCut != none.upkeepCut ||
                               b.extraMs != none.extraMs || b.extraPercent != none.extraPercent || b.poisonLevel != none.poisonLevel ||
                               b.poisonDamage != none.poisonDamage || b.heal != none.heal || b.bonus.strength != none.bonus.strength ||
                               b.bonus.intelligence != none.bonus.intelligence || b.bonus.phyDefence != none.bonus.phyDefence ||
                               b.bonus.magDefence != none.bonus.magDefence;
            reached[name] += moved ? 1 : 0;
        });
    }
    std::vector<std::string> wrong;
    std::string summary;
    for (const auto& [name, n] : readers) {
        summary += " " + name + " " + std::to_string(reached[name]) + "/" + std::to_string(n);
        if (later.contains(name)) continue;
        if (reached[name] != n) wrong.push_back(name);
    }
    MESSAGE(readers.size() << " values read:" << summary);
    std::string list;
    for (const std::string& w : wrong) list += " " + w;
    CHECK_MESSAGE(wrong.empty(), "not reaching their readers:" << list);
    CHECK(readers.size() >= 34);
}

TEST_CASE("a skill's conditions on what is worn: refused without them, its buff ended when they go, the fire shields kept (game files; K6)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    for (const char* codename : {"SKILL_CH_SWORD_SHIELD_A_01", "SKILL_CH_FIRE_SHIELD_A_01"}) {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* s = Learn(f, table, codename);
        REQUIRE_MESSAGE(s != nullptr, codename);
        // No shield: refused.
        f.player.state.worn.shield = 0;
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {s->id, 0}, Clock::now());
        const auto answers = f.host.All<proto::SkillUseAnswer>();
        REQUIRE_FALSE(answers.empty());
        CHECK_MESSAGE(answers.back().status == proto::SkillUseStatus::Weapon, codename);
        CHECK(f.player.state.buffs.empty());
        // A Chinese shield: it runs; the shield taken off: it ends, but a fire shield.
        f.player.state.worn.shield = 1;
        server::SkillCaster c1 = f.player.Caster();
        skills.Use(c1, {s->id, 0}, Clock::now());
        REQUIRE_MESSAGE(f.player.state.buffs.size() == 1, codename);
        f.player.state.worn.shield = 0;
        server::SkillCaster c2 = f.player.Caster();
        skills.EndUnfitBuffs(c2);
        const bool fire = std::string_view(codename).starts_with("SKILL_CH_FIRE_SHIELD_");
        CHECK_MESSAGE(f.player.state.buffs.size() == (fire ? 1u : 0u), codename);
    }
}

TEST_CASE("the passives' values in the unit: a Wizard's MP and reach, a Rogue's poison blade's time and poison (game files; K6)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // The Wizard's mana passive at its top rank: its fire skills' MP 17 % less, their attack 100 % more
    // with the fire passive's.
    const assets::SkillRow* decrease = table.Find(std::string_view("SKILL_EU_WIZARD_MANAP_DECREASE_A_07"));
    const assets::SkillRow* firePassive = table.Find(std::string_view("SKILL_EU_WIZARD_SPIRITP_FIRE_A_25"));
    const assets::SkillRow* bolt = table.Find(std::string_view("SKILL_EU_WIZARD_FIREA_POINT_A_01"));
    REQUIRE((decrease != nullptr && firePassive != nullptr && bolt != nullptr));
    game::SkillBook book;
    book.groups[decrease->group] = decrease->level;
    book.groups[firePassive->group] = firePassive->level;
    const game::Passives wizard = game::PassivesOf(book, table, {}, {});
    const game::CombatStats stats = StrongPlayer(11);
    const std::uint32_t plainMana = server::Skills::Mana(stats, *bolt);
    CHECK(server::Skills::Mana(stats, *bolt, wizard.modifiers) == std::uint32_t(std::trunc(double(plainMana) * 0.83)));
    CHECK(server::Skills::AttackFor(book, *bolt, wizard.modifiers).pointFactor == doctest::Approx(2.0));
    // The Rogue's: the poison blade 3 s longer, its poison 30 levels and 379 a tick more on the struck.
    const assets::SkillRow* maintain = table.Find(std::string_view("SKILL_EU_ROG_POIS_DAGP_MAINTAIN_A_05"));
    const assets::SkillRow* poison = table.Find(std::string_view("SKILL_EU_ROG_POIS_DAGP_POISON_A_09"));
    REQUIRE((maintain != nullptr && poison != nullptr));
    Field f;
    server::Skills skills(f.host, &table, 7);
    const assets::SkillRow* blade = Learn(f, table, "SKILL_EU_ROG_POISONA_BLADE_A_01");
    REQUIRE(blade != nullptr);
    f.player.book.groups[maintain->group] = maintain->level;
    f.player.book.groups[poison->group] = poison->level;
    f.player.state.passives = game::PassivesOf(f.player.book, table, f.player.state.worn, {});
    const Clock::time_point t0 = Clock::now();
    server::SkillCaster c0 = f.player.Caster();
    skills.Use(c0, {blade->id, 0}, t0);
    REQUIRE(f.player.state.buffs.size() == 1);
    CHECK(f.player.state.buffs[0].until - t0 == std::chrono::milliseconds(blade->buffDurationMs + 3000));
    Susceptible(f);
    Swing(f, skills, t0, 0.1, 1.5);
    const TestHost::Mob& mob = f.host.mobs[Field::kSelected];
    const auto it = std::find_if(mob.took.begin(), mob.took.end(), [](const game::ActiveState& a) { return a.kind == game::StateKind::Poison; });
    REQUIRE(it != mob.took.end());
    const game::StateGift gift = game::StateGiftsOf(*blade)[0];
    CHECK(it->level == gift.words[0] + 110);
    CHECK(it->value == gift.words[2] + 379);
}

TEST_CASE("every heal group heals its amount, every cure takes its states up to its grade (game files; K7)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // Each heal or cure group's highest rank among the players' skills (the resurrections are K9's).
    std::map<std::uint32_t, const assets::SkillRow*> groups;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || s.activity == 0 || (!s.codename.starts_with("SKILL_CH_") && !s.codename.starts_with("SKILL_EU_"))) continue;
        const game::SkillTraits t = game::TraitsOf(s);
        if (!(t.heal || t.cure) || t.resurrect) continue;
        const assets::SkillRow*& top = groups[s.group];
        if (top == nullptr || s.level > top->level) top = &s;
    }
    int heals = 0, overTime = 0, fields = 0, cures = 0, transfers = 0;
    std::vector<std::string> wrong;
    for (const auto& [group, s] : groups) {
        Field f;
        server::Skills skills(f.host, &table, 7);
        Learn(f, table, s->codename);
        f.player.stats.weaponMagMin = 400; // a weapon's magical attack (the heals' weapon bonus)
        f.player.stats.weaponMagMax = 600;
        f.player.stats.magBalance = 1.0;
        f.player.hp = 1000;
        f.player.stats.maxMp = 10'000'000; // room for an MP heal
        f.player.mp = 1'000'000;
        // Another player 20 away (the target of the skills that take another, a transfer's next).
        TestPlayer other;
        other.entity = 7;
        other.stats = StrongPlayer(1);
        other.hp = 1000;
        other.mp = 1000;
        f.host.bodies[other.entity].position = {std::uint16_t(98 << 8 | 168), 920.0f, 0.0f, 900.0f};
        f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
            if (entity == other.entity) return other.Caster();
            if (entity == f.player.entity) return f.player.Caster();
            return std::nullopt;
        };
        // Its target: itself when it may, else the other; none when it asks none.
        const std::uint32_t target = !s->targetRequired ? 0u : s->targetSelf ? f.player.entity : other.entity;
        TestPlayer& holder = target == other.entity ? other : f.player;
        // States to cure on the holder: a burn of level 255 (200 s), a sleep of level 1 (20 s).
        const Clock::time_point t0 = Clock::now();
        const game::CureSpec cure = game::CureOf(*s);
        if (cure) {
            game::ActiveState burn;
            burn.kind = game::StateKind::Burn;
            burn.level = 255;
            burn.durationMs = 200'000;
            burn.started = t0;
            holder.state.states.Apply(burn);
            game::ActiveState sleep;
            sleep.kind = game::StateKind::Sleep;
            sleep.level = 1;
            sleep.durationMs = 20'000;
            sleep.started = t0;
            holder.state.states.Apply(sleep);
        }
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {s->id, target}, t0);
        const std::uint32_t hpBefore = holder.hp, mpBefore = holder.mp; // its MP paid
        const auto answers = f.host.All<proto::SkillUseAnswer>();
        if (answers.empty() || answers.back().status != proto::SkillUseStatus::Ok) {
            wrong.push_back(s->codename + " (refused: " + StatusName(answers.empty() ? proto::SkillUseStatus::Unknown : answers.back().status) + ")");
            continue;
        }
        const bool timed = s->hasDuration;
        const double seconds = timed ? std::min(30.0, double(s->buffDurationMs) / 1000.0 + 1.0) : 4.0;
        for (double at = 0.0; at < seconds; at += 0.05) {
            server::SkillCaster c = f.player.Caster();
            skills.Fight(c, t0 + std::chrono::milliseconds(int(at * 1000.0)));
            server::SkillCaster o = other.Caster();
            skills.Fight(o, t0 + std::chrono::milliseconds(int(at * 1000.0)));
        }
        // Its heal: once its amount on the holder (the healer's numbers, the holder's), each pulse of
        // a heal over time, the field's pulses on the caster.
        const game::HealSpec heal = game::HealOf(*s);
        if (heal) {
            const game::HealAmount once = game::HealFor(heal, 100, f.player.stats, holder.stats);
            const std::uint32_t gainedHp = holder.hp - hpBefore, gainedMp = holder.mp > mpBefore ? holder.mp - mpBefore : 0u;
            const bool field = static_cast<bool>(assets::FindSkillParam(*s, assets::SkillTag("efr"), 2));
            if (!timed) {
                ++heals;
                if (gainedHp != std::min(once.hp, holder.stats.maxHp - hpBefore) || (once.mp > 0 && holder.mp != std::min<std::uint64_t>(std::uint64_t(mpBefore) + once.mp, holder.stats.maxMp))) {
                    wrong.push_back(s->codename + " (healed " + std::to_string(gainedHp) + " for " + std::to_string(once.hp) + ")");
                }
            } else {
                ++(field ? fields : overTime);
                const std::uint32_t each = once.hp > 0 ? once.hp : once.mp;
                const std::uint32_t gained = once.hp > 0 ? gainedHp : gainedMp;
                if (each == 0 || gained < 2 * each || gained % each != 0) wrong.push_back(s->codename + " (over time: " + std::to_string(gained) + " by " + std::to_string(each) + ")");
            }
            // A transfer that takes every character: the other player its share (falloff 50: half).
            const assets::SkillParam area = assets::FindSkillParam(*s, assets::SkillTag("efr"), 1);
            if (area && area.Int(1) == 6 && (area[5] & 0x02) != 0 && target == f.player.entity) {
                ++transfers;
                const game::HealAmount share = game::HealFor(heal, 100 - area.Int(4), f.player.stats, other.stats);
                if (other.hp - 1000 != share.hp) wrong.push_back(s->codename + " (its transfer: " + std::to_string(other.hp - 1000) + " for " + std::to_string(share.hp) + ")");
            }
        }
        // Its cure: the burn less the cure level's worth (750 ms a level), the sleep gone (at 100 %; the
        // Bard's 80 %: gone or kept); 'rcur': no more of them treated than it says.
        if (cure) {
            ++cures;
            const game::ActiveState* burn = holder.state.states.Find(game::StateKind::Burn);
            const std::uint32_t expected = 200'000u - std::uint32_t(cure.elementLevel) * 750u;
            const bool burnCut = burn != nullptr && burn->durationMs == expected;
            const bool burnKept = burn != nullptr && burn->durationMs == 200'000u;
            const bool sleepGone = !holder.state.states.In(game::StateKind::Sleep);
            const int treated = (burnCut ? 1 : 0) + (sleepGone ? 1 : 0);
            if (!burnCut && !burnKept) wrong.push_back(s->codename + " (the burn's time)");
            if (cure.most > 0 && treated > cure.most) wrong.push_back(s->codename + " (more than rcur)");
            if ((cure.most == 0 || cure.most >= 2) && cure.curseChance >= 100 && treated != 2) wrong.push_back(s->codename + " (not both cured)");
            if (treated > 0 && f.host.All<proto::EntityStates>().empty()) wrong.push_back(s->codename + " (its states not told)");
        }
    }
    MESSAGE(groups.size() << " groups: " << heals << " heals at once, " << overTime << " over time, " << fields << " fields, " << transfers << " transfers, " << cures
                          << " cures");
    std::string list;
    for (std::size_t i = 0; i < wrong.size() && i < 12; ++i) list += " " + wrong[i];
    CHECK_MESSAGE(wrong.empty(), wrong.size() << " wrong:" << list);
    CHECK(groups.size() >= 40);
}

TEST_CASE("every resurrection group: offered to a dead player of its level in reach, refused otherwise (game files; K9)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // Each player resurrection group's first and highest ranks.
    std::map<std::uint32_t, std::pair<const assets::SkillRow*, const assets::SkillRow*>> groups;
    for (const auto& [id, s] : table.rows()) {
        if (!s.inService || s.activity == 0 || (!s.codename.starts_with("SKILL_CH_") && !s.codename.starts_with("SKILL_EU_"))) continue;
        if (!game::TraitsOf(s).resurrect) continue;
        auto& [low, high] = groups[s.group];
        if (low == nullptr || s.level < low->level) low = &s;
        if (high == nullptr || s.level > high->level) high = &s;
    }
    int offered = 0, targeted = 0;
    std::vector<std::string> wrong;
    for (const auto& [group, pair] : groups) {
        const assets::SkillRow* s = pair.second;
        const game::ReviveSpec revive = game::ReviveOf(*s);
        if (!revive || revive.experiencePercent <= 0) wrong.push_back(s->codename + " (no resu)");
        if (!s->targetRequired) continue; // a party's (none until parties come)
        ++targeted;
        Field f;
        server::Skills skills(f.host, &table, 7);
        Learn(f, table, s->codename);
        TestPlayer dead;
        dead.entity = 7;
        dead.stats = StrongPlayer(1);
        dead.stats.level = revive.maxLevel;
        dead.hp = 0;
        bool isDead = true;
        f.host.bodies[dead.entity].position = {std::uint16_t(98 << 8 | 168), 920.0f, 0.0f, 900.0f};
        f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
            if (entity != dead.entity) return std::nullopt;
            server::SkillCaster c = dead.Caster();
            c.dead = isDead;
            return c;
        };
        const auto status = [&] {
            const auto answers = f.host.All<proto::SkillUseAnswer>();
            return answers.empty() ? proto::SkillUseStatus::Unknown : answers.back().status;
        };
        const Clock::time_point t0 = Clock::now();
        // A dead player of its level, 20 away: offered, its mana spent.
        const std::uint32_t mpBefore = f.player.mp;
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {s->id, dead.entity}, t0);
        // Offered (and paid) at the impact, once its preparing and casting are over.
        CHECK(f.host.revivals.empty());
        const double lead = (std::max(0, s->preparingMs) + std::max(0, s->castingMs)) / 1000.0;
        for (double at = 0.0; at < lead + 0.5; at += 0.05) {
            server::SkillCaster c = f.player.Caster();
            skills.Fight(c, t0 + std::chrono::milliseconds(int(at * 1000.0)));
        }
        if (status() != proto::SkillUseStatus::Ok || f.host.revivals.size() != 1 || f.host.revivals[0].first != dead.entity || f.player.mp >= mpBefore) {
            wrong.push_back(s->codename + " (not offered)");
            continue;
        }
        ++offered;
        // One level above what it raises, a living one, one out of reach: refused.
        f.player.state.skillCooling.clear();
        f.player.state.groupCooling.clear();
        dead.stats.level = revive.maxLevel + 1;
        server::SkillCaster c1 = f.player.Caster();
        f.player.state.busyUntil = {}; // the one before's hold over
        skills.Use(c1, {s->id, dead.entity}, t0);
        if (status() != proto::SkillUseStatus::ReviveLevel) wrong.push_back(s->codename + " (a higher level not refused)");
        dead.stats.level = 1;
        isDead = false;
        server::SkillCaster c2 = f.player.Caster();
        f.player.state.busyUntil = {}; // the one before's hold over
        skills.Use(c2, {s->id, dead.entity}, t0);
        if (status() != proto::SkillUseStatus::Target) wrong.push_back(s->codename + " (a living one not refused)");
        isDead = true;
        f.host.bodies[dead.entity].position.x = 2000.0f;
        server::SkillCaster c3 = f.player.Caster();
        f.player.state.busyUntil = {}; // the one before's hold over
        skills.Use(c3, {s->id, dead.entity}, t0);
        if (status() != proto::SkillUseStatus::Target || f.host.revivals.size() != 1) wrong.push_back(s->codename + " (out of reach not refused)");
    }
    MESSAGE(groups.size() << " resurrection groups (" << targeted << " on a target): " << offered << " offered");
    std::string list;
    for (std::size_t i = 0; i < wrong.size() && i < 12; ++i) list += " " + wrong[i];
    CHECK_MESSAGE(wrong.empty(), wrong.size() << " wrong:" << list);
    CHECK(offered == targeted);
    CHECK(offered >= 6);
}

TEST_CASE("the skill objects: Harmony's circle gives its buff to those inside, the trap goes off once, the field strikes each pulse (game files; K10)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    const auto tick = [](Field& f, server::Skills& skills, Clock::time_point t0, double from, double to) {
        for (double at = from; at < to; at += 0.05) {
            const Clock::time_point now = t0 + std::chrono::milliseconds(int(at * 1000.0));
            skills.Objects(now);
            server::SkillCaster c = f.player.Caster();
            skills.Fight(c, now);
        }
    };

    SUBCASE("Harmony: a circle of radius 60 at the caster's feet; a copy for each player inside, taken back as one leaves") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* harmony = Learn(f, table, "SKILL_CH_WATER_HARMONY_A_01");
        REQUIRE(harmony != nullptr);
        const assets::SkillParam area = assets::FindSkillParam(*harmony, assets::SkillTag("efr"), 3);
        REQUIRE(area);
        CHECK(area[2] == 60);
        TestPlayer other;
        other.entity = 7;
        other.stats = StrongPlayer(1);
        const proto::WorldPosition here = f.host.bodies[f.player.entity].position;
        f.host.bodies[other.entity].position = {here.region, here.x + 30.0f, here.y, here.z};
        f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
            if (entity == other.entity) return other.Caster();
            if (entity == f.player.entity) return f.player.Caster();
            return std::nullopt;
        };
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        const std::uint32_t mpBefore = f.player.mp;
        CHECK_FALSE(skills.Use(c0, {harmony->id, 0}, t0));
        const double lead = (harmony->preparingMs + harmony->castingMs) / 1000.0;
        tick(f, skills, t0, 0.0, lead - 0.1);
        CHECK(f.host.objects.empty()); // not before its impact
        CHECK(f.player.mp == mpBefore); // its mana at its release
        tick(f, skills, t0, lead - 0.1, lead + 0.5);
        CHECK(f.player.mp < mpBefore);
        REQUIRE(f.host.objects.size() == 1);
        CHECK(f.host.objects.begin()->second.first == harmony->id);
        CHECK(skills.ObjectCount() == 1);
        // Both inside hold its buff (the owner by its mask's bit 1, the other by bit 2).
        REQUIRE(f.player.state.buffs.size() == 1);
        REQUIRE(other.state.buffs.size() == 1);
        CHECK(other.state.buffs[0].skill == harmony->id);
        // Its protection: 'pola' 1 33 (grade 0 monsters up to level 33).
        const server::Skills::Protection guard = skills.ProtectionOf(other.state);
        CHECK(guard.grades == 1);
        CHECK(guard.level == 33);
        CHECK(skills.ProtectionOf(server::SkillState{}).grades == 0);
        // The other steps out: its copy ends within a scan.
        f.host.bodies[other.entity].position = {here.region, here.x + 200.0f, here.y, here.z};
        tick(f, skills, t0, lead + 0.5, lead + 1.0);
        CHECK(other.state.buffs.empty());
        CHECK(f.player.state.buffs.size() == 1);
        // Back in: a copy again.
        f.host.bodies[other.entity].position = {here.region, here.x + 20.0f, here.y, here.z};
        tick(f, skills, t0, lead + 1.0, lead + 1.5);
        CHECK(other.state.buffs.size() == 1);
        // Its time over: gone, the copies with it.
        const double life = harmony->buffDurationMs / 1000.0;
        tick(f, skills, t0 + std::chrono::milliseconds(int(life * 1000.0)), lead + 1.5, lead + 2.0);
        CHECK(f.host.objects.empty());
        CHECK(other.state.buffs.empty());
        CHECK(f.player.state.buffs.empty());
    }

    SUBCASE("the fire trap: waits at the caster's feet, goes off on the first monster within 50 with its blast, then is gone") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* trap = Learn(f, table, "SKILL_EU_WIZARD_FIREA_TRAP_A_01");
        REQUIRE(trap != nullptr);
        f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
            if (entity == f.player.entity) return f.player.Caster();
            return std::nullopt;
        };
        // The dummies away from the caster at first (the nearest, 30 away, is inside 50: moved off).
        const proto::WorldPosition here = f.host.bodies[f.player.entity].position;
        for (auto& [id, m] : f.host.mobs) f.host.bodies[id].position.x += 400.0f;
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        CHECK_FALSE(skills.Use(c0, {trap->id, 0}, t0));
        const double lead = (trap->preparingMs + trap->castingMs) / 1000.0;
        tick(f, skills, t0, 0.0, lead + 1.0);
        REQUIRE(f.host.objects.size() == 1);
        int hitsBefore = 0;
        for (const auto& [id, m] : f.host.mobs) hitsBefore += m.hits;
        CHECK(hitsBefore == 0);
        // One walks in, another 40 beside it (in the blast of 70).
        f.host.bodies[Field::kSelected].position = {here.region, here.x + 30.0f, here.y, here.z};
        f.host.bodies[1001].position = {here.region, here.x + 70.0f, here.y, here.z};
        tick(f, skills, t0, lead + 1.0, lead + 2.0);
        CHECK(f.host.objects.empty());
        CHECK(f.host.removedObjects.size() == 1);
        CHECK(f.host.mobs[Field::kSelected].hits == 1);
        CHECK(f.host.mobs[1001].hits == 1);
        const auto struck = f.host.All<proto::SkillObjectStruck>();
        REQUIRE(struck.size() == 1);
        CHECK(struck[0].trap);
        CHECK(struck[0].skill == trap->id);
    }

    SUBCASE("the poison field: strikes those within 100 every 'puls' while it lasts") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* field = Learn(f, table, "SKILL_EU_ROG_POISONA_FIELD_A_01");
        REQUIRE(field != nullptr);
        f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
            if (entity == f.player.entity) return f.player.Caster();
            return std::nullopt;
        };
        const int pulse = assets::FindSkillParam(*field, assets::SkillTag("puls")).Int(0);
        REQUIRE(pulse > 0);
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        CHECK_FALSE(skills.Use(c0, {field->id, 0}, t0));
        const double lead = (field->preparingMs + field->castingMs) / 1000.0;
        tick(f, skills, t0, 0.0, lead + 3.0 * pulse / 1000.0 + 0.5); // its impact on the 300 ms skill tick
        const auto struck = f.host.All<proto::SkillObjectStruck>();
        CHECK(struck.size() == 3);
        for (const auto& m : struck) CHECK_FALSE(m.trap);
        // The selected dummy 30 away was struck each time (its poison as its roll gives it).
        CHECK(f.host.mobs[Field::kSelected].hits == 3);
        CHECK(f.host.objects.size() == 1);
    }
}

TEST_CASE("the teleports: Ghost Walk and the Wizard's Teleport go to the point asked, no farther than their distance (game files; K10)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    for (const char* code : {"SKILL_CH_LIGHTNING_GYEONGGONG_B_01", "SKILL_EU_WIZARD_PSYCHICA_TELEPORT_A_01"}) {
        CAPTURE(code);
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* tele = Learn(f, table, code);
        REQUIRE(tele != nullptr);
        assets::SkillParam p = assets::FindSkillParam(*tele, assets::SkillTag("tele"));
        if (!p) p = assets::FindSkillParam(*tele, assets::SkillTag("tel2"));
        REQUIRE(p);
        const float most = float(p.Int(1));
        const proto::WorldPosition here = f.host.bodies[f.player.entity].position;
        // Without a point: refused.
        server::SkillCaster c0 = f.player.Caster();
        CHECK_FALSE(skills.Use(c0, {tele->id, 0}, Clock::now()));
        CHECK(f.host.All<proto::SkillUseAnswer>().back().status == proto::SkillUseStatus::Target);
        // A point twice as far as it goes: there as far as its distance, paid, its wait started.
        proto::UseSkill use{tele->id, 0};
        use.hasPoint = true;
        use.point = {here.region, here.x + 2.0f * most, here.y, here.z};
        const std::uint32_t mpBefore = f.player.mp;
        server::SkillCaster c1 = f.player.Caster();
        CHECK_FALSE(skills.Use(c1, use, Clock::now()));
        CHECK(f.host.All<proto::SkillUseAnswer>().back().status == proto::SkillUseStatus::Ok);
        CHECK(f.host.bodies[f.player.entity].position.x == doctest::Approx(here.x + most));
        // Its move told to those who see it, before its swing: Ghost Walk dashes there, the Wizard is put there.
        const auto blinks = f.host.All<proto::EntityBlink>();
        REQUIRE(blinks.size() == 1);
        CHECK(blinks[0].kind == (std::string_view(code).starts_with("SKILL_CH") ? proto::BlinkKind::Dash : proto::BlinkKind::Put));
        CHECK(blinks[0].to.x == doctest::Approx(here.x + most));
        CHECK(f.host.All<proto::EntitySwing>().size() == 1);
        CHECK(f.player.mp < mpBefore);
        const bool cooled = !f.host.All<proto::Cooldown>().empty() || tele->cooldownMs <= 0;
        CHECK(cooled);
        // Under its own Ice Wall (a buff whose row carries 'pw'): refused.
        const assets::SkillRow* wall = table.Find(std::string_view("SKILL_CH_COLD_BINGBYEOK_A_01"));
        REQUIRE(wall != nullptr);
        server::SkillState::Buff held;
        held.skill = wall->id;
        held.until = Clock::now() + std::chrono::seconds(60);
        f.player.state.buffs.push_back(held);
        f.player.state.skillCooling.clear();
        server::SkillCaster c2 = f.player.Caster();
        CHECK_FALSE(skills.Use(c2, use, Clock::now() + std::chrono::seconds(30)));
        CHECK(f.host.All<proto::SkillUseAnswer>().back().status == proto::SkillUseStatus::Target);
        CHECK(f.host.All<proto::EntityBlink>().size() == 1);
    }
}

TEST_CASE("a swing goes on where it was cast: its target gone beyond its range, it stands and still strikes (game files; K10)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    Field f;
    server::Skills skills(f.host, &table, 7);
    const assets::SkillRow* shot = Learn(f, table, "SKILL_CH_BOW_CRITICAL_A_01");
    REQUIRE(shot != nullptr);
    f.player.stats.range = 180; // a bow's reach (the skill takes the weapon's)
    const proto::WorldPosition here = f.host.bodies[f.player.entity].position;
    // The archer 30 from its target (in reach): the shot goes off, then the target runs 500 away.
    const Clock::time_point t0 = Clock::now();
    server::SkillCaster c0 = f.player.Caster();
    REQUIRE(skills.Use(c0, {shot->id, Field::kSelected}, t0));
    server::SkillCaster c1 = f.player.Caster();
    skills.Fight(c1, t0);
    REQUIRE(f.host.All<proto::EntitySwing>().size() == 1);
    f.host.bodies[Field::kSelected].position.x = here.x + 500.0f;
    // It stands through its whole hold: its preparing, casting and duration from the cast; its hit lands at the impact
    // wherever the target went.
    const int hold = std::max(0, shot->preparingMs) + std::max(0, shot->castingMs) + std::max(0, shot->durationMs);
    for (int at = 50; at < hold; at += 50) {
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t0 + std::chrono::milliseconds(at));
        CHECK(f.host.bodies[f.player.entity].position.x == doctest::Approx(here.x)); // no walk in the hold
    }
    CHECK(f.host.mobs[Field::kSelected].hits >= 1);
    // Then the basic attack after it (Action_AutoAttackType 1) goes after the target: the chase comes between two
    // commands, never inside one.
    REQUIRE(shot->autoAttack == 1);
    for (int at = hold; at < hold + 500; at += 50) {
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t0 + std::chrono::milliseconds(at));
    }
    CHECK(f.host.bodies[f.player.entity].position.x > here.x + 100.0f);
}

TEST_CASE("frostbite and slow stretch an action skill's life inside its reuse: p x life / 100 + max(0, reuse - life) (game files; U3-03)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // The reuse each state gives a cast (the Cooldown its caster is told).
    const auto reuseUnder = [&](std::optional<game::StateKind> state) {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* smash = Learn(f, table, "SKILL_CH_SWORD_SMASH_A_01");
        REQUIRE(smash != nullptr);
        if (state) {
            game::ActiveState a;
            a.kind = *state;
            a.level = 1;
            a.durationMs = 60'000;
            a.started = a.nextTick = Clock::now();
            REQUIRE(f.player.state.states.Apply(a));
        }
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c = f.player.Caster();
        REQUIRE(skills.Use(c, {smash->id, Field::kSelected}, t0));
        for (int at = 0; at < 3000 && f.host.All<proto::Cooldown>().empty(); at += 50) { // walked into its reach, then cast
            server::SkillCaster c1 = f.player.Caster();
            skills.Fight(c1, t0 + std::chrono::milliseconds(at));
        }
        std::uint32_t ms = 0;
        for (const proto::Cooldown& cd : f.host.All<proto::Cooldown>()) {
            if (cd.id == smash->id) ms = cd.ms;
        }
        return std::make_pair(ms, smash);
    };
    const auto [plain, smash] = reuseUnder(std::nullopt);
    REQUIRE(smash->activity == 2);
    REQUIRE(smash->durationMs > 0);
    const int life = std::max(0, smash->preparingMs) + std::max(0, smash->castingMs) + smash->durationMs;
    CHECK(plain == std::uint32_t(smash->cooldownMs)); // 100: the plain reuse
    CHECK(reuseUnder(game::StateKind::Frostbite).first == std::uint32_t(200 * life / 100 + std::max(0, smash->cooldownMs - life)));
    CHECK(reuseUnder(game::StateKind::Slow).first == std::uint32_t(125 * life / 100 + std::max(0, smash->cooldownMs - life)));
}

TEST_CASE("a chain's rows: each cast at its time (the command's 500 ms budget), the hold its last row's; a target dead at a row's cast ends the chain (game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // Chain Spear - Tiger: three rows, no preparing or casting, durations d0, d1, d2 (891, 771, 1071), Action_AutoAttackType 1.
    const assets::SkillRow* head = table.Find(std::string_view("SKILL_CH_SPEAR_CHAIN_A_1S_01"));
    REQUIRE(head != nullptr);
    const assets::SkillRow* second = table.Find(head->chainNext);
    REQUIRE(second != nullptr);
    const assets::SkillRow* third = table.Find(second->chainNext);
    REQUIRE(third != nullptr);
    REQUIRE(head->preparingMs + head->castingMs == 0);
    REQUIRE(head->durationMs > 500);
    // The rows' casts: 0; d0 - 500 (the budget spent); then d1 more. The hold: the last row's duration, plus the budget
    // used (500) as the basic attack follows and nothing waits (the whole chain as long as its durations).
    const int row1 = head->durationMs - 500, row2 = row1 + second->durationMs;
    const int holdEnd = row2 + third->durationMs + 500;
    MESSAGE("rows cast at 0, " << row1 << ", " << row2 << " ms; the hold ends at " << holdEnd);
    const auto run = [&](bool killAfterFirst) {
        Field f;
        server::Skills skills(f.host, &table, 7);
        f.player.book.masteries[head->masteries[0]] = 100;
        f.player.book.groups[head->group] = head->level;
        f.player.stats.weaponKind = head->weaponKinds[0];
        f.player.stats.range = 40; // in reach of the selected one (30 away): the swing at once
        f.player.mp = 100000;
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        REQUIRE(skills.Use(c0, {head->id, Field::kSelected}, t0));
        const auto at = [&](int ms) {
            server::SkillCaster c = f.player.Caster();
            skills.Fight(c, t0 + std::chrono::milliseconds(ms));
            return f.host.mobs[Field::kSelected].hits;
        };
        at(0); // the swing (its first row's hit lands at the next update)
        REQUIRE(f.host.All<proto::EntitySwing>().size() == 1);
        CHECK(at(10) == 1);
        if (killAfterFirst) f.host.mobs[Field::kSelected].alive = false;
        CHECK(at(row1 - 20) == 1);
        if (killAfterFirst) {
            at(row1 + 20);
            CHECK(f.player.state.pendingRows.empty()); // no later row: the chain ended at its cast
            CHECK(f.player.state.busyUntil <= t0 + std::chrono::milliseconds(row1 + 20));
            return;
        }
        CHECK(at(row1 + 20) == 2); // 500 ms before its durations say
        CHECK(at(row2 - 20) == 2);
        CHECK(at(row2 + 20) == 3);
        CHECK(f.player.state.busyUntil == t0 + std::chrono::milliseconds(holdEnd));
    };
    run(false);
    run(true);
}

TEST_CASE("a cast's release: re-checked on each skill tick, given up on a stun or a failed check (nothing paid, nothing struck, told); a buff and its mana at its release (game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    using namespace std::chrono_literals;
    // Strike Smash: no preparing, a casting of 411 ms, one row.
    const assets::SkillRow* smash = table.Find(std::string_view("SKILL_CH_SWORD_SMASH_A_01"));
    REQUIRE(smash != nullptr);
    REQUIRE(smash->castingMs > 0);
    REQUIRE(smash->chainNext == 0);
    const auto start = [&](Field& f, server::Skills& skills, Clock::time_point t0) {
        f.player.book.masteries[smash->masteries[0]] = 100;
        f.player.book.groups[smash->group] = smash->level;
        f.player.stats.weaponKind = smash->weaponKinds[0];
        f.player.stats.range = 40; // in reach of the selected one (30 away): the swing at once
        f.player.mp = 1000;
        server::SkillCaster c0 = f.player.Caster();
        REQUIRE(skills.Use(c0, {smash->id, Field::kSelected}, t0));
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t0);
        REQUIRE(f.host.All<proto::EntitySwing>().size() == 1);
        REQUIRE(f.player.state.casting);
    };
    const auto run = [](Field& f, server::Skills& skills, Clock::time_point from, Clock::time_point to) {
        for (Clock::time_point t = from; t <= to; t += 50ms) {
            server::SkillCaster c = f.player.Caster();
            skills.Fight(c, t);
        }
    };
    const auto hits = [](Field& f) { return f.host.mobs[Field::kSelected].hits; };

    SUBCASE("released: struck and paid at its release, on the caster's skill tick") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const Clock::time_point t0 = Clock::now();
        start(f, skills, t0);
        const Clock::time_point release = f.player.state.casting->release;
        CHECK(release >= t0 + std::chrono::milliseconds(smash->castingMs));
        CHECK(release < t0 + std::chrono::milliseconds(smash->castingMs + 300));
        run(f, skills, t0 + 50ms, release - 1ms);
        CHECK(hits(f) == 0);
        CHECK(f.player.mp == 1000u);
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, release);
        skills.Fight(c, release + 10ms);
        CHECK(hits(f) == 1);
        CHECK(f.player.mp < 1000u);
        CHECK_FALSE(f.player.state.casting);
        CHECK(f.host.All<proto::EntityCastCancelled>().empty());
    }
    SUBCASE("a stun before its release: given up, told to those who see the caster") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const Clock::time_point t0 = Clock::now();
        start(f, skills, t0);
        run(f, skills, t0 + 50ms, t0 + 100ms);
        server::SkillCaster c = f.player.Caster();
        skills.Interrupt(c, t0 + 150ms);
        run(f, skills, t0 + 200ms, t0 + 2s);
        CHECK(hits(f) == 0);
        CHECK(f.player.mp == 1000u);
        const auto cancelled = f.host.All<proto::EntityCastCancelled>();
        REQUIRE(cancelled.size() == 1);
        CHECK(cancelled[0].entity == f.player.entity);
        CHECK(cancelled[0].skill == smash->id);
        CHECK_FALSE(f.player.state.casting);
        CHECK(f.player.state.busyUntil <= t0 + 150ms);
    }
    SUBCASE("its target dead before its release: the next tick's check gives it up") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const Clock::time_point t0 = Clock::now();
        start(f, skills, t0);
        f.host.mobs[Field::kSelected].alive = false;
        run(f, skills, t0 + 50ms, t0 + 2s);
        CHECK(hits(f) == 0);
        CHECK(f.player.mp == 1000u);
        CHECK(f.host.All<proto::EntityCastCancelled>().size() == 1);
    }
    SUBCASE("a failed check gives up that cast alone: the command waiting behind it goes on") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const Clock::time_point t0 = Clock::now();
        start(f, skills, t0);
        server::SkillCaster c1 = f.player.Caster();
        CHECK(skills.Attack(c1, 1001, t0 + 50ms)); // behind the cast running
        REQUIRE(f.player.state.waiting);
        f.host.mobs[Field::kSelected].alive = false;
        run(f, skills, t0 + 100ms, t0 + 4s);
        CHECK(f.host.All<proto::EntityCastCancelled>().size() == 1);
        CHECK(hits(f) == 0);
        CHECK(f.host.mobs[1001].hits > 0);
    }
    SUBCASE("its weapon put away before its release: the same") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const Clock::time_point t0 = Clock::now();
        start(f, skills, t0);
        f.player.stats.weaponKind = 0;
        run(f, skills, t0 + 50ms, t0 + 2s);
        CHECK(hits(f) == 0);
        CHECK(f.player.mp == 1000u);
        CHECK(f.host.All<proto::EntityCastCancelled>().size() == 1);
    }
    SUBCASE("its mana gone before its release: the same") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const Clock::time_point t0 = Clock::now();
        start(f, skills, t0);
        f.player.mp = 1;
        run(f, skills, t0 + 50ms, t0 + 2s);
        CHECK(hits(f) == 0);
        CHECK(f.host.All<proto::EntityCastCancelled>().size() == 1);
    }
    SUBCASE("a buff with a casting (the Sword's Shield PD, 1,300 ms): on its holder, and paid for, at its release") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* pd = Learn(f, table, "SKILL_CH_SWORD_SHIELDPD_A_01");
        REQUIRE(pd != nullptr);
        REQUIRE(pd->castingMs + pd->preparingMs > 0);
        f.player.stats = StrongPlayer(pd->weaponKinds[0] != 255 ? pd->weaponKinds[0] : 2);
        WearFor(f.player, *pd);
        f.player.mp = 1000;
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {pd->id, 0}, t0);
        REQUIRE(f.host.All<proto::SkillUseAnswer>().back().status == proto::SkillUseStatus::Ok);
        CHECK(f.player.state.buffs.empty());
        CHECK(f.player.mp == 1000u);
        run(f, skills, t0 + 50ms, t0 + std::chrono::milliseconds(pd->castingMs + pd->preparingMs - 50));
        CHECK(f.player.state.buffs.empty());
        run(f, skills, t0 + std::chrono::milliseconds(pd->castingMs + pd->preparingMs), t0 + std::chrono::milliseconds(pd->castingMs + pd->preparingMs + 350));
        REQUIRE(f.player.state.buffs.size() == 1);
        CHECK(f.player.mp < 1000u);
        CHECK(f.host.All<proto::BuffAdded>().size() == 1);
    }
    SUBCASE("a buff given up: neither on nor paid for") {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* pd = Learn(f, table, "SKILL_CH_SWORD_SHIELDPD_A_01");
        REQUIRE(pd != nullptr);
        f.player.stats = StrongPlayer(pd->weaponKinds[0] != 255 ? pd->weaponKinds[0] : 2);
        WearFor(f.player, *pd);
        f.player.mp = 1000;
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        skills.Use(c0, {pd->id, 0}, t0);
        server::SkillCaster c = f.player.Caster();
        skills.Interrupt(c, t0 + 400ms);
        run(f, skills, t0 + 450ms, t0 + 3s);
        CHECK(f.player.state.buffs.empty());
        CHECK(f.player.mp == 1000u);
        CHECK(f.host.All<proto::EntityCastCancelled>().size() == 1);
    }
}

TEST_CASE("a heal on another player out of its reach: walked toward first, cast once in reach; a walk of the player's own gives it up (game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    using namespace std::chrono_literals;
    const auto run = [](Field& f, server::Skills& skills, Clock::time_point from, Clock::time_point to) {
        for (Clock::time_point t = from; t <= to; t += 50ms) {
            server::SkillCaster c = f.player.Caster();
            skills.Fight(c, t);
        }
    };
    for (const bool walkAway : {false, true}) {
        Field f;
        server::Skills skills(f.host, &table, 7);
        const assets::SkillRow* heal = Learn(f, table, "SKILL_EU_CLERIC_HEALA_TARGET_A_01");
        REQUIRE(heal != nullptr);
        REQUIRE(heal->targetRequired);
        f.player.stats = StrongPlayer(heal->weaponKinds[0] != 255 ? heal->weaponKinds[0] : 1);
        WearFor(f.player, *heal);
        TestPlayer other;
        other.entity = 7;
        other.stats = StrongPlayer(1);
        other.hp = 10;
        const proto::WorldPosition here = f.host.bodies[f.player.entity].position;
        const float away = float(std::max(heal->range, 10)) + 300.0f;
        f.host.bodies[other.entity].position = {here.region, here.x + away, here.y, here.z};
        f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
            if (entity == other.entity) return other.Caster();
            if (entity == f.player.entity) return f.player.Caster();
            return std::nullopt;
        };
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f.player.Caster();
        CHECK(skills.Use(c0, {heal->id, other.entity}, t0)); // the command goes on: a walk first
        CHECK(f.host.All<proto::SkillUseAnswer>().empty());  // nothing refused, nothing cast yet
        CHECK(f.host.stops == 1);                             // walked toward it (the test's walks arrive at once)
        const proto::FlatPosition a = proto::ToFlat(f.host.bodies[f.player.entity].position);
        const proto::FlatPosition b = proto::ToFlat(f.host.bodies[other.entity].position);
        CHECK(std::hypot(a.x - b.x, a.z - b.z) <= double(std::max(heal->range, 10)) + 12.0);
        REQUIRE(f.player.state.approach);
        if (walkAway) f.player.state.StopAttacking(); // the player's own walk (the game server's move)
        run(f, skills, t0 + 50ms, t0 + 3s);
        if (walkAway) {
            CHECK(other.hp == 10u);
            CHECK(f.host.All<proto::EntitySwing>().empty());
        } else {
            CHECK(other.hp > 10u);
            const auto answers = f.host.All<proto::SkillUseAnswer>();
            REQUIRE(answers.size() == 1);
            CHECK(answers[0].status == proto::SkillUseStatus::Ok);
            CHECK_FALSE(f.player.state.approach);
        }
    }
}

TEST_CASE("the taunts: a threat alone on the monsters their area takes, no damage (Taunting Target, Mirage; game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    using namespace std::chrono_literals;
    const auto cast = [&](std::string_view codename, std::uint32_t target) {
        auto f = std::make_unique<Field>();
        server::Skills skills(f->host, &table, 7);
        const assets::SkillRow* skill = Learn(*f, table, codename);
        REQUIRE(skill != nullptr);
        CHECK(game::SkillRouteOf(*skill) == game::SkillRoute::Taunt);
        f->player.stats = StrongPlayer(skill->weaponKinds[0] != 255 ? skill->weaponKinds[0] : 1);
        WearFor(f->player, *skill);
        const Clock::time_point t0 = Clock::now();
        server::SkillCaster c0 = f->player.Caster();
        skills.Use(c0, {skill->id, target}, t0);
        const auto answers = f->host.All<proto::SkillUseAnswer>();
        REQUIRE(answers.size() == 1);
        CHECK(answers[0].status == proto::SkillUseStatus::Ok);
        for (Clock::time_point t = t0 + 50ms; t <= t0 + 3s; t += 50ms) {
            server::SkillCaster c = f->player.Caster();
            skills.Fight(c, t);
        }
        return std::pair{std::move(f), skill};
    };
    SUBCASE("Taunting Target: its target and up to two more within 50 of it, 'tnt2' plus 20 x the weapon's mean attack") {
        auto [f, skill] = cast("SKILL_EU_WARRIOR_FRENZYA_TOUNT_A_01", Field::kSelected);
        const assets::SkillParam raise = assets::FindSkillParam(*skill, assets::SkillTag("tnt2"));
        const assets::SkillParam weapon = assets::FindSkillParam(*skill, assets::SkillTag("pwtt"));
        REQUIRE(raise);
        REQUIRE(weapon);
        const std::int64_t expected = raise.Int(0) + game::WeaponThreat(f->player.stats, weapon.Int(0), false);
        CHECK(expected > raise.Int(0));
        CHECK(f->host.mobs[Field::kSelected].threat == expected);
        CHECK(f->host.mobs[Field::kSelected].threatFrom == f->player.entity);
        int taken = 0;
        for (const auto& [id, m] : f->host.mobs) {
            CHECK(m.damage == 0u);
            if (m.threat != 0) ++taken;
        }
        CHECK(taken == 3); // its 'efr' most at once, the target counted
        CHECK(f->host.All<proto::SkillTook>().size() == 3);
        CHECK(f->host.All<proto::EntityDamaged>().empty());
    }
    SUBCASE("Mirage: no target, four around the caster within 250, the caster's threat cut") {
        auto [f, skill] = cast("SKILL_EU_WARLOCK_CONFUSIONA_AGGROLOW_A_01", 0);
        const assets::SkillParam cut = assets::FindSkillParam(*skill, assets::SkillTag("dtnt"));
        REQUIRE(cut);
        const std::int64_t expected = -(cut.Int(0) + game::WeaponThreat(f->player.stats, assets::FindSkillParam(*skill, assets::SkillTag("mwdt")).Int(0), true));
        int taken = 0;
        for (const auto& [id, m] : f->host.mobs) {
            if (m.threat == 0) continue;
            ++taken;
            CHECK(m.threat == expected);
        }
        CHECK(taken == 4);
    }
}

TEST_CASE("stealth: the Rogue's hides at half speed, not in battle, ends as a skill starts or a hit lands; detection reads it (game files; K12)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    Field f;
    server::Skills skills(f.host, &table, 7);
    const assets::SkillRow* hide = Learn(f, table, "SKILL_EU_ROG_STEALTHA_HIDING_A_01");
    REQUIRE(hide != nullptr);
    const Clock::time_point t0 = Clock::now();
    // In battle (attacked a moment ago): refused.
    f.player.state.battleUntil = t0 + std::chrono::seconds(5);
    server::SkillCaster c0 = f.player.Caster();
    CHECK_FALSE(skills.Use(c0, {hide->id, 0}, t0));
    CHECK(f.host.All<proto::SkillUseAnswer>().back().status == proto::SkillUseStatus::InBattle);
    // Out of battle: hidden, its type's bit 1 at level 3, at half speed.
    f.player.state.battleUntil = {};
    server::SkillCaster c1 = f.player.Caster();
    skills.Use(c1, {hide->id, 0}, t0);
    Release(f, skills, t0);
    REQUIRE(f.player.state.buffs.size() == 1);
    server::Skills::Stealth s = skills.StealthOf(f.player.state);
    CHECK(s.mask == 1);
    CHECK(s.level == 3);
    CHECK(s.speed == doctest::Approx(0.5f));
    // A move does not end it (skc word 1 is 2 | 8); a skill started does.
    server::SkillCaster c2 = f.player.Caster();
    CHECK_FALSE(skills.EndOnEvent(c2, 1));
    CHECK(skills.EndOnEvent(c2, 2));
    CHECK(skills.StealthOf(f.player.state).mask == 0);
    // Hidden again, a landed hit ends it (keep 0).
    f.player.state.skillCooling.clear();
    f.player.state.groupCooling.clear();
    server::SkillCaster c3 = f.player.Caster();
    skills.Use(c3, {hide->id, 0}, t0 + std::chrono::seconds(1));
    Release(f, skills, t0 + std::chrono::seconds(1));
    REQUIRE(skills.StealthOf(f.player.state).mask == 1);
    server::SkillCaster c4 = f.player.Caster();
    CHECK(skills.EndOnHit(c4, 1 | 4));
    CHECK(skills.StealthOf(f.player.state).mask == 0);
    // The Rogue's detection: its own detector, mask 5 (stealth and traps) at level 3.
    const assets::SkillRow* detect = table.Find(std::string_view("SKILL_EU_ROG_STEALTHA_DETECT_A_01"));
    REQUIRE(detect != nullptr);
    server::SkillState watcher;
    watcher.buffs.push_back({1, detect->id});
    s = skills.StealthOf(watcher);
    CHECK(s.detectMask == 5);
    CHECK(s.detectLevel == 3);
    // A reveal copy ('dttp' 5 3, the Chinese Descry) on a hidden one at level 3 shows it to all; at 8 not.
    const assets::SkillRow* descry = table.Find(std::string_view("SKILL_CH_FIRE_DESCRY_A_01"));
    REQUIRE(descry != nullptr);
    server::SkillState hidden;
    hidden.buffs.push_back({1, hide->id});
    hidden.buffs.push_back({2, descry->id});
    CHECK(skills.StealthOf(hidden).mask == 0);
    const assets::SkillRow* hideB = table.Find(std::string_view("SKILL_EU_ROG_STEALTHA_HIDING_B_01"));
    REQUIRE(hideB != nullptr);
    hidden.buffs[0].skill = hideB->id;
    CHECK(skills.StealthOf(hidden).mask == 1);
}

TEST_CASE("the Snow Shield moves a share of a hit onto the MP, 1.5 a point; a reflection returns shares on its chance within its range (game files; K12)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    Field f;
    server::Skills skills(f.host, &table, 7);
    // The Snow Shield's first step ('dgmp 20'): 200 of 1,000 onto the MP for 300 MP.
    const assets::SkillRow* shield = table.Find(std::string_view("SKILL_CH_COLD_SHIELD_A_01"));
    REQUIRE(shield != nullptr);
    REQUIRE(assets::FindSkillParam(*shield, assets::SkillTag("dgmp"))[0] == 20u);
    f.player.state.buffs.push_back({1, shield->id});
    f.player.mp = 1000;
    server::SkillCaster c = f.player.Caster();
    server::Skills::Guarded g = skills.Guard(c, 0, 1000, false, 50.0, Clock::now());
    CHECK(g.damage == 800u);
    CHECK(g.mpTaken == 300u);
    CHECK(f.player.mp == 700u);
    CHECK(g.returned == 0u);
    // Short of MP (30 for 300): trunc(270 / 1.5) = 180 back on the hit, all the MP taken.
    f.player.mp = 30;
    g = skills.Guard(c, 0, 1000, false, 50.0, Clock::now());
    CHECK(g.damage == 980u);
    CHECK(g.mpTaken == 30u);
    CHECK(f.player.mp == 0u);
    // No MP: the whole hit.
    CHECK(skills.Guard(c, 0, 1000, false, 50.0, Clock::now()).damage == 1000u);

    // The Warlock's Soul Return ('dmgr' chance, physical %, magical %, range): its chance's rate, its
    // shares, never beyond its range; the holder's own damage stays.
    f.player.state.buffs.clear();
    const assets::SkillRow* back = table.Find(std::string_view("SKILL_EU_WARLOCK_SOULA_RETURN_A_01"));
    REQUIRE(back != nullptr);
    const assets::SkillParam r = assets::FindSkillParam(*back, assets::SkillTag("dmgr"));
    REQUIRE(r);
    MESSAGE("dmgr " << r[0] << " " << r[1] << " " << r[2] << " " << r[3]);
    f.player.state.buffs.push_back({2, back->id});
    int returned = 0;
    for (int i = 0; i < 2000; ++i) {
        const bool magical = (i & 1) != 0;
        g = skills.Guard(c, 0, 1000, magical, double(r[3]) - 1.0, Clock::now());
        CHECK(g.damage == 1000u);
        if (g.returned == 0) continue;
        ++returned;
        CHECK(g.reflectSkill == back->id);
        CHECK(g.returned == (magical ? r[2] : r[1]) * 10u);
    }
    CHECK(returned > int(r[0]) * 20 - 120);
    CHECK(returned < int(r[0]) * 20 + 120);
    for (int i = 0; i < 200; ++i) CHECK(skills.Guard(c, 0, 1000, false, double(r[3]) + 1.0, Clock::now()).returned == 0u);
}

TEST_CASE("links: the guards take their share of their guarded one's hits, a divide spreads it over the party, the DOTs strike each period, a stun link stuns the attacker (game files; K12)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    Field f;
    server::Skills skills(f.host, &table, 7);
    // Three other players beside the caster (entity 1).
    std::map<std::uint32_t, TestPlayer> others;
    const proto::WorldPosition here = f.host.bodies[f.player.entity].position;
    for (const std::uint32_t id : {7u, 8u, 9u}) {
        TestPlayer& o = others[id];
        o.entity = id;
        o.stats = StrongPlayer(1);
        f.host.bodies[id].position = {here.region, here.x - 20.0f - float(id), here.y, here.z};
    }
    f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
        if (entity == f.player.entity) return f.player.Caster();
        if (const auto it = others.find(entity); it != others.end()) return it->second.Caster();
        return std::nullopt;
    };
    const Clock::time_point t0 = Clock::now();
    const auto use = [&](const assets::SkillRow* skill, std::uint32_t target, Clock::time_point at) {
        f.player.state.skillCooling.clear();
        f.player.state.groupCooling.clear();
        server::SkillCaster c = f.player.Caster();
        skills.Use(c, {skill->id, target}, at);
        const proto::SkillUseStatus status = f.host.All<proto::SkillUseAnswer>().back().status;
        Release(f, skills, at);
        return status;
    };

    SUBCASE("Scorn on an enemy player: its copy and link on it; it then aims only at the caster") {
        const assets::SkillRow* scorn = Learn(f, table, "SKILL_EU_ROG_STEALTHA_CHANGE_A_01");
        REQUIRE(scorn != nullptr);
        CHECK(game::SkillRouteOf(*scorn) == game::SkillRoute::EnemyPlayerBuff);
        f.player.stats = StrongPlayer(scorn->weaponKinds[0] != 255 ? scorn->weaponKinds[0] : 1);
        WearFor(f.player, *scorn);
        CHECK(use(scorn, 8, t0) == proto::SkillUseStatus::Target); // a player not hostile: refused
        f.host.enemyPlayers.insert(7);
        CHECK(use(scorn, 7, t0) == proto::SkillUseStatus::Ok);
        REQUIRE(others[7].state.buffs.size() == 1);
        CHECK(others[7].state.buffs[0].linkOwner == f.player.entity);
        // The scorned one is told its buff with its scorner (B0BD's hitm field).
        bool toldScorner = false;
        for (const auto& t : f.host.told) {
            const auto* b = std::get_if<proto::BuffAdded>(&t.message);
            if (!t.observers && t.entity == 7 && b != nullptr && b->skill == scorn->id) toldScorner = b->source == f.player.entity;
        }
        CHECK(toldScorner);
        // The scorned one's skills on anyone but its scorner are refused.
        server::SkillCaster scorned = others[7].Caster();
        skills.Use(scorned, {scorn->id, Field::kSelected}, t0 + std::chrono::milliseconds(100));
        CHECK(f.host.All<proto::SkillUseAnswer>().back().status == proto::SkillUseStatus::Target);
        CHECK_FALSE(skills.Attack(scorned, Field::kSelected, t0 + std::chrono::milliseconds(100)));
    }
    SUBCASE("Gross Scorn: the caster's enemy players within 100 of its target take its copy too (three at most, the target counted)") {
        const assets::SkillRow* gross = Learn(f, table, "SKILL_EU_ROG_STEALTHA_CHANGE_B_01");
        REQUIRE(gross != nullptr);
        CHECK(game::SkillRouteOf(*gross) == game::SkillRoute::EnemyPlayerBuff);
        f.player.stats = StrongPlayer(gross->weaponKinds[0] != 255 ? gross->weaponKinds[0] : 1);
        WearFor(f.player, *gross);
        f.host.enemyPlayers.insert(7);
        f.host.enemyPlayers.insert(8); // 9 is no enemy: left out
        f.host.mobs[8].stats = others[8].stats; // an enemy player is one a strike may take (the game server's Monster)
        f.host.mobs[8].alive = true;
        CHECK(use(gross, 7, t0) == proto::SkillUseStatus::Ok);
        REQUIRE(others[7].state.buffs.size() == 1);
        REQUIRE(others[8].state.buffs.size() == 1);
        CHECK(others[9].state.buffs.empty());
        CHECK(others[8].state.buffs[0].skill == gross->id);
        CHECK(others[8].state.buffs[0].linkOwner == f.player.entity);
        CHECK(f.player.state.links.size() == 2); // a link a copy
        // Both aim only at the caster.
        server::SkillCaster scorned = others[8].Caster();
        CHECK_FALSE(skills.Attack(scorned, Field::kSelected, t0 + std::chrono::milliseconds(100)));
    }
    SUBCASE("Mana Switch on a player not hostile: its link on that player, as a player's buff") {
        const assets::SkillRow* manaSwitch = Learn(f, table, "SKILL_EU_BARD_BATTLAA_MPSTEAL_A_01");
        REQUIRE(manaSwitch != nullptr);
        f.player.stats = StrongPlayer(manaSwitch->weaponKinds[0] != 255 ? manaSwitch->weaponKinds[0] : 1);
        WearFor(f.player, *manaSwitch);
        CHECK(use(manaSwitch, 7, t0) == proto::SkillUseStatus::Ok);
        REQUIRE(f.player.state.links.size() == 1);
        REQUIRE(others[7].state.buffs.size() == 1);
        CHECK(others[7].state.buffs[0].skill == manaSwitch->id);
        CHECK(others[7].state.buffs[0].linkOwner == f.player.entity);
    }
    SUBCASE("Guard Physical: a third of a physical hit to the guard; two at most; the same twice refused; beyond 1500 it ends") {
        const assets::SkillRow* guard = Learn(f, table, "SKILL_EU_WARRIOR_GUARDA_PHYSICAL_A_01");
        REQUIRE(guard != nullptr);
        CHECK(use(guard, 7, t0) == proto::SkillUseStatus::Ok);
        REQUIRE(f.player.state.links.size() == 1);
        REQUIRE(others[7].state.buffs.size() == 1);
        CHECK(others[7].state.buffs[0].linkOwner == f.player.entity);
        // Its owner is told, and only it (the original's B0BE): the link's own token, whom it is on, its time.
        std::uint32_t token = 0;
        for (const TestHost::Told& t : f.host.told) {
            const auto* added = std::get_if<proto::LinkAdded>(&t.message);
            if (added == nullptr) continue;
            CHECK_FALSE(t.observers);
            CHECK(t.entity == f.player.entity);
            CHECK(added->skill == guard->id);
            CHECK(added->target == 7u);
            CHECK(added->ms > 0u);
            token = added->token;
        }
        REQUIRE(token != 0);
        CHECK(token == f.player.state.links[0].token);
        CHECK(token != others[7].state.buffs[0].token);
        server::SkillCaster held = others[7].Caster();
        server::Skills::Guarded g = skills.Guard(held, Field::kSelected, 1000, false, 20.0, t0);
        CHECK(g.damage == 670u);
        REQUIRE(g.moved.size() == 1);
        CHECK(g.moved[0].player == f.player.entity);
        CHECK(g.moved[0].damage == 330u);
        // Its lanes are the physical ones: a magical hit stays.
        CHECK(skills.Guard(held, Field::kSelected, 1000, true, 20.0, t0).damage == 1000u);
        // The same on the same one: refused; a second guarded one: taken; a third: as many as it may.
        // (Each after the one before's hold: the action list keeps a press during it waiting.)
        f.player.state.busyUntil = {};
        CHECK(use(guard, 7, t0) == proto::SkillUseStatus::Overlap);
        f.player.state.busyUntil = {};
        CHECK(use(guard, 8, t0) == proto::SkillUseStatus::Ok);
        f.player.state.busyUntil = {};
        CHECK(use(guard, 9, t0) == proto::SkillUseStatus::Links);
        CHECK(f.player.state.links.size() == 2);
        // The guarded one goes beyond the break range: the link and its copy end; its owner's board loses it.
        f.host.bodies[7].position.x += 2000.0f;
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t0 + std::chrono::seconds(1));
        CHECK(f.player.state.links.size() == 1);
        CHECK(others[7].state.buffs.empty());
        bool removed = false;
        for (const TestHost::Told& t : f.host.told) {
            if (const auto* gone = std::get_if<proto::BuffRemoved>(&t.message)) removed = removed || (gone->token == token && t.entity == f.player.entity);
        }
        CHECK(removed);
        // The guard's death ends the other, and its copy.
        server::SkillCaster dead{f.player.entity, f.player.book, f.player.stats, f.player.hp, f.player.mp, true, {}, f.player.state};
        skills.Fight(dead, t0 + std::chrono::seconds(2));
        CHECK(f.player.state.links.empty());
        CHECK(others[8].state.buffs.empty());
    }

    SUBCASE("Guard Divide: 35 % of the hit spread evenly over the party near, the guarded one left out") {
        const assets::SkillRow* divide = Learn(f, table, "SKILL_EU_WARRIOR_GUARDA_DIVIDE_A_01");
        REQUIRE(divide != nullptr);
        CHECK(use(divide, 7, t0) == proto::SkillUseStatus::Ok);
        server::SkillCaster held = others[7].Caster();
        // No party: all of it kept.
        CHECK(skills.Guard(held, Field::kSelected, 1000, false, 20.0, t0).damage == 1000u);
        f.host.party = {f.player.entity, 7, 8};
        server::Skills::Guarded g = skills.Guard(held, Field::kSelected, 1000, false, 20.0, t0);
        CHECK(g.damage == 650u);
        REQUIRE(g.moved.size() == 2);
        CHECK(g.moved[0].damage == 175u);
        CHECK(g.moved[1].damage == 175u);
    }

    SUBCASE("Guard Aggro: 36 % of the guarded one's threat goes to the guard, told first") {
        const assets::SkillRow* aggro = Learn(f, table, "SKILL_EU_WARRIOR_GUARDA_AGGRO_A_01");
        REQUIRE(aggro != nullptr);
        CHECK(use(aggro, 7, t0) == proto::SkillUseStatus::Ok);
        TestPlayer& hitter = others[7];
        f.host.bodies[7].position = {here.region, 925.0f, 0.0f, 900.0f}; // beside the selected dummy
        server::SkillCaster hc = hitter.Caster();
        REQUIRE(skills.Attack(hc, Field::kSelected, t0));
        for (double at = 0.0; at < 2.0 && f.host.threats.empty(); at += 0.05) {
            server::SkillCaster c = hitter.Caster();
            skills.Fight(c, t0 + std::chrono::milliseconds(int(at * 1000.0)));
        }
        REQUIRE_FALSE(f.host.threats.empty());
        std::uint32_t damage = 0;
        for (const proto::EntityDamaged& d : f.host.All<proto::EntityDamaged>()) {
            if (d.attacker == 7) damage = d.damage;
        }
        REQUIRE(damage > 0);
        const auto [monster, from, threat] = f.host.threats[0];
        CHECK(monster == Field::kSelected);
        CHECK(from == f.player.entity);
        CHECK(threat == std::int64_t(std::trunc(double(damage) * 36.0 / 100.0)));
    }

    SUBCASE("the Warlock's DOTs: a strike of its own every 2 s for 12 s, shown on the monster; the same twice refused, two at most on one") {
        const assets::SkillRow* burn = Learn(f, table, "SKILL_EU_WARLOCK_DOTA_BURN_A_01");
        const assets::SkillRow* poison = table.Find(std::string_view("SKILL_EU_WARLOCK_DOTA_POISON_A_01"));
        const assets::SkillRow* blood = table.Find(std::string_view("SKILL_EU_WARLOCK_DOTA_BLOODING_A_01"));
        REQUIRE(burn != nullptr);
        REQUIRE(poison != nullptr);
        REQUIRE(blood != nullptr);
        f.player.book.groups[poison->group] = poison->level;
        f.player.book.groups[blood->group] = blood->level;
        f.host.mobs[Field::kSelected].stats.maxHp = f.host.mobs[Field::kSelected].hp = 100'000'000;
        CHECK(use(burn, Field::kSelected, t0) == proto::SkillUseStatus::Ok);
        std::size_t swung = 0;
        bool checked = false;
        for (double at = 0.0; at < 14.0; at += 0.05) {
            const Clock::time_point now = t0 + std::chrono::milliseconds(int(at * 1000.0));
            server::SkillCaster c = f.player.Caster();
            skills.Fight(c, now);
            if (f.host.All<proto::EntitySwing>().size() > swung) {
                swung = f.host.All<proto::EntitySwing>().size();
                f.player.state.attackTarget = 0;
            }
            if (!checked && at > 3.0) {
                checked = true;
                CHECK(f.host.monsterBuffs[Field::kSelected] == std::vector<std::uint32_t>{burn->id});
                CHECK(use(burn, Field::kSelected, now) == proto::SkillUseStatus::Overlap);
            }
        }
        int strikes = 0;
        for (const proto::EntityDamaged& d : f.host.All<proto::EntityDamaged>()) {
            if (d.target == Field::kSelected && (d.flags & proto::kDamageReturn) != 0 && d.imbue == burn->id) ++strikes;
        }
        CHECK(strikes == 6);
        CHECK(f.host.monsterBuffs[Field::kSelected].empty());
        CHECK(f.player.state.links.empty());
        // Two different on one target at most; the others free.
        server::SkillState::Link a;
        a.skill = burn->id;
        a.target = Field::kSelected;
        a.until = t0 + std::chrono::seconds(60);
        server::SkillState::Link b = a;
        b.skill = poison->id;
        f.player.state.links = {a, b};
        CHECK(skills.AdmitLink(f.player.state, *blood, Field::kSelected) == proto::SkillUseStatus::Links);
        CHECK(skills.AdmitLink(f.player.state, *blood, 1001) == proto::SkillUseStatus::Ok);
        f.player.state.links = {a};
        CHECK(skills.AdmitLink(f.player.state, *poison, Field::kSelected) == proto::SkillUseStatus::Ok);
    }

    SUBCASE("the Stun Link: an attacker within 150 of the linked one is stunned on its chance, as from the link's owner") {
        const assets::SkillRow* stun = Learn(f, table, "SKILL_EU_WARLOCK_SOULA_STUNLINK_A_01");
        REQUIRE(stun != nullptr);
        CHECK(use(stun, 7, t0) == proto::SkillUseStatus::Ok);
        server::SkillCaster held = others[7].Caster();
        for (int i = 0; i < 200; ++i) skills.Guard(held, Field::kSelected, 100, false, 200.0, t0);
        CHECK(f.host.mobs[Field::kSelected].took.empty());
        for (int i = 0; i < 200; ++i) skills.Guard(held, Field::kSelected, 100, false, 100.0, t0);
        const auto& took = f.host.mobs[Field::kSelected].took;
        REQUIRE_FALSE(took.empty());
        CHECK(took[0].kind == game::StateKind::Stun);
        CHECK(took[0].source == f.player.entity);
    }

    SUBCASE("Tag Point ('hntp'): on another player, never a monster; its owner told where the marked one is every second, also far away; beyond 10000 it ends") {
        const assets::SkillRow* point = Learn(f, table, "SKILL_EU_ROG_STEALTHA_POINT_A_01");
        REQUIRE(point != nullptr);
        CHECK(use(point, Field::kSelected, t0) == proto::SkillUseStatus::Target); // "This skill cannot be used on monsters."
        CHECK(use(point, f.player.entity, t0) == proto::SkillUseStatus::Target);
        CHECK(use(point, 7, t0) == proto::SkillUseStatus::Ok);
        REQUIRE(f.player.state.links.size() == 1);
        REQUIRE(others[7].state.buffs.size() == 1);
        const std::uint32_t token = f.player.state.links[0].token;
        REQUIRE(f.host.All<proto::LinkAdded>().size() == 1);
        // Its 'reqi' (a crossbow or a dagger) are its caster's: the marked one, holding none, keeps the copy
        // (only the holder's own buffs are checked).
        server::SkillCaster marked = others[7].Caster();
        skills.EndUnfitBuffs(marked);
        CHECK(others[7].state.buffs.size() == 1);
        // The marked one walks 5000 away (out of sight, inside the break range): told each second.
        f.host.bodies[7].position = {here.region, here.x + 5000.0f, here.y, here.z};
        for (int ms = 0; ms <= 3050; ms += 100) {
            server::SkillCaster c = f.player.Caster();
            skills.Fight(c, t0 + std::chrono::milliseconds(ms));
        }
        const std::vector<proto::LinkPosition> told = f.host.All<proto::LinkPosition>();
        CHECK(told.size() == 4); // 0, 1, 2 and 3 s
        REQUIRE_FALSE(told.empty());
        CHECK(told.back().token == token);
        CHECK(told.back().position.x == doctest::Approx(here.x + 5000.0f));
        // A second mark: Tag Point A holds one (lnks word 2).
        CHECK(use(point, 8, t0 + std::chrono::seconds(3)) == proto::SkillUseStatus::Links);
        // Beyond 10000 the mark ends, its copy with it.
        f.host.bodies[7].position.x = here.x + 10500.0f;
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t0 + std::chrono::seconds(4));
        CHECK(f.player.state.links.empty());
        CHECK(others[7].state.buffs.empty());
    }

    SUBCASE("Scorn ('hitm'): on enemy players alone: none until PvP, never a monster") {
        const assets::SkillRow* scorn = Learn(f, table, "SKILL_EU_ROG_STEALTHA_CHANGE_A_01");
        REQUIRE(scorn != nullptr);
        CHECK(use(scorn, Field::kSelected, t0) == proto::SkillUseStatus::Target); // "You cannot use the skill against monsters."
        CHECK(use(scorn, 7, t0) == proto::SkillUseStatus::Target);
        CHECK(f.player.state.links.empty());
        CHECK(f.player.state.attackTarget == 0u);
    }
}

TEST_CASE("transforms: the mask from a normal monster's body, the monster's shape from it (its level at most the user's, no skill while it lasts), the duplicate's look (game files; K12)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    Field f;
    server::Skills skills(f.host, &table, 7);
    const Clock::time_point t0 = Clock::now();
    const auto use = [&](const assets::SkillRow* skill, std::uint32_t target, Clock::time_point at) {
        f.player.state.skillCooling.clear();
        f.player.state.groupCooling.clear();
        server::SkillCaster c = f.player.Caster();
        skills.Use(c, {skill->id, target}, at);
        const proto::SkillUseStatus status = f.host.All<proto::SkillUseAnswer>().back().status;
        Release(f, skills, at);
        return status;
    };

    SUBCASE("the mask ('mcap 20 10371'): a normal field monster's body of level 20 or less gives the item, carrying the monster") {
        const assets::SkillRow* mask = Learn(f, table, "SKILL_EU_ROG_TRANSFORMA_MASK_A_01");
        REQUIRE(mask != nullptr);
        f.host.corpses[2000] = {1946, 20, 0, 1};
        f.host.corpses[2001] = {1947, 21, 0, 1};
        f.host.corpses[2002] = {1948, 10, 1, 1};
        CHECK(use(mask, Field::kSelected, t0) == proto::SkillUseStatus::Target); // alive: no body
        CHECK(use(mask, 2001, t0) == proto::SkillUseStatus::TargetLevel);        // above its level (0x3035)
        CHECK(use(mask, 2002, t0) == proto::SkillUseStatus::MonsterClass);       // a champion (0x3033)
        // A thieves' and a hunters' job monster, a quest's (TID4 2, 3, 4: 0x3006).
        for (const int type : {2, 3, 4}) {
            f.host.corpses[2003] = {1949, 10, 0, type};
            CHECK(use(mask, 2003, t0) == proto::SkillUseStatus::MonsterClass);
        }
        CHECK(f.host.dropped.empty());
        CHECK(use(mask, 2000, t0) == proto::SkillUseStatus::Ok);
        REQUIRE(f.host.dropped.size() == 1);
        CHECK(f.host.dropped[0] == std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>{f.player.entity, 10371u, 1946u});
    }

    SUBCASE("the monster's shape (SKILL_ETC_TRANS_MONSTER_01 'msch 1'): not above the user's level; its basic skill the attack; no skill; over with its buff") {
        const assets::SkillRow* shape = table.Find(std::string_view("SKILL_ETC_TRANS_MONSTER_01"));
        const assets::SkillRow* smash = Learn(f, table, "SKILL_CH_SWORD_SMASH_A_01");
        REQUIRE(shape != nullptr);
        REQUIRE(smash != nullptr);
        const assets::SkillRow* basic = table.Find(std::string_view("SKILL_CH_SWORD_BASE_01"));
        REQUIRE(basic != nullptr);
        f.player.stats.level = 20;
        server::SkillCaster c = f.player.Caster();
        CHECK_FALSE(skills.TransformInto(c, *shape, 1946, 21, basic->id, t0));
        CHECK(f.host.All<proto::SkillUseAnswer>().back().status == proto::SkillUseStatus::Level); // 0x3008
        CHECK(f.player.state.transform.mode == 0);
        CHECK(skills.TransformInto(c, *shape, 1946, 20, basic->id, t0));
        CHECK(f.player.state.transform.mode == 1);
        CHECK(f.player.state.transform.model == 1946u);
        CHECK(f.host.looks == std::vector<std::uint32_t>{f.player.entity});
        CHECK(skills.DefaultAttack(f.player.Caster()) == basic);
        CHECK(use(smash, Field::kSelected, t0) == proto::SkillUseStatus::Transformed);
        // Its time over: its own shape again.
        server::SkillCaster later = f.player.Caster();
        skills.ExpireBuffs(later, t0 + std::chrono::milliseconds(shape->buffDurationMs + 1));
        CHECK(f.player.state.transform.mode == 0);
        CHECK(f.host.looks.size() == 2);
        CHECK(use(smash, Field::kSelected, t0 + std::chrono::milliseconds(shape->buffDurationMs + 2)) != proto::SkillUseStatus::Transformed);
    }

    SUBCASE("the duplicate ('msch 2 60'): another player's look up to level 60; any skill started ends it") {
        const assets::SkillRow* duple = Learn(f, table, "SKILL_EU_ROG_TRANSFORMA_DUPLE_A_01");
        REQUIRE(duple != nullptr);
        TestPlayer other;
        other.entity = 7;
        other.stats = StrongPlayer(1);
        other.stats.level = 61;
        const proto::WorldPosition here = f.host.bodies[f.player.entity].position;
        f.host.bodies[other.entity].position = {here.region, here.x + 20.0f, here.y, here.z};
        f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
            if (entity == other.entity) return other.Caster();
            if (entity == f.player.entity) return f.player.Caster();
            return std::nullopt;
        };
        CHECK(use(duple, other.entity, t0) == proto::SkillUseStatus::Target);
        other.stats.level = 60;
        // In a job's suit or free battle (before the level), a murderer (after it): 0x3039.
        f.host.suited.insert(other.entity);
        CHECK(use(duple, other.entity, t0) == proto::SkillUseStatus::Duplicate);
        f.host.suited.clear();
        f.host.murderers.insert(other.entity);
        CHECK(use(duple, other.entity, t0) == proto::SkillUseStatus::Duplicate);
        f.host.murderers.clear();
        CHECK(use(duple, other.entity, t0) == proto::SkillUseStatus::Ok);
        CHECK(f.player.state.transform.mode == 2);
        CHECK(f.player.state.transform.copied == other.entity);
        CHECK(f.host.looks.size() == 1);
        server::SkillCaster c = f.player.Caster();
        CHECK(skills.EndOnEvent(c, 2));
        CHECK(f.player.state.transform.mode == 0);
        CHECK(f.host.looks.size() == 2);
    }
}

TEST_CASE("the sockets' skills: a worn item's stones give theirs, passives in its numbers, actives cast with their reuse; gone with the item (game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // A bow holding HEAL_E at level 3 and CRITICAL_E at 2; a chest holding GOLD_E at 3, ER_E at 1 and HWAN_E at 2.
    game::ItemInstance bow, chest;
    for (int k = 0; k < 3; ++k) {
        game::alchemy::MakeSocket(bow, k);
        game::alchemy::MakeSocket(chest, k);
    }
    game::alchemy::InsertStone(bow, 0, 31234, 3, 26090);
    game::alchemy::InsertStone(bow, 1, 31227, 2, 26088);
    game::alchemy::InsertStone(chest, 0, 31208, 3, 26083);
    game::alchemy::InsertStone(chest, 1, 31217, 1, 26086);
    game::alchemy::InsertStone(chest, 2, 31210, 2, 26084);
    Field f;
    server::Skills skills(f.host, &table, 7);
    f.host.players = [&](std::uint32_t entity) -> std::optional<server::SkillCaster> {
        if (entity == f.player.entity) return f.player.Caster();
        return std::nullopt;
    };
    f.player.stats = StrongPlayer(6);
    f.player.stats.critical = 20;
    // Worn: what the game server makes them from (UpdateStats: every worn item's filled sockets).
    game::alchemy::AddSocketSkills(bow, f.player.book.itemSkills);
    game::alchemy::AddSocketSkills(chest, f.player.book.itemSkills);
    CHECK(f.player.book.itemSkills.size() == 5);
    // The passives: CRITICAL_E's cr (11 % more), GOLD_E's gdr (0xB6 15), HWAN_E's hwit (0xC3 7), each its own source.
    const game::BuffBasis basis = server::Skills::BasisOf(f.player.stats);
    f.player.state.passives = game::PassivesOf(f.player.book, table, f.player.state.worn, basis);
    REQUIRE(f.player.state.passives.entries.size() == 3);
    for (const game::Passives::Entry& e : f.player.state.passives.entries) CHECK(e.active);
    const game::StatSheet sheet = f.player.state.Sheet();
    CHECK(sheet.Value(0xB6, 0.0f) == 15.0f);
    CHECK(sheet.Value(0xC3, 0.0f) == 7.0f);
    CHECK(sheet.Value(game::stat::kCritical, 100.0f) == doctest::Approx(111.0f));
    // A learned book only: none of them.
    CHECK(game::PassivesOf({}, table, f.player.state.worn, basis).entries.empty());
    // HEAL_E (level 3: 70 % of the most HP) cast on itself: healed, its five minutes' reuse started; again refused.
    f.player.stats.maxHp = 10'000;
    f.player.hp = 1'000;
    const Clock::time_point t0 = Clock::now();
    server::SkillCaster c0 = f.player.Caster();
    skills.Use(c0, {31234, 0}, t0);
    auto answers = f.host.All<proto::SkillUseAnswer>();
    REQUIRE_FALSE(answers.empty());
    CHECK(answers.back().status == proto::SkillUseStatus::Ok);
    for (double at = 0.0; at < 2.0; at += 0.05) {
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t0 + std::chrono::milliseconds(int(at * 1000.0)));
    }
    MESSAGE("HEAL_E level 3: HP 1000 -> " << f.player.hp << " of 10000");
    CHECK(f.player.hp > 1'000);
    bool reuse = false;
    for (const proto::Cooldown& c : f.host.All<proto::Cooldown>()) reuse = reuse || (c.id == 31234 && c.ms == 300'000);
    CHECK(reuse);
    f.host.told.clear();
    server::SkillCaster c1 = f.player.Caster();
    skills.Use(c1, {31234, 0}, t0 + std::chrono::seconds(10));
    answers = f.host.All<proto::SkillUseAnswer>();
    REQUIRE_FALSE(answers.empty());
    CHECK(answers.back().status == proto::SkillUseStatus::Cooling);
    // ER_E (a buff on itself for 10 s: parry + 20 %): on its board.
    f.host.told.clear();
    server::SkillCaster c2 = f.player.Caster();
    skills.Use(c2, {31217, 0}, t0 + std::chrono::seconds(11));
    bool buffed = false;
    for (const server::SkillState::Buff& b : f.player.state.buffs) buffed = buffed || b.skill == 31217;
    CHECK(buffed);
    // Taken off: the book without them, the skills unknown, the passives gone, and ER_E's running buff ended at once
    // (the game server passes the skills its items took away).
    const std::vector<std::uint32_t> takenAway = f.player.book.itemSkills;
    f.player.book.itemSkills.clear();
    CHECK(game::PassivesOf(f.player.book, table, f.player.state.worn, basis).entries.empty());
    f.host.told.clear();
    server::SkillCaster off = f.player.Caster();
    skills.EndBuffsOf(off, takenAway);
    bool still = false;
    for (const server::SkillState::Buff& b : f.player.state.buffs) still = still || b.skill == 31217;
    CHECK_FALSE(still);
    CHECK_FALSE(f.host.All<proto::BuffRemoved>().empty());
    f.host.told.clear();
    server::SkillCaster c3 = f.player.Caster();
    skills.Use(c3, {31243, 0}, t0 + std::chrono::seconds(12));
    answers = f.host.All<proto::SkillUseAnswer>();
    REQUIRE_FALSE(answers.empty());
    CHECK(answers.back().status == proto::SkillUseStatus::Unknown);
}

TEST_CASE("the KB socket stone: a knock-back on enemy players alone, never a monster; its area takes the enemy players about (game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    const assets::SkillRow* kb = table.Find(31250); // SKILL_ITEM_ETC_SOCKET_STONE_KB_E_05: kb 80 % at 50
    REQUIRE(kb != nullptr);
    CHECK(kb->targetEnemyPlayer);
    CHECK_FALSE(kb->targetEnemyMonster);
    Field f;
    server::Skills skills(f.host, &table, 7);
    f.player.book.itemSkills = {kb->id};
    f.player.stats.range = 50.0f;
    // On a monster: refused.
    const Clock::time_point t0 = Clock::now();
    server::SkillCaster c0 = f.player.Caster();
    CHECK_FALSE(skills.Use(c0, {kb->id, Field::kSelected}, t0));
    std::vector<proto::SkillUseAnswer> answers = f.host.All<proto::SkillUseAnswer>();
    REQUIRE_FALSE(answers.empty());
    CHECK(answers.back().status == proto::SkillUseStatus::Target);
    // The selected one (30 ahead) and the one 30 behind the caster are enemy players; 1001 (38 away) and 1002 (34) are
    // monsters within the area (around the caster, 30) and stay untouched.
    constexpr std::uint32_t kBehind = 1011;
    f.host.enemyPlayers = {Field::kSelected, kBehind};
    int knocked = 0;
    for (int n = 0; n < 6; ++n) {
        const Clock::time_point t = t0 + std::chrono::seconds(1 + 181 * n); // past its 180 s reuse each time
        server::SkillCaster c = f.player.Caster();
        REQUIRE(skills.Use(c, {kb->id, Field::kSelected}, t));
        for (int ms = 0; ms <= 1500; ms += 50) {
            server::SkillCaster cf = f.player.Caster();
            skills.Fight(cf, t + std::chrono::milliseconds(ms));
        }
    }
    knocked = f.host.mobs[Field::kSelected].knockedBack + f.host.mobs[kBehind].knockedBack;
    CHECK(f.host.mobs[Field::kSelected].hits >= 6);
    CHECK(f.host.mobs[kBehind].hits >= 6);
    CHECK(knocked >= 6); // 80 % each of 12
    CHECK(f.host.mobs[1001].hits == 0);
    CHECK(f.host.mobs[1002].hits == 0);
    CHECK(f.host.mobs[Field::kSelected].knockedDown + f.host.mobs[kBehind].knockedDown == 0);
}

TEST_CASE("the sockets' MENTAL stone ('msr'): a cast's MP is free at its chance (game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    // An attack skill with an MP cost, cast many times with MENTAL_E level 3 (30 % chance, 100 % off) worn.
    const assets::SkillRow* skill = nullptr;
    for (const auto& [id, s] : table.rows()) {
        if (s.inService && s.activity != 0 && s.mpCost > 0 && s.codename.starts_with("SKILL_CH_BOW_") && game::TraitsOf(s).attack) {
            if (skill == nullptr || s.id < skill->id) skill = &s;
        }
    }
    REQUIRE(skill != nullptr);
    TestHost host;
    server::Skills skills(host, &table, 11);
    TestPlayer p;
    p.stats = StrongPlayer(6);
    WearFor(p, *skill);
    p.book.groups[skill->group] = skill->level;
    p.book.itemSkills = {31239};
    p.state.passives = game::PassivesOf(p.book, table, p.state.worn, server::Skills::BasisOf(p.stats));
    const std::uint32_t mana = server::Skills::Mana(p.stats, *skill, p.state.passives.modifiers);
    REQUIRE(mana > 0);
    int free = 0, paid = 0;
    for (int i = 0; i < 2000; ++i) {
        server::SkillCaster c = p.Caster();
        const std::uint32_t cost = skills.PaidMana(c, *skill);
        if (cost == 0) ++free;
        else if (cost == mana) ++paid;
    }
    MESSAGE(skill->codename << ": " << free << " of 2000 casts free");
    CHECK(free + paid == 2000);
    CHECK(free > 500);
    CHECK(free < 700);
    // Without it: always paid.
    p.book.itemSkills.clear();
    p.state.passives = game::PassivesOf(p.book, table, p.state.worn, server::Skills::BasisOf(p.stats));
    server::SkillCaster c = p.Caster();
    CHECK(skills.PaidMana(c, *skill) == mana);
}

TEST_CASE("the action list: one running, one waiting (the newest wins); a skill hands on to the basic attack only by its AutoAttackType 1; a stop (game files)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    Field f;
    server::Skills skills(f.host, &table, 7);
    const assets::SkillRow* crit = Learn(f, table, "SKILL_CH_BOW_CRITICAL_A_01"); // AutoAttackType 1
    const assets::SkillRow* normal = Learn(f, table, "SKILL_CH_BOW_NORMAL_A_01"); // 2
    REQUIRE(crit != nullptr);
    REQUIRE(normal != nullptr);
    CHECK(crit->autoAttack == 1);
    CHECK(normal->autoAttack == 2);
    f.player.stats.range = 180;
    const Clock::time_point t0 = Clock::now();
    const auto swings = [&] { return f.host.All<proto::EntitySwing>(); };
    // Nothing running: the critical shot goes off at once.
    server::SkillCaster c0 = f.player.Caster();
    REQUIRE(skills.Use(c0, {crit->id, Field::kSelected}, t0));
    server::SkillCaster c1 = f.player.Caster();
    skills.Fight(c1, t0);
    REQUIRE(swings().size() == 1);
    CHECK(swings().back().skill == crit->id);
    CHECK(f.player.state.Held(t0) == 1);
    // During its hold: the normal shot waits behind it; pressed again, still one waiting; the client told 2.
    const Clock::time_point t1 = t0 + std::chrono::milliseconds(100);
    server::SkillCaster c2 = f.player.Caster();
    REQUIRE(skills.Use(c2, {normal->id, Field::kSelected}, t1));
    server::SkillCaster c3 = f.player.Caster();
    REQUIRE(skills.Use(c3, {normal->id, Field::kSelected}, t1));
    CHECK(f.player.state.Held(t1) == 2);
    REQUIRE(f.player.state.waiting.has_value());
    CHECK_FALSE(f.player.state.waiting->attack);
    CHECK(f.host.All<proto::ActionHeld>().back().count == 2);
    CHECK(swings().size() == 1);
    // Its hold over: the waiting one goes off.
    const Clock::time_point t2 = f.player.state.busyUntil + std::chrono::milliseconds(10);
    for (int ms = 0; ms <= 500 && swings().size() < 2; ms += 50) {
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t2 + std::chrono::milliseconds(ms));
    }
    REQUIRE(swings().size() == 2);
    CHECK(swings().back().skill == normal->id);
    // The normal shot (AutoAttackType 2) hands on to nothing: after its hold the fight is over.
    const Clock::time_point t3 = f.player.state.busyUntil + std::chrono::milliseconds(10);
    server::SkillCaster c4 = f.player.Caster();
    skills.Fight(c4, t3);
    CHECK(f.player.state.attackTarget == 0u);
    CHECK(f.player.state.Held(t3) == 0);
    // The critical shot again (AutoAttackType 1): after its hold, the basic attack goes on on the dummy.
    f.player.state.skillCooling.clear();
    f.player.state.groupCooling.clear();
    const Clock::time_point t4 = t3 + std::chrono::seconds(1);
    server::SkillCaster c5 = f.player.Caster();
    REQUIRE(skills.Use(c5, {crit->id, Field::kSelected}, t4));
    server::SkillCaster c6 = f.player.Caster();
    skills.Fight(c6, t4);
    const Clock::time_point t5 = f.player.state.busyUntil + std::chrono::milliseconds(10);
    for (int ms = 0; ms <= 3000 && swings().back().skill == crit->id; ms += 50) {
        server::SkillCaster c = f.player.Caster();
        skills.Fight(c, t5 + std::chrono::milliseconds(ms));
    }
    CHECK(f.player.state.attackTarget == Field::kSelected);
    CHECK(swings().back().skill != crit->id); // the basic attack's swing
    // A pick-up asked mid-swing waits behind it (an attack click's way), then the
    // fight ends and the item is walked to; asked between swings, it replaces the attack at once.
    const Clock::time_point t6 = f.player.state.busyUntil - std::chrono::milliseconds(50);
    server::SkillCaster c8 = f.player.Caster();
    CHECK(skills.HoldPickUp(c8, 4242u, t6));
    CHECK(f.player.state.Held(t6) == 2);
    CHECK(f.host.pickedUp.empty());
    server::SkillCaster c9 = f.player.Caster();
    skills.Fight(c9, f.player.state.busyUntil + std::chrono::milliseconds(10));
    REQUIRE(f.host.pickedUp.size() == 1);
    CHECK(f.host.pickedUp.back() == 4242u);
    CHECK(f.player.state.attackTarget == 0u);
    f.player.state.attackTarget = Field::kSelected;
    server::SkillCaster c10 = f.player.Caster();
    CHECK_FALSE(skills.HoldPickUp(c10, 4343u, f.player.state.busyUntil + std::chrono::milliseconds(10)));
    CHECK(f.player.state.attackTarget == 0u);
    // A stop with one held: it ends.
    f.player.state.attackTarget = Field::kSelected;
    server::SkillCaster c7 = f.player.Caster();
    skills.Stop(c7, f.player.state.busyUntil + std::chrono::milliseconds(10));
    CHECK(f.player.state.attackTarget == 0u);
}

TEST_CASE("a wall's HP: its 'pw' words read from the running buff; what it takes adds up, at its HP the wall ends (game files; U3-11)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    Field f;
    server::Skills skills(f.host, &table, 7);
    const assets::SkillRow* wall = table.Find(std::string_view("SKILL_CH_COLD_BINGBYEOK_A_01"));
    REQUIRE(wall != nullptr);
    const assets::SkillParam pw = assets::FindSkillParam(*wall, assets::SkillTag("pw"));
    REQUIRE(pw);
    const std::uint32_t hp = std::uint32_t(pw.Int(1));
    REQUIRE(hp > 10);
    CHECK_FALSE(skills.WallOf(f.player.state).has_value());
    server::SkillState::Buff held;
    held.token = 77;
    held.skill = wall->id;
    held.until = Clock::now() + std::chrono::seconds(60);
    f.player.state.buffs.push_back(held);
    std::optional<game::WallSpec> spec = skills.WallOf(f.player.state);
    REQUIRE(spec.has_value());
    CHECK(spec->kinds == pw.Int(0));
    CHECK(spec->defence == pw.Int(2));
    CHECK(spec->absorption == pw.Int(3));
    CHECK(spec->hpLeft == hp);
    // Part of it taken: still up, its HP less.
    server::SkillCaster c0 = f.player.Caster();
    CHECK_FALSE(skills.WallTook(c0, hp - 10));
    spec = skills.WallOf(f.player.state);
    REQUIRE(spec.has_value());
    CHECK(spec->hpLeft == 10);
    // The rest and more: the wall ends, told to its holder.
    server::SkillCaster c1 = f.player.Caster();
    CHECK(skills.WallTook(c1, 25));
    CHECK(f.player.state.buffs.empty());
    CHECK_FALSE(skills.WallOf(f.player.state).has_value());
    REQUIRE_FALSE(f.host.All<proto::BuffRemoved>().empty());
    CHECK(f.host.All<proto::BuffRemoved>().back().token == 77);
}

TEST_CASE("a bard's dance only while one of its songs plays (reqc 0x20, 0x3032); it ends with the last song (game files; U3-04)") {
    if (GameDir() == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, GameDir(), error), error);
    assets::SkillTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    Field f;
    server::Skills skills(f.host, &table, 7);
    const assets::SkillRow* song = table.Find(std::string_view("SKILL_EU_BARD_BATTLAA_GUARD_A_01"));
    const assets::SkillRow* dance = Learn(f, table, "SKILL_EU_BARD_DANCEA_WARRIOR_A_01");
    REQUIRE(song != nullptr);
    REQUIRE(dance != nullptr);
    f.player.book.groups[song->group] = song->level;
    // No song: the dance refused, its own code.
    server::SkillCaster c0 = f.player.Caster();
    CHECK_FALSE(skills.Use(c0, {dance->id, 0}, Clock::now()));
    CHECK(f.host.All<proto::SkillUseAnswer>().back().status == proto::SkillUseStatus::Music);
    CHECK_FALSE(skills.PlaysMusic(f.player.state));
    // A song running: music; the dance passes that check.
    server::SkillState::Buff playing;
    playing.token = 501;
    playing.skill = song->id;
    playing.until = Clock::time_point::max();
    f.player.state.buffs.push_back(playing);
    CHECK(skills.PlaysMusic(f.player.state));
    server::SkillCaster c1 = f.player.Caster();
    skills.Use(c1, {dance->id, 0}, Clock::now());
    CHECK(f.host.All<proto::SkillUseAnswer>().back().status != proto::SkillUseStatus::Music);
    // A dance running and the song ending (its time up): the dance ends with it.
    server::SkillState::Buff dancing;
    dancing.token = 502;
    dancing.skill = dance->id;
    dancing.until = Clock::time_point::max();
    f.player.state.buffs = {playing, dancing};
    f.player.state.buffs[0].until = Clock::now() - std::chrono::seconds(1);
    server::SkillCaster c2 = f.player.Caster();
    skills.ExpireBuffs(c2, Clock::now());
    CHECK(f.player.state.buffs.empty());
}
