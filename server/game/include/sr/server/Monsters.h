#pragma once

// Monsters in the world: the spawn points (nests) and what the monsters do.
//
// - A nest is a point of npcpos.txt (the client's file: which monster, where) with the original
//   server's policy for it (its nests and tactics in the database: how many at once, how far around the point they
//   come up, how soon a gap refills, whether they attack on sight). A point without a policy holds one monster,
//   peaceful.
// - Nests wake where players are: a nest is active while its region's navigation is in (the world
//   holds the regions around every player), filled at once on waking, emptied when the navigation
//   goes. The fields far from everyone cost nothing.
// - A monster is a General, or rolled at spawn into a Champion (the nest's chance; one in seven of
//   those a Giant); uniques are uniques by their reference. Its health is its reference's times its
//   kind's (game::proto::MonsterMaxHp).
// - Between fights a monster idles 2 to 6 s, then (three times in four) walks 40 to 120 away at its
//   walk speed; a walk that would take it out of its nest's spawn circle heads back toward the
//   point instead (the distances ours).
// - Fighting (C10b): a monster hit by a player, or an aggressive one (its nest's policy; champions,
//   giants and elites always) that sees a player within its nest's sight, runs at it and uses its skills (characterdata cols 89..98, those that hit) in
//   reach, one after another with the skill's own time between them. It gives up when the target
//   dies, leaves, or leads it farther than 500 from its target or its nest's leash from home: it runs
//   home and its health comes back. Killed, it lies 5 s and goes; its nest refills after its delay.
// - States (K3): the states its hits give it (rolled by the skills against its
//   level and its Resist columns) last their time and do what they do: frozen, asleep or stunned it
//   stands and does nothing; rooted it does not move; frostbite and slow slow it and its swings; fear
//   makes it drop its fight with the one who feared it, confusion any fight; burn, poison and bleed
//   tick; the curses change its numbers. A hit with damage wakes it and breaks a stun one time in
//   four, magical damage breaks a root. A knock-down lays it down (only down attacks reach it), a
//   knock-back pushes it; either way it does nothing for that time.
// - The skill lab's monsters (MonsterNest::lab, a developer's DevSpawn): a still one never moves nor fights (it keeps its
//   records of who hit it and how much threat, and its states and knocks happen to it); a fleeing one walks or runs
//   straight away from its player, turning as that player comes round, and never fights; a normal one is as any.

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "sr/game/Buffs.h"
#include "sr/game/Combat.h"
#include "sr/game/States.h"
#include "sr/game/WorldProtocol.h"

namespace sr::assets {
class CharacterTable;
class SkillTable;
struct ObjectRow;
} // namespace sr::assets

