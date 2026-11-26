#pragma once

// A game server: it reports itself in the database (the gateway lists it,
// the login server sends players to it), admits a player with the login's one-time ticket, keeps
// the account's one online session, and runs the character lobby: list, create, name check,
// delete (a wait, restorable), restore, select. After the select, the player enters the world
// (World.h, C6b): its ticks run in the loop, positions are saved when a player leaves and every
// half minute.
//
// Creation follows the client's data (game/CharacterRules.h). Nothing blocks the loop: database
// work runs on the pool's threads and posts back.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "sr/assets/GameData.h"
#include "sr/assets/RegionInfo.h"
#include "sr/assets/StallNetworkData.h"
#include "sr/assets/Shop.h"
#include "sr/assets/Gacha.h"
#include "sr/game/Berserk.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/Guild.h"
#include "sr/game/OpenMarket.h"
#include "sr/game/Sets.h"
#include "sr/game/Spirit.h"
#include "sr/net/Connection.h"
#include "sr/server/Drops.h"
#include "sr/server/Fortress.h"
#include "sr/server/FlagWorld.h"
#include "sr/server/Arena.h"
#include "sr/server/QinTomb.h"
#include "sr/server/Roc.h"
#include "sr/server/Temples.h"
#include "sr/server/Monsters.h"
#include "sr/server/Service.h"
#include "sr/server/ShardLayout.h"
#include "sr/server/World.h"

