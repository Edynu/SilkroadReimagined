#pragma once

// The quests on the game server (the definitions game/Quest.h, kept in sr_quests).
//
// - A quest is offered by its givers to a player who meets its start (levels, race, earlier quests, hwan, day or
//   night, the window's quota) and has not finished it as often as it may be; taking it gives its start items.
// - Its goals count as the player plays: kills of its monsters (the kill's shares, the players its experience
//   goes to), its items dropped for its holder by its monsters at their chances (onto the ground, the holder's
//   alone) and held in the bag, items handed over and talks at its NPCs.
// - Its goals met, its finisher takes the gathered items and gives the reward: experience times the server's
//   rate-exp, skill experience and skill points times rate-sp, gold, items (one chosen of them when it says so),
//   bag places, the hwan level.
// - A quest is given up from the quest window (its quest items taken back) and may be taken again.
// - The character's quests are kept in the original's _CharQuest (status, times finished, the start and end
//   times, the goals' counts in QuestData1 / QuestData2, four 16-bit counts each).
//
// - A collect goal is met when the player's collection book holds its theme complete.
//
// Not offered yet: the quests whose goals or starts need what is not built (players killed, jobs, the original's
// own script handlers). The game server is its host.

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "sr/game/Quest.h"
#include "sr/game/QuestProtocol.h"
#include "sr/game/WorldProtocol.h"

