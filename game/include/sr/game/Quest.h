#pragma once

// A quest as our servers hold it: what starts it, what it asks
// (its goals), what it gives, and what its NPCs say. The original's quests come from its quest script
// (server/common QuestScript.h, read as data) with their rewards from its database; a server owner's own
// quests are written in the same text format (one file per quest; an editor comes later). Both are kept in
// the database (sr_quests, the definition's text) and read by the game server alike.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sr::game {

// A text a quest shows: a key into the game's text tables (written @KEY) or the words themselves ("...").
struct QuestText {
    std::string key;     // SN_..., UIIT_...; empty when literal
    std::string literal; // the words, when no key
    bool Empty() const { return key.empty() && literal.empty(); }
    bool operator==(const QuestText&) const = default;
};
inline QuestText TextKey(std::string key) { return {std::move(key), {}}; }

enum class QuestGoalKind : std::uint8_t {
    Kill,       // kill a number of monsters (any of a list)
    Drop,       // gather an item monsters drop for the quest's holder, at a chance per monster
    Deliver,    // bring items to an NPC
    Talk,       // talk to an NPC
    KillPlayer, // kill players under conditions (the temple quests)
    Collect,    // a Forgotten World talisman collection, its theme complete
    Capture,    // a monster caught by trap brought to an NPC (the original's mission kind 8, CMissionCatchMonster; QuestDef::UseRule::capture)
    Record,     // ours: a record's count reached (`goal record NAME N [or NAME N]...`: the beta quests; QuestDef::records' names)
};
const char* QuestGoalName(QuestGoalKind k);

// The class of a monster a kill counts for (none listed: any).
enum class MonsterClass : std::uint8_t { Normal, Champion, Giant, Unique };

struct QuestGoal {
    QuestGoalKind kind = QuestGoalKind::Kill;
    std::uint32_t count = 1;           // kills, items, players
    std::vector<std::string> monsters; // Kill: the targets; Capture: the caught COS
    std::vector<MonsterClass> classes; // Kill: the classes counted (empty: any)
    std::string item;                  // Drop: the item gathered
    struct Source {
        std::string monster; // its codename, or `any` (every monster)
        double percent = 0;  // the chance a kill drops the item
        // The chance times the level scale of the killer against the monster (QuestLevelScale; the premium's
        // purification seeds).
        bool byLevel = false;
        bool operator==(const Source&) const = default;
    };
    std::vector<Source> sources; // Drop
    struct Items {
        std::string item;
        std::uint32_t count = 1;
        bool operator==(const Items&) const = default;
    };
    std::vector<Items> items; // Deliver: what is handed over
    // Deliver: what the NPC hands back at the delivery (the original's deliver mission's second list:
    // QNO_TQ_ORNAMENT_2's refined venom, QNO_CA_TREASURE_4's water), taken at the finish.
    std::vector<Items> received;
    std::string npc;          // Deliver, Talk: to whom; Drop, KillPlayer, Collect: where it is told (the original's)
    std::vector<std::string> alsoAt; // Talk: other NPCs any of whom takes it as well (the Beginner's Assistant's merchants)
    QuestText talk;           // Deliver, Talk: what the NPC says then
    QuestText text;           // the goal's line in the quest window (SN_CON_*)
    // A helper (the original's mission whose counting flag is 0, a gathering):
    // counted for the quest's own use (its drops stop at the count) but not shown, not needed to finish the quest and
    // not taken at the hand-in (QNO_CA_WAREHOUSE_1's earwigs, used up to search for the grass).
    bool hidden = false;
    // Kill: counted only for a holder in berserk (the hwan tests' ghosts: a kill counts only while the holder is in
    // berserk).
    bool berserk = false;
    // Drop: paid by the piece (the original's gathering paid by the piece): the item keeps dropping past the count
    // while under way, and the hand-in takes every piece held, paying each one this gold (the sum at most 10,000,000),
    // experience and skill experience, beside the quest's own reward (QNO_CH_PRIEST_1's lithography pieces: 125 gold
    // and 100 a piece).
    bool apiece = false;
    std::int64_t apieceGold = 0, apieceExp = 0, apieceSp = 0;
    // KillPlayer: the victims counted
    int minLevel = 0;
    std::int32_t region = 0; // 0 any
    std::vector<std::string> jobs; // trader, hunter, thief (empty: any)
    std::string theme;             // Collect
    // Record: the options (name, count), any one reached meets it; its progress the first one's (capped at its count).
    std::vector<std::pair<std::string, int>> records;
    bool operator==(const QuestGoal&) const = default;
};

