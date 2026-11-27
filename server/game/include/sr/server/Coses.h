#pragma once

// The players' COS on the game server: rides, transports and pets.
//
// - A ride's or a transport's scroll (TID 3/3/3/2) used from the bag summons the COS its Desc1 names (by the
//   level ladder of its Desc2, game::CosOfScroll) beside its owner, an entity of the world (World::AddCos); the
//   scroll is used up. Refused (the original's texts): dead, in a dungeon, within 20 s of a fight, with one of the
//   same place out already, below the scroll's level.
// - A ride (kind 1) is boarded at once and is gone when its rider gets off (getting off is its end); a transport
//   (kind 2) is boarded and left through Ride, and stays (it steps aside when left). A rider goes at its mount's speeds
//   (characterdata cols 46, 47) and its mount walks every leg it walks (World::Carry); everyone who sees the rider
//   hears CosRiding, and so do newcomers (after the rider's appearance). Boarding is refused within 20 s of a fight.
// - A transport is summoned and boarded only in a trader's or a thief's suit, a thief's not in a town (the 1.188
//   client's UIIT_MSG_STRGERR_YOU_CANT_ACTIVATE_CART, UIIT_MSG_CMSERR_HUNTER_CANT_RIDE_CART,
//   UIIT_MSG_STRGERR_ROBBER_CANT_SUMMON_CART_IN_TOWN). Left standing it follows at its run pace, and its owner may not
//   walk farther than 100 m from it (UIIT_MSG_COSERR_TOO_FAR_FROM_TRADECART). Its trade bag comes with the jobs.
// - A pet (an attack pet, kind 3; a grab pet, kind 4) comes from its summoning item (TID 3/2/1/x), which stays in
//   the bag: the item's _Items.Data names its _CharCOS row (made at the first summon, its level-1 row, full HP, an
//   attack pet's HGP full, a grab pet's rental from then on). Refused as a scroll's COS is (but not in a dungeon), and
//   when it is dead, its rental is over, or it is above its owner's level. Out, its row is Summoned; Unsummon
//   (CosDismiss) saves it Inactive with its HP. One attack pet and one grab pet at a time, beside a ride or transport.
//   An attack pet's fight, HGP and revival come next.
// - A grab pet has a bag (_InvCOS): 28 places (game::kGrabPetBag) of its characterdata row's _RefObjChar.InventorySize
//   allocated (140 when the reference tables are not in the database), 28 more while a watch's job runs
//   (UseMallItemOnPet). While its grabbing is on (its settings, _CharCOS.PetOption,
//   proto::kGrab*) it goes for the nearest ground item its owner may take within kGrabRange of its owner, of the kinds
//   set (only those dropped for its owner unless "all items"), and picks it up: gold to its owner, an item into its bag
//   (onto a stack of it with room, else the first empty place). Its bag full, grabbing turns off
//   (UIIT_MSG_COSPETERR_CANT_PICKITEM1). Items move between its bag and its owner's inventory (CosBagMove; not a pet's
//   summoning item into it, UIIT_MSG_COSPETERR_CANT_MOVETO_SUMITEM), both saved together (SaveCosItems).
// - Unridden, a COS keeps near its owner: farther than game::kCosFollowFar it walks back towards it (Follow: at once).
// - A ride and a transport are _CharCOS rows from their summon (the original's _AddNewCOS) and come along on their
//   owner's teleports (made again beside him, the one ridden boarded again; across game servers by their rows). A ride
//   ends (its row deleted) when its rider gets off it, logs out or dies on it.
//   A transport is a _CharCOS row from its summon (its bag _InvCOS, every change written in its owner's order of saves
//   with his gold: a purchase, a sale, a pick-up), as the original's: its owner's leaving (a logout, a dropped
//   connection, a move to another game server) despawns it and keeps the row, and his next entry makes it again beside
//   him with its goods, ridden again if it was (Entered); only its death and its owner's sending it away delete the row
//   (its goods on the ground). A pet goes back into its item on its owner's death or leaving
//   (Inactive), and comes out again after a teleport or a move to another game server (its row stays Summoned: the
//   arriving server summons the pets of the items it finds Summoned, Entered). While it is out its item stays in the
//   bag (KeepsItem: the original's UIIT_MSG_COSPETERR_CANT_TRADE, _CANT_MOVETO_CHEST2, _CANT_SELL_ACTIVATED_COSITEM);
//   should it leave all the same, the pet goes back into it.
// - A grab pet's rental over: out, it goes back into its item; its row is Dead (the 1.188 item tooltip's
//   UIIT_STT_COSNEWUI_ITEM_EXPIRERENT) and it is never summoned again (UIIT_MSG_COSPETERR_CANT_USEITEM, the client's own
//   refusal for a dead pet's item). Its owner's client hears each pet item's row (CosPetItems).
// - A pet is named once (CosRename; the original's _COS_Naming): a name the character names' rules allow, no other pet's.
// - A dead pet comes back through an item dropped on its item (CosUseOnPet, game::PetTreatment): a grab pet's rental
//   renewed by a Clock of Reincarnation (Param1 minutes from now), an attack pet revived by a Grass of Life (ours: with
//   its full HP; the original's amount is not known yet). Its row is Inactive again, summoned as before; one item is
//   used.
// - An attack pet fights: told to attack a monster (CosCommand Attack), taking on its owner's
//   targets, fighting back and defending its owner when offensive; defensive, only on orders. It swings as a monster does (the first
//   of its characterdata skills that hits, the skill's time between swings) with its row's numbers (as a monster's,
//   game::MonsterCombatStats), each hit rolled by the original's formula (game::RollHit); monsters fight it as they
//   fight players (Fighters). Its damage counts as its owner's; it gains experience as its owner gets a kill's
//   (GainExperience), rising through its levels' rows. At no health it is dead: its row Dead, back into its item, its
//   experience less its death penalty (game::PetDeathPenalty). The pets' HP recovery kit heals it (HealPet).
// - A COS standing out of a fight gets back 1.6 % of its most HP every 4 s (the NPCs' natural recovery, Recover).
//
// A COS and its owner are always on the same game server. The game server is its host; the database work is the
// host's (CosLoad / CosCreate / CosSave), its answers back on the server's thread.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <random>