namespace sr::server {

// A character's quest as _CharQuest keeps it.
struct QuestRecord {
    std::uint32_t quest = 0;
    game::proto::QuestStatus status = game::proto::QuestStatus::Achieving;
    std::uint16_t times = 0; // AchievementCount
    std::array<std::uint16_t, game::proto::kQuestGoals> progress{};
    std::int64_t started = 0, ended = 0; // Unix seconds
};
// The goals' counts packed as QuestData1 / QuestData2 keep them (four 16-bit counts each, the first goal lowest).
std::pair<std::int64_t, std::int64_t> PackQuestProgress(const std::array<std::uint16_t, game::proto::kQuestGoals>& progress);
std::array<std::uint16_t, game::proto::kQuestGoals> UnpackQuestProgress(std::int64_t data1, std::int64_t data2);

class QuestHost {
public:
    virtual ~QuestHost() = default;
    struct Player {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::string name;
        int level = 1;
        std::string race; // china, europe
        int hwan = 0;
        bool free = true; // alive and in no dealing
        std::uint64_t gold = 0;
        std::uint16_t region = 0; // where it stands
        game::proto::WorldPosition position; // and the place (an item's use asks it: QuestDef::UseRule::at)
        // The premium's quests (its package's UQL1 parts): the uses left of each, by the
        // quest's codename, while the package runs (none: no premium).
        std::map<std::string, int> premiumQuests;
        // In a party sharing both experience and items automatically (a `start party` quest, QuestDef::party; the
        // original's quest class). The party's settings are every game server's alike.
        bool sharedParty = false;
        // A quest's trial (QuestDef::Trial): in berserk (the body mode 1), a job's suit worn (the job state not 4), dead.
        bool berserk = false, jobSuit = false, dead = false;
        // Standing in a town (_RefRegion IsBattleField 0): no capture's `raise` there (QNO_EU_IVY_1's minute).
        bool town = false;
        // Its counted deeds (ours, sr_char_records: the beta quests' `start record`), by name.
        std::map<std::string, int> deeds;
        // The job of the suit worn (the original's job state): trader, thief,
        // hunter; empty: none worn. A quest's `start job` asks it (the offer test, flag 0x1000).
        std::string job;
    };
    virtual std::optional<Player> QuestPlayer(std::uint32_t entity) = 0;
    virtual void QuestSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // The NPC's codename when the player stands within reach of it (empty: no such NPC, or too far).
    virtual std::string QuestNpcNear(std::uint32_t player, std::uint32_t npc) = 0;
    // The hour of the game's clock (0..23).
    virtual int QuestHour() = 0;
    // The count of an item (its codename) in the player's bag.
    virtual std::uint32_t QuestItemCount(std::uint32_t player, const std::string& item) = 0;
    // Items into the bag, all of them or none (false: no room or no such item).
    virtual bool QuestGive(std::uint32_t player, const std::vector<game::QuestGoal::Items>& items) = 0;
    // The same, the items bound to the character (QuestReward::bound; ours). A host without binding gives them plain.
    virtual bool QuestGiveBound(std::uint32_t player, const std::vector<game::QuestGoal::Items>& items) { return QuestGive(player, items); }
    // Items out of the bag, all of them or none (false: not all there).
    virtual bool QuestTake(std::uint32_t player, const std::vector<game::QuestGoal::Items>& items) = 0;
    // Gold out of the player's purse, all of it or none (false: not that much).
    virtual bool QuestTakeGold(std::uint32_t player, std::uint64_t gold) = 0;
    // Whether the bag has room for these items (nothing changed).
    virtual bool QuestRoom(std::uint32_t player, const std::vector<game::QuestGoal::Items>& items) = 0;
    // The reward's numbers, after the rates: experience and skill experience earned, gold, skill points, bag places,
    // the hwan level reached (0: unchanged). Saved by the host.
    struct Reward {
        std::uint64_t exp = 0, skillExp = 0, gold = 0;
        std::uint32_t sp = 0, inventory = 0, hwan = 0;
        std::vector<game::QuestReward::Points> points; // the temple quests' points (TEMPLE_ANUBIS, TEMPLE_HAROERIS)
    };
    virtual void QuestReward(std::uint32_t player, const Reward& reward) = 0;
    // A quest item dropped by a monster at `monster`'s place for the player alone.
    virtual void QuestDrop(std::uint32_t player, std::uint32_t monster, const std::string& item) = 0;
    // A character's quest kept (_CharQuest's row, made or replaced).
    virtual void QuestSave(std::int64_t character, const QuestRecord& record) = 0;
    // Whether the player's collection book has the theme complete (a Forgotten World talisman theme).
    virtual bool QuestThemeDone(std::uint32_t player, const std::string& theme) = 0;
    // A premium quest finished: one of its package's uses spent (saved by the host).
    virtual void QuestPremiumSpent(std::uint32_t player, const std::string& quest) {
        (void)player;
        (void)quest;
    }
    // A monster's level by its codename (0: unknown; a `by-level` drop then counts it as the player's own level).
    virtual int QuestMonsterLevel(const std::string& codename) {
        (void)codename;
        return 0;
    }
    // Whether the bag has an empty place (the original's count of empty places above 0: an item use's search).
    virtual bool QuestFreePlace(std::uint32_t player) {
        (void)player;
        return true;
    }
    // A text key said to the player (the original's 0x30EC): an item use's
    // words. Ours: the chat's line (proto::InstanceNotice, the client's text key line).
    virtual void QuestSay(std::uint32_t player, const std::string& key) {
        (void)player;
        (void)key;
    }
    // --- The original's other item handlers ----------------------------------------
    // The living monster the player has selected (none: nothing, or no living monster).
    struct Selected {
        std::uint32_t entity = 0;
        std::string codename;
        game::proto::WorldPosition position;
    };
    virtual std::optional<Selected> QuestSelected(std::uint32_t player) {
        (void)player;
        return std::nullopt;
    }
    // A monster of that codename raised at `at` (on ground the player's place reaches, its layer), fighting as its tactics
    // say, never refilled: its nest (none: no such monster, or no ground there). Gone by itself once dead.
    virtual std::optional<std::size_t> QuestSpawn(std::uint32_t player, const std::string& codename, const game::proto::WorldPosition& at) {
        (void)player;
        (void)codename;
        (void)at;
        return std::nullopt;
    }
    // A raised monster taken away without dying (its time over: the original's despawn timer).
    virtual void QuestDespawn(std::size_t nest) { (void)nest; }
    // The selected monster caught: taken out of the world without dying, the COS `cos` in its place as the player's,
    // keeping by it. Its entity (0: it could not be made). Whether it still stands; taken away.
    virtual std::uint32_t QuestCatch(std::uint32_t player, std::uint32_t monster, const std::string& cos) {
        (void)player;
        (void)monster;
        (void)cos;
        return 0;
    }
    virtual bool QuestCosStanding(std::uint32_t cos) {
        (void)cos;
        return false;
    }
    virtual void QuestCosRemove(std::uint32_t cos) { (void)cos; }
    // A caught COS brought back beside its owner (on its world entry, the quest
    // service's spawn of kind 2); its entity (0: none could be made).
    virtual std::uint32_t QuestResummon(std::uint32_t player, const std::string& cos) {
        (void)player;
        (void)cos;
        return 0;
    }
    // A trapped monster taken out of the world without dying (the trap's catch).
    virtual bool QuestTakeMonster(std::uint32_t monster) {
        (void)monster;
        return false;
    }
    // The game clock's day (the game's days, a game day 28.8 real minutes), the same
    // on every game server of the shard; a daily resupply compares it (QuestDef::resupplyDaily).
    virtual std::uint16_t QuestDay() { return 0; }
    // A raised monster's nest's monsters standing (0: dead, or gone).
    virtual int QuestSpawnStanding(std::size_t nest) {
        (void)nest;
        return 0;
    }
    // A quest's instance (QuestDef::UseRule::World): the player's party's members on this game server (itself among them;
    // empty: no party; the original's walker finds only the players its own server holds), whether it is its master, whether
    // it has a trade transport out.
    virtual std::vector<std::uint32_t> QuestPartyHere(std::uint32_t player) {
        (void)player;
        return {};
    }
    virtual bool QuestPartyMaster(std::uint32_t player) {
        (void)player;
        return false;
    }
    virtual bool QuestTransportOut(std::uint32_t player) {
        (void)player;
        return false;
    }
    // An instance of the world opened by the player (its layer; 0: none); a player taken into a layer at its own place; a
    // player out of its instance, back where it came from.
    virtual std::uint32_t QuestWorldOpen(std::uint32_t player, const std::string& world) {
        (void)player;
        (void)world;
        return 0;
    }
    virtual bool QuestWorldEnter(std::uint32_t player, std::uint32_t layer) {
        (void)player;
        (void)layer;
        return false;
    }
    virtual void QuestWorldLeave(std::uint32_t player) { (void)player; }
    // A player taken into a layer at `at` (a trial's start, _RefInstance_World_Start_Pos; false: not taken).
    virtual bool QuestWorldEnterAt(std::uint32_t player, std::uint32_t layer, const game::proto::WorldPosition& at) {
        (void)at;
        return QuestWorldEnter(player, layer);
    }
};

class Quests {
public:
    using Clock = std::chrono::steady_clock;
    struct Rates {
        double experience = 1.0, skill = 1.0; // rate-exp; rate-sp (skill experience and skill points)
        // rate-gold: a reward's gold times it, and the original's cap of 10,000,000 a reward times it too (at 10 a quest
        // may pay 100,000,000)
        double gold = 1.0;
    };
    Quests(QuestHost& host, Rates rates);