namespace sr::server {

class World;

// The original's 32 Resist columns of each monster (characterdata RefObjID -> ResistFrozen ..
// Resist32), from the database: the client's files do not carry them.
using MonsterResistances = std::unordered_map<std::uint32_t, std::array<std::uint8_t, 32>>;
// A monster's defence against states from its level and its Resist columns.
game::StateDefence MonsterStateDefence(int level, const std::array<std::uint8_t, 32>& resist);

struct MonsterNest {
    std::uint32_t refObjId = 0; // characterdata
    game::proto::WorldPosition position;
    float radius = 0;      // how far it may be led away (the leash)
    float spawnRadius = 0; // where its monsters come up and wander, around the point
    int championPercent = 0;
    float delayMin = 1, delayMax = 1; // seconds before a gap refills
    int maxCount = 1;
    bool aggressive = false; // attacks players it sees
    float sight = 0;
    // A fortress's structure (Monsters::AddFixed): it stands where it is
    // put whether the navigation is in or not, never moves nor fights nor heals, and is not refilled.
    bool fixed = false;
    float yaw = 0; // which way a fixed one faces
    // An instance's nest (Monsters::AddNest): the original's nest and hive
    // (Tab_RefNest, Tab_RefHive); `respawn` false: it spawns its count once and never refills (a gap left when its
    // region's navigation goes comes back with it); `waiting`: it spawns only once activated (ActivateNest).
    std::uint32_t nestId = 0, hive = 0;
    bool respawn = true;
    bool waiting = false;
    // Gone by itself once its monsters are dead and lain (a summon, a trigger's monsters).
    bool temporary = false;
    int rarity = -1; // its monsters' rarity (-1: its reference's, a champion at the nest's chance)
    // Its hive's cap over all the hive's nests (Tab_RefHive dwOverwriteMaxTotalCount; 0: none). A unique's nests
    // (characterdata rarity 3) keep one state for their hive, awake or not: its monster exists while its region
    // sleeps and is put back when it wakes; at most this many at once, the next hatching after a death at a
    // nest of the hive picked at random, once that nest's delay is over. A unique nest without a hive is a hive of its
    // own.
    int hiveMax = 0;
    // A bandit (Bandits.h): whom it goes for (game::BanditKind: a thief monster the traders and
    // hunters and theirs, a hunter monster the thieves and theirs), its tactics row's btChangeTarget (0 keeps its target
    // when another hits it, 1 turns to that one at once, 2 the ordinary choice), and gone without dying `leaveMs` after
    // its last hit given or taken, and on entering a town (Monsters::SetTowns).
    std::uint8_t bandit = 0;
    int changeTarget = 2;
    int leaveMs = 0;
    // A fortress war's guard or tablet monster (Monsters::AddWarMonster): the guild (and
    // union) it stands for. It goes only for the players fighting in a war against that side (Player::atWar), never for
    // those of its side, and never wanders off.
    std::uint32_t warGuild = 0, warAlliance = 0;
    // The skill lab's (L2; proto::DevSpawn): how its monster behaves (Normal: as any), the player a
    // fleeing one walks or runs away from, and its health (0: its own).
    game::proto::LabBehaviour lab = game::proto::LabBehaviour::Normal;
    std::uint32_t labFrom = 0;
    std::uint32_t labHp = 0;
};

class Monsters {
public:
    using Clock = std::chrono::steady_clock;

    // A player monsters may fight, as the game server sees it this update.
    struct Player {
        std::uint32_t entity = 0;
        game::proto::WorldPosition position;
        bool alive = true;
        const game::CombatStats* stats = nullptr;
        // Harmony's protection (Skills::Protection): monsters of these grades (bit 1 grade 0, 2 grade 1,
        // 4 grade 3, 8 grade 6) up to this level do not start a fight with it (one it attacks still fights back).
        std::uint8_t protectedGrades = 0;
        int protectedLevel = 0;
        // Hidden from monsters (stealth, invisibility): it cannot be a target.
        bool hidden = false;
        // The jobs: its job state (a COS its owner's; game::kNoJob none), the other of the pair a
        // bandit goes between (a player's transport, a COS's owner; 0: none), whether it is a COS whose owner rides it
        // (a bandit's pick gives way to the owner), and whether only bandits go for it (a transport).
        std::uint8_t job = 4;
        std::uint32_t partner = 0;
        bool ridden = false;
        bool banditsOnly = false;
        // The transport a player rides (0: none): a rider is never struck, every monster's fight with them goes to it
        // (upstream's review of 2026-11-22).
        std::uint32_t mount = 0;
        // A fortress war (MonsterNest::warGuild): its guild and union, and whether it fights in a fortress's war there.
        std::uint32_t guild = 0, alliance = 0;
        bool atWar = false;
    };
    // What the monsters did this update, for the game server to tell (and to apply to players).
    struct Events {
        std::vector<game::proto::EntitySwing> swings;
        struct Hit {
            std::uint32_t monster = 0, target = 0;
            game::HitResult hit;
            const assets::SkillRow* skill = nullptr; // the swing's skill: the states its hit may give
            int level = 1;                           // the monster's
        };
        std::vector<Hit> hits;
        std::vector<std::uint32_t> healed; // gave up and went home: their health back to full
        std::vector<std::uint32_t> statesChanged; // a state ended (or a death ended them all)
        // A unique of the world's hives came into being (its region awake or not; MonsterNest::hiveMax).
        struct Unique {
            std::uint32_t refObjId = 0;
            game::proto::WorldPosition position; // its nest's
        };
        std::vector<Unique> uniques;
    };
    struct Damaged {
        bool hit = false;    // it was a living monster
        bool killed = false; // this hit killed it
        std::uint32_t hp = 0;
        std::uint32_t refObjId = 0;
        std::uint8_t rarity = 0;
        // A kill: who hurt it and by how much (the experience's shares), in order of the first hit.
        std::vector<std::pair<std::uint32_t, std::uint64_t>> damageBy;
        // States: whether they changed (one taken, woken from, ended by death); a knock and where to.
        bool statesChanged = false;
        std::int64_t nest = -1; // its nest's index (AddNest's), for a kill's triggers
        std::uint32_t killer = 0; // a kill: the attacker of the killing hit
        bool unique = false;      // a kill of a unique of the world's hives (Events::uniques told its coming)
        game::Knock knock = game::Knock::None;
        game::proto::WorldPosition knockTo;
        std::uint32_t knockMs = 0;
    };
    // A state's tick (burn, poison, bleed) on a monster this update.
    struct Tick {
        std::uint32_t monster = 0;
        game::StateTick tick;
        Damaged damaged;
    };
    // A hit's states and motion for Damage (rolled by the skills: game::RollStates, game::RollKnock).
    struct Affliction {
        bool magical = false; // its damage had a magical part: a root ends
        std::vector<game::ActiveState> states;
        game::Knock knock = game::Knock::None;
        game::proto::WorldPosition from; // the attacker: a knock pushes away from it
        float distance = 0;
        std::uint32_t knockMs = 0;
        // A buff's changes on it for a time (the Chinese water's cancels, K4); replaced and refused as
        // a player's buffs are (game::AdmitBuff).
        const assets::SkillRow* debuff = nullptr;
        std::vector<game::StatChange> debuffChanges;
        std::uint32_t debuffMs = 0;
        // The hit's threat (not given: its damage), and the percent
        // the attacker's record changes by first (a `dtnt`'s second word, negative).
        bool threatGiven = false;
        std::int64_t threat = 0;
        int threatPercent = 0;
    };
    // What the skills roll a hit's states and knocks against.
    struct Target {
        game::StateDefence defence;
        std::uint32_t states = 0; // its mask
        int susceptibility = 0;   // disease's
        bool down = false;        // knocked down: only a down attack ('da') reaches it
        bool canFall = false, canPush = false;
        int recoverMs = 0;        // KO_RecoverTime
    };