#include "sr/game/Combat.h"
#include "sr/game/Cos.h"
#include "sr/game/CosProtocol.h"
#include "sr/game/States.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/Monsters.h"
#include "sr/server/Store.h"

namespace sr::assets {
class CharacterTable;
class ItemTable;
class SkillTable;
struct ItemRow;
struct SkillRow;
} // namespace sr::assets
namespace sr::game {
struct ItemInstance;
class Inventory;
} // namespace sr::game

namespace sr::server {

class World;

class CosHost {
public:
    virtual ~CosHost() = default;
    // A player in this server's world as its COS needs it (none: not a player here).
    struct Owner {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::string name;
        int level = 1;
        bool alive = true;
        bool free = true;    // in no dealing (a stall, an exchange)
        bool fought = false; // in a fight within game::kCosAfterFightSeconds
        std::uint8_t job = 0; // the job's suit worn (its TID4): 1 trader, 2 thief, 3 hunter; 0 none
        bool inTown = false;   // a region whose battlefield byte is 0 (_RefRegion.IsBattleField): a thief summons no transport there
        bool murderer = false; // PvP state 2: his pets' deaths take the murderer's numbers
        // The original's summon and boarding refusals: its logout counting down, on its way elsewhere (a move, an entry),
        // a free-PvP state (changing, or on), hidden (stealth, invisibility), in a Battle Arena match, sitting or
        // getting up or down.
        bool loggingOut = false, travelling = false, freePvpChanging = false, freePvp = false, hidden = false, inArena = false, sitting = false;
    };
    virtual std::optional<Owner> CosOwner(std::uint32_t entity) = 0;
    virtual void CosSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // To everyone who sees `entity`, itself too.
    virtual void CosBroadcast(std::uint32_t entity, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // The player got on or off a mount (its own state: what riders may not do).
    virtual void CosRiding(std::uint32_t owner, bool riding) = 0;
    virtual World& CosWorld() = 0;
    virtual const assets::CharacterTable* CosCharacters() const = 0;
    // A pet's row read (none: no such row), made (its id: 0 failed; the item `serial` linked to it, in the database and
    // in the owner's bag), saved. `done` runs on the server's thread.
    virtual void CosLoad(std::int64_t id, std::function<void(std::optional<CosRow>)> done) = 0;
    virtual void CosCreate(std::uint32_t owner, const CosRow& row, std::int64_t serial, std::function<void(std::int64_t id)> done) = 0;
    virtual void CosSave(const CosRow& row) = 0;
    // Whether `owner`'s bag still holds the item `serial` (a pet's summoning item).
    virtual bool CosHasItem(std::uint32_t owner, std::int64_t serial) = 0;
    // A pet's name: one the naming rules allow (a character's: game::NameRules), then given to its row once (NameCos);
    // `done` on the server's thread.
    virtual bool CosNameAllowed(std::string_view name) const = 0;
    // One of the stack at `place` of `owner`'s bag used up (told to its owner, saved).
    virtual void CosTakeOne(std::uint32_t owner, std::uint8_t place) = 0;

    // --- An attack pet's fight ---
    virtual const assets::SkillTable* CosSkills() const = 0;
    // The level table (what each level needs to the next; index the level), the characters'.
    virtual std::span<const std::uint64_t> CosLevels() const = 0;
    // A level's monster experience (leveldata GUST_Mob_Exp; index the level): a pet's death penalty's cap.
    virtual std::span<const std::uint32_t> CosMonsterExperience() const = 0;
    // A living monster it may fight: its numbers, health and place (none: none, or dead).
    struct Foe {
        const game::CombatStats* stats = nullptr;
        std::uint32_t hp = 0;
        game::proto::WorldPosition position;
        // How it meets states (an imbue's on the pet's hits; none: none are rolled).
        std::optional<Monsters::Target> target;
    };
    virtual std::optional<Foe> CosFoe(std::uint32_t monster) = 0;
    // `pet`'s hit landed on `monster`: its health less the damage, then the states it gave (`affliction`, null: none),
    // everyone who sees it told (`imbue`: the imbue skill it carried, 0 none), a kill shared (the pet's damage its owner's).
    virtual void CosStrike(std::uint32_t pet, std::uint32_t monster, const game::HitResult& hit, const Monsters::Affliction* affliction,
                           std::uint32_t imbue) = 0;
    // The living monsters within `radius` of a point (an imbue's jump).
    virtual std::vector<std::uint32_t> CosMonstersNear(const game::proto::WorldPosition& at, float radius) {
        (void)at;
        (void)radius;
        return {};
    }
    virtual void CosName(std::int64_t id, const std::string& name, std::function<void(CosNaming)> done) = 0;

    // --- A grab pet's picking up and bag ---
    // The ground items within `range` of `owner` it may take now (as its own pick-up decides: not kept for another),
    // with what kind each is (proto::kGrabGold / kGrabEquipment / kGrabOther) and whether it was dropped for it.
    struct GroundSeen {
        std::uint32_t entity = 0;
        game::proto::WorldPosition position;
        std::uint32_t kind = 0;
        bool mine = false;
    };
    virtual std::vector<GroundSeen> CosGroundNear(std::uint32_t owner, float range) = 0;
    // `pet` picks up the ground item `ground` for `owner`: gold to the owner; an item handed to `put` (with an id of
    // its own) to go into the pet's bag. Taken (gone from the ground, the pet's bend shown), Gone, or NoRoom (`put`
    // refused it: it stays).
    enum class Picked : std::uint8_t { Taken, Gone, NoRoom };
    virtual Picked CosPickUp(std::uint32_t owner, std::uint32_t pet, std::uint32_t ground, const std::function<bool(game::ItemInstance&)>& put) = 0;
    virtual const assets::ItemTable* CosItems() const = 0;
    // The owner's inventory (moves between it and a pet's bag), the places of it that changed told to the owner
    // (ItemMoved), an item as it is stored made an item of the game.
    virtual game::Inventory* CosInventory(std::uint32_t owner) = 0;
    virtual void CosInventoryChanged(std::uint32_t owner, const std::vector<std::uint8_t>& touched) = 0;
    virtual game::ItemInstance CosItemOf(const StoredItem& stored) = 0;
    // A pet's bag saved: its places `bagTouched` as `bag` has them, with the owner's inventory places `inventoryTouched`
    // (and its saves still waiting), in one transaction after everything before it (SaveCosItems).
    virtual void CosSaveBag(std::uint32_t owner, std::int64_t cos, const std::map<std::uint8_t, game::ItemInstance>& bag,
                            const std::vector<std::uint8_t>& bagTouched, const std::vector<std::uint8_t>& inventoryTouched) = 0;
    // The states a COS is in now (Afflict, their ends), for those who see it (proto::EntityStates).
    virtual void CosStates(std::uint32_t entity, std::vector<game::proto::StateRecord> states) {
        (void)entity;
        (void)states;
    }
    // A transport's trade bag for an owner of `level` (the original's: it grows with the owner's level, at most its row's
    // _RefObjChar.InventorySize; 0: none).
    virtual int CosTransportBagSize(std::uint32_t model, int level) const {
        (void)model;
        (void)level;
        return 0;
    }
    // `owner`'s transport went (dismissed, dead, its owner gone): its goods with it (their drop
    // comes with step 7).
    virtual void CosTransportGone(std::uint32_t owner) { (void)owner; }
    // A sale at an NPC (CosSell): the NPC within reach of the owner; the price paid to it (its gold told and saved).
    virtual bool CosNpcNear(std::uint32_t owner, std::uint32_t npc) const { (void)owner, (void)npc; return false; }
    virtual void CosSellPay(std::uint32_t owner, std::uint64_t price) { (void)owner, (void)price; }
    // `owner`'s transport, kept in its row through his leaving, is out again (Entered): its goods count again.
    virtual void CosTransportBack(std::uint32_t owner) { (void)owner; }
    // A transport summoned: its trade scale's rule told to its owner (the deal window's), nothing else.
    virtual void CosTransportSummoned(std::uint32_t owner) { (void)owner; }
    // A transport killed: every slot of its bag dropped whole where it fell, its goods with no owner's mark.
    virtual void CosCargoDropped(std::uint32_t owner, const game::proto::WorldPosition& at, std::map<std::uint8_t, game::ItemInstance> bag) {
        (void)owner;
        (void)at;
        (void)bag;
    }
    // --- A transport's row (_CharCOS and its bag _InvCOS), all in its owner's character's order of saves ---
    // Made at its summon (`done` with its id, 0: failed); its older rows (`older`: the transports' models; kept and never
    // brought back) gone first with their goods.
    virtual void CosCartCreate(std::int64_t character, const CosRow& row, std::vector<std::uint32_t> older, std::function<void(std::int64_t)> done) {
        (void)character;
        (void)row;
        (void)older;
        done(0);
    }
    // Its bag changed: written as the owner's transport holds it at the save's turn, with his gold (one transaction).
    virtual void CosCartSave(std::int64_t character) { (void)character; }
    // It went with its row kept (its owner left, travelled without it, died on it): its health, whether it was ridden and
    // its bag as it went (written with them: a save asked before may find it gone).
    virtual void CosCartRelease(std::int64_t character, std::int64_t row, std::uint32_t hp, bool ridden, const std::map<std::uint8_t, game::ItemInstance>& bag) {
        (void)character;
        (void)row;
        (void)hp;
        (void)ridden;
        (void)bag;
    }
    // It went for good (killed, sent away): its row and bag deleted.
    virtual void CosCartDelete(std::int64_t character, std::int64_t row) {
        (void)character;
        (void)row;
    }
    // The character's transports kept (rows of these models, newest first, with their bags).
    virtual void CosCartLoad(std::int64_t character, std::vector<std::uint32_t> models, std::function<void(std::vector<CosRow>)> done) {
        (void)character;
        (void)models;
        done({});
    }
    // A ride, transport or grab pet killed (`killer` the killing hit's attacker): its killers credited as a monster's
    // (the original's: its row's experience by the damage's shares and a party's, a murderer's PK relief, the quests'
    // kill event). `damageBy` its damagers in order of the first hit.
    virtual void CosKilled(std::uint32_t cos, std::uint32_t model, const std::vector<std::pair<std::uint32_t, std::uint64_t>>& damageBy, std::uint32_t killer) {
        (void)cos;
        (void)model;
        (void)damageBy;
        (void)killer;
    }
    // A town region (`_RefRegion.IsBattleField` 0): a thief's transport stays out of them.
    virtual bool CosTown(std::uint16_t region) const {
        (void)region;
        return false;
    }

    // --- The mall's items on a pet ---
    // The wall clock in Unix seconds: the pets' timed jobs end on it (tests move it).
    virtual std::int64_t CosNow() const;
    // A pet's timed job written (replacing one of its category and job: _TimedJobForPet), or taken away.
    virtual void CosSavePetJob(std::int64_t cos, const PetJobRow& job) {
        (void)cos;
        (void)job;
    }
    virtual void CosEndPetJob(std::int64_t cos, std::uint8_t category, std::uint32_t job) {
        (void)cos;
        (void)category;
        (void)job;
    }
};

class Coses {
public:
    using Clock = std::chrono::steady_clock;