struct QuestReward {
    std::uint64_t exp = 0, skillExp = 0, gold = 0;
    std::uint32_t sp = 0;        // skill points
    std::uint32_t inventory = 0; // places added to the bag
    std::uint32_t hwan = 0;      // the hwan (berserk) level given
    struct Item {
        std::string item;
        std::uint32_t count = 1;
        bool operator==(const Item&) const = default;
    };
    std::vector<Item> items;
    std::uint32_t choose = 0;  // the player picks this many of the items (0: all are given)
    bool byCountry = false;    // only the items of the player's country are offered
    bool shown = true;         // the reward window lists it (the original's IsView)
    bool bound = false;        // ours (`reward bound`): its items bound to the character (the beta quests' weapons)
    struct Points {
        std::uint32_t amount = 0;
        std::string name; // TEMPLE_ANUBIS, ...
        bool operator==(const Points&) const = default;
    };
    std::vector<Points> points;
    bool operator==(const QuestReward&) const = default;
};

// One thing an NPC says about a quest (the original's BASIC_MENUSTRING_* moments).
struct QuestTalk {
    std::string npc;
    std::string moment; // greeting, offer, accepted, denied, not-done, bag-full, done, done-now, middle, offer-again, ...
    QuestText text;
    bool operator==(const QuestTalk&) const = default;
};
// The moment's name for the original's BASIC_MENUSTRING_* (empty when unknown) and back.
std::string_view QuestMomentOf(std::string_view basicMenuString);
std::string_view BasicMenuStringOf(std::string_view moment);

struct QuestDef {
    std::string codename;
    std::uint32_t id = 0; // the original's number for its quests; ours above them
    int level = 0;        // the level the quest list shows
    QuestText name, rewardTitle, rewardText, guideNpc, guideCondition;