    Monsters(World& world, const assets::CharacterTable& characters, const assets::SkillTable* skills, std::vector<MonsterNest> nests,
             std::uint32_t seed, std::shared_ptr<const MonsterResistances> resistances = nullptr);
    ~Monsters();
    Monsters(const Monsters&) = delete;
    Monsters& operator=(const Monsters&) = delete;

    // Wakes and empties nests (once a second), refills gaps, ticks and ends states, moves the idle
    // and fights. Its states' ticks this update go to `ticks`.
    void Update(Clock::time_point now, const std::vector<Player>& players, Events& out, std::vector<Tick>* ticks = nullptr);
    // A fixed monster (a fortress's structure, MonsterNest::fixed) of characterdata `refObjId` standing
    // at `at` facing `yaw`, with `hp` of `maxHp` (0: its reference's); 0 when it could not be made.
    // RemoveFixed takes it away (standing or lying). `bodyRadius` (0: its reference's) is added to the
    // reaches of those who attack it.
    // `destroyed`: it stands as rubble, at no health. A fixed one killed stays until RemoveFixed.
    std::uint32_t AddFixed(std::uint32_t refObjId, const game::proto::WorldPosition& at, float yaw, std::uint32_t hp, std::uint32_t maxHp,
                           float bodyRadius = 0.0f, bool destroyed = false);
    void RemoveFixed(std::uint32_t entity);
    // A fortress war's guard or tablet monster (MonsterNest::warGuild) of characterdata `refObjId` standing at `at` now,
    // for that side, with `hp` (0: its most); 0 when it could not be made. RemoveFixed takes it away.
    std::uint32_t AddWarMonster(std::uint32_t refObjId, const game::proto::WorldPosition& at, float yaw, std::uint32_t warGuild, std::uint32_t warAlliance,
                                std::uint32_t hp = 0);