    // The definitions (sr_quests); the ones that cannot be played yet are kept out. How many are offered.
    struct Loaded {
        int offered = 0, waiting = 0; // waiting: what they need is not built
    };
    Loaded Load(std::vector<game::QuestDef> quests);
    const game::QuestDef* Find(std::uint32_t id) const;
    const game::QuestDef* Find(std::string_view codename) const;

    // A player in the world with its kept quests (_CharQuest); it is told its list.
    void Enter(std::uint32_t entity, std::vector<QuestRecord> records);
    void Leave(std::uint32_t entity);
    // A request (false: it does not decode).
    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload);
    // The NPC's rows for the player (after its menu).
    void TellNpc(std::uint32_t entity, std::uint32_t npc);
    // A monster killed: `credited` are the players its experience went to. Kills count, quest items drop.
    void Killed(std::uint32_t monster, const std::string& codename, std::uint8_t rarity, std::span<const std::uint32_t> credited);
    // A unique died (the original's UNIQUE_DEAD event, QuestDef::onUniqueDead): its watchers swap their items.
    void UniqueDied(const std::string& codename);
    // A player killed by another (the temple quests' kill-player goals, LuaSetKillPlayerMissionData's MKP_LEVEL, MKP_REGION
    // and MKP_JOB): the killer's goals whose victims it fits counted. `job` the victim's: trader, hunter, thief (empty: none).
    void PlayerKilled(std::uint32_t killer, int level, std::uint16_t region, const std::string& job);
    // The player's bag changed (an item picked up, dropped, sold, moved out): the gathering goals recounted.
    void BagChanged(std::uint32_t entity);
    // Timers run out.
    void Tick(Clock::time_point now);
    // The player's level changed: the quests it may take now (their NPCs' marks).
    void LevelChanged(std::uint32_t entity);
    // An item used from the bag offered to the player's quests (the original's quest manager, event 0x16:
    // over the quests' item handlers; QuestDef::onUse): true when one of them
    // takes it (the caller uses it up), false when none does (the caller refuses it 2). A rule refusing it says its
    // words; one taking it starts its wait, whose end (Tick) rolls and gives.
    bool UseItem(std::uint32_t entity, const std::string& item);
    // A developer's (the GM's /quest done): the quest (its codename) and the ones it needs first marked finished once more,
    // no reward, its items left as they are (a quest's tries from the step they need). The count marked (0: no such quest).
    int DevFinish(std::uint32_t entity, std::string_view codename);
    // A search under way for that quest (tests).
    bool Searching(std::uint32_t entity, std::uint32_t quest) const;
    // The player died: its caught COS runs away (the rule's `owner-died`).
    void Died(std::uint32_t entity);
    // The player's trap caught a monster (the quest's event 0x1D): its quests' `trap` rules for that monster.
    void Trapped(std::uint32_t entity, std::uint32_t monster, const std::string& codename);
    // Where a quest's record keeps the original's bytes of its coded handlers (the record's +0..+0xF are QuestData1 / QuestData2,
    // our eight 16-bit counts): a capture's minutes (byte +4: word 2), its mark (word +0xA: 10 caught, 5 lost), the day of the
    // last daily resupply (word +0xC). A quest using them keeps its goals below them (Load warns otherwise).
    static constexpr std::size_t kCatchMinutesSlot = 2, kCatchMarkSlot = 5, kSupplyDaySlot = 6;
    static constexpr std::uint16_t kCaught = 10, kLost = 5;
    // A capture held for that quest (tests): its COS (0: none) and its minutes left.
    std::pair<std::uint32_t, std::uint32_t> Caught(std::uint32_t entity, std::uint32_t quest) const;
    // The raised monsters waiting for their time's end (tests).
    std::size_t Raised() const { return raised_.size(); }
    // The quests' instances under way (tests): their layers.
    std::vector<std::uint32_t> Worlds() const;
    // The trials under way (tests): each one's player and layer.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> Trials() const;
    // The seconds the client's gauge shows for the player's quest (0: none; the original's 0x30DA, SetQuestDisplayTimer).
    std::uint32_t Gauge(std::uint32_t entity, std::uint32_t quest) const;