    explicit Coses(CosHost& host);

    enum class Use : std::uint8_t { NotCos, Summoned, Kept, Refused };
    // An item used from the bag by `owner`: Summoned (a scroll's COS: the host takes one), Kept (a pet's item: it stays;
    // its pet comes out once its row is read or made), Refused (its owner heard why) or NotCos.
    Use UseItem(std::uint32_t owner, const assets::ItemRow& item, const game::ItemInstance& instance, Clock::time_point now);
    // A request of the player `owner` (false: it does not decode).
    bool Handle(std::uint32_t owner, game::proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now);
    // The owner's ride or transport sold back at an NPC (CosSell).
    void Sell(std::uint32_t owner, std::uint32_t npc);
    // The unridden follow their owners; the pets coming back come out once their owner stands.
    void Tick(Clock::time_point now);
    // Every COS of `owner` sent away: its death or leaving (`comesBack` false: its pets Inactive), a teleport or a move
    // to another game server (true: its pets stay Summoned and come out again where it arrives). `keepTransport`: its
    // ride and transport stay out (a teleport on this server: BringTransport). Its transport's row stays (it comes back at
    // the next entry); its ride's row stays only on a move to another game server (`moving`), else it is its end.
    void DismissAll(std::uint32_t owner, bool comesBack = false, bool keepTransport = false, bool moving = false);
    // Every COS of `owner` of that kind sent away, as its owner's unsummon (its row Inactive); how many went.
    int DismissKind(std::uint32_t owner, game::CosKind kind);
    // Its owner died: as the original's, only the COS it rides goes (its ride slot; a ridden transport too, its goods
    // not dropped); its pets and a transport it walks beside stay out.
    void OwnerDied(std::uint32_t owner);
    // `owner`'s ride or transport (kept through DismissAll) brought beside `at`, where its owner arrives (the thieves'
    // town scroll, which works with a transport out:; a gate's or a ferry's link that allows one); told
    // again and boarded again, if it was, once its owner stands there (Tick).
    void BringTransport(std::uint32_t owner, const game::proto::WorldPosition& at);
    // A thief riding a transport may not take it from a field into a town: refused, its owner told why
    // (UIIT_MSG_JSERR_ROBBER_CANT_ENTER_TOWN_WITH_CART). On foot they may go; the transport stops at the town's edge
    // (and the leash holds them).
    bool KeptOutOfTown(std::uint32_t owner, const game::proto::WorldPosition& target);
    // A pet's row and the summoning item in its owner's bag that names it.
    struct PetLink {
        std::int64_t id = 0, serial = 0;
    };
    // `owner` entered the world with these pets linked from its bag (its summoning items' _Items.Data): those
    // Summoned come out once it stands; so does its transport kept (its row), with its goods.
    void Entered(std::uint32_t owner, std::vector<PetLink> pets);
    // What carries `owner` (0: it is on foot).
    std::uint32_t MountOf(std::uint32_t owner) const;
    // A ride or a transport of `owner`'s is out (the ferries' rule).
    bool TransportOut(std::uint32_t owner) const;
    // The trader's transport and its trade bag (none: no transport out): the bag's places 0..size-1.
    struct Cart {
        std::uint32_t entity = 0;
        std::uint32_t model = 0;
        std::map<std::uint8_t, game::ItemInstance>* bag = nullptr;
        int size = 0;
        std::int64_t row = 0; // its _CharCOS row (0: not made yet)
    };
    std::optional<Cart> TransportCart(std::uint32_t owner);
    // Its transport's bag holds anything.
    bool CarriesGoods(std::uint32_t owner) const;
    // Its bag changed (a purchase, a sale): told to its owner (CosBag).
    void CartChanged(std::uint32_t entity);
    // Its owner's level rose: its transport's bag grows with it (the original's 0x30C9 kind 2; ours the bag told again).
    void OwnerLevelChanged(std::uint32_t owner);
    // A walk of `owner` to `target` held by its transport left standing (the original's tether, checked on every step):
    // a walk towards it goes; one leading farther than game::kCosLeash from it goes only to that line (`target` moved
    // there, its owner told UIIT_MSG_COSERR_TOO_FAR_FROM_TRADECART); one starting beyond the line and leading farther does
    // not go (false). `toward` (a walk that way, a click on the sky) becomes a walk to the line when it is cut.
    bool Tether(std::uint32_t owner, game::proto::WorldPosition& target, bool& toward);
    // A COS's owner (0: `entity` is no COS).
    std::uint32_t OwnerOf(std::uint32_t entity) const;
    // A COS's level (0: not a COS).
    int LevelOf(std::uint32_t entity) const;
    // A COS's numbers and health as a target (null / 0: no COS, or one with none: a ride); whether its owner rides it.
    const game::CombatStats* StatsOf(std::uint32_t entity) const;
    std::uint32_t HpOf(std::uint32_t entity) const;
    // The original's hold on a summoned pet's item: while its pet is out, the item `serial` of `owner`'s bag is not
    // dropped, sold, stored, traded or put in a stall. True: it is such an item (and `owner` heard `text`).
    bool KeepsItem(std::uint32_t owner, std::int64_t serial, const char* text);
    // The same, told nobody: `owner`'s item `serial` is a pet out.
    bool Out(std::uint32_t owner, std::int64_t serial) const;