    // What starts it
    int minLevel = 1, maxLevel = 0;      // maxLevel 0: no upper bound
    std::vector<std::string> afterAll;   // every one of these finished first
    std::vector<std::string> afterOne;   // one of these finished first
    // `start after all Q*N`: Q finished at least N times (the original's count by each of the quests listed,
    // compared with its finish count, QNO_KT_ACCESSORY_2 after ACCESSORY_1 three times); absent: once.
    std::map<std::string, std::uint16_t> afterTimes;
    // `start never Q...`: each one never taken (its finish count 0 and its state 5, none; Hotan's accessory merchant's
    // chain closed to whoever began the smith's, and the reverse).
    std::vector<std::string> never;
    // `start before Q...`: offered only while none of these has been finished (its finish count
    // 0; Jangan's liquor supplies, QNO_CH_POTION_5 and _EUROPE_3, until the lottery's QNO_CH_LOTTERY_2).
    std::vector<std::string> before;
    // `start record NAME N [or NAME N]...` (ours, the beta quests): one of these counts reached. A NAME is a counted deed of
    // the character (sr_char_records: event-matches, roc, tomb-hardest, fortress-war), `finished` the different quests it
    // has finished (the beta ones, QBETA_*, not counted), or `finished:PREFIX` those whose codename starts with PREFIX.
    struct RecordNeed {
        std::vector<std::pair<std::string, int>> any;
        bool operator==(const RecordNeed&) const = default;
    };
    std::vector<RecordNeed> records;
    // Every one of these under way (the original's coded quests' third list: the offer asks
    // each one's state for 1, 7 or 8; QNO_EU_EASTEU_2 is taken while QNO_EU_EASTEU_1 runs).
    std::vector<std::string> during;
    // Taken only in a party that shares experience and items automatically (the original's start flag 0x10000 with
    // its NPC says the `not-party` talk otherwise: QNO_EU_GENERAL_2, QNO_EU_EASTEU_21, QNO_EU_IVY_3).
    bool party = false;
    std::string race;                    // china, europe (empty: any)
    std::vector<std::string> jobs;       // the job worn (empty: none needed)
    std::uint32_t hwan = 0;              // LuaSetHwanLevel, kept as read (the original's offer test asks no hwan level)
    std::string time;                    // day, night (empty: any)
    std::uint32_t quota = 0;             // starts allowed per day or night window, for all players together
    // A daily quest (the original's LuaSetRealTime): finished at its limit on an earlier local calendar day, its count goes
    // back to 0 at the next offer; at its limit its giver says `count-limit`.
    bool realClock = false;
    // Given by itself, without an NPC's offer, when the character reaches its start level (the original's
    // _RefCharDefault_Quest: the Beginner's Assistant). No finisher: it finishes at the NPC of its last goal.
    bool given = false;
    // Offered only while the character's premium package lets it be taken (its UQL1 part names the quest and has uses
    // left); each finish spends one of them.
    bool premium = false;
    std::uint32_t timerSeconds = 0;      // a countdown once started
    std::uint32_t repeat = 1;            // how often a character may finish it (0: without limit)
    bool giveUpLocks = false;            // given up, it is not offered again (the original's: questcontentsdata col 4 0)
    // Given up, its items stay in the bag (the Roc's prize quests' own abort, the mission
    // registers cleared, no item taken; the base abort's script takes them).
    bool giveUpKeepsItems = false;
    // Offered and taken only while the character holds one of each (the start's flag 4 and its list: the bag's count of
    // each above 0; the Roc's prize quests' Marks of Alliance and the Roc's spoils).
    std::vector<std::string> startHolding;
    // `start holding-one ITEM...`: one of them at least (the same test's second list,a count of the
    // held, none refusing; Jangan's lottery, any of the liquor merchants' six drinks).
    std::vector<std::string> startHoldingOne;
    // Finished: these quests' records cleared, as never done, so they may be taken again (the Roc's key chain:
    // QNO_RM_MEETROC_2's finish clears _1's; _6's clears _2's).
    std::vector<std::string> finishResets;
    std::vector<std::string> givers;     // who offers it
    std::vector<std::string> npcs;       // whose menus show it
    std::string finisher;                // who takes it back
    std::vector<QuestGoal::Items> startItems; // given on starting it
    // Needed at the hand-in besides the goals, taken there (the original's QuestNeedItem and its coded quests' own
    // checks: Hotan's bag asks two General's Tokens and 10,000 gold); short of them, the
    // finisher says the `missing` talk.
    std::vector<QuestGoal::Items> needItems;
    std::uint64_t needGold = 0;
    // `need record NAME N [or NAME N]...` (ours): a deed's count asked at the hand-in, not at the offer (the quest taken at
    // any time; short of it the finisher says `missing`); the names as `start record`'s (the beta quests).
    std::vector<RecordNeed> needRecords;
    std::vector<QuestGoal> goals;
    QuestReward reward;
    // An exchange of a gathered item for skill points, the player's pick of one row at a giver (the original's
    // QSP_ALL_POTION_3: _RefQuestRewardItems' type 4 rows). With
    // tiers the quest may be finished as soon as the smallest is held; the row picked takes its items and pays its SP.
    struct Tier {
        std::string item;
        std::uint32_t count = 0;
        std::uint32_t sp = 0;
        QuestText talk; // the row's words at the NPC
        bool operator==(const Tier&) const = default;
    };
    std::vector<Tier> tiers;
    std::vector<QuestTalk> talks;
    // A unique's death a holder witnesses (the original's UNIQUE_DEAD handlers, QNO_SD_MA_029..034): when that
    // monster dies, each player under way with the quest who stands in `region` (any: none given) and holds
    // `take` has one taken and one `give` put in the bag (the goal that gathers `give` counts it).
    struct UniqueDead {
        std::string monster;
        std::optional<std::uint16_t> region;
        std::string take, give;
        bool operator==(const UniqueDead&) const = default;
    };
    std::vector<UniqueDead> onUniqueDead;
    // A kill credited to a holder under way that its kill goal counted (or, with none naming the monster, any credited
    // kill of it): the original's coded quests' kills. In order: `need` held (else nothing), a free bag
    // place (else `full` said, nothing), under `cap` of `give` (else nothing); then one `give` given, one `take` taken and
    // every `clear` taken (a Spirit Lost Stone into an Added one per golem, under 100; QNO_TQ_ORNAMENT_4's calabash for
    // the blood one, the explosive stones cleared).
    struct KillRule {
        std::string monster;
        std::string need, take, give;
        std::vector<std::string> clear;
        std::uint32_t cap = 0; // 0: no cap
        QuestText full;        // empty: no bag place asked
        bool operator==(const KillRule&) const = default;
    };
    std::vector<KillRule> onKill;
    // Given again at its givers while it is under way and none is held (the original's talk handlers' "Receive keys"
    // rows:'s five Seal Keys, the traps, the ropes, the pickaxes); the giver says
    // `resupply`, or `bag-full` with no room.
    std::vector<QuestGoal::Items> resupply;
    // `raise MOB every S [radius A B]`: while under way, a MOB raised about a living holder outside a town every S seconds
    // (the original's quest timer, the record's countdown byte: QNO_RM_SLAVE2_2's timer raising a
    // Wing Tribe within 0..20 every 60 s, the ones its goal counts).
    struct Raise {
        std::string monster;
        std::uint32_t seconds = 60;
        float nearest = 0.0f, farthest = 20.0f;
        bool operator==(const Raise&) const = default;
    };
    std::vector<Raise> raises;
    // Given again once a game day (the coded quests' "come again tomorrow": the record's day against the game clock's;
    // EASTEU_5's): the day recorded when they are offered; the same day the giver says `resupply-spent`.
    // `resupplyFromStart`: the start's items count as the day's.
    bool resupplyDaily = false, resupplyFromStart = false;
    // `resupply-daily after`: its `resupply` handed out once a game day after it is finished, at its givers, who keep a row
    // for it (Jangan's liquor: _10 the day's given, _07 no room, _09 and the drinks).
    bool resupplyAfterFinish = false;
    // `talk-game NAME`: its giver's talk is a game of the class's own, played by the server with talk rows (QuestSays::rows):
    // `huhoan-drinks`, QNO_CH_BIGMAN_1's drinking game.
    std::string talkGame;
    // Every one of these held taken at the hand-in (a capture mission's traps and caught
    // item, a kind 4's items).
    std::vector<std::string> payTake;
    // One of its items used from the bag (the original's coded quests' item uses). Offered to the player's quests
    // under way: a rule whose checks pass takes the item (used up), else its talk is said and the use goes on to the
    // next quest (none taking it: refused 2). The original's ground search (and its kin):
    // at one of its places, under its cap, a free bag place; then `seconds` (its countdown, one tick a second) and the
    // roll (`chance` of `outOf`, the original's rand() % 101 against its bound; outOf 0: always) puts one `give` in the
    // bag (its gathering goal counts it), else `failed` is said.
    struct UseRule {
        std::string item;
        // The quest's states it is taken in, the original's numbers: 1 under way with no goal
        // progressed yet, 7 a goal progressed, 8 its goals met. Empty: 1 and 7. Another state, or a quest not under way:
        // refused with `refuse`.
        std::vector<std::uint8_t> statuses;
        bool firstTime = false;  // refused (`refuse`) once the quest was finished before
        bool oneAtATime = false; // refused while its own wait runs (the record's countdown byte, +4), saying `busy`
        QuestText busy;
        QuestText refuse;        // said when the quest's state refuses it (empty: nothing)
        struct Place {
            std::uint16_t region = 0;
            float x = 0, y = 0, z = 0, radius = 0; // within this 3D distance of the point (the original's check)
            bool operator==(const Place&) const = default;
        };
        std::vector<Place> at; // where it works, any of them (none: anywhere)
        QuestText away;        // said elsewhere
        std::string capItem;   // refused once `cap` of it are held (0: no cap)
        std::uint32_t cap = 0;
        QuestText capped;      // said then (empty: nothing)
        bool capFirst = false; // the cap asked before the place too (QNO_CA_WAREHOUSE_1, QNO_TQ_ORNAMENT_3: silently)
        QuestText full;        // said with no free bag place (empty: not asked)
        std::uint32_t seconds = 0;
        std::uint32_t chance = 0, outOf = 0;
        std::string give;      // one put in the bag when the roll wins
        QuestText failed;      // said when the roll loses
        QuestText got;         // said when given