    // What the player has (tests).
    const QuestRecord* Record(std::uint32_t entity, std::uint32_t quest) const;
    bool Offered(std::uint32_t entity, const game::QuestDef& q) const;
    // A job's quest under way (one that needs a job's suit, or counts job kills; no leaving
    // the league while one runs, 0x482E).
    bool JobQuestUnderWay(std::uint32_t entity) const;

private:
    struct PlayerQuests {
        std::uint32_t entity = 0;
        std::int64_t character = 0;
        std::map<std::uint32_t, QuestRecord> records;
        std::set<std::uint32_t> told; // definitions sent this session
        std::map<std::string, game::proto::QuestMark> marks; // the NPCs' marks last told
        int level = 0; // the level last seen (a rise gives the quests given by level)
        // An item use's wait under way, by quest (the original's record byte +4, one a quest): its rule and its end.
        struct Search {
            const game::QuestDef::UseRule* rule = nullptr;
            Clock::time_point until;
            int stage = -1; // a stage rule's (QuestDef::UseRule::stages)
        };
        std::map<std::uint32_t, Search> searches;
        // A capture held, by quest (the original's COS beside its owner, its minutes at the record's +4).
        struct Catch {
            const game::QuestDef::UseRule* rule = nullptr;
            std::uint32_t cos = 0;
            std::uint32_t minutes = 0;
            Clock::time_point nextMinute;
        };
        std::map<std::uint32_t, Catch> catches;
        // The gauge's end by quest (a trial's or an instance's time, shown as the quest's seconds left).
        std::map<std::uint32_t, Clock::time_point> gauges;
        // QuestDef::raises: when each (quest, its raise) raises next (unset: the countdown starts at the first tick)
        std::map<std::pair<std::uint32_t, std::size_t>, Clock::time_point> raiseAt;
        // A talk game under way (QuestDef::talkGame), by quest: Huhoan's drinks.
        struct TalkGame {
            int target = 0, sum = 0;
            bool poured = false, exchange = false;
            std::vector<std::size_t> rows; // what each row sent stands for (a drink's index, or kAsk / kTrade / kRefuse)
        };
        std::map<std::uint32_t, TalkGame> games;
    };
    // The original's state of a quest record (1 under way, 7 a goal progressed, 8 goals met; 4 not under way).
    static std::uint8_t OriginalState(const QuestRecord& r, const game::QuestDef* q = nullptr);
    // A search's end: its roll, the item given or the failure said; false when the item is to be kept (the coin's
    // `kept-in`, a toss whose grant failed: the original returns 0).
    bool EndSearch(std::uint32_t entity, PlayerQuests& p, std::uint32_t quest, const game::QuestDef::UseRule& rule, int stage, std::uint8_t state);
    // A capture's end: its COS away (a caught item's every one taken), `text` said, the record marked lost (5) with no
    // minutes left; `minutesLeft`: kept running unmarked instead (QuestDef::UseRule::Capture::diedMinutes).
    void DropCatch(std::uint32_t entity, PlayerQuests& p, std::uint32_t quest, const game::QuestText* text, std::uint32_t minutesLeft = 0);
    // A catch begun or brought back: kept in `catches` and in the record (its minutes and mark).
    void HoldCatch(PlayerQuests& p, std::uint32_t quest, const game::QuestDef::UseRule& rule, std::uint32_t cos, std::uint32_t minutes,
                   std::uint16_t mark, Clock::time_point now);
    // The catches the records keep brought back on the world entry (the COS beside its owner).
    void ResumeCatches(std::uint32_t entity, PlayerQuests& p);
    // The rule's step (a stage rule's), kept in the record's last count (the original's QuestData1 byte 0).
    static constexpr std::size_t kStepSlot = game::proto::kQuestGoals - 1;
    // Raised monsters with a time (QuestDef::UseRule::Spawn::seconds).
    struct RaisedMonster {
        std::size_t nest = 0;
        Clock::time_point until;
    };
    std::vector<RaisedMonster> raised_;
    // A quest's instance under way (QuestDef::UseRule::World): its layer, its master, who is inside, its phase.
    struct QuestWorld {
        std::uint32_t quest = 0, layer = 0, master = 0;
        const game::QuestDef::UseRule* rule = nullptr;
        std::set<std::uint32_t> inside;
        std::map<std::uint32_t, Clock::time_point> leaving; // out of the party: out at that time
        enum class Phase : std::uint8_t { Waiting, Opening, Fighting, Ending } phase = Phase::Waiting;
        std::size_t line = 0;    // Opening: the `comes` lines said so far
        std::size_t general = 0; // the generals' turn (World::generals)
        std::optional<Clock::time_point> ends; // the time's end, from the (first) battle's begin
        Clock::time_point next; // Waiting: the unique's coming; Fighting: the time's end; Ending: everyone out
        std::size_t unique = SIZE_MAX;
        std::set<std::uint32_t> warned; // the warnings said (minutes left)
    };
    std::vector<QuestWorld> worlds_;
    // The instance's entry (the item handler's checks after the rule's own, then the party walker): true when opened.
    bool OpenWorld(std::uint32_t entity, const QuestHost::Player& who, const game::QuestDef& q, const game::QuestDef::UseRule& u, Clock::time_point now);
    void TickWorlds(Clock::time_point now);
    // A quest's trial under way (QuestDef::Trial): its player, its layer, its phase and times.
    struct TrialRun {
        std::uint32_t quest = 0, player = 0, layer = 0;
        const game::QuestDef::Trial* spec = nullptr;
        enum class Phase : std::uint8_t { Waiting, Fighting, Ending } phase = Phase::Waiting;
        Clock::time_point next;    // Waiting: the boss's coming; Ending: everyone out
        Clock::time_point ends;    // Fighting: the time's end
        Clock::time_point nextAdd; // Fighting: the next add (before the first warning)
        std::size_t boss = SIZE_MAX;
        std::set<std::uint32_t> warned;
    };
    std::vector<TrialRun> trials_;
    // The trial's entry at its NPC (the talk's row); false when refused.
    bool EnterTrial(std::uint32_t entity, const QuestHost::Player& who, const game::QuestDef& q, Clock::time_point now);
    void TickTrials(Clock::time_point now);
    // The quest's goals met by the trial (the quest's state 8: its kill goals at their count).
    void TrialPassed(std::uint32_t entity, const game::QuestDef& q);
    // The client's gauge set (seconds 0: off) and told.
    void SetGauge(std::uint32_t entity, std::uint32_t quest, std::uint32_t seconds, Clock::time_point now);
    // The NPCs' marks for the player, told when they changed.
    void TellMarks(std::uint32_t entity, PlayerQuests& p);
    struct Quota {
        std::uint32_t left = 0;
        bool night = false; // the window it counts for
    };

