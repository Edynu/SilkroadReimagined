#pragma once

// The players' skills and their fight (C7f, C10b): using a skill (known, the
// weapon it takes, its mana, not waiting), the basic attacks and the skills' swings walked into
// reach, their hits landing, the buffs and the imbue on a player, their end.
//
// The unit keeps a player's fight and skill state (SkillState) and its rules; what is outside them
// it asks of its host: the world (where an entity is, a walk), the monsters (their numbers, a hit),
// the rewards of a kill, the quiver, and the messages to the caster and to those who see it. The
// game server is its host in play; a test is one too, which casts every skill without the network.

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <unordered_map>
#include <variant>
#include <vector>

#include "sr/game/Buffs.h"
#include "sr/game/Combat.h"
#include "sr/game/Heals.h"
#include "sr/game/Passives.h"
#include "sr/game/Skills.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/Monsters.h"

namespace sr::assets {
class SkillTable;
struct SkillRow;
} // namespace sr::assets

namespace sr::server {

// A player's fight and skills, as the unit keeps them (one per player in the world).
struct SkillState {
    using Clock = std::chrono::steady_clock;
    // A buff: a skill's effect until a time (a toggle's: until it is turned off or its MP runs out);
    // the client removes it by its token. What it changes on its holder's numbers (K4,
    // worked out as it starts). One that pulses (an area of its own around the
    // caster, 'puls'): its period and its next pulse. A toggle ('onff'): its MP every period.
    struct Buff {
        std::uint32_t token = 0, skill = 0;
        Clock::time_point until;
        std::vector<game::StatChange> changes;
        int pulseMs = 0;
        Clock::time_point nextPulse;
        int upkeepMs = 0;
        std::uint32_t upkeepMp = 0;
        Clock::time_point nextUpkeep;
        // A heal over time (K7: its 'heal' with a 'puls' and no area of its own): what each pulse gives
        // its holder, worked out as it was cast.
        game::HealAmount pulseHeal;
        // A companion that strikes ('summ': the Bow's hawk): every period while
        // its holder attacked within one, its power (the attack the strike is worked out from) and its
        // magical one; its last strike.
        int companionMs = 0;
        std::uint32_t companionPower = 0, companionMagic = 0;
        Clock::time_point lastStrike;
        // A link's copy: its owner, whose link it goes with.
        std::uint32_t linkOwner = 0;
        // The holder's own (cast on itself, a scroll's): what it wears must meet its conditions ('reqi'). One
        // another gave it (a party member's buff, a link's copy, an object's) is never checked: the original
        // checks only its role-1 instances.
        bool own = true;
    };
    std::vector<Buff> buffs;
    // Its links ('lnks'): the owner's side of each link skill it cast on
    // another, the copy there a buff of the skill on a player (its token), or on a monster only the
    // DOT's strikes ('lks2': its 'att' and states every 'puls' on that target alone); the hits a guard
    // took over ('lkdr' ends at its third word).
    // Its transform (msch), held by one buff (its token): 1 a monster's
    // shape (its model; its basic skill the attack, no skill else), 2 another player's look (the Rogue's
    // duplicate: whose), 3 and 4 the slot alone (the Warlock's illusion, the Cleric's rebirth).
    struct Transform {
        int mode = 0;
        std::uint32_t model = 0, copied = 0, basic = 0, token = 0;
    };
    Transform transform;
    // Each link has a token of its own (its owner's buffs board shows it by that: proto::LinkAdded); a
    // mark ('hntp') tells its owner where its target is every second (nextReport).
    struct Link {
        std::uint32_t skill = 0, target = 0, copy = 0, token = 0;
        Clock::time_point until, nextPulse, nextReport;
        int pulseMs = 0;
        int hits = 0;
        bool mark = false;
    };
    std::vector<Link> links;
    // Its passives as they stand and what it wears for the skills' conditions (K6,
    // the game server works them out from its skills and its items).
    game::Passives passives;
    game::WornKinds worn;
    // The states it is in (K7's cures end them; the monsters' pass gives them) and the heals and cures
    // of the skills cast on the way (each on one player, its share of a transfer's).
    game::StateSet states;
    struct PendingSupport {
        Clock::time_point at;
        std::uint32_t target = 0;
        const assets::SkillRow* row = nullptr;
        int percent = 100;
    };
    std::vector<PendingSupport> pendingSupports;
    // The resurrections cast on the way: offered to their dead at the impact, once the cast is over.
    struct PendingRevival {
        Clock::time_point at;
        std::vector<std::uint32_t> targets;
        const assets::SkillRow* skill = nullptr;
    };
    std::vector<PendingRevival> pendingRevivals;
    // Its passives' and its buffs' changes, each its own source (a passive by its id, a buff by its token).
    game::StatSheet Sheet() const {
        game::StatSheet sheet;
        passives.AddTo(sheet);
        for (const Buff& b : buffs) sheet.Add(b.token, b.changes);
        return sheet;
    }
    // The target its last attack skill went off on and when (a companion strikes it: Buff::companionMs).
    std::uint32_t companionTarget = 0;
    Clock::time_point lastAttack;
    // The monster it attacks (0: none), when its next swing may start, where it last walked to get
    // into reach and when it may aim again; the hits of its swings still to land.
    std::uint32_t attackTarget = 0;
    Clock::time_point nextSwing, nextAim;
    game::proto::WorldPosition aim;
    struct PendingHit {
        Clock::time_point at;
        std::uint32_t target = 0;
        game::AttackSpec attack;
        float reach = 0;
        int percent = 100; // its share of the damage (an area's further targets get less)
        const assets::SkillRow* row = nullptr; // the row whose states and knocks it gives
        // The imbue running as its swing began (kept for the swing's rows), and
        // whether an imbue's jump added its target (the magical parts only).
        const assets::SkillRow* imbue = nullptr;
        bool magicalOnly = false;
        std::uint8_t strike = 0; // its place in its swing (proto::EntityDamaged::strike)
        // A skill object's (Skills::Objects): struck from where the object stands, whatever the
        // distance to its owner (a knock pushes away from the object).
        bool fromObject = false;
        game::proto::WorldPosition origin;
        // A companion's strike (its skill; 0 none): its attack its power, whatever the distance.
        std::uint32_t companion = 0;
        // A strike that is no swing's (an aura's pulse, a skill object's): told as its row's return.
        bool returned = false;
        // The use it belongs to: the equipment's wear is rolled once a use, when its last hit has landed.
        std::uint32_t use = 0;
    };
    std::vector<PendingHit> pendingHits;
    // A use's wear so far: the caster's hits not blocked, each player target's landed hits and blocks.
    std::uint32_t uses = 0;
    struct UseWear {
        int weaponHits = 0;
        std::map<std::uint32_t, std::pair<int, int>> targets; // player -> (landed, blocked)
    };
    std::map<std::uint32_t, UseWear> wear;
    // A chain's later rows (col 9):
    // each a cast of its own at its time, its area chosen then, struck at its impact; a row whose target is dead or gone
    // by its cast ends the chain (no later row cast).
    struct PendingRow {
        Clock::time_point castAt, impactAt;
        const assets::SkillRow* row = nullptr;
        const assets::SkillRow* imbue = nullptr; // the imbue as the swing began (handed on to its rows)
        std::uint32_t target = 0;
        int strikes = 0;   // the swing's hits before it (EntityDamaged::strike)
        float reach = 0;   // the swing's reach (its selected target is struck wherever it went)
        bool first = false; // the swing's first row, released after its casting (its arrows taken at the swing)
    };
    std::vector<PendingRow> pendingRows;
    // The casting slot: a cast not yet released, re-checked on every skill tick till then
    // (what is worn, its target, its mana).
    struct Casting {
        const assets::SkillRow* skill = nullptr;
        std::uint32_t target = 0; // 0 or the caster: no target to check
        Clock::time_point release, nextCheck;
        std::uint32_t mp = 0;     // its mana, paid at the release
    };
    std::optional<Casting> casting;
    // Buffs whose cast is not released yet: put on, and paid for, at the release.
    struct PendingBuff {
        Clock::time_point at;
        std::uint32_t holder = 0;
        const assets::SkillRow* skill = nullptr;
    };
    std::vector<PendingBuff> pendingBuffs;
    // A skill on another player out of its reach: the caster walks toward it, the command cast once in reach (as the
    // original does for every targeted command).
    struct Approach {
        game::proto::UseSkill use;
    };
    std::optional<Approach> approach;
    // The taunts and aggro cuts whose impact is to come: the area around `centre` (0: the
    // caster), the threat `source`'s (the caster, or the selected player for the Bard's Discord Wave).
    struct PendingTaunt {
        Clock::time_point at;
        const assets::SkillRow* skill = nullptr;
        std::uint32_t centre = 0, source = 0;
    };
    std::vector<PendingTaunt> pendingTaunts;
    // The part of the hold a waiting command cuts short: the chain's budget the basic attack after it would take back
    // (the last row holds it only when nothing waits).
    Clock::duration holdSpare{};
    // Its skill ticks (every 300 ms from the first): a cast's impact lands on
    // the first after its preparing and casting; the mana a swing pays at its impact (a swing stopped
    // before then pays nothing; its wait stays: B 1.1, 1.7).
    Clock::time_point skillEpoch;
    bool skillTicking = false;
    // Until a cast's impact it stands: a walk asked meanwhile is not taken (the cast stops every move; a move does not
    // cancel it).
    Clock::time_point castUntil;
    // A skill of several rows (col 9's chain) running: no walk taken until its last row went off (the move checks it).
    Clock::time_point chainUntil;
    // The action list: the command running (the
    // fight on attackTarget: queued's skill, else the basic attack) and at most one waiting behind it. The one running
    // is busy (casting, or mid-swing) until busyUntil: a new command then waits (the newest waiting wins); before (closing
    // in, between swings) it replaces the one running at once. A skill gone off holds it its preparing, casting and
    // duration; then the basic attack follows only after one of Action_AutoAttackType 1 (afterSkill), else it ends.
    struct Waiting {
        bool attack = false;           // a basic attack on `target`, else the skill asked for
        game::proto::UseSkill use;
        std::uint32_t target = 0;
        std::uint32_t pickUp = 0;      // a ground item asked for (as an attack click)
    };
    std::optional<Waiting> waiting;
    Clock::time_point busyUntil;
    int afterSkill = -1;     // the skill holding the front's Action_AutoAttackType (-1: none)
    std::uint8_t heldTold = 0; // the count last told its client
    std::uint8_t Held(Clock::time_point now) const {
        return std::uint8_t((attackTarget != 0 || now < busyUntil ? 1 : 0) + (waiting ? 1 : 0));
    }
    // In battle until then (attacking or attacked: 20 s; no stealth then).
    Clock::time_point battleUntil;
    // In berserk (the game server's): no stealth, no transform then.
    bool berserk = false;
    struct PendingCost {
        Clock::time_point at;
        std::uint32_t mp = 0;
    };
    std::vector<PendingCost> pendingCosts;
    // A skill to go off as the next swing, and the waits running: by skill group and by cool-down
    // group (skilldata col 18's top byte).
    const assets::SkillRow* queued = nullptr;
    std::unordered_map<std::uint32_t, Clock::time_point> skillCooling, groupCooling;
    // Its chance history (criticals, blocks, ignoring defence: game::ChanceRolls).
    game::ChanceRolls rolls;