    // --- An attack pet's fight ---
    // The attack pets out and alive, for the monsters to fight as they fight players (Monsters::Update).
    void Fighters(std::vector<Monsters::Player>& out) const;
    // A monster's hit on a pet (its entity): its health less the damage, everyone who sees it told; at none it is
    // dead (its row Dead, back into its item). Alive, it fights back.
    void Hurt(std::uint32_t pet, std::uint32_t monster, const game::HitResult& hit);
    // States a monster's hit rolled on a COS (a transport, a pet: a character as its owner is; a ride takes none), as a monster's
    // are kept: put on it and told to those who see it; frozen, asleep or stunned it stands, frostbite and slow slow
    // it, a burn or a poison takes its health at its ticks (Tick). False: none took. Its state defence: a monster's of
    // its level (StateDefenceOf; no resistances of its own: the COS rows carry none).
    bool Afflict(std::uint32_t entity, const std::vector<game::ActiveState>& states, Clock::time_point now);
    std::optional<game::StateDefence> StateDefenceOf(std::uint32_t entity) const;
    int SusceptibilityOf(std::uint32_t entity) const;
    // Its owner hit `monster` (an aggressive pet joins in), or was hit by it (the pet defends it).
    void OwnerFights(std::uint32_t owner, std::uint32_t monster);
    void OwnerHurt(std::uint32_t owner, std::uint32_t monster);
    // Its owner's attack pet's level (0: none out).
    int AttackPetLevel(std::uint32_t owner) const;