    PlayerQuests* Of(std::uint32_t entity);
    const PlayerQuests* Of(std::uint32_t entity) const;
    void Send(std::uint32_t entity, game::proto::MessageId id, const std::vector<std::byte>& payload);
    template <typename T>
    void Send(std::uint32_t entity, const T& message);
    void Define(std::uint32_t entity, PlayerQuests& p, const game::QuestDef& q);
    game::proto::QuestState StateOf(std::uint32_t entity, const game::QuestDef& q, const QuestRecord& r);
    void Changed(std::uint32_t entity, PlayerQuests& p, const game::QuestDef& q, QuestRecord& r);
    // The goals' state recomputed (the gathered items counted); true when it changed.
    bool Evaluate(std::uint32_t entity, const game::QuestDef& q, QuestRecord& r);
    bool OfferedTo(const QuestHost::Player& who, const PlayerQuests& p, const game::QuestDef& q) const;
    bool Started(const game::QuestDef& q) const; // the quota allows a start now
    static bool UnderWay(const QuestRecord& r);
    // What is due at the NPC for the player's quest: a goal (its index) not done there, or none.
    std::optional<std::size_t> DueAt(std::uint32_t entity, const game::QuestDef& q, const QuestRecord& r, const std::string& npc);
    bool Involves(const game::QuestDef& q, const QuestRecord& r, const std::string& npc) const;