    // A new walk, a pick-up, death: the fight stops (its hits on the way too).
    void StopAttacking() {
        attackTarget = 0;
        queued = nullptr;
        waiting.reset();
        afterSkill = -1;
        approach.reset();
    }
    void StopFighting() {
        StopAttacking();
        pendingHits.clear();
        pendingRows.clear();
        pendingBuffs.clear();
        pendingSupports.clear();
        pendingTaunts.clear();
        casting.reset();
        holdSpare = {};
        chainUntil = {};
        pendingCosts.clear();
        pendingRevivals.clear();
    }
};

// A player as the skills see it for one call: the game server's record (or a test's).
struct SkillCaster {
    std::uint32_t entity = 0;
    const game::SkillBook& book;
    const game::CombatStats& stats;
    std::uint32_t& hp;
    std::uint32_t& mp;
    bool dead = false;
    std::chrono::steady_clock::time_point untouchableUntil;
    SkillState& state;
};

// What the skills tell: to the caster, or to everyone who sees an entity.
using SkillMessage = std::variant<game::proto::SkillUseAnswer, game::proto::Cooldown, game::proto::BuffAdded, game::proto::BuffRemoved,
                                  game::proto::EntitySwing, game::proto::EntityBlink, game::proto::EntityDamaged, game::proto::EntityStates, game::proto::EntityKnocked,
                                  game::proto::SkillObjectStruck, game::proto::SkillTook, game::proto::LinkAdded, game::proto::LinkPosition,
                                  game::proto::ActionHeld, game::proto::EntityCastCancelled>;

class SkillHost {
public:
    using Clock = std::chrono::steady_clock;
    virtual ~SkillHost() = default;
    // The world.
    virtual std::optional<game::proto::WorldPosition> Position(std::uint32_t entity) const = 0;
    // Where it faces (a mark's target, for its owner's minimap).
    virtual float Facing(std::uint32_t entity) const {
        (void)entity;
        return 0.0f;
    }
    virtual bool Moving(std::uint32_t entity) const = 0;
    virtual void MoveTo(std::uint32_t entity, const game::proto::WorldPosition& to) = 0;
    // The monsters: a living one's numbers (null: none, or dead), a hit on one, a kill's rewards.
    virtual const game::CombatStats* Monster(std::uint32_t entity) const = 0;
    virtual std::uint32_t MonsterHp(std::uint32_t entity) const = 0;
    // The living monsters whose middle is within `radius` of a point, by entity id.
    virtual std::vector<std::uint32_t> MonstersNear(const game::proto::WorldPosition& at, float radius) const = 0;
    // The players whose middle is within `radius` of a point, by entity id, and a player's party
    // (none yet: parties come with the social pass; ours).
    virtual std::vector<std::uint32_t> PlayersNear(const game::proto::WorldPosition& at, float radius) const = 0;
    virtual std::vector<std::uint32_t> PartyOf(std::uint32_t player) const {
        (void)player;
        return {};
    }
    // A resurrection for a dead player (K9): the host asks it (the original's proposal) and raises
    // it with the skill's numbers when it accepts.
    virtual void OfferRevival(std::uint32_t caster, std::uint32_t target, const assets::SkillRow& skill) {
        (void)caster;
        (void)target;
        (void)skill;
    }
    // A living monster as a hit's states see it (its defence, its states, down or not), and its
    // states as the client is told them (K3).
    virtual std::optional<Monsters::Target> MonsterTarget(std::uint32_t entity) const = 0;
    virtual std::vector<game::proto::StateRecord> MonsterStates(std::uint32_t entity, Clock::time_point now) const = 0;
    // A hit: its damage, then its states and knock (null: none).
    virtual Monsters::Damaged HitMonster(std::uint32_t monster, std::uint32_t attacker, std::uint32_t damage, Clock::time_point now,
                                         const Monsters::Affliction* affliction) = 0;
    virtual void Killed(std::uint32_t monster, const Monsters::Damaged& damaged) = 0;
    // Up to `count` arrows or bolts from the caster's quiver for its weapon, the bag's next stack put in when it runs out;
    // how many it took (0: none fits or none left).
    virtual int TakeAmmunition(std::uint32_t caster, int count) = 0;
    // The caster's HP or mana changed; a player's buffs changed (its numbers to make again from them,
    // its speed, its stat panel).
    virtual void VitalsChanged(std::uint32_t caster) = 0;
    virtual void StatsChanged(std::uint32_t player) = 0;
    // A reuse of five minutes or more began: it is written, so a logout or a change of server does not end it.
    virtual void LongCooldownStarted(std::uint32_t player, std::uint32_t skill) {
        (void)player;
        (void)skill;
    }
    // Another player as a buff's holder (none: not a player in the world).
    virtual std::optional<SkillCaster> Player(std::uint32_t entity) = 0;
    // PvP: whether `attacker` may attack the player `target` (guild wars).
    // Such a player then answers the monster calls above as a target (its numbers, HP, a hit, its death).
    virtual bool Hostile(std::uint32_t attacker, std::uint32_t target) const {
        (void)attacker;
        (void)target;
        return false;
    }
    // The same for the players an area attack of `attacker` gathers (mode 3: no new victims,
    // an innocent's area never on an assaulter).
    virtual bool HostileInArea(std::uint32_t attacker, std::uint32_t target) const { return Hostile(attacker, target); }
    // Whether `target` is in `caster`'s line of sight: the walk on the floor from one to the other is not blocked, a wall,
    // a building's side, an edge (the navigation's walk from one to the other, over at most 6 regions). A world without
    // navigation sees everyone.
    virtual bool InSight(std::uint32_t caster, std::uint32_t target) const {
        (void)caster;
        (void)target;
        return true;
    }
    // Why `attacker` may not attack `target` (Ok: no rule of the host's refuses it): the jobs' PvP and job monsters'
    // rules, a rider (0x3021).
    virtual game::proto::SkillUseStatus AttackRefusal(std::uint32_t attacker, std::uint32_t target) const {
        (void)attacker;
        (void)target;
        return game::proto::SkillUseStatus::Ok;
    }
    // The duplicate skill's bars on its target: before its level check a job's suit worn or free
    // battle (`murderer` false), after it a murderer (`murderer` true).
    virtual bool DuplicateBarred(std::uint32_t target, bool murderer) const {
        (void)target;
        (void)murderer;
        return false;
    }
    virtual bool IsPlayer(std::uint32_t entity) const {
        (void)entity;
        return false;
    }
    // Whether it stands in a fortress's world (the skills' bow and crossbow ranges shorten there).
    virtual bool InFortressWorld(std::uint32_t entity) const {
        (void)entity;
        return false;
    }
    // A fortress's structure; what a hit by `attacker` does to `target` when it is one (none: it is not).
    virtual bool IsStructure(std::uint32_t entity) const {
        (void)entity;
        return false;
    }
    // A structure's contact point for an attacker at `from`, its reach measured to it (none: not a structure,
    // measured centre to centre with the bodies).
    virtual std::optional<game::proto::WorldPosition> NearestPointOf(std::uint32_t target, const game::proto::FlatPosition& from) const {
        (void)target;
        (void)from;
        return std::nullopt;
    }
    virtual std::optional<std::uint32_t> StructureDamage(std::uint32_t attacker, std::uint32_t target) {
        (void)attacker;
        (void)target;
        return std::nullopt;
    }
    // A pick-up that waited in the action list, its turn come (Skills::HoldPickUp).
    virtual void PickUpWaited(std::uint32_t player, std::uint32_t item) {
        (void)player;
        (void)item;
    }
    virtual void ToCaster(std::uint32_t caster, const SkillMessage& message) = 0;
    virtual void ToObservers(std::uint32_t entity, const SkillMessage& message) = 0;
    // A skill object (Skills::Objects): an entity of the skill standing at a point, seen by those near
    // it (0: none could be made), and its going.
    virtual std::uint32_t SpawnObject(std::uint32_t owner, const assets::SkillRow& skill, const game::proto::WorldPosition& at) {
        (void)owner;
        (void)skill;
        (void)at;
        return 0;
    }
    virtual void RemoveObject(std::uint32_t object) { (void)object; }
    // A quest trap's catch (Skills::SetTrap; the owner's event 0x1D with
    // the monster, then the trap goes): a monster's reference id, and the event to the owner's quests.
    virtual std::uint32_t MonsterRefOf(std::uint32_t monster) const {
        (void)monster;
        return 0;
    }
    virtual void QuestTrapped(std::uint32_t owner, std::uint32_t monster) {
        (void)owner;
        (void)monster;
    }
    // A heal's threat: to every monster whose target the healed one
    // is, from the healer.
    virtual void HealThreat(std::uint32_t healer, std::uint32_t healed, std::int64_t threat) {
        (void)healer;
        (void)healed;
        (void)threat;
    }
    // A threat alone on a monster from a player (a guard's share of its guarded one's, 'lkag').
    virtual void AddThreat(std::uint32_t monster, std::uint32_t from, std::int64_t threat) {
        (void)monster;
        (void)from;
        (void)threat;
    }
    // A dead monster's body (a mask's target, 'mcap'); an item dropped at a player's feet for it (its
    // data: the mask's monster); a player's look changed whole (a transform begun or over).
    virtual std::optional<Monsters::Corpse> MonsterCorpse(std::uint32_t entity) const {
        (void)entity;
        return std::nullopt;
    }
    virtual void DropItem(std::uint32_t owner, std::uint32_t item, std::uint32_t data) {
        (void)owner;
        (void)item;
        (void)data;
    }
    virtual void LookChanged(std::uint32_t player) { (void)player; }
    // A use's wear on a player: `kind` 0 its weapon for `count` hits it landed unblocked,
    // 1 its armour for `count` hits taken, 2 its shield for `count` blocked; one roll of 2 % x count.
    virtual void EquipmentWorn(std::uint32_t player, int kind, int count) {
        (void)player;
        (void)kind;
        (void)count;
    }
    // The link skills whose copies a monster holds (the Warlock's DOTs): those who see it are told them
    // as its buffs (the original's B0BD around it), so their effects show on it.
    virtual void MonsterBuffs(std::uint32_t monster, std::vector<std::uint32_t> skills) {
        (void)monster;
        (void)skills;
    }
    // A blink (Skills' teleports): at once towards `to`, at most `most` away, as far as the way goes;
    // where it ends (none: it could not).
    virtual std::optional<game::proto::WorldPosition> Blink(std::uint32_t entity, const game::proto::WorldPosition& to, float most) {
        (void)entity;
        (void)to;
        (void)most;
        return std::nullopt;
    }
};

class Skills {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr float kPlayerRadius = 5.0f;   // a player's body in reaches (ours)
    static constexpr double kArrowSpeed = 450.0;   // world units a second (skilleffect.txt's MOV speed for the bow and crossbow)