    // A nest added (an instance's, a summon; its position's layer is its monsters'): its index, kept until
    // RemoveNest (or, temporary, until it is done). It fills when its region's navigation is in, as the others do.
    std::size_t AddNest(const MonsterNest& spec);
    // Its monsters and bodies gone, the nest with them.
    void RemoveNest(std::size_t nest);
    // A waiting nest starts (ACTION_MONSTER_ACTIVE_NEST).
    void ActivateNest(std::size_t nest);
    // A nest that will never have a monster again: not waiting, spawned its count, all of them dead (a refilling
    // nest never is).
    bool NestCleared(std::size_t nest) const;
    const MonsterNest* NestSpec(std::size_t nest) const;
    // Its monsters standing, and (one that does not refill) those it has still to spawn.
    std::pair<int, int> NestCounts(std::size_t nest) const;
    // Its monsters standing (their entities).
    std::vector<std::uint32_t> NestMonsters(std::size_t nest) const;
    // A hit on a monster from `attacker` (a player's entity): its health less `damage`; it turns on
    // the attacker; then the hit's states and knock (null: none). A fixed one only loses the health.
    Damaged Damage(std::uint32_t monster, std::uint32_t attacker, std::uint32_t damage, Clock::time_point now, const Affliction* affliction = nullptr);
    // A buff of its own on a living monster (the Roc's berserk): `changes` for `ms`, under `skill`'s
    // name (one of the same skill replaced); false when it is not a living monster.
    bool Boost(std::uint32_t monster, const assets::SkillRow* skill, std::vector<game::StatChange> changes, std::chrono::milliseconds ms,
               Clock::time_point now);
    // Threat without a hit (a heal's on the monsters fighting the healed one), and the
    // monsters whose current target an entity is.
    void AddThreat(std::uint32_t monster, std::uint32_t source, std::int64_t threat, Clock::time_point now);
    std::vector<std::uint32_t> TargetingOf(std::uint32_t entity) const;
    // A monster's current target and its record's threat (0: none), for tests and tools.
    std::pair<std::uint32_t, std::int64_t> CurrentTarget(std::uint32_t monster) const;
    // A living monster's fighting numbers, its states' changes in (null: none, or dead).
    const game::CombatStats* Stats(std::uint32_t entity) const;
    // A living monster as a hit's states see it (none: none, or dead).
    std::optional<Target> TargetOf(std::uint32_t entity) const;
    // A dead monster's body while it lies (a mask's target, 'mcap'): its model, level, kind and type
    // (characterdata TID4: 1 a field monster, 2 a thieves' job monster, 3 a hunters', 4 a quest's).
    struct Corpse {
        std::uint32_t model = 0;
        int level = 0;
        std::uint8_t rarity = 0;
        int type = 0;
    };
    std::optional<Corpse> CorpseOf(std::uint32_t entity) const;
    // The states it is in, with their time left (EntityStates).
    std::vector<game::proto::StateRecord> StatesOf(std::uint32_t entity, Clock::time_point now) const;
    // A living monster's health now (0: none, or dead).
    std::uint32_t Hp(std::uint32_t entity) const;
    // A living monster's health raised by `amount`, up to its most (a fortress structure's repair kit): its health then
    // (0: none, or dead).
    std::uint32_t Heal(std::uint32_t entity, std::uint32_t amount);

    // The living monsters whose middle is within `radius` of a point (in its region and those around
    // it), by entity id: an area skill's candidates.
    std::vector<std::uint32_t> Near(const game::proto::WorldPosition& at, float radius) const;
    std::size_t Alive() const;
    // A monster's characterdata RefObjID, living or lying (0: not a monster).
    std::uint32_t RefOf(std::uint32_t entity) const;
    std::size_t Nests() const;
    std::size_t ActiveNests() const;
    // The towns (`_RefRegion.IsBattleField` 0): any monster stepping into one is gone, but one whose
    // nest stands in one.
    void SetTowns(std::function<bool(std::uint16_t region)> town);
    // The towns' safe areas about them by position (regioninfo's town rectangles, the original's): the same
    // rules as the towns' regions (no monster in, none fights a player there).
    void SetTownAreas(std::function<bool(const game::proto::WorldPosition& at)> area);

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