        // --- The original's other item handlers ---------------------------------
        QuestText met;         // said in state 8 (its goals met) when 8 is not among `statuses` (else `refuse`)
        // Only in a party sharing both experience and items automatically (QNO_EU_GENERAL_2); else `noParty`.
        bool party = false;
        QuestText noParty;
        // Used on the selected monster: alive and one of `targets`, within `targetRange`
        // (3D; 0: anywhere). Nothing selected: `noTarget` (empty: `wrongTarget`); too far: `tooFar`; another: `wrongTarget`.
        std::vector<std::string> targets;
        float targetRange = 0;
        QuestText noTarget, tooFar, wrongTarget;
        // The result's extras: one `take` taken with the gift (none held: nothing given); the roll lost gives `orGive`
        // instead of saying `failed` (the amulet's coin); nothing given once `limit` of `limitItem` are held (asked at the
        // result, silently: the item is used all the same); in `keptIn`'s states the result happens but the item is kept
        // (the use refused 2: QNO_WC_WAREHOUSE_W_2 at 8).
        std::string take, orGive, limitItem;
        std::uint32_t limit = 0;
        std::vector<std::uint8_t> keptIn;
        // Monsters raised around the player (the quest service's spawn): `count` of `monster`, each
        // within a radius drawn between `near` and `far` (the original's banded spawn, game::QuestSpawnPoint), gone after
        // `seconds` unless killed first (0: they stay; the original's despawn timer).
        struct Spawn {
            std::string monster;
            std::uint32_t count = 1;
            float near = 20, far = 100;
            std::uint32_t seconds = 0;
            bool operator==(const Spawn&) const = default;
        };
        std::vector<Spawn> spawns;
        // A capture by trap: the selected monster (`targets`)
        // taken out of the world and the quest's COS `cos` in its place, keeping by its owner for `minutes`; caught when
        // rand() % 101 < `roll`, plus `step` a level the player stands above the COS (`below`: less `step` a level the COS
        // stands above the player). Refused while one is held (`already`); `failed` / `got` say the roll. A minute's words
        // (`warnings`); its time over `ran`, its owner dead `ownerDied`, the COS dead `cosDied`: gone.
        // The catch is kept in the quest's record as the original keeps it (its minutes, and 10 caught / 5 lost:
        // Quests::kCatchMinutesSlot, kCatchMarkSlot), so it lasts over a relog and a move to the other game
        // server (its COS brought back beside its owner on the world entry).
        // `item`: the catch is an item instead of a COS (QNO_EU_IVY_1): one put in the bag, every one taken when its time
        // is over or its owner dies. `raise`: one of that monster
        // raised each minute within `raiseNear`..`raiseFar` of its holder outside a town (IVY_1's guardians).
        // `diedMinutes`: its owner's death leaves its minutes at this, unmarked (GORIA_6's quirk, `ran` said again
        // when they run out, and `already` refuses meanwhile).
        struct Capture {
            std::string cos;
            std::string item;
            std::uint32_t minutes = 20;
            int roll = 50, step = 3;
            bool below = false;
            QuestText already, ran, ownerDied, cosDied;
            std::vector<std::pair<std::uint32_t, QuestText>> warnings; // minutes left, said then
            std::string raise;
            float raiseNear = 0, raiseFar = 0;
            std::uint32_t diedMinutes = 0;
            // One roll, several catches (QNO_CH_HWAN_1_4_1 / QNO_EU_HWAN_1_4_1): rand % 101
            // against each `below` in turn, the first under it gives its item (a grant that fails: nothing more) and says its
            // words; none: `failed`. No minutes held, nothing taken back. `stays`: the monster is not taken out of the world.
            struct Outcome {
                std::string item;
                int below = 0;
                QuestText said;
                bool operator==(const Outcome&) const = default;
            };
            std::vector<Outcome> outcomes;
            bool stays = false;
            bool operator==(const Capture&) const = default;
        };
        std::optional<Capture> capture;
        // Taken by the trap's catch, not by the bag's use: the item's skill sets a trap ('trap' with 'qest'),
        // whose catch of one of `targets` is the quest's event 0x1D (->
        // the trap's catch); its checks in the original's order: the state (silent), the monster (silent), `full`, `already`,
        // the roll.
        bool trap = false;
        // The places in turn (QNO_RM_OLDWOMAN_5: the rule's step, kept in the record's last count, picks the one): each its
        // place, its wait and its chance (`chance` in 101: the original's rand() % 101 <= its bound). Elsewhere `away`.
        struct Stage {
            Place at;
            std::uint32_t seconds = 0, chance = 101;
            bool operator==(const Stage&) const = default;
        };
        std::vector<Stage> stages;
        // Sites in turn (QNO_EU_IVY_3's Horn Trumpet): the first one the player stands at (3D, its
        // radius) without its item held raises its monster (`spawn`'s radius and life); none: `away`.
        struct Site {
            Place at;
            std::string item;
            Spawn spawn;
            bool operator==(const Site&) const = default;
        };
        std::vector<Site> sites;
        // Every one of these held (else refused silently), one of each taken once `give` is given (IVY_3's Punisher's Blood
        // makes the Key of Sign from the three signs); `room`: a free bag place, else refused silently.
        std::vector<std::string> need;
        bool room = false;
        // The place asked before the party (EASTEU_21's and IVY_3's handlers; GORIA_7's asks the party
        // first).
        bool placeFirst = false;
        // The quest's instance world (_RefGame_World's INS_QNO_*): used by the master of a
        // party (else `notMaster`; none: silently), no trade transport out (else `transport`), its party on this server taken
        // into a layer of it at their places: each member who finished the quest before or holds the item, without a
        // transport out (else `transport` said to it). The item itself is kept (the handler returns 0: refused 2, the
        // original's quirk). Inside, `after` seconds on, `unique` comes 20..100 from the master (`comes` said in turn, then
        // `begun`), and the fight lasts `minutes` (`warnings` at their minutes left); slain, `slain` and all go out; the
        // time over, `over` and all go out 10 s later; a member out of the party goes out 5 s after
        // UIIT_MSG_QUEST_INS_CONDITION (the world's tick, e.g. GORIA_7's, its death).
        struct World {
            std::string world, unique;
            std::uint32_t after = 30, minutes = 20;
            std::vector<QuestText> comes;
            QuestText begun, slain, over, notMaster, transport;
            std::vector<std::pair<std::uint32_t, QuestText>> warnings; // minutes left, said then
            // Uniques in turn instead of one (QNO_TQ_ORNAMENT_4's and QNO_TQ_STONEBOARD_4's four generals): the first
            // `after` seconds on, each next `next` seconds after the one before
            // dies; each its line 2 s after its coming (`says`), its words when slain (`dies`; the last one's ends it, all
            // out), the time's end its last words (`last`). The time (`minutes`, `warnings`, `begun`) runs from the first
            // one's battle.
            struct General {
                std::string monster;
                QuestText says, dies, last;
                bool operator==(const General&) const = default;
            };
            std::vector<General> generals;
            std::uint32_t next = 10;
            // Where each comes about the master (or the first one inside): the quest service's banded spawn, its radius drawn
            // in `firstNear`..`firstFar` for the first, `near`..`far` for the next ones (20..100; GENERALORDER_4's first
            // 190..200, the next 90..100: the original's rand / 32767 x 10 + 190, + 90).
            float firstNear = 20, firstFar = 100, near = 20, far = 100;
            // The last one slain raises a way out instead of everyone going out (the original's runtime teleport,
            // INS_QUEST_TELEPORT, where it came with 20..100 added to its x and its z, facing a random way): to `out`, or
            // with none back where its user came in (ORNAMENT_4's and STONEBOARD_4's empty link); the members leave by it,
            // the time's end puts out the rest.
            bool wayOut = false;
            std::optional<Place> out;
            bool operator==(const World&) const = default;
        };
        std::optional<World> instance;
        // A talk rule instead of an item's (`on talk NPC`, ours: the original's GENERALORDER_4 secret door, a talk row): the
        // quest under way (its `statuses`; `item` empty) at `npc`, the party's master is asked `ask` with the rows `row` and
        // SN_TALK_COMMON_DENY (no party sharing both: the quest's `not-party`; a member: `instance->notMaster`; a transport
        // out: `instance->transport`); the row picked, the party and the master asked again (the quest's `not-party`,
        // `middle`: the original's slots), then its `instance` opens as an item's would, its members those holding the quest
        // in the shared world, taken to `start`.
        std::string npc;
        QuestText row, ask;
        std::optional<Place> start;
        bool operator==(const UseRule&) const = default;
    };
    std::vector<UseRule> onUse;