    Skills(SkillHost& host, const assets::SkillTable* table, std::uint32_t seed);

    // The basic attack of a weapon kind (itemdata TID4; 1 bare hands; an unknown kind: bare hands).
    const assets::SkillRow* BasicAttack(const game::CombatStats& stats) const;
    // A skill's mana: its amount and its share of the most (skilldata cols 53, 55), at the caster's
    // percent of costs, less its passives' cuts.
    static std::uint32_t Mana(const game::CombatStats& stats, const assets::SkillRow& skill, const game::SkillModifiers& modifiers = {});
    // What a cast pays: its Mana, then a socket stone's 'msr' (the keeper's 0xC4 its chance, 0xC5 the percent off: the
    // MENTAL stones' 100, no cost), rolled at each payment.
    std::uint32_t PaidMana(SkillCaster& caster, const assets::SkillRow& skill);
    // Whether the weapon kind is one the skill takes (cols 50, 51; 255 none asked).
    static bool WeaponFits(int weaponKind, const assets::SkillRow& skill);
    // What a swing of the skill hits with for this caster: its attack, with the caster's level of
    // the skill's mastery (the higher of its two; the original's GetSkillMasteryRank) and what its
    // passives' values do to it (K6).
    static game::AttackSpec AttackFor(const game::SkillBook& book, const assets::SkillRow& skill, const game::SkillModifiers& modifiers = {});