    void Talk(std::uint32_t entity, const game::proto::QuestTalk& t);
    void Accept(std::uint32_t entity, const game::proto::QuestAccept& a);
    void HandIn(std::uint32_t entity, const game::proto::QuestHandIn& h);
    // A talk game's turn (QuestDef::talkGame): a talk (no row) or the row picked.
    void PlayTalkGame(std::uint32_t entity, PlayerQuests& p, const game::QuestDef& q, std::uint32_t npc, std::optional<std::uint8_t> row);
    void Abandon(std::uint32_t entity, std::uint32_t quest, game::proto::QuestEnd why);
    void Say(std::uint32_t entity, std::uint32_t npc, std::uint32_t quest, std::string moment, game::proto::QuestChoice choice, std::uint8_t goal = 0);
    void Answer(std::uint32_t entity, std::uint32_t quest, game::proto::QuestCode code);
    void Pay(std::uint32_t entity, PlayerQuests& p, const game::QuestDef& q, QuestRecord& r, std::uint32_t npc, std::span<const std::uint8_t> choices);
    // A tiered quest's row picked (QuestDef::tiers): its items taken, its skill points paid, the quest finished.
    void PayTier(std::uint32_t entity, PlayerQuests& p, const game::QuestDef& q, QuestRecord& r, std::uint32_t npc, std::span<const std::uint8_t> choices);
    // The quest finished and paid (`reward` after the rates): its record, the player told, the NPC's last words.
    void Finished(std::uint32_t entity, PlayerQuests& p, const game::QuestDef& q, QuestRecord& r, std::uint32_t npc, const QuestHost::Reward& reward);
    bool NeedsHeld(std::uint32_t entity, const QuestHost::Player& who, const game::QuestDef& q); // the items, gold and deeds needed
    // A record's count for `start record` / `need record`: a deed's (who.deeds), or `finished[:PREFIX]` the different quests
    // finished (the beta ones not counted); and whether every need has one of its counts reached.
    int RecordCount(const QuestHost::Player& who, const PlayerQuests& p, const std::string& name) const;
    bool RecordsMet(const QuestHost::Player& who, const PlayerQuests& p, const std::vector<game::QuestDef::RecordNeed>& needs) const;
    void Start(std::uint32_t entity, PlayerQuests& p, const game::QuestDef& q, const std::string& who); // its record begun
    bool Finishes(const game::QuestDef& q, const std::string& npc) const; // its finisher, or with none its goals' NPCs

    QuestHost& host_;
    Rates rates_;
    std::vector<game::QuestDef> quests_;
    std::unordered_map<std::uint32_t, std::size_t> byId_;
    std::unordered_map<std::string, std::size_t> byCode_;
    std::unordered_map<std::uint32_t, PlayerQuests> players_;
    std::unordered_map<std::uint32_t, Quota> quotas_;
    std::mt19937 random_{std::random_device{}()};
};

} // namespace sr::server