namespace sr::server {

struct GameServerConfig {
    std::string name = "SroGame";
    // How it is listed.
    std::uint16_t id = 1;
    std::string serverName = "Silkroad";
    std::uint8_t group = 1;
    std::string groupName = "Silkroad";
    // The address players are sent to (the listener's port is added).
    std::string publicHost = "127.0.0.1";
    std::uint32_t capacity = 3000;   // the most players it takes (each carries the whole shard: ShardLayout.h)
    std::uint32_t shardCapacity = 0; // the shard's, all its game servers together (reported; 0: theirs added up)
    net::ListenConfig listen;
    std::string databaseUrl;
    // This shard's schema: its characters, items, storage; the accounts are
    // sro_vt_account's. Another shard on the same database names its own (sro_vt_shard2, ...).
    std::string shardSchema = "sro_vt_shard";
    // The game's logs (GameLog.h) in the shard's log schema: written (false: none), and the file their rows wait in while
    // the database cannot take them (empty: gamelog-<id>.spill in the working folder).
    bool gameLog = true;
    std::filesystem::path gameLogSpill;
    // Which game server runs which part of the world (ShardLayout.h; none: this one runs it all).
    std::shared_ptr<const ShardLayout> layout;
    // How long a player gone to another game server may take to arrive, before it counts as logged out.
    std::chrono::seconds moveWait{20};
    unsigned networkThreads = 2;
    unsigned databaseThreads = 2;
    std::chrono::milliseconds reportInterval{5000};
    // A session on a game server whose last report is older counts as gone.
    int staleSeconds = 15;
    // Characters per account on this server (those waiting to be deleted count); the v1.150
    // client says 3 (UIO_MSG_ERROR_CHARACTER_OVER: "Cannot create more than 3 characters.").
    int maxCharacters = 3;
    // How long a deleted character can still be restored.
    int deleteSeconds = 7 * 24 * 3600;
    std::shared_ptr<const game::CharacterRules> rules; // required
    // The teleports (gates and where they lead); null: no gate takes anyone anywhere.
    std::shared_ptr<const assets::TeleportTable> teleports;
    // The teleport zones (`_RefEventZone` rows of event TELEPORT): a navmesh event
    // zone's name in lower case -> the teleport point it takes a player through (its strParam1).
    std::unordered_map<std::string, std::string> teleportZones;
    // The NPCs that stand in the world (npcpos.txt's NPC rows outside dungeons; C7i).
    struct NpcSpawn {
        std::uint32_t refObjId = 0;
        std::string codename;
        game::proto::WorldPosition position;
        float yaw = 0; // which way it faces (0: +z)
        // A custom NPC's name (ours: the Beta Helper), shown over its head; empty: the model's own. Its codename is the
        // name's (game::CustomNpcCodename).
        std::string name;
    };
    std::vector<NpcSpawn> npcs;
    GameRates rates; // Drops.h
    // What monsters drop (monster_drops.tsv); null: nothing.
    std::shared_ptr<const DropTable> drops;
    // magicoption.txt (the database's _RefMagicOpt): alchemy's options; null: no alchemy.
    std::shared_ptr<const std::vector<assets::MagicOptionRow>> magicOptions;
    // The magic option groups (_RefMagicOptGroup): the socket stones' skills by level; none:
    // no stone goes into a socket.
    std::vector<assets::MagicOptGroupRow> magicOptGroups;
    // The options each kind of item may get (magicoptionassign.txt: the database's _RefMagicOptAssign through
    // DatabaseText): the drops' random blues and the avatars' lists at the smith;
    // null: no avatar is granted anything.
    std::shared_ptr<const std::vector<assets::MagicOptionAssignRow>> magicOptionAssign;
    // The item sets (refsetitemgroup.txt / _RefSetItemGroup and their groups' MagicType 2 rows: game::LoadSetTable);
    // null: no set gives anything.
    std::shared_ptr<const game::SetTable> sets;
    // The Devil's Spirit's options and skills by plus (game::LoadSpiritTable); null: a worn spirit
    // gives nothing. Its awakening's end while worn is looked for every `spiritCheck` (the original's 10 s:
    // arms it; tests shorten it).
    std::shared_ptr<const game::SpiritTable> spirits;
    std::chrono::milliseconds spiritCheck{10000};
    // leveldata.txt: the experience each level needs (index: the level), and what a general monster of
    // each level gives (col 5, the skill experience's measure); levelgold.txt: a monster's gold by
    // its level (C10d). Empty: no levelling.
    std::vector<std::uint64_t> levels;
    std::vector<std::uint32_t> monsterExperience;
    std::vector<std::uint32_t> masteryCosts; // leveldata col 2: the skill points a mastery's next level costs
    std::vector<std::pair<std::uint32_t, std::uint32_t>> levelGold;
    // characterdata (the monsters' references); null: no monsters.
    std::shared_ptr<const assets::CharacterTable> characters;
    // The monsters' spawn points (npcpos.txt's monster rows outside dungeons, with their policy).
    std::vector<MonsterNest> monsterNests;
    // The monsters' Resist columns (the database's _RefObjChar; K3); null: no monster resists a state.
    std::shared_ptr<const MonsterResistances> monsterResistances;
    // The hwan levels' berserk modes (_RefHWANLevel; none: the mode's speed alone), and the mode's time
    // (tests shorten it).
    std::vector<game::HwanLevel> hwanLevels;
    std::uint32_t berserkMs = game::kBerserkMs;
    // How long a guild master's election runs (tests shorten it).
    int guildElectionSeconds = game::kElectionSeconds;
    int guildWarEndSeconds = game::kGuildWarEndSeconds; // a war given up ends this much later
    // How long a free PvP team change takes (10 s; tests shorten it).
    double freePvpSeconds = 10.0;
    // The shard's bus (S6): the game servers of one shard (one schema) share its
    // guilds through PostgreSQL's LISTEN / NOTIFY on the channel sr_guilds_<schema>.
    bool shardBus = true;
    // The fortress war: the fortresses in service with their structures'
    // places (empty: no fortress war), and its schedule (`_Schedule`, or a cycle for tests and tries).
    std::vector<FortressSpec> fortresses;
    FortressConfig fortress;
    // Capture the Flag: its switch (the shard's event-ctf), its schedule (ctf-times, or a
    // cycle for tries: --ctf-cycle), the matches' least players and most a side.
    FlagWorldConfig flagWorld;
    // The Battle Arena: its switch (event-arena), its schedule (`_Schedule` 16..23, or
    // --arena-cycle), its worlds (tests give theirs; else the database's).
    ArenaConfig arena;
    // The Roc: its switch (the shard's event-roc, on), its schedule (`_Schedule` of SCHEDULE_DEF_MEET_ROC,
    // or --roc-cycle O,P), the Peak's times sped up for tries (--roc-time-scale).
    RocConfig roc;
    // The Qin-Shi tomb's snake gate and upper floors: its switch (the shard's event-qin-tomb, on), its schedule (`_Schedule`
    // of define 2, or --qin-cycle O,P), the copies' last hour sped up for tries (--qin-time-scale).
    QinTombConfig qinTomb;
    // The job temples: their switch (the shard's event-temples, on), their schedule (`_Schedule` 9..11,
    // or --temple-cycle P).
    TempleConfig temples;
    // The weather's clock against the wall clock (tries: 60 runs an hour's weather in a minute); every server of a shard alike.
    double weatherSpeed = 1.0;
    // The bargain goods' sale hours (`_Schedule` rows of game::kScheduleSpecialGoods; none: never).
    std::vector<game::ScheduleRow> bargainSchedule;
    // The merchants' shops (refshop* tables); null: no NPC sells anything.
    std::shared_ptr<const assets::ShopTable> shops;
    // The item mall's catalogue (GROUP_MALL's shops, Mall.h); null: no mall (and no silk told).
    std::shared_ptr<const assets::MallTable> mall;
    // The Open Market (OpenMarket.h): out of service in vSRO 1.188, ours on by default; its
    // NPC NPC_OPEN_MARKET_JUEL then stands at its nests. Its fees (the brackets' percents).
    bool openMarket = true;
    game::openmarket::Fees openMarketFees;
    // Magic POP's sets and machines (gachaitemset / gachanpcmap, the database's rows; MagicPop.h); null: none.
    std::shared_ptr<const assets::GachaTable> gacha;
    // Tests: every Magic POP draw this value (0..32,767); none: random.
    std::optional<std::uint32_t> popDraw;
    // skilldata (what scrolls cast); null: no scroll casts anything.
    std::shared_ptr<const assets::SkillTable> skills;
    // The stall network: the towns (regioninfo.txt) and the network's groups
    // (fmn*data.txt); null: read from `files` at the start (none there: stalls work, the network is off).
    std::shared_ptr<const assets::RegionAreaTable> towns;
    std::shared_ptr<const assets::StallNetworkData> stallNetwork;
    // A job suit's change and, once it is on, its lock (10 s each; tests shorten them).
    double jobSuitSeconds = 10.0, jobLockSeconds = 10.0;
    // The bandits' waves: one each `banditMinSeconds` + up to `banditSpanSeconds` (the original's
    // 60 + 60; SroGame --bandit-seconds A,B for tries).
    double banditMinSeconds = 60.0, banditSpanSeconds = 60.0;
    // How near a gate's spot a traveller must stand to talk to it or use it (ours; the original's is not known yet).
    float gateReach = 300.0f;
    // The system menu's Restart and Exit wait this long before the character leaves the world
    // (ours: the original's server chose it and the client only shows the seconds it is told).
    std::chrono::seconds logoutCountdown{5};
    // Developer commands (DevGoto): for development servers only. A build without SR_DEV_COMMANDS (a public release)
    // ignores it: GameServer::Start turns it off and says so.
    bool devCommands = false;
    // The PK switch (step 7 of the attack check; how the original sets it is not known):
    // set, no player attacks an innocent nor a hostile job (0x3025); free PvP and
    // guild wars stay. A shard setting (pk-prohibited), off by default.
    bool pkProhibited = false;
    // The beta quests (game/BetaQuests.h; ours, a test shard's): a Beta Helper beside So-Ok in each town and its ten
    // quests of Seal of Sun weapons. A shard setting (beta-quests), off by default.
    bool betaQuests = false;
    // The guild's mercenaries (game/GuildMercenary.h; ours: 1.188's data refuses their scrolls, so its players never had
    // one): on, a scroll summons its guild's soldiers and the guild's NPC sells their abilities. A shard setting
    // (guild-mercenaries), off by default: off, all of it is refused as the original's.
    bool guildMercenaries = false;
    // A hostility record's seconds (20; tests shorten them).
    int hostileSeconds = 20;
    // The wall clock in Unix seconds (the PK keeper's 48 h, DailyPK's reset at local midnight); null: the system's.
    // Tests move it.
    std::function<std::int64_t()> wallClock;
    // The skills' rolls' seed (tests: a fixed one makes a chance's outcome repeatable); 0: a random one.
    std::uint32_t skillSeed = 0;
    // The world: its settings, the jobs its zones step on (required for the world), the game's
    // archives for the navigation (null: no navigation, walks unchecked).
    WorldConfig world;
    core::JobSystem* jobs = nullptr;
    const assets::FileSystem* files = nullptr;
    std::chrono::seconds saveInterval{30};
    // How often the world's ticks are logged (WorldStats; 0: never).
    std::chrono::seconds statsInterval{10};
    // How often its load is sampled into sr_server_samples (the site's server page; 0: never).
    std::chrono::seconds sampleInterval{30};
    PeerLimits peers{std::chrono::milliseconds(10'000), std::chrono::milliseconds(15 * 60'000)};
};

class GameServer {
public:
    GameServer();
    ~GameServer();
    // Connects to the database (migrating it), clears this server's stale sessions, reports itself
    // and listens.
    bool Start(const GameServerConfig& config, std::string& error);
    void Poll(std::chrono::milliseconds wait);
    void Run(const std::atomic<bool>& stop);
    // Says goodbye in the database (listed as offline at once) and ends every session.
    void Stop();
    std::uint16_t Port() const;
    // Players past the ticket.
    std::size_t Online() const;
    // The world (null before Start).
    const World* GetWorld() const;
    // The guilds and unions (null before Start).
    const class Guilds* GetGuilds() const;
    // The fortress war (null without fortresses).
    const class Fortresses* GetFortresses() const;
    // The item mall (null without its catalogue).
    class Mall* GetMall();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace sr::server