    // The player's requests: an attack on a monster (0: stop), a skill from the quick slots. True
    // when the player now fights a monster (the host cancels what fighting cancels: a logout's
    // countdown, a walk to pick something up).
    // `fromList`: the action list's waiting command started (not queued again).
    bool Attack(SkillCaster& caster, std::uint32_t target, Clock::time_point now, bool fromList = false);
    bool Use(SkillCaster& caster, const game::proto::UseSkill& use, Clock::time_point now, bool fromList = false);
    // A ground item asked for (the original's pick-up command): with the one running
    // busy it waits behind it (true: held, the host's PickUpWaited takes it later); before, it replaces the one
    // running (the fight ends; false: the host picks it up now).
    bool HoldPickUp(SkillCaster& caster, std::uint32_t item, Clock::time_point now);
    // The player's stop (ActionStop): one command held, it ends (but a skill whose cast went off, or a chain under way);
    // two, the waiting one is dropped.
    void Stop(SkillCaster& caster, Clock::time_point now);
    // The caster's cast given up, told to those who see it: nothing paid, nothing struck. A stun, a knock, a freeze or a
    // sleep on it empties its command list too; a failed re-check (`emptyList` false) gives up that cast alone, the
    // command waiting behind it going on.
    void Interrupt(SkillCaster& caster, Clock::time_point now, bool emptyList = true);
    // The count it holds told to its client when it changed.
    void TellHeld(SkillCaster& caster, Clock::time_point now);
    // An action gone off at once (Stand, CastAround, CastObject): its hold on the list's front; the fight ended or not.
    void Commanded(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now);
    // Every tick: the hits whose time has come, then the walk into reach and the next swing.
    void Fight(SkillCaster& caster, Clock::time_point now);
    // Every tick, before the players' fights: the skill objects (an area of kind 3
    // 31-skill-presentation.md B 5.1-5.4). One stands at its caster's feet from the skill's impact
    // until its time ('dura'), its owner's death or leaving, its link's end ('lnks': the owner too far
    // away) or (a 'trap') its going off. Every 300 ms it looks around its radius: a buff object gives
    // the players its mask takes (1 the owner, 2 everyone, 4 the party) a copy of its buff for the time
    // it has left (Harmony's circle) and ends the copy of one who steps out; an attack object (mask
    // 0x08) strikes the monsters there with its owner's attack, a field every 'puls', a trap the first
    // one that comes and the area of kind 1 around it, then it is gone.
    void Objects(Clock::time_point now);
    std::size_t ObjectCount() const { return objects_.size(); }
    // A player's stealth and detection from its buffs: hidden by its
    // 'hide' (type bit, level; none while a 'dttp' copy on it detects that), the speed factor it walks
    // at, and its own detector ('dtt': mask, level).
    struct Stealth {
        std::uint8_t mask = 0, level = 0;
        float speed = 1.0f;
        std::uint8_t detectMask = 0, detectLevel = 0;
    };
    Stealth StealthOf(const SkillState& state) const;
    // The events that end buffs (`skc` word 1: 1 a move, 2 starting a skill), and a landed
    // hit (word 0's lanes; ended with a chance of 100 - word 2): told to the holder; true when any ended.
    bool EndOnEvent(SkillCaster& holder, std::uint32_t event);
    bool EndOnHit(SkillCaster& holder, int lanes);
    // What Harmony's buffs on a player keep off (their 'pola'): the monsters'
    // grades (the OR of the first words: bit 1 grade 0, 2 grade 1, 4 grade 3, 8 grade 6) up to a level
    // (the highest second word) do not start a fight with it (Monsters::Player).
    struct Protection {
        std::uint8_t grades = 0;
        int level = 0;
    };
    Protection ProtectionOf(const SkillState& state) const;
    // A hit the holder takes, `distance` from its attacker: its Snow
    // Shield ('dgmp', its own buff) moves a share onto its MP (1.5 MP a point; what the MP lacks goes back
    // on the hit), and its reflection ('dmgr': a passive's when it has one, else a buff's) on its chance
    // returns shares of the hit's physical and magical parts to an attacker within its range; the holder's
    // own damage stays. The MP is taken here (`mpTaken`: the host tells the vitals).
    // Then its links' copies (33 4): a guard's ('lkdr') shares move to the guard; a divide ('lkdd') spreads
    // a share over its party near it; a stun link ('abnb') gives its states to an attacker within its
    // range, as from the link's owner. The shares moved are the host's to take (`moved`).
    struct Guarded {
        std::uint32_t damage = 0;   // what the holder takes
        std::uint32_t mpTaken = 0;
        std::uint32_t returned = 0; // to the attacker
        std::uint32_t reflectSkill = 0;
        struct Moved {
            std::uint32_t player = 0, damage = 0, skill = 0;
        };
        std::vector<Moved> moved;
    };
    Guarded Guard(SkillCaster& holder, std::uint32_t attacker, std::uint32_t damage, bool magical, double distance, Clock::time_point now);
    // A link skill's admission for its owner on a target (33 4.1), over the
    // owner's links of its kind: the same skill on the target, or another of a kind not 0 (but a DOT's),
    // Overlap; as many as its third word (a DOT's: on that target), Links; else Ok.
    game::proto::SkillUseStatus AdmitLink(const SkillState& owner, const assets::SkillRow& skill, std::uint32_t target) const;
    // The monster mask used (its item's skill, 'msch 1'): the holder takes
    // the monster's shape (`model`, `level`, its basic skill `basic`) for the skill's time; refused (false,
    // told) when the monster's level is above the holder's (0x3008) or a stronger buff refuses it.
    bool TransformInto(SkillCaster& holder, const assets::SkillRow& skill, std::uint32_t model, int level, std::uint32_t basic, Clock::time_point now);
    // A quest's trap set from the bag (a 3/3/13/1 item whose skill has 'trap' and 'qest': the
    // original's special consumable casts it): its object at the user's feet now (Objects), whose catch of a monster its 'qest' names is the
    // owner's quest event (SkillHost::QuestTrapped). False: no such skill.
    bool SetTrap(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now);
    static bool IsQuestTrap(const assets::SkillRow& skill);
    // What a plain attack swings: a monster shape's basic skill, else the weapon's.
    const assets::SkillRow* DefaultAttack(const SkillCaster& caster) const;
    std::size_t MonsterCopies(std::uint32_t monster) const;