    // --- The guild's mercenaries (game/GuildMercenary.h; the shard's switch guild-mercenaries) ---
    // `count` soldiers of `shape` out beside `owner` for `minutes`, each fighting as an offensive attack pet does (its
    // owner's foes, its own attackers; monsters only: ours), its numbers raised by the guild's `abilities`. Their entities.
    std::vector<std::uint32_t> SummonSoldiers(std::uint32_t owner, const assets::ObjectRow& shape, int count, int minutes, std::uint8_t abilities,
                                              Clock::time_point now);
    // How many of `owner`'s soldiers are out.
    int SoldiersOut(std::uint32_t owner) const;
    // Every soldier of `owner` sent away (the original's 0x725A, its unsummon on any of them: no checks). The count.
    int ReleaseSoldiers(std::uint32_t owner);
    // The guild's abilities changed: `owner`'s soldiers' numbers again from their rows (their health kept as a share).
    void SoldierAbilities(std::uint32_t owner, std::uint8_t abilities);
    // Experience for `owner`'s attack pet (the original's award, called as its owner gets a
    // kill's: the game server works it out for the pet's level): its levels and rows (game::GrowPet), its owner told.
    void GainExperience(std::uint32_t owner, std::uint64_t experience);
    // The pets' HP recovery kit used by `owner` (game::IsPetHpPotion), its amount worked out for its owner: on its
    // living COS with the least of its health left (ours: the original's client names the COS, ours sends only the
    // item), at once, everyone who sees it told. False: none to give it to (its owner heard why). With a kind, only a
    // COS of that kind (game::CosKind::Transport: a ride or a transport; the auto potion's rows of ours).
    // The COS healed (its entity; 0: none of its owner's alive).
    std::uint32_t HealPet(std::uint32_t owner, std::uint32_t hp, game::CosKind kind = game::CosKind::None);
    // The health and most health of `owner`'s living COS of that kind (as HealPet takes it; none: none out).
    std::optional<std::pair<std::uint32_t, std::uint32_t>> Health(std::uint32_t owner, game::CosKind kind) const;
    std::size_t Out() const { return coses_.size(); }