    // A test in the quest's own instance, entered at its NPC's talk (the hwan army tests: QNO_CH_HWAN_1_1 and its kin).
    // Under way with its goals not met, `npc` says its `trial` talk with a row
    // (`row`); picked, the player goes alone into a new instance of `world` at `start` (_RefInstance_World_Start_Pos) unless
    // a trade transport is out (or, `noJob`, a job's suit is worn): `refuse` said. Inside, `after` seconds on, `boss` comes
    // at its place (within near..far); the test lasts `minutes` (the client's gauge; `warnings` at minutes left). `adds`
    // come at the boss's coming and every `every` seconds until the first warning. Over: `passOnTime` passes the test (its
    // goals met, `passed` said, out at once), else out silently. The boss slain: passed (`berserk`: in berserk `take` all
    // taken and one `give` given, else `failed` said), out `out` seconds later. The player dead before: the boss taken away,
    // `lost` said, out 15 s later. The quest no longer under way: out.
    struct Trial {
        std::string world;
        std::string npc;
        QuestText row, refuse, passed, lost, failed;
        bool noJob = false, passOnTime = false, berserk = false;
        UseRule::Place start; // its radius unused
        std::uint32_t after = 10, minutes = 10, out = 0;
        std::string boss;
        UseRule::Place bossAt;
        float near = 0, far = 0;
        std::vector<std::pair<std::uint32_t, QuestText>> warnings; // minutes left, said then
        struct Adds {
            std::string monster;
            UseRule::Place at;
            float near = 0, far = 0;
            std::uint32_t every = 30;
            bool operator==(const Adds&) const = default;
        };
        std::optional<Adds> adds;
        std::string take, give;
        // A scene instead of a boss (the White Viper chain: SR_GameServer's CQNO_TQ_BAEKRYOUNG_10, 13..16): the client's
        // script\q_script\NAME.txt played on entry; `sceneSeconds` later the quest's goals are met and the player goes out.
        std::string scene;
        std::uint32_t sceneSeconds = 0;
        bool operator==(const Trial&) const = default;
    };
    std::optional<Trial> trial;