    // Buffs (K4): one from outside (a scroll's: its time renewed when it runs already; null when a
    // buff it may not stand beside runs), one restored with the time it had left (entering), one the
    // player ends (false: none such, or one that may not be ended), the ones that ended now and the
    // toggles' MP (told to the holder), the imbue running (null: none).
    const SkillState::Buff* AddBuff(SkillCaster& holder, const assets::SkillRow& skill, std::uint32_t ms, Clock::time_point now);
    void RestoreBuff(SkillCaster& holder, const assets::SkillRow& skill, std::uint32_t ms, Clock::time_point now);
    bool CancelBuff(SkillCaster& holder, std::uint32_t token);
    void ExpireBuffs(SkillCaster& caster, Clock::time_point now);
    // What it wears changed: the buffs whose conditions no longer hold end (told to the holder; the
    // host makes its numbers again after it).
    void EndUnfitBuffs(SkillCaster& caster);
    // The running buffs of these skills end at once, whoever gave them (a socket's skills as its item comes off).
    void EndBuffsOf(SkillCaster& holder, const std::vector<std::uint32_t>& skills);
    const assets::SkillRow* Imbue(const SkillState& state, Clock::time_point now) const;
    game::proto::BuffAdded BuffMessage(const SkillState::Buff& buff, Clock::time_point now) const;
    // The holder's numbers a buff takes a share of as it starts.
    static game::BuffBasis BasisOf(const game::CombatStats& stats);
    // Whether a buff of the skill outlives the logout: the original keeps it as a timed job when its
    // skill has 'cbuf' (the mall's, events', quests' and scrolls' buffs); the
    // players' own skills' buffs end with it.
    static bool OutlivesLogout(const assets::SkillRow& skill);
    // A skill's reuse of this long or more is kept in the database (as
    // a timed job, category 6, job 200); a shorter one only in the game server's memory.
    static constexpr int kLongCooldownMs = 300'000;
    // The reuse a character came back with, with the time it has left.
    void RestoreCooldown(SkillCaster& caster, const assets::SkillRow& skill, std::uint32_t ms, Clock::time_point now);
    // A token for a buff made outside (a saved one restored).
    std::uint32_t NextBuffToken() { return nextBuffToken_++; }