    // --- The mall's items on a pet (game::PetItemJobs) ---
    // `stack` (an item of 3/3/13/15: a watch, a pet skill) used by `owner` on the pet of its summoning item `petItem`
    // (the original's special consumable, case 15): its answer (0 done, its jobs started on
    // the pet; else the code: 0x18B8 no such pet out, 0x1894 one of the same running, 3 the wrong pet, 2 the rest) and
    // how many of the stack it takes (1; a grab pet's job its minutes).
    struct PetItemUse {
        std::uint16_t code = 0;
        int taken = 0;
    };
    PetItemUse UseMallItemOnPet(std::uint32_t owner, const assets::ItemRow& item, const game::ItemInstance& stack, const game::ItemInstance& petItem);
    // A pet's running jobs as tests and tools see them: its bag's places now, the skills on it.
    struct PetJobsView {
        int bagSize = 0;
        std::vector<std::uint32_t> skills;
    };
    std::optional<PetJobsView> JobsOf(std::uint32_t entity) const;

private:
    struct Cos {
        std::uint32_t entity = 0, owner = 0;
        std::uint32_t model = 0, item = 0;
        game::CosKind kind = game::CosKind::None;
        std::uint32_t hp = 0, maxHp = 0;
        float walkSpeed = 0, runSpeed = 0;
        bool ridden = false;
        // A pet's: its row (id 0: a scroll's COS) and its summoning item's serial.
        CosRow row;
        std::int64_t serial = 0;
        // A grab pet's bag (by place, 0..bagSize-1) and the ground item it is going for (0: none).
        std::map<std::uint8_t, game::ItemInstance> bag;
        int bagSize = 0;
        std::uint32_t picking = 0;
        // An attack pet's fight: its numbers (its row's, as a monster's), its attack (the first of its skills that hits),
        // the monster it fights (0: none), when it swings next and the hits of its swing still to land, its stance.
        game::CombatStats stats;
        const assets::SkillRow* skill = nullptr;
        std::uint32_t target = 0;
        Clock::time_point nextSwing{};
        struct Pending {
            Clock::time_point at;
            std::uint32_t target = 0;
            game::AttackSpec attack;
            float reach = 0;
        };
        std::vector<Pending> pending;
        bool aggressive = true;
        game::ChanceRolls rolls;
        // The states on it (Afflict) and the speed they leave it (percent; 100: none).
        game::StateSet states;
        int speedPercent = 100;
        // Its hunger: its row's numbers (stats halved from them when hungry), its HGP eaten a second, the fraction left.
        game::CombatStats baseStats;
        double hgpRate = 0, hgpCarry = 0;
        // Its natural recovery's next tick ({}: not started).
        Clock::time_point nextRegen{};
        // Its timed jobs running (the mall's items): a skill on it (category 0; an
        // attack pet's imbue and its buff's changes), a composite item's (category 5: a grab pet's bag places more).
        struct Job {
            PetJobRow row;
            const assets::SkillRow* skill = nullptr;
        };
        std::vector<Job> jobs;
        int bagMost = 0; // a grab pet's places allocated (its row's InventorySize): the bag never grows past them
        // A transport's: its _CharCOS row (0: not made yet) and its owner's character (its saves' order).
        std::int64_t cart = 0, character = 0;
        // Who hurt it and by how much, in order of the first hit (its killers' credit, CosKilled).
        std::vector<std::pair<std::uint32_t, std::uint64_t>> damageBy;
        // A guild's soldier: when its minutes are up, its guild's abilities on its numbers.
        Clock::time_point until{};
        std::uint8_t abilities = 0;
    };
    // A soldier's numbers and attack from its row and its guild's abilities (no hunger, no pet row).
    void ArmSoldier(Cos& c, const assets::ObjectRow& shape);
    // `owner`'s soldiers not in a fight take `monster`.
    void EngageSoldiers(std::uint32_t owner, std::uint32_t monster);
    // Its states changed: its stand and its speed from them, those who see it told.
    void StatesChanged(Cos& c, Clock::time_point now);
    // The states' ticks due (a burn's, a poison's: its health) and their ends, each update.
    void TickStates(Clock::time_point now);
    // A job of the pet's started (`tell`: its owner told the bag's new size) or ended (its row taken away, its owner told).
    void StartJob(Cos& c, const PetJobRow& row, bool tell);
    void EndJob(Cos& c, std::size_t index);
    // The pet's jobs over by now ended (each second, with the follow).
    void EndJobs(Cos& c, std::int64_t unix);
    // The imbue among its skills running (none: none).
    const assets::SkillRow* ImbueOf(const Cos& c) const;
    // An attack pet's hit on `monster` with its imbue: the imbue's states rolled against it.
    Monsters::Affliction ImbueAffliction(Cos& c, const assets::SkillRow& imbue, const CosHost::Foe& foe, const game::HitResult& hit, Clock::time_point now);
    // A COS's natural recovery, each kRegenPeriod (Tick).
    void Recover(Cos& c, Clock::time_point now);
    // Its HGP set (`tell`: saved, its owner told), its numbers halved or not by it.
    void SetHgp(Cos& c, int hgp, bool tell);
    void Hunger(Cos& c);
    void Eat(Cos& c, double seconds);
    // An attack pet's numbers and attack from its row (its level's).
    void Arm(Cos& c, const assets::ObjectRow& shape);
    void Fight(Cos& c, Clock::time_point now);
    void Engage(Cos& c, std::uint32_t monster);
    Cos* AttackPetOf(std::uint32_t owner);
    game::proto::CosSummoned SummonedOf(const Cos& c) const;
    bool Grab(Cos& c);      // a grab pet's picking up, each follow tick (true: it is busy with an item, no follow)
    void SendBag(const Cos& c);
    void SaveBag(Cos& c, const std::vector<std::uint8_t>& bagTouched, const std::vector<std::uint8_t>& inventoryTouched = {});
    // Where an item of `refId` x `count` goes in a pet's bag: a stack of it with room, else the first empty place (-1: none).
    int BagPlaceFor(const Cos& c, std::uint32_t refId, int count) const;
    void MoveInBag(std::uint32_t owner, const game::proto::CosBagMove& m);
    void SetOption(Cos& c, std::uint32_t option);
    Use UsePet(std::uint32_t owner, const assets::ItemRow& item, const game::ItemInstance& instance, game::CosKind kind);
    // A pet's row read or made: it comes out (`asked`: its owner used its item, and hears why it cannot).
    void PetReady(std::uint32_t owner, CosRow row, std::int64_t serial, std::uint32_t item, bool asked);
    // Out beside its owner (0: no ground there): a scroll's COS or a pet, by its characterdata row.
    std::uint32_t Place(std::uint32_t owner, const assets::ObjectRow& row, const std::string& name = {});
    bool PlaceTaken(std::uint32_t owner, game::CosKind kind) const;
    void Refuse(std::uint32_t owner, const char* text, std::string arg = {});
    bool Ride(std::uint32_t owner, std::uint32_t entity, bool ride, Clock::time_point now);
    // Sent away; a pet's row saved Summoned (`comesBack`), Dead (`dead`: its rental over) or Inactive. A transport's
    // cargo drops where it stands and its row goes only when it is killed (`dead`) or terminated (`terminate`: its
    // owner sends it away); every other end keeps its row and goods for its owner's next entry (as the original's).
    void Dismiss(std::uint32_t entity, bool comesBack = false, bool dead = false, bool terminate = false, bool moving = false);
    // The rides' and transports' models (characterdata rows of those kinds: their rows are kept).
    const std::vector<std::uint32_t>& TransportModels();
    std::vector<std::uint32_t> transportModels_;
    bool transportModelsRead_ = false;
    // A kept transport made again beside its owner, standing (its row's bag, its health, ridden again if it was).
    void RestoreCart(std::uint32_t owner, const CosRow& row, Clock::time_point now);
    // A pet's row as its item shows it, to its owner (CosPetItems).
    void SendPetItem(std::uint32_t owner, const CosRow& row);
    // A grab pet's rental run out: its row Dead (true: it was not yet; save it).
    static bool Expire(CosRow& row);
    void Rename(std::uint32_t owner, const game::proto::CosRename& m);
    // A Clock of Reincarnation or a Grass of Life used on a pet's item (CosUseOnPet).
    void UseOnPet(std::uint32_t owner, const game::proto::CosUseOnPet& m);
    // Back towards its owner when it is farther than `far` (Follow: 0).
    bool Follow(Cos& c, double far);
    template <typename T>
    void Send(std::uint32_t owner, const T& message);