    // The original's settings not traced yet, kept as they are
    std::uint32_t divide = 0;          // LuaSetDivideType: a sharing party's kill share (0 in turn, 1 4 5 all near, 2 3 the killer); 1 a party quest
    std::uint32_t differentString = 0; // LuaSetDifferentString
    bool disabled = false;
    std::vector<std::string> special;  // handlers the original ran in its script (unique-dead, conversation, ...)
    bool operator==(const QuestDef&) const = default;
};

// The level scale of a kill's quest drop (a `by-level` source): the monster at most 3 below and 10 above the player: 1;
// more than 10 above:
// 1 - 0.02 x (the gap - 10); 4..6 below: 0.9; 7 or more below: 1 - 0.15 x (the gap - 3); never under 0.1.
double QuestLevelScale(int playerLevel, int monsterLevel);

// The definition as text and back; the text read back gives the same definition.
std::string WriteQuest(const QuestDef& q);
bool ParseQuest(std::string_view text, QuestDef& out, std::string& error);

// Whether `npc` takes the quest back (the server's hand-in and the client's reward window alike): any of its givers for
// a quest of tiers, its finisher, or with none the NPC of a goal it takes (a delivery's, a talk's or a capture's, or
// one of the talk's others: the Beginner's Assistant's merchants).
bool QuestFinishesAt(const QuestDef& q, std::string_view npc);

} // namespace sr::game