    // Whom one row of a skill strikes, and with what share of its damage: the selected target (all
    // of it; 0 none), then those its area of `kind` holds (its 'efr': 1 a swing's, 2 a pulse's).
    struct Struck {
        std::uint32_t target = 0;
        int percent = 100;
    };
    // `centre`: where an area around the caster is (a skill object's place; null: the caster's).
    std::vector<Struck> TargetsOf(const SkillCaster& caster, const assets::SkillRow& row, std::uint32_t selected, std::uint32_t kind = 1,
                                  const game::proto::WorldPosition* centre = nullptr) const;
    // The players a heal or a cure reaches (K7): the selected one, then those its
    // area's mask takes (0x01 the caster, 0x02 every character, 0x04 the party; a party mask's area is
    // the party), a transfer's at its shares.
    std::vector<Struck> FriendsOf(const SkillCaster& caster, const assets::SkillRow& row, std::uint32_t selected, std::uint32_t kind = 1) const;
    // The mastery level an imbue's force hits with: the skill's, at most 90
    // unless the imbue's own is above that.
    static int ImbueMastery(int skillLevel, int imbueLevel);

private:
    void CastBuff(SkillCaster& caster, SkillCaster& holder, const assets::SkillRow& skill, Clock::time_point now);
    // Links (33 4): one begun (its copy's token on a player, 0 on a monster; from its impact), every tick
    // of its owner (their life, a DOT's strikes; the copies it holds whose owner's link is gone), one
    // ended (its copy with it), the copies' owners and rows of a tag, a link's states on a monster.
    void StartLink(SkillCaster& owner, const assets::SkillRow& skill, std::uint32_t target, std::uint32_t copy, Clock::time_point from,
                   Clock::time_point until);
    void Links(SkillCaster& caster, Clock::time_point now);
    void EndLink(SkillCaster& owner, std::size_t index);
    std::vector<std::pair<std::uint32_t, const assets::SkillRow*>> CopiesOf(const SkillState& holder, const char* tag) const;
    void Afflict(SkillCaster& owner, std::uint32_t monster, const assets::SkillRow& row, Clock::time_point now);
    void TellCopies(std::uint32_t monster);
    void CastAround(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now);
    bool Support(SkillCaster& caster, const game::proto::UseSkill& use, const assets::SkillRow& skill, Clock::time_point now);
    bool Revive(SkillCaster& caster, const game::proto::UseSkill& use, const assets::SkillRow& skill, Clock::time_point now);
    void LandSupports(SkillCaster& caster, Clock::time_point now);
    void HealOne(SkillCaster& healer, SkillCaster& holder, const assets::SkillRow& row, int percent, Clock::time_point now);
    SkillState::Buff MakeBuff(const SkillCaster& holder, const assets::SkillRow& skill, std::uint32_t ms, Clock::time_point now,
                              const game::SkillBoosts* boosts = nullptr);
    void EndBuff(SkillCaster& holder, std::size_t index);
    void StartCooldown(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now);
    void LandHits(SkillCaster& caster, Clock::time_point now);
    // One row of a swing cast now: its targets chosen, its hits at its impact (a DOT's link started); the hits it strikes
    // its selected target with.
    int StrikeRow(SkillCaster& caster, const assets::SkillRow& row, const assets::SkillRow* imbue, std::uint32_t target, float reach, int strikes,
                  Clock::time_point impact, Clock::time_point now);
    // A chain's later rows at their casts (SkillState::pendingRows).
    void CastRows(SkillCaster& caster, Clock::time_point now);
    // Whether another player is in a skill's reach (its range, both
    // bodies, two more); out of it the command waits while the caster walks toward it (SkillState::approach).
    bool ReachFriend(SkillCaster& caster, const game::proto::UseSkill& u, const assets::SkillRow& skill, Clock::time_point now);
    // The one a Scorn's copy on it makes the only one it may aim at (0: none).
    std::uint32_t ForcedTarget(const SkillState& state) const;
    void Approach(SkillCaster& caster, Clock::time_point now);
    // A taunt or an aggro cut (SkillRoute::Taunt) and its impact.
    bool Taunt(SkillCaster& caster, const game::proto::UseSkill& u, const assets::SkillRow& skill, Clock::time_point now);
    void LandTaunts(SkillCaster& caster, Clock::time_point now);
    // The casting slot's re-checks on the skill ticks, and the buffs whose release came.
    void CheckCasting(SkillCaster& caster, Clock::time_point now);
    void LandBuffs(SkillCaster& caster, Clock::time_point now);
    // A buff put on its holder (the release of CastBuff's cast): its admission again, its mana, its board.
    bool InstallBuff(SkillCaster& caster, SkillCaster& holder, const assets::SkillRow& skill, Clock::time_point now);
    // The casting slot taken by a cast released at `release` (its first re-check on the next skill tick).
    void Occupy(SkillCaster& caster, const assets::SkillRow& skill, std::uint32_t target, Clock::time_point release, std::uint32_t mp, Clock::time_point now);
    // A cast's mana at its release (now when it has none), the slot taken till then.
    void PayAtRelease(SkillCaster& caster, const assets::SkillRow& skill, std::uint32_t target, Clock::time_point release, Clock::time_point now);
    // The arrows or bolts a row spends: its 'cnsm' count a hit x its hits.
    static int AmmunitionOf(const assets::SkillRow& row);
    void Pulse(SkillCaster& caster, Clock::time_point now);
    void Recover(SkillCaster& caster, std::uint32_t hp, std::uint32_t mp);
    void CastObject(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now);
    void Companions(SkillCaster& caster, Clock::time_point now);
    // A cast begun: the caster stops where it stands and stays until its impact (preparing and casting
    // over, on its skill tick); what it casts lands then whatever the distance. The impact.
    Clock::time_point Stand(SkillCaster& caster, const assets::SkillRow& skill, Clock::time_point now);
    // The caster's first skill tick at or after `at` (SkillState::skillEpoch).
    static Clock::time_point OnTick(SkillState& state, Clock::time_point now, Clock::time_point at);