    CosHost& host_;
    std::unordered_map<std::uint32_t, Cos> coses_;                       // by its entity
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> owned_; // an owner's, by its entity
    std::unordered_set<std::uint32_t> loading_;                           // owners whose pet's row is being read or made
    std::unordered_map<std::uint32_t, std::vector<PetLink>> returning_;    // owners' pets to bring out once they stand
    // Owners' rides and transports kept through a travel (DismissAll's keepTransport): told again (CosSummoned, the bag)
    // once their owner stands at the other end, the one ridden boarded again (the user's report 2026-11-14: a ferry's
    // transport kept by the server but forgotten by the client, which clears its own at WorldEntered; a ride gone).
    struct Travelling {
        std::uint32_t entity = 0;
        bool ridden = false;
    };
    struct Travel {
        std::vector<Travelling> list;
    };
    std::unordered_map<std::uint32_t, Travel> travelling_;
    // Owners whose kept transports are being read (Entered), and those read, to make once their owner stands (Tick).
    std::unordered_set<std::uint32_t> cartLoading_;
    std::unordered_map<std::uint32_t, std::vector<CosRow>> carts_;
    // Owners walking to their ride or transport to board it (Ride from farther than the reach): boarded on arrival.
    std::unordered_map<std::uint32_t, std::uint32_t> boarding_;
    void Boardings(Clock::time_point now);
    void Arrived(); // Tick: those whose owner stands at the other end, told again
    Clock::time_point nextFollow_{}, lastFollow_{};
    std::mt19937 random_{0x5EED0C05u};
};

} // namespace sr::server