    // A skill object (Objects): its entity, owner and skill, where it stands, its life and its scans,
    // its area (efr kind 3: radius, the most it takes, whom), what it is, and the players holding a copy
    // of its buff (by their buff's token).
    struct SkillObject {
        std::uint32_t entity = 0, owner = 0;
        const assets::SkillRow* skill = nullptr;
        game::proto::WorldPosition at;
        Clock::time_point until, nextScan, nextPulse;
        float radius = 0, link = 0;
        std::size_t most = 0;
        std::uint32_t mask = 0;
        int pulseMs = 0;
        bool attack = false, trap = false;
        std::vector<std::uint32_t> quest; // a quest trap's monsters ('qest': up to three reference ids)
        struct Member {
            std::uint32_t entity = 0, token = 0;
        };
        std::vector<Member> members;
    };
    struct PendingObject {
        Clock::time_point at;
        std::uint32_t owner = 0;
        const assets::SkillRow* skill = nullptr;
    };
    void EndObject(std::size_t index);
    void ScanObject(SkillObject& object, SkillCaster& owner, Clock::time_point now);
    std::vector<SkillObject> objects_;
    std::vector<PendingObject> pendingObjects_;
    // The DOTs' copies on monsters: by monster, their owners and skills.
    std::unordered_map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> monsterCopies_;

    SkillHost& host_;
    const assets::SkillTable* table_ = nullptr;
    std::unordered_map<int, const assets::SkillRow*> basicAttacks_;
    // The fortress hammer's swing (weapon kind 16): its race's row, SKILL_CH_ / SKILL_EU_FORTRESSWEAPON (skilleffect.txt
    // plays ANI_HAMMER from the sword's set or the one-handed sword's).
    const assets::SkillRow* hammerChina_ = nullptr;
    const assets::SkillRow* hammerEurope_ = nullptr;
    std::mt19937 random_;
    std::uint32_t nextBuffToken_ = 1;
};

} // namespace sr::server
