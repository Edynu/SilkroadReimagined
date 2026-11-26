#include "sr/game/MasteryGap.h"
#include "sr/server/ShardBus.h"
#include "sr/server/GameServer.h"

#include "sr/server/Alchemy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <format>
#include <sstream>
#include <map>
#include <numbers>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <queue>
#include <random>
#include <set>
#include <type_traits>
#include <utility>

#include "sr/core/Log.h"
#include "sr/core/Trace.h"
#include "sr/assets/Dress.h"
#include "sr/assets/Model.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Terrain.h"
#include "sr/assets/RegionInfo.h"
#include "sr/assets/UiLayout.h"
#include "sr/assets/StallNetworkData.h"
#include "sr/game/Combat.h"
#include "sr/game/Inventory.h"
#include "sr/game/Places.h"
#include "sr/game/Potions.h"
#include "sr/game/Skills.h"
#include "sr/game/Storage.h"
#include "sr/game/LobbyProtocol.h"
#include "sr/game/WorldProtocol.h"
#include "sr/game/StallProtocol.h"
#include "sr/game/BetaQuests.h"
#include "sr/game/QuestProtocol.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/EventProtocol.h"
#include "sr/game/FlagWorld.h"
#include "sr/game/CosProtocol.h"
#include "sr/game/JobProtocol.h"
#include "sr/game/Jobs.h"
#include "sr/game/Pk.h"
#include "sr/game/PkProtocol.h"
#include "sr/game/SupportProtocol.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/MallServices.h"
#include "sr/server/Database.h"
#include "sr/server/SupportStore.h"
#include "sr/server/GameLog.h"
#include "sr/server/Fortress.h"
#include "sr/game/Academy.h"
#include "sr/server/Academies.h"
#include "sr/server/OpenMarket.h"
#include "sr/server/Community.h"
#include "sr/server/Exchanges.h"
#include "sr/server/Coses.h"
#include "sr/server/Stalls.h"
#include "sr/server/Quests.h"
#include "sr/server/QuestStore.h"
#include "sr/server/SummonEvents.h"
#include "sr/game/QuestItems.h"
#include "sr/server/Instances.h"
#include "sr/server/FlagWorld.h"
#include "sr/server/Arena.h"
#include "sr/server/EventStore.h"
#include "sr/server/InstanceStore.h"
#include "sr/server/JobStore.h"
#include "sr/server/Leagues.h"
#include "sr/server/Mall.h"
#include "sr/server/MagicPop.h"
#include "sr/server/SilkStore.h"
#include "sr/server/ServiceStore.h"
#include "sr/game/Mall.h"
#include "sr/game/TimedServices.h"
#include "sr/game/SpecialItems.h"
#include "sr/game/GuildMercenary.h"
#include "sr/game/GuildRecall.h"
#include "sr/server/Trades.h"
#include "sr/server/Bandits.h"
#include "sr/server/Guilds.h"
#include "sr/server/Parties.h"
#include "sr/server/Skills.h"
#include "sr/server/AdminStore.h"
#include "sr/server/Store.h"

namespace sr::server {

namespace proto = game::proto;

namespace {

proto::LobbyStatus FromNameCheck(game::NameCheck check) {
    switch (check) {
    case game::NameCheck::Ok: return proto::LobbyStatus::Ok;
    case game::NameCheck::Forbidden: return proto::LobbyStatus::NameNotAllowed;
    case game::NameCheck::BadLength:
    case game::NameCheck::BadCharacter: break;
    }
    return proto::LobbyStatus::BadName;
}

proto::LobbyStatus FromChange(CharacterChange change) {
    switch (change) {
    case CharacterChange::Done: return proto::LobbyStatus::Ok;
    case CharacterChange::NotFound: return proto::LobbyStatus::NotFound;
    case CharacterChange::Deleting: return proto::LobbyStatus::Deleting;
    case CharacterChange::NotDeleting: return proto::LobbyStatus::NotDeleting;
    case CharacterChange::GuildMaster: return proto::LobbyStatus::GuildMaster;
    case CharacterChange::Failed: break;
    }
    return proto::LobbyStatus::Busy;
}

proto::CharacterSummary Summary(const CharacterRow& c) {
    proto::CharacterSummary s;
    s.model = c.model;
    s.name = c.name;
    s.height = c.height;
    s.level = c.level;
    s.experience = c.experience;
    s.strength = c.strength;
    s.intelligence = c.intelligence;
    s.statPoints = c.statPoints;
    s.hp = c.hp;
    s.mp = c.mp;
    s.deleting = c.deleting;
    s.deleteMinutes = c.deleting ? std::uint32_t((std::max<std::int64_t>(c.deleteSeconds, 0) + 59) / 60) : 0;
    s.guildRank = c.guildRole;
    s.academyRank = c.academyRole;
    for (const CharacterRow::Item& item : c.equipment) s.equipment.push_back({item.item, item.plus});
    return s;
}

// What the game servers of a shard tell each other of chat.
enum : std::uint8_t { kChatBusWhisper = 1, kChatBusDelivered = 2, kChatBusGlobal = 3, kChatBusRefused = 4 };
struct ChatBusEnvelope {
    std::uint16_t from = 0, to = 0; // to 0: every server
    std::uint8_t kind = 0;
    std::uint32_t request = 0; // a whisper: the asker's number for it
    std::string target;        // a whisper: its name's key
    std::vector<std::uint8_t> line; // a proto::ChatLine
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("to", to);
        v("kind", kind);
        v("request", request);
        v("target", target, 64);
        v("line", line, 1024);
    }
};

// Names are unique ignoring case (the schema's name_key); they are AS.
std::string NameKey(std::string_view name) {
    std::string key(name);
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

// A chat text the server passes on: UTF-8, 1 to 100 characters, no control characters.
bool ChatTextOk(std::string_view text) {
    std::size_t characters = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        const std::size_t n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > text.size()) return false;
        for (std::size_t k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        if (n == 1 && (b < 0x20 || b == 0x7F)) return false;
        i += n;
        ++characters;
    }
    return characters >= 1 && characters <= proto::kChatCharacters;
}

// Where a character first enters: its race's town, where the create screen looked (ours,
// inferred: the original's start places are the server's), put down on the floor nearest the height.
WorldPlayer StartPlace(game::CreationRules::Race race) {
    WorldPlayer p;
    if (race == game::CreationRules::Race::Europe) {
        p.position = {assets::RegionId(79, 105), 335.0f, 140.0f, 1460.0f}; // Constantinople
    } else {
        p.position = {assets::RegionId(168, 98), 960.42f, 50.0f, 458.26f}; // Jangan
    }
    p.dropOnGround = true;
    return p;
}

} // namespace

struct GameServer::State : SkillHost, PartyHost, GuildHost, FortressHost, CommunityHost, AcademyHost, ExchangeHost, StallHost, QuestHost, InstanceHost, AlchemyHost, CosHost, BanditHost,
                           LeagueHost, TradeHost, MallHost, PopHost, FlagWorldHost, ArenaHost, SummonHost, OpenMarketHost, RocHost, TempleHost,
                           QinTombHost {
    GameServerConfig config;
    std::unique_ptr<net::NetService> net;
    std::unique_ptr<net::Listener> listener;
    std::unique_ptr<Completions> completions;
    DatabasePool database;
    std::unique_ptr<Peers> peers;
    std::size_t online = 0;
    Clock::time_point nextReport;
    bool reporting = false;
    // The world, and each player's way into it.
    std::unique_ptr<World> world;
    std::unique_ptr<Monsters> monsters;
    std::unique_ptr<Guilds> guilds; // guilds and unions (Guilds.h)
    std::unique_ptr<Fortresses> fortresses; // the fortress war (Fortress.h; null without fortresses)
    struct PlayerWorld {
        std::int64_t selected = 0; // the character the lobby selected
        // Its counted deeds (sr_char_records; the beta quests' `start record`): loaded with it, raised by AddDeed.
        std::map<std::string, int> deeds;
        bool moving = false;  // leaving for another game server of the shard
        bool arrived = false; // come from one, on its way into the world
        std::uint32_t entity = 0;  // in the world
        bool entering = false;
        // The temples' board icon (proto::TempleIcon): shown (1), hidden (0), not decided yet (-1); in the temples last seen.
        std::int8_t templeIcon = -1;
        bool inTemples = false;
        // The weather it was last told (proto::WeatherNow, as game::ClimateWeather::Packed); 0 none yet.
        std::uint16_t weatherSent = 0;
        // When it is next told the world's clock (proto::WorldClock); none yet: its place in the round is given at its first tick.
        std::chrono::steady_clock::time_point clockNext{};
        // Its account's trade and chat punishments (loaded at the entry; RestrictionOf, Restricted).
        AccountRestrictions restrictions;
        // The system menu's Restart / Exit: counting down until it leaves.
        bool loggingOut = false;
        proto::LogoutKind logoutKind = proto::LogoutKind::Restart;
        Clock::time_point logoutAt;
        // A trader league member's safe trade (none for the others), and its saves' order: the
        // database's threads may run them in any order, so each carries a number and only the newest writes.
        std::optional<SafeTradeRow> safeTrade;
        struct SaveOrder {
            std::mutex mutex;
            std::uint64_t issued = 0, written = 0;
        };
        std::shared_ptr<SaveOrder> safeTradeSaves = std::make_shared<SaveOrder>();
        // Its connection gone, the character still in the world until `logoutAt` (the original's dropped connection,
        // 5 s, 20 s in combat in a suit, 60 s with goods in a COS).
        bool dropped = false;
        // A dropped connection's session, held until the character has left and its saves have landed.
        std::int64_t heldAccount = 0, heldSession = 0;
        // The storage keeper the account's storage was opened at: every move and gold move checks it is still near.
        std::uint32_t storageKeeper = 0;
        std::uint32_t guildChestNpc = 0; // the NPC the guild's storage was opened at
        // Its name in the world (whispers find it by it), and its chat: when its lines came, until
        // when it may not chat.
        std::string name;
        std::uint32_t model = 0; // its characterdata RefObjID
        float itemLuck = 0.0f;   // its avatars' luck ('luca', 0xAD): the strengthening's, beside its buffs' (UpdateStats)
        // A weapon in its hand (unbroken, counting) at its last numbers; the bare hands' reinforcements once one left it
        // since it came in (taken off or broken: game::BareHands, the original's quirk).
        bool armed = false;
        game::BareHands bareHands = game::BareHands::AsEntered;
        std::array<int, 13> setRaise{}; // each worn place's set raise of its total plus at its last numbers (UpdateStats)
        std::deque<std::pair<Clock::time_point, proto::ChatKind>> chatTimes;
        Clock::time_point chatAllowedAt;
        // Its items, and who wears them (the checks on wearing).
        game::Inventory inventory;
        game::Wearer wearer;
        // Its worn sets as the original's set operator keeps them (UpdateStats plays it).
        game::SetWear sets;
        // Its Devil's Spirit: whether its contributions are on (worn, awake, a row for its
        // plus), the skills it gives (among book.itemSkills), when its awakening is next looked at.
        bool spiritOn = false;
        std::vector<std::uint32_t> spiritSkills;
        Clock::time_point spiritCheckAt{};
        std::uint32_t returnPoint = 0; // designated at a gate (0: none yet)
        // A return scroll read: when it takes the character to its return point (none: not reading).
        std::optional<Clock::time_point> returnAt;
        // The guild recall answered yes: where returnAt takes it instead (the recall's gauge, then the master's place).
        std::optional<proto::WorldPosition> recallTo;
        // A reverse return scroll read: where it goes when returnAt comes (its
        // option: proto::kReverseTo*, 0 an ordinary return scroll) and the map's point for option 7.
        std::uint8_t reverseOption = 0;
        bool toThiefDen = false; // the scroll read is the thieves' town's (its transport comes along)
        std::uint32_t thiefDenPoint = 0; // where that scroll leads: the point of the building its Param4 names
        std::uint32_t reversePoint = 0;
        // The places it goes back to: where it last read a return scroll, where it last died (region 0: none).
        CharacterRow::Spot recallSpot, deathSpot;
        std::optional<Clock::time_point> warDeathAt; // died in a fortress's area during its war
        // The account's storage once a keeper opened it (loaded on the first open of the session).
        std::optional<game::Storage> storage;
        bool storageLoading = false;
        std::uint32_t guildChest = 0; // the guild whose storage it has open (0: none)
        // What it sold at shops this session (the last 5), and what each costs to buy back.
        struct Sold {
            game::ItemInstance item;
            std::uint64_t price = 0;
        };
        std::vector<Sold> sold;
        // A ground item it is walking to (picked up on arrival; any new command cancels it).
        std::uint32_t pickTarget = 0;
        // Fighting (C10b): its numbers (from its level, STR / INT and what it wears), its health and
        // mana; dead until it rises. Its fight, its skills' waits and its buffs: `combat` (Skills.h).
        int level = 1, strength = 20, intelligence = 20;
        int maxLevel = 1; // the highest level it reached: a level reached again after a death's level down gives no points
        std::uint64_t experience = 0;
        std::uint32_t skillPoints = 0, skillExperience = 0;
        std::uint32_t guildPointsDue = 0; // skill points earned for the guild, not yet given (by ten)
        std::uint16_t statPoints = 0;
        bool progressDirty = false; // saved with the places, or on leaving
        game::SkillBook book;       // its masteries and skills (C7f)
        game::CombatStats stats;
        std::uint32_t hp = 0, mp = 0;
        bool dead = false;
        bool hidden = false; // stealth or invisibility (Skills::StealthOf): monsters cannot target it
        // What its last death took (a resurrection gives a share of it back;
        // K9) and the resurrection offered to it (who, which skill), until it rises.
        std::uint64_t deathLoss = 0;
        // The skills of its buffs as those who see it were last told them (EntityBuffs).
        std::vector<std::uint32_t> toldBuffs;
        struct Offer {
            std::uint32_t caster = 0;
            const assets::SkillRow* skill = nullptr;
        };
        std::optional<Offer> revivalOffer;
        // Risen from death: untouchable until then (no monster takes it on, it attacks nobody;
        // the original's "Cannot attack or be attacked for 5 seconds during untouchable status").
        Clock::time_point untouchableUntil;
        // Knocked by another player's hit: down or back until then; no walk, no skill meanwhile
        // (refuses a move in the knock's motions 8, 0x10, 0x12).
        game::Knock knocked = game::Knock::None;
        Clock::time_point knockedUntil;
        Clock::time_point lastHurt, nextRegen;
        // The recovery items: the kinds still locked (game::ReuseLock's
        // bits) and until when; a flat potion's steps still to come, HP and MP each (the original's
        // queue).
        std::map<std::uint32_t, Clock::time_point> reuseLocks;
        struct PotionSteps {
            std::uint32_t step = 0;
            int left = 0;
            Clock::time_point next;
        };
        std::vector<PotionSteps> hpSteps, mpSteps;
        // Its auto potion: the settings it keeps, and by row when a row that holds may be tried again
        // (none: it did not hold at the last look, so it is tried at once when it does).
        game::AutoPotion autoPotion;
        // Its job's league and alias (_chartrijob, _char.nickname16) and the job's state the suit in
        // place 8 gives (game::kNoJob without one).
        game::JobInfo job;
        std::string alias;
        std::uint8_t jobState = game::kNoJob;
        std::int64_t jobLeftAt = 0; // when it last left a league (Unix seconds; 0 none within 7 days)
        // Its job suit going on or coming off: the move made when the 10 s are over.
        struct SuitChange {
            proto::ItemMove move;
            bool on = true;
            Clock::time_point at;
        };
        std::optional<SuitChange> suitChange;
        Clock::time_point jobLockUntil{}; // once on: 10 s without taking it off (nor, step 6, attacking players)
        std::array<std::optional<Clock::time_point>, game::AutoPotion::kRows> autoNext{};
        SkillState combat;
        // Berserk (game/Berserk.h): its gauge (kept in _char.remainhwancount), its hwan level (the
        // mode's row), and until when it is in the mode (none: not in it).
        std::uint8_t berserkPoints = 0;
        int hwanLevel = 0;
        // The Action page: its motion (proto::kMotionStand or kMotionSit, or
        // kMotionChanging towards motionTo until motionUntil), its moves' pace, the marks over its head, and
        // the player it traces (0 none) with when the trace looks again.
        std::uint8_t motion = proto::kMotionStand, motionTo = proto::kMotionStand;
        Clock::time_point motionUntil;
        bool walking = false;
        std::uint8_t marks = 0;
        std::uint32_t stallSkin = 0; // its stall's decoration (_StaticAvatar.Param1)
        // The item mall's services: its time tickets (categories 3, 9), the premium package
        // (category 10) and whether its booth was chosen, the pet growth potion's end (Unix seconds; 0 none).
        std::vector<game::TimedService> services;
        game::PremiumState premium;
        bool premiumBooth = false;
        std::int64_t growthUntil = 0;
        std::uint32_t growthItem = 0;
        // The bonus scrolls running (3/3/3/10): one by key, its end on the wall clock (Unix
        // seconds; kept in `_timedjob`, ours: a category by key).
        struct BonusRun {
            game::BonusKind kind = game::BonusKind::None;
            std::uint32_t key = 0, item = 0;
            int value = 0;
            std::int64_t until = 0;
        };
        std::vector<BonusRun> bonuses;
        // The items' COOLTIME waits running (game::CoolTimeOf): by game::CoolTimeKey (the group, or the item for group 0),
        // until then.
        std::unordered_map<std::uint32_t, Clock::time_point> coolTimes;
        Clock::time_point servicesTick, servicesSaved, servicesTold;
        // Free its team (0 none, 1..5), and a change under way to `freePvpTo` until then.
        std::uint8_t freePvp = 0, freePvpTo = 0;
        std::uint32_t labMaxHp = 0; // the skill lab's most health for it (DevVitals; 0: its own)
        bool riding = false; // on a ride or a transport (Coses.h)
        std::optional<Clock::time_point> freePvpAt;
        std::uint32_t traceTarget = 0;
        Clock::time_point nextTrace;
        std::optional<Clock::time_point> berserkUntil;
        // Murderers: its PvP state (game::PvpState), the three counts kept in _char, its
        // hostility records (the players it struck: entity -> seconds left, 20 on each hit) and the PK
        // keeper running (`_TimedJob` 2/0: the seconds left of its period at its stamp, Unix seconds).
        std::uint8_t pvpState = 0;
        std::uint8_t dailyPk = 0;
        std::uint16_t totalPk = 0;
        std::int32_t pkPoints = 0;
        std::int64_t honorPoints = 0; // the academy's (_TrainingCampMember), a shop good's price
        std::unordered_map<std::uint32_t, int> hostility;
        std::optional<CharacterRow::PkKeeper> pkKeeper;
    };
    // Items on the ground (C7i), by entity: the item (or gold), whose it is until when (0: anyone's),
    // when it goes. Each goes after its time through a queue ordered by that time: a tick looks only
    // at the items whose time has come (the original's server sweeps them all).
    struct GroundItem {
        game::ItemInstance item; // refId 0: gold
        std::uint64_t gold = 0;
        std::int64_t owner = 0; // a character id
        Clock::time_point ownerUntil, expires;
        bool ownerOnly = false; // a quest item dropped for its quest's holder: nobody else picks it up
    };
    std::unordered_map<std::uint32_t, GroundItem> ground;
    std::priority_queue<std::pair<Clock::time_point, std::uint32_t>, std::vector<std::pair<Clock::time_point, std::uint32_t>>,
                        std::greater<>>
        groundExpiry;
    // The drops kept for their owner, by when they are anyone's (GroundItemFree to those who see them then).
    std::priority_queue<std::pair<Clock::time_point, std::uint32_t>, std::vector<std::pair<Clock::time_point, std::uint32_t>>,
                        std::greater<>>
        groundOwned;
    static constexpr auto kGroundLifetime = std::chrono::seconds(180); // ours: for a row without a DecayTime
    // How long an item lies on the ground: its row's DecayTime (ms; dropped specialty goods 30 minutes), gold its gold
    // row's (`model`, ITEM_ETC_GOLD_*).
    Clock::duration GroundLife(const game::ItemInstance& item, std::uint32_t model) const {
        const std::uint32_t ref = item.refId != 0 ? item.refId : model;
        const assets::ItemRow* row = ref != 0 && config.rules->items ? config.rules->items->Find(ref) : nullptr;
        if (row != nullptr && row->decayMs > 0) return std::chrono::milliseconds(row->decayMs);
        return kGroundLifetime;
    }
    // The pick-up's reach: 10, compared inclusively (the original's); ours allows 5 more for where a walk on the
    // navigation stops.
    static constexpr float kPickUpReach = 15.0f;
    std::mt19937 groundRandom{std::random_device{}()};
    // The connections' session tokens (Enter): random, so no two connections of the shard share one.
    std::mt19937_64 sessionTokens{(std::uint64_t(std::random_device{}()) << 32) ^ std::random_device{}()};

    // A kill's drops around where the monster fell, kept 30 s for the one who did it most harm (ours:
    // the original's time is not traced); each item's look its ground model, gold a pouch by its amount.
    static constexpr auto kDropOwned = std::chrono::seconds(30);
    // The byte the client colours a ground item's name by: the item's
    // rarity, | 1 for equipment with magic options (an ordinary one with blues: 1, light blue).
    std::uint8_t GroundState(const game::ItemInstance& item) const {
        const assets::ItemRow* row = item.refId != 0 && config.rules->items ? config.rules->items->Find(item.refId) : nullptr;
        if (row == nullptr) return 0;
        const game::ItemKind kind = game::KindOf(*row);
        const bool equipment = kind == game::ItemKind::Weapon || kind == game::ItemKind::Shield || kind == game::ItemKind::Armour ||
                               kind == game::ItemKind::Accessory;
        return std::uint8_t(row->rarity | (equipment && !item.options.empty() ? 1 : 0));
    }
    // A quest or event item (3/3/9) on the ground: the name of the player it belongs to, which the 1.188 client shows in
    // brackets after the item's (upstream's review 2026-11-22; whose name the original sends for these is not known:
    // ours, its owner's).
    static std::string_view OwnedItemName(const assets::ItemRow& row, std::string_view owner) {
        return row.typeId[0] == 3 && row.typeId[1] == 3 && row.typeId[2] == 9 ? owner : std::string_view();
    }
    // `rarity`: the dropper's (its grade picks the scatter's reach).
    void DropLoot(const proto::WorldPosition& at, std::int64_t owner, std::vector<DropTable::Drop> drops, std::uint8_t rarity = 0) {
        std::uniform_real_distribution<float> turn(0.0f, 6.2831853f);
        // Each item a random angle and a distance drawn evenly between the least and the most: 8..20; a unique's or a
        // grade 8's 10..30, a giant's 10..40, a titan's 20..100 (the original's by the dropper's kind).
        const int grade = rarity & 0x0F;
        const float least = grade == 3 || grade == 8 || grade == 4 ? 10.0f : grade == 5 ? 20.0f : 8.0f;
        const float most = grade == 3 || grade == 8 ? 30.0f : grade == 4 ? 40.0f : grade == 5 ? 100.0f : 20.0f;
        std::uniform_real_distribution<float> reach(least, most);
        const proto::FlatPosition centre = proto::ToFlat(at);
        std::string ownerName;
        std::uint32_t ownerEntity = 0;
        for (const auto& [peerId, p] : players) {
            if (owner != 0 && p.selected == owner && p.entity != 0) {
                ownerName = p.name;
                ownerEntity = p.entity;
            }
        }
        for (std::size_t i = 0; i < drops.size(); ++i) {
            DropTable::Drop& d = drops[i];
            std::uint32_t model = d.item.refId != 0 ? d.item.refId : GoldModel(d.gold);
            if (model == 0) continue;
            const float angle = turn(groundRandom), radius = reach(groundRandom);
            const proto::WorldPosition there = proto::FromFlat({centre.x + radius * std::sin(angle), centre.z + radius * std::cos(angle)}, at.y);
            const std::uint32_t amount = d.item.refId != 0 ? d.item.count : std::uint32_t(std::min<std::uint64_t>(d.gold, 0xFFFFFFFFu));
            // Specialty goods carry their owner's name, the alias they were bought under (the user's account of the
            // original, 2026-11-22: its owner's name shown on them).
            const assets::ItemRow* row = d.item.refId != 0 && config.rules->items ? config.rules->items->Find(d.item.refId) : nullptr;
            const std::string_view name = row == nullptr                   ? std::string_view()
                                          : game::IsSpecialtyGood(*row) ? std::string_view(d.item.creator)
                                                                        : OwnedItemName(*row, ownerName);
            const std::uint32_t id = world->AddGroundItem(model, there, turn(groundRandom), amount, d.item.plus, GroundState(d.item), name, ownerEntity);
            const Clock::duration life = GroundLife(d.item, model);
            GroundItem g{std::move(d.item), d.gold, owner, Clock::now() + kDropOwned, Clock::now() + life};
            groundExpiry.push({g.expires, id});
            if (ownerEntity != 0) groundOwned.push({g.ownerUntil, id});
            ground[id] = std::move(g);
        }
    }

    // An item onto the ground near a character's feet (a little apart, so drops do not stack).
    void PutOnGround(const PlayerWorld& player, game::ItemInstance item, std::uint64_t gold, std::uint32_t model) {
        const auto place = world->Where(player.entity);
        if (!place) return;
        std::uniform_real_distribution<float> spread(-12.0f, 12.0f), turn(0.0f, 6.2831853f);
        const proto::FlatPosition at = proto::ToFlat(place->position);
        const proto::WorldPosition there = proto::FromFlat({at.x + spread(groundRandom), at.z + spread(groundRandom)}, place->position.y);
        const std::uint32_t amount = item.refId != 0 ? item.count : std::uint32_t(std::min<std::uint64_t>(gold, 0xFFFFFFFFu));
        const std::uint32_t id = world->AddGroundItem(model, there, turn(groundRandom), amount, item.plus, GroundState(item));
        const Clock::duration life = GroundLife(item, model);
        GroundItem g{std::move(item), gold, 0, {}, Clock::now() + life};
        groundExpiry.push({g.expires, id});
        ground[id] = std::move(g);
    }

    // The ground items whose time has come: gone for everyone near. The drops whose owner's time is over: anyone's,
    // told to those who see them.
    void ExpireGround() {
        const auto now = Clock::now();
        while (!groundOwned.empty() && groundOwned.top().first <= now) {
            const std::uint32_t id = groundOwned.top().second;
            groundOwned.pop();
            if (!ground.contains(id) || !world->FreeGroundItem(id)) continue; // picked up or gone already
            const std::vector<std::byte> bytes = proto::Encode(proto::GroundItemFree{id});
            world->ForEachObserver(id, [&](std::uint64_t to) {
                if (Peer* p = peers->Find(to)) p->connection->Send(proto::GroundItemFree::kId, bytes);
            });
        }
        while (!groundExpiry.empty() && groundExpiry.top().first <= now) {
            const std::uint32_t id = groundExpiry.top().second;
            groundExpiry.pop();
            const auto it = ground.find(id);
            if (it == ground.end() || it->second.expires > now) continue; // picked up already
            ground.erase(it);
            world->Leave(id);
        }
    }

    // Gold on the ground looks like a pouch by its amount (ours: the thresholds are not traced).
    std::uint32_t GoldModel(std::uint64_t gold) const {
        const char* code = gold < 1000 ? "ITEM_ETC_GOLD_01" : gold < 10000 ? "ITEM_ETC_GOLD_02" : "ITEM_ETC_GOLD_03";
        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(std::string_view(code)) : nullptr;
        return row != nullptr ? row->refObjId : 0u;
    }

    // An item of the bag dropped (the original asks first; worn items are refused before asking).
    void DropItem(Peer& peer, PlayerWorld& player, const proto::DropItem& drop) {
        proto::ItemMoved answer;
        answer.from = answer.to = drop.place;
        auto it = drop.place >= game::slot::kWorn ? player.inventory.places.find(drop.place) : player.inventory.places.end();
        if (it == player.inventory.places.end()) {
            answer.status = proto::ItemStatus::NoItem;
            SendMessage(*peer.connection, answer);
            return;
        }
        if (it->second.bound) { // a bound item is never dropped (the beta quests' weapons)
            answer.status = proto::ItemStatus::Bound;
            SendMessage(*peer.connection, answer);
            return;
        }
        // Its CanDrop (a mall item, a quest item, a pet's scroll stay in the bag): 0x1838.
        if (const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(it->second.refId) : nullptr; row != nullptr && !game::CanDropByHand(*row)) {
            SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, 0x1838}); // UIIT_MSG_STRGERR_CANNOT_BE_DROPPED
            answer.status = proto::ItemStatus::Failed;
            SendMessage(*peer.connection, answer);
            return;
        }
        game::ItemInstance item = std::move(it->second);
        const std::int64_t serial = item.serial;
        player.inventory.places.erase(it);
        answer.emptied.push_back(drop.place);
        SendMessage(*peer.connection, answer);
        QueueItemSave(player.selected, player.inventory, {drop.place}, {serial});
        LogItem(player, logs::ItemOp::Drop, item.refId, serial, item.count, drop.place, logs::kNoSlot);
        item.serial = 0;
        const std::uint32_t model = item.refId;
        core::LogInfo("{}: {} drops item {} x{}", config.name, player.name, model, item.count);
        PutOnGround(player, std::move(item), 0, model);
        if (quests) quests->BagChanged(player.entity);
    }

    void DropGold(Peer& peer, PlayerWorld& player, const proto::DropGold& drop) {
        // None: 0x1829; more than 100,000,000 at once: refused (the original's 2); more than it holds: 0x180F.
        if (drop.amount == 0) return SendMessage(*peer.connection, proto::ItemRefused{0, 0x1829});
        if (drop.amount > 100'000'000u) return SendMessage(*peer.connection, proto::ItemMoved{proto::ItemStatus::Failed});
        if (drop.amount > player.inventory.gold) return SendMessage(*peer.connection, proto::ItemRefused{0, 0x180F});
        const std::uint32_t model = GoldModel(drop.amount);
        if (model == 0) return;
        player.inventory.gold -= drop.amount;
        SendMessage(*peer.connection, proto::GoldChanged{player.inventory.gold});
        QueueGoldSave(player.selected, player.inventory.gold);
        LogGold(player, logs::ItemOp::DropGold, std::int64_t(drop.amount));
        PutOnGround(player, {}, drop.amount, model);
    }

    // A ground item picked up: near enough, anyone's (or this one's), room in the bag.
    // A ground item asked for: picked up now when within reach, else walked to first (FinishPickups).
    void PickUp(Peer& peer, PlayerWorld& player, const proto::PickUp& pick) {
        const auto me = world->Where(player.entity);
        const auto there = world->Where(pick.entity);
        if (!ground.contains(pick.entity) || !me || !there) {
            proto::ItemMoved answer;
            answer.status = proto::ItemStatus::NoItem;
            SendMessage(*peer.connection, answer);
            return;
        }
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
        if (std::hypot(a.x - b.x, a.z - b.z) > kPickUpReach) {
            player.pickTarget = pick.entity;
            world->MoveTo(player.entity, there->position);
            return;
        }
        TakeFromGround(peer, player, pick.entity);
    }

    // The walks to ground items that have ended: picked up when they got there.
    void FinishPickups() {
        for (auto& [peerId, player] : players) {
            if (player.pickTarget == 0 || player.entity == 0 || world->Moving(player.entity)) continue;
            const std::uint32_t target = std::exchange(player.pickTarget, 0u);
            if (Peer* peer = peers->Find(peerId)) TakeFromGround(*peer, player, target);
        }
    }

    void TakeFromGround(Peer& peer, PlayerWorld& player, std::uint32_t entity) {
        const proto::PickUp pick{entity};
        proto::ItemMoved answer;
        const auto it = ground.find(pick.entity);
        const auto me = world->Where(player.entity);
        const auto there = world->Where(pick.entity);
        const auto refuse = [&](proto::ItemStatus status) {
            answer.status = status;
            SendMessage(*peer.connection, answer);
            const char* why = status == proto::ItemStatus::NoItem     ? "gone"
                              : status == proto::ItemStatus::TooFar   ? "too far"
                              : status == proto::ItemStatus::NotYours ? "kept for another"
                              : status == proto::ItemStatus::BagFull  ? "the bag is full"
                                                                      : "failed";
            core::LogInfo("{}: {} cannot pick up ground item {}: {}", config.name, player.name, pick.entity, why);
        };
        if (it == ground.end() || !me || !there) return refuse(proto::ItemStatus::NoItem);
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
        if (std::hypot(a.x - b.x, a.z - b.z) > kPickUpReach) return refuse(proto::ItemStatus::TooFar);
        GroundItem& g = it->second;
        if (g.ownerOnly && g.owner != player.selected) return refuse(proto::ItemStatus::NotYours);
        // Its CanPick (fireworks, a campfire, a quest's weapon stay where they are): 0x1839.
        if (const assets::ItemRow* row = g.item.refId != 0 && config.rules->items ? config.rules->items->Find(g.item.refId) : nullptr; row != nullptr && row->canPick == 0) {
            SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, 0x1839}); // UIIT_MSG_STRGERR_CANNOT_BE_PICKED
            return refuse(proto::ItemStatus::Failed);
        }
        // A quest's item while every gathering goal for it holds its count (the original's event 0x0F to the quests):
        // 0x1874 UIIT_MSG_STRGERR_CANNOT_BE_PICKED_AT_QUEST_ACHIEVE_CONDITION.
        if (const assets::ItemRow* row = g.item.refId != 0 && config.rules->items ? config.rules->items->Find(g.item.refId) : nullptr;
            row != nullptr && quests && quests->PickUpRefused(player.entity, row->codename)) {
            SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, 0x1874});
            return refuse(proto::ItemStatus::Failed);
        }
        // Specialty goods: a trader only those his alias bought (0x1863), a thief
        // only those he did not (0x186F), a hunter none (0x1863), no job none (0x1864); not after the thieves' town
        // scroll is read (0x186E); a transport out needed (0x1861); into it, as a new item with the old creator.
        if (const assets::ItemRow* good = g.item.refId != 0 && config.rules->items ? config.rules->items->Find(g.item.refId) : nullptr;
            good != nullptr && game::IsSpecialtyGood(*good)) {
            const auto refuseCode = [&](std::uint16_t code) {
                answer.code = code;
                refuse(proto::ItemStatus::JobSuit);
            };
            const bool mine = !player.alias.empty() && g.item.creator == player.alias;
            if (player.jobState == kNoJob) return refuseCode(0x1864);
            if (player.jobState == std::uint8_t(game::JobType::Hunter)) return refuseCode(0x1863);
            if (player.jobState == std::uint8_t(game::JobType::Trader) && !mine) return refuseCode(0x1863);
            if (player.jobState == std::uint8_t(game::JobType::Thief) && mine) return refuseCode(0x186F);
            if (player.returnAt && player.toThiefDen) return refuseCode(0x186E);
            const auto cart = coses ? coses->TransportCart(player.entity) : std::nullopt;
            if (!cart || cart->bag == nullptr) return refuseCode(0x1861);
            int place = -1;
            for (int p = 0; p < cart->size && place < 0; ++p) {
                if (!cart->bag->contains(std::uint8_t(p))) place = p;
            }
            if (place < 0) return refuse(proto::ItemStatus::BagFull);
            game::ItemInstance taken = g.item;
            taken.serial = 0;
            (*cart->bag)[std::uint8_t(place)] = std::move(taken);
            coses->CartChanged(cart->entity);
            if (trades) trades->CargoChanged(player.entity);
            if (game::IsBargainGood(*good)) ResetSafeTrade(player); // bargain goods off the ground
            core::LogInfo("{}: {} picks up {} x{} into its transport (made by {})", config.name, player.name, good->codename, g.item.count, g.item.creator);
            ground.erase(it);
            world->Leave(pick.entity);
            const std::vector<std::byte> bytes = proto::Encode(proto::EntityAction{player.entity, proto::EntityActionKind::PickUp});
            world->ForEachObserver(player.entity, [&](std::uint64_t to) {
                if (Peer* p = peers->Find(to)) p->connection->Send(proto::EntityAction::kId, bytes);
            });
            return;
        }
        // Kept for another, unless both are of a party sharing items.
        if (g.owner != 0 && g.owner != player.selected && Clock::now() < g.ownerUntil &&
            !(parties && parties->SharesItems(std::uint32_t(g.owner), std::uint32_t(player.selected)))) {
            return refuse(proto::ItemStatus::NotYours);
        }
        // Who it goes to: the picker, or in a party sharing items the members near (Parties.h): gold
        // split among them, an item to the one whose turn it is.
        const auto receiver = [&](std::uint32_t entity) -> std::pair<PlayerWorld*, Peer*> {
            if (entity == player.entity) return {&player, &peer};
            PlayerWorld* to = PlayerByEntity(entity);
            Peer* toPeer = peers->Find(world->PeerOf(entity));
            return to != nullptr && toPeer != nullptr ? std::pair{to, toPeer} : std::pair{&player, &peer};
        };
        if (g.item.refId == 0) {
            const std::vector<std::pair<std::uint32_t, std::uint64_t>> split =
                parties ? parties->SplitGold(player.entity, g.gold) : std::vector<std::pair<std::uint32_t, std::uint64_t>>{{player.entity, g.gold}};
            for (const auto& [entity, amount] : split) {
                auto [to, toPeer] = receiver(entity);
                to->inventory.gold += amount;
                SendMessage(*toPeer->connection, proto::GoldChanged{to->inventory.gold});
                QueueGoldSave(to->selected, to->inventory.gold);
                if (split.size() > 1) {
                    parties->TellLoot(to->entity, 0, 0, amount);
                } else {
                    SendMessage(*toPeer->connection, proto::ItemGained{0, 0, amount}); // "[%d] gold gained."
                }
            }
        } else {
            auto [to, toPeer] = receiver(parties && !g.ownerOnly ? parties->LootRecipient(player.entity) : player.entity);
            const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(g.item.refId) : nullptr;
            const int place = row != nullptr ? PlaceFor(to->inventory, *row, g.item.refId, g.item.count) : -1;
            if (place < 0) return refuse(proto::ItemStatus::BagFull);
            if (!Put(*to, place, g.item)) return refuse(proto::ItemStatus::Failed);
            answer.from = answer.to = std::uint8_t(place);
            answer.changed.push_back(to->inventory.Record(std::uint8_t(place)));
            SendMessage(*toPeer->connection, answer);
            QueueItemSave(to->selected, to->inventory, {std::uint8_t(place)}, {});
            if (const game::ItemInstance* got = to->inventory.At(std::uint8_t(place)))
                LogItem(*to, logs::ItemOp::PickUp, got->refId, got->serial, g.item.count, logs::kNoSlot, std::uint8_t(place));
            // The party hears what went to whom (the original's loot notice for a party's pick-ups); out of one, the
            // picker its "item [...] gained." line.
            const std::uint16_t count = std::uint16_t(std::min<std::uint32_t>(g.item.count, 0xFFFF));
            if (parties && parties->PartyOf(to->entity) != 0) {
                parties->TellLoot(to->entity, g.item.refId, count, 0);
            } else {
                SendMessage(*toPeer->connection, proto::ItemGained{g.item.refId, count, 0});
            }
            if (quests) quests->BagChanged(to->entity); // a quest's gathered item counted
        }
        core::LogInfo("{}: {} picks up ground item {}", config.name, player.name, pick.entity);
        ground.erase(it);
        world->Leave(pick.entity);
        // The bend for it, for everyone who sees the character (the player too).
        const std::vector<std::byte> bytes = proto::Encode(proto::EntityAction{player.entity, proto::EntityActionKind::PickUp});
        world->ForEachObserver(player.entity, [&](std::uint64_t to) {
            if (Peer* p = peers->Find(to)) p->connection->Send(proto::EntityAction::kId, bytes);
        });
    }

    // The NPCs in the world, by entity.
    std::unordered_map<std::uint32_t, const GameServerConfig::NpcSpawn*> npcs;

    // What an NPC offers (ours, by its codename as the original's server decides it, not from data): the storage
    // keepers' storage for now; shops come with the stores.
    std::uint32_t NpcServices(const std::string& codename, std::uint32_t refObjId = 0) const {
        // Every other NPC talks for now (quests come later); a guild NPC has only its shop and its guild
        // rows (functions 1 and 15; the talk row is function 2).
        std::uint32_t services = game::IsGuildNpcCodename(codename) ? proto::kNpcGuild : proto::kNpcTalk;
        if (config.shops && config.shops->Find(codename) != nullptr) services |= proto::kNpcShop;
        if (codename.find("_WAREHOUSE") != std::string::npos) services |= proto::kNpcStorage;
        if (fortresses && codename.ends_with("_FORTRESS_OFFICIAL")) services |= proto::kNpcFortressOfficial;
        if (fortresses && codename.find("_FORTRESS_BATTLEAIDE") != std::string::npos) services |= proto::kNpcFortressBattleAide;
        if (fortresses && codename.find("_FORTRESS_MANAGER") != std::string::npos) services |= proto::kNpcFortressManager;
        if (fortresses && codename.find("_GATE_PULLEY_") != std::string::npos) services |= proto::kNpcFortressPulley;
        if (fortresses && codename.find("_FORTRESS_SMITH") != std::string::npos) services |= proto::kNpcFortressSmith;
        if (fortresses && codename.find("_FORTRESS_TRAINER") != std::string::npos) services |= proto::kNpcFortressTrainer;
        if (fortresses && codename == "NPC_SIEGE_DUNGEON_TELEPORT") services |= proto::kNpcFortressDungeon;
        // Magic POP's machine (NPC function 0x11: a `_RefGachaNpcMap` row) and Gori (0x12).
        if (magicPop && refObjId != 0 && magicPop->IsMachine(refObjId)) services |= proto::kNpcPopMachine;
        if (magicPop && MagicPop::IsGori(codename)) services |= proto::kNpcPopExchange;
        if (game::IsSpecialtyNpc(codename)) services |= proto::kNpcTrade; // its price board's row
        if (game::IsRepairNpc(codename)) services |= proto::kNpcRepair;  // the store's Repair and Repair all
        if (game::IsAvatarGrantNpc(codename)) services |= proto::kNpcAvatarGrant; // the avatars' blessing
        // A league's keeper (functions 0x14 / 0x15 / 0x16): its rows.
        switch (game::LeagueOfNpc(codename)) {
        case game::JobType::Trader: services |= proto::kNpcJobTrader; break;
        case game::JobType::Thief: services |= proto::kNpcJobThief; break;
        case game::JobType::Hunter: services |= proto::kNpcJobHunter; break;
        default: break;
        }
        return services;
    }

    // A storage keeper near the player (the talk's checks), or why not.
    proto::GateStatus AtStorageKeeper(const PlayerWorld& player, std::uint32_t entity) const {
        const auto it = npcs.find(entity);
        const auto me = world->Where(player.entity);
        const auto npc = world->Where(entity);
        if (it == npcs.end() || !npc || (NpcServices(it->second->codename) & proto::kNpcStorage) == 0) return proto::GateStatus::NoSuchGate;
        const proto::FlatPosition a = me ? proto::ToFlat(me->position) : proto::FlatPosition{}, b = proto::ToFlat(npc->position);
        return me && std::hypot(a.x - b.x, a.z - b.z) <= config.gateReach ? proto::GateStatus::Ok : proto::GateStatus::TooFar;
    }

    void SendStorage(Peer& peer, const game::Storage& s, std::uint8_t guild = 0) {
        proto::StorageList list;
        list.size = s.size;
        list.gold = s.gold;
        list.guild = guild;
        for (const auto& [place, item] : s.places) list.items.push_back(s.Record(place));
        SendMessage(*peer.connection, list);
    }
    game::Storage StorageOf(const StorageRow& row, std::uint8_t size) {
        game::Storage s;
        s.size = size;
        s.gold = row.gold;
        for (const StoredItem& i : row.items) {
            game::ItemInstance item;
            item.serial = i.id; // its _items row: kept wherever it is moved
            item.refId = i.refId;
            item.plus = i.plus;
            item.variance = i.variance;
            item.rentUntil = i.rentUntil;
            item.advanced = i.advanced;
            item.bound = i.bound;
            item.advancedRecipe = i.advancedRecipe;
            item.sockets = i.sockets;
            FromData(i, item);
            for (std::uint64_t m : i.magic) item.options.push_back({std::uint32_t(m & 0xFFFFFFFFu), std::uint32_t(m >> 32)});
            s.places[i.slot] = std::move(item);
        }
        return s;
    }

    // --- The guild's storage ---------------------------------------------

    // Each guild's storage: who has it open (one member at a time; the original's shard manager keeps
    // the lock), and its places once loaded (kept for the server's life, so the saves queued behind
    // each move are what the next opening sees).
    // A guild's storage: its holder here, its contents while this server holds its lock across the shard
    // (sr_guild_storage_lock; dropped with the lock: another server may change it then), and its
    // lock's work one task at a time in the order asked (the lock and the load, the release). A move's save is a
    // barrier submitted at once (SaveStorageMove), so it lands before the release asked after it.
    struct GuildChest {
        std::uint64_t user = 0; // the peer holding it
        std::string userName;
        std::optional<game::Storage> storage;
        std::deque<std::pair<std::function<void(Database&)>, std::function<void()>>> tasks;
        bool running = false;
    };
    std::unordered_map<std::uint32_t, GuildChest> guildChests;
    void ChestTask(std::uint32_t guild, std::function<void(Database&)> task, std::function<void()> done = {}) {
        GuildChest& c = guildChests[guild];
        c.tasks.emplace_back(std::move(task), std::move(done));
        if (!c.running) NextChestTask(guild);
    }
    void NextChestTask(std::uint32_t guild) {
        GuildChest& c = guildChests[guild];
        if (c.tasks.empty()) {
            c.running = false;
            return;
        }
        c.running = true;
        auto [task, done] = std::move(c.tasks.front());
        c.tasks.pop_front();
        database.Submit([this, guild, task = std::move(task), done = std::move(done)](Database& db) {
            task(db);
            completions->Post([this, guild, done] {
                if (done) done();
                NextChestTask(guild);
            });
        });
    }

    // At an NPC that keeps guilds near the player: the
    // member's checks (Guilds::StorageAccessOf), then its guild's storage, its size by the guild's level.
    void OpenGuildStorage(Peer& peer, PlayerWorld& player, std::uint32_t npc) {
        const auto it = npcs.find(npc);
        const auto me = world->Where(player.entity), there = world->Where(npc);
        if (it == npcs.end() || !there || (NpcServices(it->second->codename) & proto::kNpcGuild) == 0) {
            return RefuseGate(peer, npc, "guild storage", proto::GateStatus::NoSuchGate);
        }
        const proto::FlatPosition a = me ? proto::ToFlat(me->position) : proto::FlatPosition{}, b = proto::ToFlat(there->position);
        if (!me || std::hypot(a.x - b.x, a.z - b.z) > config.gateReach) return RefuseGate(peer, npc, "guild storage", proto::GateStatus::TooFar);
        if (!guilds) return;
        const Guilds::StorageAccess access = guilds->StorageAccessOf(player.selected);
        if (access.refusal != 0) return SendMessage(*peer.connection, proto::StorageRefused{access.refusal, {}});
        GuildChest& chest = guildChests[access.guild];
        if (chest.user != 0 && chest.user != peer.id && peers->Find(chest.user) != nullptr) {
            return SendMessage(*peer.connection, proto::StorageRefused{1, chest.userName});
        }
        chest.user = peer.id;
        chest.userName = player.name;
        player.guildChest = access.guild;
        player.guildChestNpc = npc;
        const std::uint8_t size = std::uint8_t(game::GuildStorageSize(access.level));
        if (chest.storage) {
            chest.storage->size = size;
            return SendStorage(peer, *chest.storage, 1);
        }
        // The lock across the shard's servers (another's holder refuses with its name), then the
        // storage as the database has it now.
        struct Taken {
            bool taken = false;
            std::string holder;
            std::optional<StorageRow> row;
        };
        auto taken = std::make_shared<Taken>();
        ChestTask(access.guild,
                  [taken, guild = access.guild, server = config.id, character = player.selected](Database& db) {
                      taken->holder = TakeGuildStorageLock(db, guild, server, character, taken->taken);
                      if (taken->taken) taken->row = LoadGuildChest(db, guild);
                  },
                  [this, taken, peerId = peer.id, guild = access.guild, size] {
                      GuildChest& c = guildChests[guild];
                      Peer* peer = peers->Find(peerId);
                      if (!taken->taken) {
                          if (c.user == peerId) c.user = 0;
                          if (const auto p = players.find(peerId); p != players.end() && p->second.guildChest == guild) p->second.guildChest = 0;
                          if (peer != nullptr) SendMessage(*peer->connection, proto::StorageRefused{1, taken->holder});
                          return;
                      }
                      if (!taken->row) return;
                      if (!c.storage) c.storage = StorageOf(*taken->row, size);
                      if (c.user == peerId && peer != nullptr) SendStorage(*peer, *c.storage, 1);
                  });
    }
    // Put away (0x7251; or the player left): another member may open it.
    void CloseGuildStorage(std::uint64_t peerId, PlayerWorld& player) {
        if (player.guildChest == 0) return;
        if (const auto c = guildChests.find(player.guildChest); c != guildChests.end() && c->second.user == peerId) {
            // Put away: the lock goes after its saves; the contents are loaded anew next time.
            c->second.user = 0;
            c->second.storage.reset();
            ChestTask(player.guildChest, [guild = player.guildChest, server = config.id](Database& db) { ReleaseGuildStorageLock(db, guild, server); });
        }
        player.guildChest = 0;
    }
    // The guild's storage a player holds: still its member with the right (Guilds::StorageAccessOf) and the NPC it was
    // opened at still in reach. Not: put away (its lock given back).
    bool GuildChestStillHeld(Peer& peer, PlayerWorld& player) {
        if (player.guildChest == 0 || !guilds) return false;
        const Guilds::StorageAccess access = guilds->StorageAccessOf(player.selected);
        const auto me = world->Where(player.entity), there = world->Where(player.guildChestNpc);
        bool near = false;
        if (me && there) {
            const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
            near = std::hypot(a.x - b.x, a.z - b.z) <= config.gateReach;
        }
        if (access.refusal == 0 && access.guild == player.guildChest && near) return true;
        CloseGuildStorage(peer.id, player);
        return false;
    }
    // The storage a move or gold goes to: the account's, or the guild's the player holds.
    game::Storage* StorageFor(const Peer& peer, PlayerWorld& player, bool guild) {
        if (!guild) return player.storage ? &*player.storage : nullptr;
        const auto c = guildChests.find(player.guildChest);
        return c != guildChests.end() && c->second.user == peer.id && c->second.storage ? &*c->second.storage : nullptr;
    }

    // The storage (C7i): loaded from the database the first time in a session, then kept.
    void OpenStorage(Peer& peer, PlayerWorld& player, const proto::StorageOpen& open) {
        if (open.guild != 0) return OpenGuildStorage(peer, player, open.entity);
        if (const auto status = AtStorageKeeper(player, open.entity); status != proto::GateStatus::Ok) return RefuseGate(peer, open.entity, "storage", status);
        player.storageKeeper = open.entity;
        if (player.storage) return SendStorage(peer, *player.storage);
        if (player.storageLoading || peer.account == 0) return;
        player.storageLoading = true;
        database.Submit([this, peerId = peer.id, account = peer.account, server = config.id](Database& db) {
            std::optional<StorageRow> row = LoadStorage(db, account, server);
            completions->Post([this, peerId, row = std::move(row)] {
                const auto it = players.find(peerId);
                Peer* peer = peers->Find(peerId);
                if (it == players.end() || peer == nullptr) return;
                it->second.storageLoading = false;
                if (!row) return;
                it->second.storage = StorageOf(*row, std::min(row->size, game::Storage::kMaxSize));
                SendStorage(*peer, *it->second.storage);
            });
        });
    }

    // An item to, from or within the storage: checked by game::Storage, answered with the places as
    // they are now, both sides saved (the storage by place, the inventory as its moves are).
    void MoveStorageItem(Peer& peer, PlayerWorld& player, const proto::StorageMove& m) {
        proto::StorageMoved answer;
        answer.guild = m.guild;
        game::Storage* storage = StorageFor(peer, player, m.guild != 0);
        if (storage == nullptr || !config.rules->items) {
            answer.status = proto::ItemStatus::Failed;
            SendMessage(*peer.connection, answer);
            return;
        }
        using Side = game::Storage::Side;
        // The account's storage keeper still near (the original checks the NPC on every move: 3).
        if (m.guild == 0 && (player.storageKeeper == 0 || AtStorageKeeper(player, player.storageKeeper) != proto::GateStatus::Ok)) {
            answer.status = proto::ItemStatus::TooFar;
            SendMessage(*peer.connection, answer);
            return;
        }
        // The guild's: still a member with the storage's right, and the NPC it was opened at still near; else put away.
        if (m.guild != 0 && !GuildChestStillHeld(peer, player)) {
            answer.status = proto::ItemStatus::TooFar;
            SendMessage(*peer.connection, answer);
            return;
        }
        // A bound item (the beta quests' weapons) stays out of a guild's storage; its own storage takes it.
        if (m.guild != 0 && !m.fromStorage && m.toStorage) {
            if (const auto it = player.inventory.places.find(m.from); it != player.inventory.places.end() && it->second.bound) {
                answer.status = proto::ItemStatus::Bound;
                SendMessage(*peer.connection, answer);
                return;
            }
        }
        // Which storage takes it (its CanBorrow: 0x80 the account's, 0x40 a guild's; a guild's takes no quest item and
        // nothing that cannot be traded): else 0x1843.
        if (!m.fromStorage && m.toStorage) {
            const game::ItemInstance* item = player.inventory.At(m.from);
            const assets::ItemRow* row = item != nullptr ? config.rules->items->Find(item->refId) : nullptr;
            const bool quest = row != nullptr && row->typeId[0] == 3 && row->typeId[1] == 3 && row->typeId[2] == 9;
            if (row != nullptr && (m.guild == 0 ? (row->canBorrow & 0x80) == 0 : ((row->canBorrow & 0x40) == 0 || quest || !row->canTrade))) {
                SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, 0x1843});
                answer.status = proto::ItemStatus::Failed;
                SendMessage(*peer.connection, answer);
                return;
            }
        }
        // Into a storage from the bag: its keeping fee, the item's KeepingFee x its pieces (half for a guild's), at least 1,
        // at most 10,000,000; too little gold: 0x180F.
        std::uint64_t fee = 0;
        if (!m.fromStorage && m.toStorage) {
            const game::ItemInstance* item = player.inventory.At(m.from);
            const assets::ItemRow* row = item != nullptr ? config.rules->items->Find(item->refId) : nullptr;
            if (row != nullptr) {
                const double whole = double(row->keepingFee) * double(item->count) * (m.guild != 0 ? 0.5 : 1.0);
                fee = std::clamp<std::uint64_t>(std::uint64_t(whole), 1, 10'000'000);
                if (player.inventory.gold < fee) {
                    SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, 0x180F});
                    answer.status = proto::ItemStatus::NotEnoughGold;
                    SendMessage(*peer.connection, answer);
                    return;
                }
            }
        }
        const auto inventoryBefore = player.inventory.places;
        const auto storageBefore = storage->places;
        // What crosses between the bag and the storage, for the logs (the original's 20 into it, 21 out).
        std::optional<game::ItemInstance> crossing;
        if (m.fromStorage != m.toStorage) {
            if (const game::ItemInstance* i = m.fromStorage ? storage->At(m.from) : player.inventory.At(m.from)) crossing = *i;
        }
        game::Storage::MoveResult r = storage->Move(*config.rules->items, player.inventory, m.fromStorage ? Side::Storage : Side::Inventory, m.from,
                                                    m.toStorage ? Side::Storage : Side::Inventory, m.to, m.quantity);
        // An item keeps its id on either side; the part a split takes is a new item: one of the reserved ids (none left: undone).
        const auto giveId = [&](std::map<std::uint8_t, game::ItemInstance>& places, std::uint8_t place) {
            const auto it = places.find(place);
            if (it == places.end() || it->second.serial != 0) return true;
            if (freeItemIds.empty()) return false;
            it->second.serial = freeItemIds.back();
            freeItemIds.pop_back();
            return true;
        };
        bool ids = true;
        if (r.status == proto::ItemStatus::Ok) {
            for (std::uint8_t place : r.inventoryTouched) ids = ids && giveId(player.inventory.places, place);
            for (std::uint8_t place : r.storageTouched) ids = ids && giveId(storage->places, place);
        }
        if (!ids) {
            player.inventory.places = inventoryBefore;
            storage->places = storageBefore;
            r.status = proto::ItemStatus::Failed;
        }
        TopUpItemIds();
        answer.status = r.status;
        if (r.status != proto::ItemStatus::Ok) {
            SendMessage(*peer.connection, answer);
            return;
        }
        StorageSaveSide side;
        if (m.guild != 0) {
            side.guild = player.guildChest;
        } else {
            side.account = peer.account;
        }
        side.gold = storage->gold;
        for (std::uint8_t place : r.storageTouched) {
            if (const game::ItemInstance* i = storage->At(place)) {
                answer.storageChanged.push_back(storage->Record(place));
                side.present.push_back(StoredOf(place, *i));
            } else {
                answer.storageEmptied.push_back(place);
                side.emptied.push_back(place);
            }
        }
        for (std::uint8_t place : r.inventoryTouched) {
            if (player.inventory.At(place)) {
                answer.inventoryChanged.push_back(player.inventory.Record(place));
            } else {
                answer.inventoryEmptied.push_back(place);
            }
        }
        // The fee taken with the move (written with it, below).
        if (fee != 0) {
            player.inventory.gold -= fee;
            SendMessage(*peer.connection, proto::GoldChanged{player.inventory.gold});
        }
        SendMessage(*peer.connection, answer);
        if (crossing && m.guild == 0) {
            const std::int32_t moved = m.quantity != 0 ? std::int32_t(std::min<std::uint32_t>(m.quantity, crossing->count)) : std::int32_t(crossing->count);
            LogItem(player, m.toStorage ? logs::ItemOp::ToStorage : logs::ItemOp::FromStorage, crossing->refId, crossing->serial, moved, m.from, m.to,
                    m.toStorage ? logs::Storage::Storage : logs::Storage::Inventory);
        }
        // Both sides in one transaction (the original's _STRG_MOVE_ITEM), after everything submitted before it, with the bag's
        // own saves still waiting folded in (SaveSideOf): neither side's earlier saves can land after it; a fee's gold too.
        ExchangeSaveSide bag = SaveSideOf(player.selected, player.inventory, r.inventoryTouched);
        bag.gone.insert(bag.gone.end(), r.gone.begin(), r.gone.end());
        database.SubmitBarrier([this, bag = std::move(bag), side = std::move(side), withGold = fee != 0](Database& db) {
            if (!SaveStorageMove(db, bag, side, withGold)) completions->Post([this, character = bag.character] { SaveFailed(character, "a storage move"); });
        });
    }

    // Gold into or out of the open storage: both columns in one transaction (the original's _UPDATE_USER_N_CHAR_GOLD /
    // _UPDATE_GUILD_N_CHAR_GOLD), in the order the moves were made.
    void MoveStorageGold(Peer& peer, PlayerWorld& player, const proto::StorageGold& g) {
        game::Storage* storage = StorageFor(peer, player, g.guild != 0);
        if (storage == nullptr) return;
        if (g.guild == 0 && (player.storageKeeper == 0 || AtStorageKeeper(player, player.storageKeeper) != proto::GateStatus::Ok)) {
            return SendMessage(*peer.connection, proto::ItemRefused{0, 0x1834}); // UIIT_MSG_STRGERR_STORAGE_OPERATION_BLOCKED
        }
        if (g.guild != 0 && !GuildChestStillHeld(peer, player)) return SendMessage(*peer.connection, proto::ItemRefused{0, 0x1834});
        // No amount: 0x1829; more than the side it comes from holds: 0x180F (the original's answers; a full receiving side, ours,
        // stays silent: it cannot come about with 64-bit gold).
        if (g.amount == 0) return SendMessage(*peer.connection, proto::ItemRefused{0, 0x1829}); // UIIT_MSG_STRGERR_SPECIFY_POSITIVE_NUMBER_ONLY
        std::uint64_t& from = g.deposit ? player.inventory.gold : storage->gold;
        std::uint64_t& to = g.deposit ? storage->gold : player.inventory.gold;
        if (from < g.amount) return SendMessage(*peer.connection, proto::ItemRefused{0, 0x180F}); // UIIT_MSG_STRGERR_NOT_ENOUGH_GOLD
        if (to > UINT64_MAX - g.amount) return;
        from -= g.amount;
        to += g.amount;
        if (g.guild == 0) LogGold(player, g.deposit ? logs::ItemOp::GoldToStorage : logs::ItemOp::GoldFromStorage, std::int64_t(g.amount));
        SendMessage(*peer.connection, proto::GoldChanged{player.inventory.gold});
        SendMessage(*peer.connection, proto::StorageGoldChanged{storage->gold, g.guild});
        ExchangeSaveSide bag;
        bag.character = player.selected;
        bag.gold = player.inventory.gold;
        GoldNow(player.selected, bag.gold);
        StorageSaveSide side;
        if (g.guild != 0) {
            side.guild = player.guildChest;
        } else {
            side.account = peer.account;
        }
        side.gold = storage->gold;
        database.SubmitBarrier([this, bag = std::move(bag), side = std::move(side)](Database& db) {
            if (!SaveStorageMove(db, bag, side, true)) completions->Post([this, character = bag.character] { SaveFailed(character, "the storage's gold"); });
        });
    }

    // --- Shops (C7i) ---

    // The merchant near the player whose shop it is, or why not.
    const assets::NpcShop* ShopAt(const PlayerWorld& player, std::uint32_t entity, proto::GateStatus& status) const {
        const auto it = npcs.find(entity);
        const auto me = world->Where(player.entity);
        const auto npc = world->Where(entity);
        const assets::NpcShop* shop = it != npcs.end() && config.shops ? config.shops->Find(it->second->codename) : nullptr;
        if (shop == nullptr || !npc) {
            status = proto::GateStatus::NoSuchGate;
            return nullptr;
        }
        const proto::FlatPosition a = me ? proto::ToFlat(me->position) : proto::FlatPosition{}, b = proto::ToFlat(npc->position);
        status = me && std::hypot(a.x - b.x, a.z - b.z) <= config.gateReach ? proto::GateStatus::Ok : proto::GateStatus::TooFar;
        return status == proto::GateStatus::Ok ? shop : nullptr;
    }

    void Buyback(const PlayerWorld& player, proto::ShopAnswer& answer) const {
        for (std::size_t i = 0; i < player.sold.size(); ++i) {
            game::Inventory one;
            one.places[std::uint8_t(i)] = player.sold[i].item;
            answer.buyback.push_back({one.Record(std::uint8_t(i)), player.sold[i].price});
        }
    }

    // The deal done: the places it changed and the gold (and a purchase's honour points) saved in one transaction, after
    // everything submitted before it, with the bag's own saves still waiting folded in (SaveSideOf).
    void FinishDeal(Peer& peer, PlayerWorld& player, proto::ShopAnswer& answer, const std::vector<std::uint8_t>& touched,
                    const std::vector<std::int64_t>& gone, std::int64_t honorSpent = 0) {
        for (std::uint8_t place : touched) {
            if (player.inventory.At(place)) {
                answer.changed.push_back(player.inventory.Record(place));
            } else {
                answer.emptied.push_back(place);
            }
        }
        Buyback(player, answer);
        SendMessage(*peer.connection, proto::GoldChanged{player.inventory.gold});
        SendMessage(*peer.connection, answer);
        ExchangeSaveSide side = SaveSideOf(player.selected, player.inventory, touched);
        side.gone.insert(side.gone.end(), gone.begin(), gone.end());
        database.SubmitBarrier([this, side = std::move(side), honorSpent](Database& db) {
            if (!SaveShopDeal(db, side, honorSpent)) completions->Post([this, character = side.character] { SaveFailed(character, "a shop deal"); });
        });
    }

    void RefuseDeal(Peer& peer, const PlayerWorld& player, std::uint32_t entity, proto::ItemStatus status) {
        proto::ShopAnswer answer;
        answer.status = status;
        answer.entity = entity;
        Buyback(player, answer);
        SendMessage(*peer.connection, answer);
    }

    // Where an item of `count` goes in the bag: onto a stack of the same item with room for all of
    // it, else the first empty place; -1 when there is none.
    static int PlaceFor(const game::Inventory& inv, const assets::ItemRow& row, std::uint32_t refId, int count) {
        if (game::MaxStack(row) > 1) {
            for (const auto& [place, item] : inv.places) {
                if (place >= game::slot::kWorn && item.refId == refId && int(item.count) + count <= game::MaxStack(row)) return place;
            }
        }
        return inv.FreeBagPlace();
    }

    // Puts an item into the bag at `place` (merged, or new with one of the reserved ids).
    bool Put(PlayerWorld& player, int place, game::ItemInstance item) {
        auto it = player.inventory.places.find(std::uint8_t(place));
        if (it != player.inventory.places.end()) {
            it->second.count = std::uint16_t(it->second.count + item.count);
            return true;
        }
        if (freeItemIds.empty()) return false;
        item.serial = freeItemIds.back();
        freeItemIds.pop_back();
        player.inventory.places[std::uint8_t(place)] = std::move(item);
        return true;
    }

    void OpenShop(Peer& peer, PlayerWorld& player, const proto::ShopOpen& open) {
        proto::GateStatus status;
        if (ShopAt(player, open.entity, status) == nullptr) return RefuseGate(peer, open.entity, "shop", status);
        proto::ShopAnswer answer;
        answer.entity = open.entity;
        answer.opened = true;
        Buyback(player, answer);
        SendMessage(*peer.connection, answer);
        if (trades) trades->ShopOpened(player.entity, open.entity); // a specialty NPC's prices
    }

    // A good bought: the shop's price (refpricepolicyofitem, gold) times the count, the item as the
    // package makes it (refscrapofpackageitem: plus, white stats, durability or the stack).
    void BuyGood(Peer& peer, PlayerWorld& player, const proto::ShopBuy& buy) {
        proto::GateStatus status;
        const assets::NpcShop* shop = ShopAt(player, buy.entity, status);
        if (shop == nullptr) return RefuseGate(peer, buy.entity, "shop", status);
        const assets::ShopGood* good = nullptr;
        for (const assets::ShopTabGroup& group : shop->groups) {
            for (const assets::ShopTab& tab : group.tabs) {
                if (tab.index != buy.tab) continue;
                for (const assets::ShopGood& g : tab.goods) {
                    if (g.slot == buy.slot) good = &g;
                }
            }
        }
        const assets::ItemRow* row = good != nullptr && config.rules->items ? config.rules->items->Find(std::string_view(good->itemCodename)) : nullptr;
        if (row == nullptr || good->paymentType != 1) return RefuseDeal(peer, player, buy.entity, proto::ItemStatus::NotForSale);
        // A stackable comes as one stack (up to its stack size); anything else up to 5, each in a
        // place of its own (the original's ConfirmBuy).
        const bool stacks = game::MaxStack(*row) > 1;
        const int count = stacks ? std::clamp(int(buy.count), 1, game::MaxStack(*row)) : std::clamp(int(buy.count), 1, 5);
        // The fortress tax of the NPC's town on top (a negative rate lowers the price):
        // on the package's price, times the count, as the client shows it ((unit + tax) x count).
        const std::uint64_t untaxed = good->price * std::uint64_t(count);
        std::uint32_t taxFortress = 0;
        const std::int64_t tax = TaxOn(NpcRef(buy.entity), player, good->price, &taxFortress) * count;
        const std::uint64_t price = tax >= 0 ? untaxed + std::uint64_t(tax) : untaxed - std::min<std::uint64_t>(untaxed, std::uint64_t(-tax));
        // A specialty good goes into the trader's transport, never the bag (Trades.h).
        if (game::IsSpecialtyGood(*row)) {
            if (trades) {
                HoldGold(player.selected);
                trades->Buy(player.entity, buy.entity, *row, count, price, Clock::now(), tax);
                ReleaseGold(player.selected);
            }
            return;
        }
        if (player.inventory.gold < price) return RefuseDeal(peer, player, buy.entity, proto::ItemStatus::NotEnoughGold);
        // A guild's good: gold and the guild's points together.
        const std::uint32_t points = good->guildPoints * std::uint32_t(count);
        if (points != 0) {
            const Guilds::PointsPayment pay = guilds ? guilds->CanPayPoints(player.selected, points) : Guilds::PointsPayment::NotMaster;
            if (pay != Guilds::PointsPayment::Ok) {
                return RefuseDeal(peer, player, buy.entity, pay == Guilds::PointsPayment::Short ? proto::ItemStatus::GuildPointsShort : proto::ItemStatus::GuildBuyDenied);
            }
        }
        // Its honour points and tokens, with the gold (every price the package has, the tokens from the bag's
        // stacks of them; honour short 0x18D4, the Egypt tokens 0x184A, Arena Coins 0x184D).
        const std::int64_t honor = std::int64_t(good->honorPoints) * count;
        if (honor > player.honorPoints) return RefuseDeal(peer, player, buy.entity, proto::ItemStatus::HonorShort);
        std::vector<game::QuestGoal::Items> tokens;
        for (std::size_t k = 0; k < good->tokens.size(); ++k) {
            if (good->tokens[k] == 0) continue;
            const std::uint32_t need = good->tokens[k] * std::uint32_t(count);
            if (QuestItemCount(player.entity, assets::kShopTokens[k]) < need) {
                return RefuseDeal(peer, player, buy.entity, k == 4 ? proto::ItemStatus::CoinsShort : proto::ItemStatus::TokensShort);
            }
            tokens.push_back({assets::kShopTokens[k], need});
        }
        game::ItemInstance item;
        item.refId = row->refObjId;
        item.plus = std::uint8_t(good->plus);
        item.variance = good->variance;
        item.count = std::uint16_t(stacks ? count : 1);
        if (!stacks) item.durability = good->data > 0 ? std::uint32_t(good->data) : game::NewDurability(*row);
        const auto before = player.inventory.places;
        std::vector<std::uint8_t> touched;
        for (int n = 0; n < (stacks ? 1 : count); ++n) {
            const int place = PlaceFor(player.inventory, *row, row->refObjId, item.count);
            if (place < 0 || !Put(player, place, item)) {
                player.inventory.places = before;
                return RefuseDeal(peer, player, buy.entity, place < 0 ? proto::ItemStatus::BagFull : proto::ItemStatus::Failed);
            }
            touched.push_back(std::uint8_t(place));
        }
        player.inventory.gold -= price;
        CollectTax(taxFortress, tax);
        if (points != 0) guilds->SpendPoints(player.selected, points, game::kGpReasonGuildItems);
        if (honor > 0) player.honorPoints -= honor; // saved with the deal
        core::LogInfo("{}: {} buys {} x{} for {}, {} guild points, {} honour points{}", config.name, player.name, row->codename, count, price, points, honor,
                      tokens.empty() ? std::string() : std::format(" and {} x{}", tokens.front().item, tokens.front().count));
        proto::ShopAnswer answer;
        answer.entity = buy.entity;
        FinishDeal(peer, player, answer, touched, {}, honor);
        for (const std::uint8_t place : touched) {
            if (const game::ItemInstance* got = player.inventory.At(place))
                LogItem(player, logs::ItemOp::Buy, got->refId, got->serial, item.count, logs::kNoSlot, place, logs::Storage::Inventory, std::int64_t(price / touched.size()));
        }
        if (!tokens.empty()) QuestTake(player.entity, tokens); // the tokens out of their stacks (counted above)
    }

    // An item sold: its sale price (itemdata col 31) times the count; kept for buying back.
    void SellItem(Peer& peer, PlayerWorld& player, const proto::ShopSell& sell) {
        proto::GateStatus status;
        if (ShopAt(player, sell.entity, status) == nullptr) return RefuseGate(peer, sell.entity, "shop", status);
        auto it = sell.place >= game::slot::kWorn ? player.inventory.places.find(sell.place) : player.inventory.places.end();
        const assets::ItemRow* row = it != player.inventory.places.end() && config.rules->items ? config.rules->items->Find(it->second.refId) : nullptr;
        if (row == nullptr) return RefuseDeal(peer, player, sell.entity, proto::ItemStatus::NoItem);
        if (!row->canSell) return RefuseDeal(peer, player, sell.entity, proto::ItemStatus::NotForSale);
        // A magic cube with items in it is not sold (0x188E).
        if (game::IsMagicCube(*row)) {
            if (const auto cube = cubeItems.find(it->second.serial); cube != cubeItems.end() && !cube->second.empty()) {
                SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, game::cubecode::kSellRefused});
                return RefuseDeal(peer, player, sell.entity, proto::ItemStatus::NotForSale);
            }
        }
        // A grab pet's watch goes whole whatever the count asked (the original's sale forces it).
        const int count = sell.count == 0 || game::IsWholeItem(*row) ? it->second.count : std::clamp(int(sell.count), 1, int(it->second.count));
        // The fortress's tax comes off the sale's whole: a positive rate pays the seller less, a negative one more; none
        // for the holder's guild and union.
        std::uint32_t taxFortress = 0;
        const std::uint64_t gross = row->sellPrice * std::uint64_t(count);
        const std::int64_t tax = TaxOn(NpcRef(sell.entity), player, gross, &taxFortress);
        const std::uint64_t price = std::uint64_t(std::max<std::int64_t>(0, std::int64_t(gross) - tax));
        game::ItemInstance sold = it->second;
        sold.count = std::uint16_t(count);
        LogItem(player, logs::ItemOp::Sell, sold.refId, sold.serial, count, sell.place, logs::kNoSlot, logs::Storage::Inventory, std::int64_t(price));
        std::vector<std::int64_t> gone;
        if (count == it->second.count) {
            gone.push_back(it->second.serial);
            player.inventory.places.erase(it);
        } else {
            it->second.count = std::uint16_t(it->second.count - count);
        }
        sold.serial = 0;
        player.inventory.gold += price;
        player.sold.push_back({std::move(sold), price});
        if (player.sold.size() > 5) player.sold.erase(player.sold.begin());
        CollectTax(taxFortress, tax);
        core::LogInfo("{}: {} sells {} x{} for {} (tax {})", config.name, player.name, row->codename, count, price, tax);
        proto::ShopAnswer answer;
        answer.entity = sell.entity;
        FinishDeal(peer, player, answer, {sell.place}, gone);
    }

    // One of the sold items bought back for what it fetched.
    void BuyBack(Peer& peer, PlayerWorld& player, const proto::ShopBuyback& back) {
        proto::GateStatus status;
        if (ShopAt(player, back.entity, status) == nullptr) return RefuseGate(peer, back.entity, "shop", status);
        if (back.index >= player.sold.size()) return RefuseDeal(peer, player, back.entity, proto::ItemStatus::NoItem);
        PlayerWorld::Sold& entry = player.sold[back.index];
        if (player.inventory.gold < entry.price) return RefuseDeal(peer, player, back.entity, proto::ItemStatus::NotEnoughGold);
        const int place = player.inventory.FreeBagPlace();
        if (place < 0) return RefuseDeal(peer, player, back.entity, proto::ItemStatus::BagFull);
        if (!Put(player, place, entry.item)) return RefuseDeal(peer, player, back.entity, proto::ItemStatus::Failed);
        player.inventory.gold -= entry.price;
        player.sold.erase(player.sold.begin() + back.index);
        proto::ShopAnswer answer;
        answer.entity = back.entity;
        FinishDeal(peer, player, answer, {std::uint8_t(place)}, {});
    }

    // One place's repair at an NPC: equipment
    // the NPC repairs (game::Repairable: not an accessory, its CanRepair, no "repair invalid", an honor item's charge left;
    // else 0x1C11, or nothing when every place is asked), a broken one revived first (its CanRevive 0x1C12, its CostRevive
    // 0x1C07), the points game::PriceRepair's within the gold (a float32 point, the broken one's first point free), one
    // Repair charge spent. kRepaired, kNothingRepaired, else the code.
    static constexpr std::uint16_t kRepaired = 1, kNothingRepaired = 2;
    std::uint16_t RepairPlace(PlayerWorld& player, std::uint8_t place, bool single, std::uint64_t& spent, bool& revived) {
        const std::vector<assets::MagicOptionRow>* options = config.magicOptions.get();
        const auto found = player.inventory.places.find(place);
        game::ItemInstance* item = found != player.inventory.places.end() ? &found->second : nullptr;
        const assets::ItemRow* row = item != nullptr && config.rules->items ? config.rules->items->Find(item->refId) : nullptr;
        if (row == nullptr) return single ? game::repaircode::kNoItem : kNothingRepaired;
        if (row->typeId[0] != 3 || row->typeId[1] != 1 || !game::Repairable(*row, *item, options)) return single ? game::repaircode::kNotRepairable : kNothingRepaired;
        const std::uint32_t most = std::uint32_t(game::ComputeStats(*row, *item, options, 0, true).durability.high);
        if (item->durability == 0 && !row->canRevive) return game::repaircode::kCannotRevive;
        if (item->durability == 0 && player.inventory.gold < row->costRevive) return game::repaircode::kNoGold;
        const game::RepairPrice price = game::PriceRepair(*row, item->durability, most, player.inventory.gold, false);
        if (price.points == 0) return item->durability >= most ? kNothingRepaired : game::repaircode::kNoGold;
        player.inventory.gold -= std::min(price.gold, player.inventory.gold);
        spent += price.gold;
        revived = item->durability == 0;
        item->durability = std::min(most, item->durability + price.points);
        for (game::ItemInstance::Option& o : item->options) {
            const assets::MagicOptionRow* m = options != nullptr ? game::FindOption(*options, o.id) : nullptr;
            if (m != nullptr && m->optionName == "MATTR_REPAIR" && o.value > 1) --o.value;
        }
        return kRepaired;
    }

    // An NPC's repair: an NPC that repairs in reach (ours: the fortress smith; else the original's code 5), some gold
    // (0x1C07); mode 2
    // every place in turn, stopping where the gold runs out, mode 1 the one place (not equipment or not repairable 0x1C11, a
    // broken one that cannot be revived 0x1C12, its revival's gold 0x1C07). Each piece costs game::PriceRepair cut to the gold
    // left (takes it); an honor item spends a Repair charge.
    void RepairItems(Peer& peer, PlayerWorld& player, const proto::NpcRepair& repair) {
        proto::NpcRepaired answer;
        const auto refuse = [&](std::uint16_t code) {
            answer.code = code;
            SendMessage(*peer.connection, answer);
        };
        const auto it = npcs.find(repair.npc);
        const auto me = world->Where(player.entity);
        const auto npc = world->Where(repair.npc);
        if (it == npcs.end() || !me || !npc || (NpcServices(it->second->codename) & proto::kNpcFortressSmith) == 0) return refuse(5);
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(npc->position);
        if (std::hypot(a.x - b.x, a.z - b.z) > config.gateReach) return refuse(5);
        if (player.inventory.gold == 0) return refuse(0x1C07);
        if (!config.rules->items || (repair.mode != 1 && repair.mode != 2)) return refuse(5);
        std::vector<std::uint8_t> touched;
        std::uint64_t spent = 0;
        // One place's repair: 1 done (or nothing to do), else the code.
        const auto one = [&](std::uint8_t place) -> std::uint16_t {
            bool revived = false;
            const std::uint16_t code = RepairPlace(player, place, repair.mode == 1, spent, revived);
            if (code == kRepaired) touched.push_back(place);
            return code == kRepaired || code == kNothingRepaired ? 1 : code;
        };
        if (repair.mode == 1) {
            if (const std::uint16_t code = one(repair.place); code != 1) return refuse(code);
        } else {
            std::vector<std::uint8_t> places;
            for (const auto& [place, item] : player.inventory.places) places.push_back(std::uint8_t(place));
            std::sort(places.begin(), places.end());
            for (const std::uint8_t place : places) {
                if (one(place) == 0x1C07) break;
            }
        }
        answer.ok = true;
        answer.gold = spent;
        for (const std::uint8_t place : touched) answer.changed.push_back(player.inventory.Record(place));
        core::LogInfo("{}: {} has {} pieces repaired at {} for {} gold", config.name, player.name, touched.size(), it->second->codename, spent);
        SendMessage(*peer.connection, proto::GoldChanged{player.inventory.gold});
        SendMessage(*peer.connection, answer);
        QueueGoldSave(player.selected, player.inventory.gold);
        if (!touched.empty()) QueueItemSave(player.selected, player.inventory, touched, {});
    }

    void TalkToNpc(Peer& peer, PlayerWorld& player, const proto::NpcTalk& talk) {
        // A Capture the Flag match's pole, key or dropped flag (FlagWorld.h): its action, no menu.
        if (flagWorlds && flagWorlds->Select(player.entity, talk.entity)) return;
        if (arenas && arenas->Select(player.entity, talk.entity)) return; // a Battle Arena flag map's pole or flag
        const auto it = npcs.find(talk.entity);
        // A party recall pillar of the player's instance: its one row.
        if (it == npcs.end() && instances && instances->AtPillar(player.entity, talk.entity)) {
            return SendMessage(*peer.connection, proto::NpcMenu{talk.entity, proto::kNpcPartySummon});
        }
        // Busy (an exchange, its stall or another's, an alchemy run): 0x1C0B.
        if ((exchanges && exchanges->Busy(player.entity)) || (stalls && (stalls->Owner(player.entity) || stalls->Visiting(player.entity))) ||
            (alchemy && alchemy->Busy(player.entity))) {
            return RefuseGate(peer, talk.entity, "npc", proto::GateStatus::Busy);
        }
        const auto me = world->Where(player.entity);
        const auto npc = world->Where(talk.entity);
        if (it == npcs.end() || !npc) return RefuseGate(peer, talk.entity, "npc", proto::GateStatus::NoSuchGate);
        const proto::FlatPosition a = me ? proto::ToFlat(me->position) : proto::FlatPosition{}, b = proto::ToFlat(npc->position);
        if (!me || std::hypot(a.x - b.x, a.z - b.z) > config.gateReach) return RefuseGate(peer, talk.entity, "npc", proto::GateStatus::TooFar);
        SendMessage(*peer.connection, proto::NpcMenu{talk.entity, NpcServices(it->second->codename, it->second->refObjId) | PointServices(it->second->refObjId),
                                                     NpcTaxFor(it->second->refObjId, player), SafeTradeAt(player, it->second->codename)});
        if (quests) quests->TellNpc(player.entity, talk.entity); // its quests' rows
        if (flagWorlds) flagWorlds->TellDesk(player.entity, talk.entity, it->second->codename); // Event So-Ok's desk
        if (arenas) arenas->TellDesk(player.entity, talk.entity, it->second->codename);         // the arena manager's state
    }

    // --- Fighting (C10b) ---------------------------------------------------------------------------

    std::mt19937 fightRandom{std::random_device{}()};
    // The monsters' chance histories for the states their hits give players (the attacker's, as the players' own).
    std::unordered_map<std::uint32_t, game::ChanceRolls> monsterStateRolls;
    template <typename T>
    void Broadcast(std::uint32_t entity, const T& message) {
        // Combat's details only for the nearer tiers of a view (World.h): a swing for the nearest, a
        // hit for the nearest two, a death for everyone. The other side of a fight always hears of it
        // (in a crowded view its target or attacker may rank far).
        int maxTier = 2;
        std::uint32_t also = 0;
        if constexpr (std::is_same_v<T, proto::EntitySwing>) {
            maxTier = 0;
            also = message.target;
        } else if constexpr (std::is_same_v<T, proto::EntityDamaged>) {
            maxTier = message.hp == 0 ? 2 : 1;
            also = message.attacker;
        }
        const std::vector<std::byte> bytes = proto::Encode(message);
        const std::uint64_t alsoPeer = also != 0 && also != entity ? world->PeerOf(also) : 0;
        bool alsoTold = false;
        world->ForEachObserverWithin(entity, maxTier, [&](std::uint64_t to) {
            alsoTold = alsoTold || to == alsoPeer;
            if (Peer* p = peers->Find(to)) p->connection->Send(T::kId, bytes);
        });
        if (alsoPeer != 0 && !alsoTold) {
            if (Peer* p = peers->Find(alsoPeer)) p->connection->Send(T::kId, bytes);
        }
    }

    void SendProgress(Peer& peer, const PlayerWorld& player) {
        proto::Progress p;
        p.level = std::uint8_t(player.level);
        p.experience = player.experience;
        p.skillExperience = player.skillExperience;
        p.skillPoints = player.skillPoints;
        const auto u32 = [](double v) { return std::uint32_t(std::max(0.0, std::trunc(v))); };
        const auto u16 = [](double v) { return std::uint16_t(std::clamp(std::trunc(v), 0.0, 65535.0)); };
        p.strength = u16(player.stats.strength); // its buffs' in
        p.intelligence = u16(player.stats.intelligence);
        p.statPoints = player.statPoints;
        p.phyAttackMin = u32(player.stats.phyAttackMin);
        p.phyAttackMax = u32(player.stats.phyAttackMax);
        p.magAttackMin = u32(player.stats.magAttackMin);
        p.magAttackMax = u32(player.stats.magAttackMax);
        p.phyDefence = u16(player.stats.phyDefence);
        p.magDefence = u16(player.stats.magDefence);
        p.hitRate = u16(player.stats.hitRate);
        p.parryRate = u16(player.stats.evasion);
        SendMessage(*peer.connection, p);
    }

    CharacterProgress ProgressOf(const PlayerWorld& player) const {
        return {player.selected,         std::uint8_t(player.level),        player.experience,          player.skillPoints,
                player.skillExperience,  std::uint16_t(player.strength),    std::uint16_t(player.intelligence), player.statPoints};
    }

    // A kill's experience (the original's): the monster's
    // (characterdata col 85) times its kind's (a party monster's base 7.5, else 1; then by its grade: champion x 2,
    // giant x 15, titan x 60, elite x 4, grade 7 x 30; 0, 3 and 8 the base; another grade 1), the player's
    // share of the damage, a level-gap scale (a monster more than 10 above: 2 % less a level beyond;
    // the player 4..6 above: 0.9; 7 or more above: 15 % less a level beyond 3; at least 0.1), and a
    // bonus of 3 % a level for monsters not 3 or more below (1 + 0.03 x min(13, gap + 3)); skill
    // experience the same times 100 over leveldata's experience of a general monster of the
    // player's level. Each then by the gap between the
    // level and the highest mastery (game::MasteryGapRatesOf), then the rates; at least 1 experience.
    void Reward(Peer& peer, PlayerWorld& player, std::uint32_t monster, const assets::ObjectRow& row, std::uint8_t rarity, double share) {
        if (config.levels.empty() || share <= 0.0) return;
        const double population = (rarity >> 4) == 1 ? 7.5 : 1.0;
        double kind = 1.0;
        switch (rarity & 0x0F) {
        case 0:
        case 3:
        case 8: kind = population; break;
        case 1: kind = population * 2.0; break;
        case 4: kind = population * 15.0; break;
        case 5: kind = population * 60.0; break;
        case 6: kind = population * 4.0; break;
        case 7: kind = population * 30.0; break;
        default: break;
        }
        const int gap = row.level - player.level; // the monster above the player
        double scale = 1.0;
        if (gap > 10) scale = 1.0 - 0.02 * double(gap - 10);
        else if (-gap >= 7) scale = 1.0 - 0.15 * double(-gap - 3);
        else if (-gap >= 4) scale = 0.9;
        scale = std::clamp(scale, 0.1, 1.0);
        double bonus = gap > -3 ? 1.0 + 0.03 * double(std::min(13, gap + 3)) : 1.0;
        // In a party sharing experience, a monster 5, 7 or 9 levels above: 0.15, 0.3 or 0.45 more (twice that for a
        // monster of Country 1); the bonus held within 1 and 4.
        if (parties && (parties->SettingsOf(player.entity) & game::kPartyExpShare) != 0 && gap > 4) {
            const double extra = gap >= 9 ? 0.45 : gap >= 7 ? 0.3 : 0.15;
            bonus = std::clamp(bonus + extra * (row.country == 1 ? 2.0 : 1.0), 1.0, 4.0);
        }
        const double base = double(row.experience) * kind * share * scale * bonus;
        int highestMastery = 0;
        for (const auto& [mastery, level] : player.book.masteries) highestMastery = std::max(highestMastery, int(level));
        const game::MasteryGapRates gapRates = game::MasteryGapRatesOf(player.level, highestMastery);
        std::uint64_t experience = std::uint64_t(std::llround(base * gapRates.experience * config.rates.experience));
        if (base > 0.0 && config.rates.experience > 0.0) experience = std::max<std::uint64_t>(1, experience);
        const std::size_t basisLevel = std::size_t(std::max(1, player.level));
        const double basis = basisLevel < config.monsterExperience.size() && config.monsterExperience[basisLevel] > 0
                                 ? double(config.monsterExperience[basisLevel])
                                 : std::max(1.0, double(row.experience));
        std::uint32_t skill = std::uint32_t(std::llround(base * gapRates.skill * 100.0 / basis * config.rates.skillExperience));
        // A socket stone's 'spi' (the keeper's 0xC2): trunc(the kill's SP x min(it, 100) / 100) more.
        if (const int spi = int(player.combat.Sheet().Value(0xC2, 0.0f)); spi > 0 && skill > 0) {
            skill += std::uint32_t(std::trunc(double(skill) * double(std::min(spi, 100)) / 100.0));
        }
        // The time services' bonus (+P4 % experience, +P5 % skill experience while their bonus seconds last).
        const std::uint64_t ownExperience = experience;
        const std::uint32_t ownSkill = skill;
        if (const auto [expBonus, skillBonus] = ServiceBonus(player); expBonus > 0 || skillBonus > 0) {
            experience += experience * std::uint64_t(expBonus) / 100;
            skill += std::uint32_t(std::uint64_t(skill) * std::uint64_t(skillBonus) / 100);
        }
        // The academy: an apprentice's accumulation takes the kill's share of the
        // monster's whole; a buff received spends up to 10 of its points for as many percent more, at most 30 % of the
        // kill's own.
        if (academies) {
            const double whole = double(row.experience) * kind;
            if (const int spent = academies->Kill(player.selected, whole > 0.0 ? base / whole : 0.0); spent > 0) {
                const auto most = [](double v) { return std::uint64_t(std::trunc(v * game::academy::kBonusMostOfKill)); };
                experience += std::min<std::uint64_t>(experience * std::uint64_t(spent) / 100, most(double(ownExperience)));
                skill += std::uint32_t(std::min<std::uint64_t>(std::uint64_t(skill) * std::uint64_t(spent) / 100, most(double(ownSkill))));
            }
        }
        // Its attack pet's, as the player gets its own: the same kill's experience worked out for the pet's level, the
        // same share (ours: without the masteries' gap, a player's own).
        if (const int petLevel = coses ? coses->AttackPetLevel(player.entity) : 0; petLevel > 0) {
            const int petGap = row.level - petLevel;
            double petScale = 1.0;
            if (petGap > 10) petScale = 1.0 - 0.02 * double(petGap - 10);
            else if (-petGap >= 7) petScale = 1.0 - 0.15 * double(-petGap - 3);
            else if (-petGap >= 4) petScale = 0.9;
            petScale = std::clamp(petScale, 0.1, 1.0);
            const double petBonus = petGap > -3 ? 1.0 + 0.03 * double(std::min(13, petGap + 3)) : 1.0;
            const double petBase = double(row.experience) * kind * share * petScale * petBonus;
            std::uint64_t petExperience = std::uint64_t(std::llround(petBase * config.rates.experience));
            if (petBase > 0.0 && config.rates.experience > 0.0) petExperience = std::max<std::uint64_t>(1, petExperience);
            // The potion of growth: +100 % of the pet's (its skill's expi).
            if (player.growthUntil > ServiceNow()) petExperience *= 2;
            coses->GainExperience(player.entity, petExperience);
        }
        if (experience == 0 && skill == 0) return;
        SendMessage(*peer.connection, proto::ExperienceGained{monster, experience, skill});
        GainExperience(peer, player, experience, skill);
    }

    // Experience in: the levels it fills (each +1 STR, +1 INT and 3 stat points to spend; health and
    // mana full), skill experience into skill points (400 each).
    static constexpr std::uint32_t kSkillExperiencePerPoint = 400; // one skill point (0x190)
    void GainExperience(Peer& peer, PlayerWorld& player, std::uint64_t experience, std::uint32_t skill) {
        const int startLevel = player.level;
        player.experience += experience;
        while (player.level < config.rates.levelCap && std::size_t(player.level) < config.levels.size() &&
               config.levels[std::size_t(player.level)] > 0 && player.experience >= config.levels[std::size_t(player.level)]) {
            player.experience -= config.levels[std::size_t(player.level)];
            ++player.level;
            // +1 STR, +1 INT, 3 points: only for a level never reached before (one lost to a death gives none again).
            if (player.level > player.maxLevel) {
                player.maxLevel = player.level;
                ++player.strength;
                ++player.intelligence;
                player.statPoints = std::uint16_t(player.statPoints + 3);
            }
        }
        if (player.level >= config.rates.levelCap && std::size_t(player.level) < config.levels.size()) {
            player.experience = std::min<std::uint64_t>(player.experience, config.levels[std::size_t(player.level)] - 1);
        }
        const std::uint64_t total = std::uint64_t(player.skillExperience) + skill;
        const std::uint32_t earned = std::uint32_t(total / kSkillExperiencePerPoint);
        player.skillPoints += earned;
        player.skillExperience = std::uint32_t(total % kSkillExperiencePerPoint);
        // A guild member's skill points earned are its guild's points too, given by ten.
        if (earned > 0 && guilds && guilds->GuildOf(player.selected) != 0) {
            player.guildPointsDue += earned;
            if (player.guildPointsDue >= 10) {
                guilds->PointsEarned(player.selected, player.guildPointsDue);
                player.guildPointsDue = 0;
            }
        }
        player.progressDirty = true;
        if (player.level != startLevel) {
            player.wearer.level = player.level;
            UpdateStats(nullptr, player);
            player.hp = player.stats.maxHp;
            player.mp = player.stats.maxMp;
            world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
            SendVitals(peer, player);
            // Those who see it (and its own bar over its head) see its health full.
            Broadcast(player.entity, proto::EntityDamaged{player.entity, 0, 0, player.hp, 0});
            Broadcast(player.entity, proto::EntityLevelUp{player.entity, std::uint8_t(player.level)});
            if (quests) quests->LevelChanged(player.entity); // the quests it may take now
            core::LogInfo("{}: {} reaches level {}", config.name, player.name, player.level);
            LogChar(player, logs::CharEvent::LevelChanged, startLevel, player.level);
            if (guilds) guilds->LevelChanged(player.selected, player.level);
            if (academies) academies->LevelChanged(player.selected, player.level, player.maxLevel);
            if (coses) coses->OwnerLevelChanged(player.entity); // its transport's bag grows
            // Saved at once: a level is worth keeping.
            QueueProgressSave(player);
            player.progressDirty = false;
        }
        SendProgress(peer, player);
    }

    // --- Skills (C7f) ---------------------------------------------------------------------------

    void SendSkillBook(Peer& peer, const PlayerWorld& player) {
        proto::SkillBookList list;
        if (config.skills) {
            for (const assets::MasteryRow& m : config.skills->masteries()) {
                if (game::MasteryOfRace(*config.skills, m.id, player.wearer.country)) {
                    list.masteries.push_back({m.id, std::uint8_t(player.book.MasteryLevel(m.id))});
                }
            }
            list.skills = player.book.Skills(*config.skills);
        }
        SendMessage(*peer.connection, list);
    }

    // A mastery's next level for its skill points (the first level free), within the character's
    // level and the race's allowance (game::CanRaiseMastery).
    void RaiseMastery(Peer& peer, PlayerWorld& player, const proto::RaiseMastery& r) {
        if (!config.skills) return;
        const game::SkillCheck check = game::CanRaiseMastery(player.book, *config.skills, config.masteryCosts, r.mastery, player.wearer.country,
                                                             player.level, player.skillPoints);
        if (check != game::SkillCheck::Ok) {
            SendMessage(*peer.connection, proto::SkillBookAnswer{std::uint8_t(check), r.mastery, 0});
            return;
        }
        const int level = player.book.MasteryLevel(r.mastery);
        player.skillPoints -= game::MasteryCost(config.masteryCosts, level);
        player.book.masteries[r.mastery] = level + 1;
        player.progressDirty = true;
        CharacterWrite(player.selected, [character = player.selected, mastery = r.mastery, next = level + 1, p = ProgressOf(player)](Database& db) {
            SaveMastery(db, character, mastery, next);
            SaveProgress(db, {p});
        });
        player.progressDirty = false;
        core::LogInfo("{}: {} raises mastery {} to {}", config.name, player.name, r.mastery, level + 1);
        SendMessage(*peer.connection, proto::SkillBookAnswer{0, r.mastery, 0});
        SendSkillBook(peer, player);
        SendProgress(peer, player);
    }

    // A skill's next level for its skill points (game::CanLearnSkill).
    void LearnSkill(Peer& peer, PlayerWorld& player, const proto::LearnSkill& r) {
        const assets::SkillRow* skill = config.skills ? config.skills->Find(r.skill) : nullptr;
        if (skill != nullptr) skill = &config.skills->Head(*skill); // a chain's sub-row learns as its head, for its skill points
        const game::SkillCheck check =
            skill == nullptr ? game::SkillCheck::Unknown : game::CanLearnSkill(player.book, *config.skills, *skill, player.wearer.country, player.skillPoints,
                                                                                   player.strength, player.intelligence);
        if (check != game::SkillCheck::Ok) {
            SendMessage(*peer.connection, proto::SkillBookAnswer{std::uint8_t(check), 0, r.skill});
            return;
        }
        player.skillPoints -= std::uint32_t(skill->spCost);
        // The group's row before this one goes (the original keeps one row per group).
        const auto had = player.book.groups.find(skill->group);
        const assets::SkillRow* before = had != player.book.groups.end() ? config.skills->GroupLevel(skill->group, had->second) : nullptr;
        player.book.groups[skill->group] = skill->level;
        CharacterWrite(player.selected, [character = player.selected, previous = before ? before->id : 0u, id = skill->id, p = ProgressOf(player)](Database& db) {
            SaveSkill(db, character, previous, id);
            SaveProgress(db, {p});
        });
        player.progressDirty = false;
        core::LogInfo("{}: {} learns {}", config.name, player.name, skill->codename);
        SendMessage(*peer.connection, proto::SkillBookAnswer{0, 0, r.skill});
        SendSkillBook(peer, player);
        if (skill->activity == 0) UpdateStats(&peer, player); // a passive's numbers (K6)
        SendProgress(peer, player);
    }

    // A stat point spent: one more strength or intelligence, the numbers again.
    void RaiseStat(Peer& peer, PlayerWorld& player, const proto::RaiseStat& r) {
        if (player.statPoints == 0 || r.stat > 1) return;
        --player.statPoints;
        if (r.stat == 0) ++player.strength;
        else ++player.intelligence;
        player.progressDirty = true;
        UpdateStats(&peer, player);
        SendProgress(peer, player);
    }

    void SendVitals(Peer& peer, const PlayerWorld& player) {
        SendMessage(*peer.connection, proto::Vitals{player.hp, player.stats.maxHp, player.mp, player.stats.maxMp});
    }

    // Who wears what, as it stands now: its masteries (raised, withdrawn) for the European armour's requirements.
    game::Wearer WearerOf(const PlayerWorld& player) const {
        game::Wearer who = player.wearer;
        who.masteries = player.book.masteries;
        who.masteriesKnown = true;
        return who;
    }

    // A blue's changes into its item's: a summing code (game::BlueSums) adds to the item's change of the same parameter and
    // kind, any other replaces it (the keeper's upsert of a source's change).
    static void MergeBlue(std::vector<game::StatChange>& into, const std::vector<game::StatChange>& blue, bool sums) {
        for (const game::StatChange& c : blue) {
            const auto it = std::find_if(into.begin(), into.end(), [&](const game::StatChange& x) { return x.stat == c.stat && x.kind == c.kind; });
            if (it == into.end()) into.push_back(c);
            else if (sums) it->value += c.value;
            else *it = c;
        }
    }

    // Its numbers again (entering, wearing something else); its health kept within the new most.
    void UpdateStats(Peer* peer, PlayerWorld& player) {
        // The stat panel's numbers before (told again when they changed: what it wears, its buffs, its passives;
        // the C page showed those of its entry until a level or a point raised them; the user's report 2026-11-13).
        const auto panel = [](const game::CombatStats& s) {
            const auto t = [](double v) { return std::trunc(v); };
            return std::array<double, 10>{t(s.strength), t(s.intelligence), t(s.phyAttackMin), t(s.phyAttackMax), t(s.magAttackMin),
                                          t(s.magAttackMax), t(s.phyDefence), t(s.magDefence), t(s.hitRate), t(s.evasion)};
        };
        const std::array<double, 10> before = panel(player.stats);
        std::vector<game::WornItem> worn;
        // Its sets: every worn piece counts, as the original's set operator plays what
        // changed since its last numbers; the raise each piece's whites were last made with goes into its total plus.
        std::vector<game::SetPiece> pieces;
        if (config.rules->items) {
            // Its requirements before a piece gives anything: the level and the European masteries: a piece whose
            // masteries were lowered since (a skill withdrawal) stays worn and gives nothing, its set counting it no more.
            game::Wearer who = WearerOf(player);
            who.level = player.level;
            for (int place = 0; place < game::slot::kWorn; ++place) {
                const game::ItemInstance* item = player.inventory.At(std::uint8_t(place));
                if (item == nullptr) continue;
                const assets::ItemRow* row = config.rules->items->Find(item->refId);
                const bool fits = row != nullptr && game::RequirementsMet(*row, who) == game::EquipCheck::Ok;
                if (row != nullptr) {
                    pieces.push_back({item->serial, item->refId, place, row->setId, item->durability == 0 && game::NewDurability(*row) != 0, fits,
                                      game::SetStamp(*item)});
                }
                // Its requirements short: worn, but nothing of it counts (its stats, its kind, its options, its sockets).
                if (fits) {
                    worn.push_back({place, row, item, 0});
                }
            }
        }
        if (config.sets) {
            player.sets.Update(*config.sets, pieces, player.level);
            // An unfit piece a set's change installed anyway counts as worn until it is next taken out (SetWear::Forced).
            for (const game::SetPiece& p : pieces) {
                if (p.fits || !player.sets.Forced(p)) continue;
                const game::ItemInstance* item = player.inventory.At(std::uint8_t(p.place));
                const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(p.refId) : nullptr;
                if (item != nullptr && row != nullptr) worn.push_back({p.place, row, item, 0});
            }
            std::sort(worn.begin(), worn.end(), [](const game::WornItem& a, const game::WornItem& b) { return a.place < b.place; });
            for (game::WornItem& w : worn) {
                for (const game::SetPiece& p : pieces) {
                    if (p.place == w.place) w.setPlus = player.sets.RaiseOf(p);
                }
            }
        }
        player.setRaise.fill(0);
        for (const game::WornItem& w : worn) player.setRaise[std::size_t(w.place)] = w.setPlus;
        // Its passives (K6): what it wears for their conditions and its
        // skills', their changes on the numbers it has without them; a buff whose conditions no
        // longer hold ends.
        player.combat.worn = game::WornKindsOf(worn);
        // Its Devil's Spirit in avatar place 4: while awake, its plus's options and skills and its
        // own blues ( ->); its
        // skills' 'reqi 14 1' holds while it is on and awake.
        game::SpiritGift spirit;
        if (config.rules->items) {
            if (const game::ItemInstance* item = player.inventory.Avatar(game::avatar::kSpirit)) {
                if (const assets::ItemRow* row = config.rules->items->Find(item->refId)) {
                    static const game::SpiritTable kNoSpirits;
                    spirit = game::SpiritGiftOf(config.spirits ? *config.spirits : kNoSpirits, *row, *item, config.magicOptions.get(), ServiceNow());
                }
            }
        }
        player.combat.worn.spirit = spirit.awake;
        // The skills its worn items' sockets give (taken away with the item): its passives run below, its actives are
        // cast as learned ones.
        std::vector<std::uint32_t> socketSkillsBefore = std::move(player.book.itemSkills);
        player.book.itemSkills.clear();
        for (const game::WornItem& w : worn) {
            if (w.item != nullptr && w.place != game::slot::kJobSuit) game::alchemy::AddSocketSkills(*w.item, player.book.itemSkills);
        }
        // The spirit's, attached to the learned map and told nothing (the 1.188 client reads the same table).
        for (const std::uint32_t id : spirit.skills) {
            if (std::find(player.book.itemSkills.begin(), player.book.itemSkills.end(), id) == player.book.itemSkills.end()) player.book.itemSkills.push_back(id);
        }
        // Those its items took away: their running buffs end at once (ends every running instance of the skill). Not
        // the spirit's: their detach ends nothing; their buffs end by their
        // 'reqi 14 1' (EndUnfitBuffs below, the original's re-check and its run's).
        std::erase_if(socketSkillsBefore, [&](std::uint32_t id) {
            return std::find(player.book.itemSkills.begin(), player.book.itemSkills.end(), id) != player.book.itemSkills.end() ||
                   std::find(player.spiritSkills.begin(), player.spiritSkills.end(), id) != player.spiritSkills.end();
        });
        player.spiritSkills = spirit.skills;
        player.spiritOn = spirit.found;
        if (config.skills) {
            const game::CombatStats plain = game::PlayerCombatStats(player.level, player.strength, player.intelligence, worn, nullptr);
            player.combat.passives = game::PassivesOf(player.book, *config.skills, player.combat.worn, Skills::BasisOf(plain));
        }
        if (skillUnit) {
            SkillCaster caster = CasterOf(player);
            skillUnit->EndUnfitBuffs(caster);
            skillUnit->EndBuffsOf(caster, socketSkillsBefore);
        }
        // Its passives' and buffs' changes through the keeper (K4): STR and INT
        // first, the numbers made from them, then each number; its walks' speed.
        game::StatSheet sheet = player.combat.Sheet();
        // Its worn items' blues, each item a source of the keeper: STR, INT, HP, MP, critical, ... A broken
        // item gives nothing (as its whites: PlayerCombatStats). A harp worn gives its bearer half again its most mana
        // (TID4 14: param 4, percent, 50.0).
        {
            const std::vector<assets::MagicOptionRow>* options = config.magicOptions.get();
            constexpr std::uint32_t kWornSource = 0x17E00000u; // + the place: apart from the skills' and buffs' sources
            for (const game::WornItem& w : worn) {
                if (w.item->durability == 0 && game::NewDurability(*w.row) != 0) continue;
                std::vector<game::StatChange> changes;
                if (w.place == game::slot::kWeapon && w.row->typeId == std::array<int, 4>{3, 1, 6, 14}) changes.push_back({game::stat::kMaxMp, 1, 50.0f});
                for (const game::ItemInstance::Option& o : w.item->options) {
                    if (const assets::MagicOptionRow* row = options != nullptr ? game::FindOption(*options, o.id) : nullptr) {
                        // The block rate's blues read the item's own block (a shield's white).
                        float ownBlock = 0.0f;
                        if ((row->attribute == 0x627269 || row->attribute == 0x62726E61) && game::KindOf(*w.row) == game::ItemKind::Shield) {
                            ownBlock = float(game::ComputeStats(*w.row, *w.item, options, w.setPlus, true).blocking.low);
                        }
                        MergeBlue(changes, game::BlueChanges(row->attribute, o.value, ownBlock), game::BlueSums(row->attribute));
                    }
                }
                if (!changes.empty()) sheet.Add(kWornSource + std::uint32_t(w.place), changes);
            }
            // The Devil's Spirit's: its plus's options, then its own blues, the spirit their source.
            if (!spirit.changes.empty()) sheet.Add(kWornSource + 0x100u + std::uint32_t(game::avatar::kSpirit), spirit.changes);
        }
        // The avatars' blues: the hat, the dress, the attachment and the flag (the Devil's Spirit's above), each its own
        // source; one whose
        // requirements fail gives nothing (as the worn places). Their luck ('luca') is the strengthening's (0xAD).
        player.itemLuck = 0.0f;
        if (const std::vector<assets::MagicOptionRow>* options = config.magicOptions.get(); options != nullptr && config.rules->items) {
            constexpr std::uint32_t kAvatarSource = 0xA7A70000u; // + the avatar place
            for (const auto& [place, item] : player.inventory.avatars) {
                if (place == game::avatar::kSpirit) continue;
                const assets::ItemRow* row = config.rules->items->Find(item.refId);
                if (row == nullptr || game::RequirementsMet(*row, WearerOf(player)) != game::EquipCheck::Ok) continue;
                std::vector<game::StatChange> changes;
                for (const game::ItemInstance::Option& o : item.options) {
                    const assets::MagicOptionRow* m = game::FindOption(*options, o.id);
                    if (m == nullptr) continue;
                    MergeBlue(changes, game::BlueChanges(m->attribute, o.value), game::BlueSums(m->attribute));
                }
                for (const game::StatChange& c : changes) {
                    if (c.stat == 0xAD) player.itemLuck += c.value;
                }
                if (!changes.empty()) sheet.Add(kAvatarSource + std::uint32_t(place), changes);
            }
        }
        // The complete armour set's bonus: the six places armour of one
        // family: clothes / robe walk and run x 1.2 and the MP cost 80 %, protector / light x 1.1 and 90 %, armor / heavy nothing.
        if (config.rules->items) {
            constexpr std::uint32_t kArmourSetSource = 0xA5E70000u;
            const int family = player.inventory.CompleteArmourFamily(*config.rules->items);
            const float factor = family == 1 || family == 9 ? 20.0f : family == 2 || family == 10 ? 10.0f : 0.0f;
            if (factor > 0.0f) {
                sheet.Add(kArmourSetSource, std::vector<game::StatChange>{{game::stat::kWalkSpeed, 2, factor}, {game::stat::kRunSpeed, 2, factor},
                                                                           {game::stat::kMpCost, 0, -factor}});
            }
        }
        // Its sets' standing rows of a code the keeper takes, each set their source (none in the 1.188 data: its 'rals'
        // goes to the state roll, StateDefenceOf; its 'reis' into the pieces' total plus, above).
        for (const auto& [setId, option] : player.sets.OtherStanding()) {
            constexpr std::uint32_t kSetSource = 0x5E700000u; // + the SetID
            const std::vector<game::StatChange> c = game::BlueChanges(option.code, option.value);
            if (!c.empty()) sheet.Add(kSetSource + std::uint32_t(setId), c);
        }
        // The states it is in: their changes on its numbers, as a monster's.
        if (player.combat.states.Mask() != 0) {
            const game::CombatStats base = game::PlayerCombatStats(player.level, player.strength, player.intelligence, worn, nullptr);
            player.combat.states.AddChanges(sheet, base);
        }
        // In berserk: its hwan level's speed (SPDU: walk and run at that percent
        // of themselves) and cut of damage taken (DABR, every lane), as the original
        // puts them; the damage dealt twice plus the stat 0xBD (CombatStats::berserk).
        if (player.berserkUntil) {
            const game::HwanLevel h = game::HwanLevelOf(config.hwanLevels, player.hwanLevel);
            constexpr std::uint32_t kBerserkSource = 0xB0B0B0B0u;
            std::vector<game::StatChange> mode = {{game::stat::kWalkSpeed, 3, float(h.speedPercent)},
                                                  {game::stat::kRunSpeed, 3, float(h.speedPercent)}};
            if (h.damageCut != 0) {
                for (std::uint16_t lane = 0; lane < 4; ++lane) {
                    mode.push_back({std::uint16_t(game::stat::kDamageTaken + lane), 2, -float(h.damageCut)});
                }
            }
            sheet.Add(kBerserkSource, mode);
        }
        // The bare hands after a weapon left its hand (from a move or from its durability at 0).
        const bool armed = std::any_of(worn.begin(), worn.end(), [](const game::WornItem& w) {
            return w.place == game::slot::kWeapon && !(w.item->durability == 0 && game::NewDurability(*w.row) != 0);
        });
        if (player.armed && !armed) player.bareHands = game::BareHands::AfterWeapon;
        player.armed = armed;
        // The items' own whites with their blues (hit rate, parry, durability).
        player.stats = game::PlayerCombatStats(player.level, player.strength, player.intelligence, worn, config.magicOptions.get(), sheet, player.bareHands);
        // Who wears what next: its masteries for the European armour's requirements.
        player.wearer.masteries = player.book.masteries;
        player.wearer.masteriesKnown = true;
        if (player.labMaxHp > 0) player.stats.maxHp = player.labMaxHp;
        player.stats.berserk = player.berserkUntil.has_value();
        player.stats.european = player.wearer.country == 1;
        player.hp = std::min(player.hp, player.stats.maxHp);
        player.mp = std::min(player.mp, player.stats.maxMp);
        if (player.entity != 0) {
            world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
            // Its stealth and detection: hidden from those who do not
            // detect it (ours: out of their view; the original hides it by its body mode), slowed.
            const Skills::Stealth stealth = skillUnit ? skillUnit->StealthOf(player.combat) : Skills::Stealth{};
            world->SetHidden(player.entity, stealth.mask, stealth.level);
            world->SetDetection(player.entity, stealth.detectMask, stealth.detectLevel);
            player.hidden = stealth.mask != 0;
            // A monster's shape walks at the monster's speed (its run over the players'; the transform
            // keeps its speeds; their use by the speed update inferred).
            float shape = 1.0f;
            if (player.combat.transform.mode == 1 && config.characters) {
                if (const assets::ObjectRow* row = config.characters->Find(player.combat.transform.model); row != nullptr && row->runSpeed > 0.0f) {
                    shape = row->runSpeed / std::max(1.0f, config.world.runSpeed);
                }
            }
            // Frostbite and slow: its walks at their percent (StateSet::SpeedPercent, as a monster's).
            const float states = float(player.combat.states.SpeedPercent()) / 100.0f;
            // On a ride or a transport: the mount's own speeds whole (Coses::Ride; the original
            // copies the vehicle's walk and run), never the rider's speed buffs, scrolls or potions, nor
            // its slowing states (they stay on the rider and count again when it gets off).
            if (player.riding) {
                world->SetSpeedFactor(player.entity, 1.0f);
            } else {
                world->SetSpeedFactor(player.entity, sheet.Value(game::stat::kRunSpeed, 1.0f) * (stealth.mask != 0 ? stealth.speed : 1.0f) * shape * states);
            }
        }
        if (peer != nullptr) SendVitals(*peer, player);
        if (peer != nullptr && player.entity != 0 && panel(player.stats) != before) SendProgress(*peer, player);
    }

    // The skills and the fight (Skills.h): the player as the unit sees it, its requests, its tick.
    SkillCaster CasterOf(PlayerWorld& p) { return {p.entity, p.book, p.stats, p.hp, p.mp, p.dead, p.untouchableUntil, p.combat}; }
    void InterruptCast(PlayerWorld& p, Clock::time_point now) {
        if (!skillUnit) return p.combat.StopFighting();
        SkillCaster caster = CasterOf(p);
        skillUnit->Interrupt(caster, now);
    }
    void Attack(Peer& peer, PlayerWorld& player, const proto::AttackTarget& a) {
        if (player.combat.states.Locked()) return; // frozen, asleep or stunned
        if (StructureRefused(peer, player, a.target)) return;
        SkillCaster caster = CasterOf(player);
        if (!skillUnit || !skillUnit->Attack(caster, a.target, Clock::now())) return;
        player.pickTarget = 0;
    }
    void UseSkill(Peer& peer, PlayerWorld& player, const proto::UseSkill& u) {
        // Knocked down or back: no skill until it stands (its actions were cancelled); frozen, asleep or
        // stunned, none while it lasts.
        if ((player.knocked != game::Knock::None && Clock::now() < player.knockedUntil) || player.combat.states.Locked()) {
            SendMessage(*peer.connection, proto::SkillUseAnswer{u.skill, proto::SkillUseStatus::NotYet});
            return;
        }
        if (StructureRefused(peer, player, u.target)) {
            SendMessage(*peer.connection, proto::SkillUseAnswer{u.skill, proto::SkillUseStatus::Target});
            return;
        }
        SkillCaster caster = CasterOf(player);
        if (!skillUnit || !skillUnit->Use(caster, u, Clock::now())) return;
        player.pickTarget = 0;
    }
    void FightPlayers(Clock::time_point now) {
        // Each player's party for the world's sight (a hidden one's members see it).
        if (parties) {
            for (auto& [peerId, player] : players) {
                if (player.entity != 0) world->SetParty(player.entity, parties->PartyOf(player.entity));
            }
        }
        // A knock over: it stands, its own speed again.
        for (auto& [peerId, player] : players) {
            if (player.entity == 0 || player.knocked == game::Knock::None || now < player.knockedUntil) continue;
            player.knocked = game::Knock::None;
            StatsChanged(player.entity);
        }
        if (!skillUnit) return;
        skillUnit->Objects(now);
        for (auto& [peerId, player] : players) {
            if (player.entity == 0) continue;
            SkillCaster caster = CasterOf(player);
            skillUnit->Fight(caster, now);
        }
    }

    // A unique of this server's world came, was killed or went: written for the website, with the
    // game server's time, and told on the shard's bus (sr_uniques_<schema>: kind, model, name, killer).
    void TellUnique(UniqueEvent::Kind kind, std::uint32_t refObjId, const proto::WorldPosition& at, const PlayerWorld* killer) {
        UniqueEvent e;
        e.kind = kind;
        e.refObjId = refObjId;
        const assets::ObjectRow* row = config.characters ? config.characters->Find(refObjId) : nullptr;
        e.codename = row != nullptr ? row->codename : std::to_string(refObjId);
        e.name = row != nullptr ? TextName(row->nameKey) : std::string();
        if (e.name.empty()) e.name = e.codename;
        e.server = config.id;
        e.region = at.region;
        e.x = at.x;
        e.z = at.z;
        if (killer != nullptr) {
            e.killerId = killer->selected;
            e.killerName = killer->name;
        }
        e.at = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (kind == UniqueEvent::Kind::Killed) {
            core::LogInfo("{}: the unique {} was killed by {}", config.name, e.name, e.killerName.empty() ? "no player" : e.killerName);
        } else {
            core::LogInfo("{}: the unique {} appeared in region {}", config.name, e.name, at.region);
        }
        // Told after it is written (one job), so a listener that reads on hearing it finds it (SroWeb).
        database.Submit([e, name = config.name, channel = "sr_uniques_" + config.shardSchema](Database& db) {
            if (!RecordUniqueEvent(db, e)) {
                core::LogWarning("{}: the unique {} could not be recorded", name, e.name);
                return;
            }
            Notify(db, channel, std::format("{}\t{}\t{}\t{}", int(e.kind), e.refObjId, e.name, e.killerName));
        });
    }

    // A kill's experience and drops: an exp-shared party's damage counts as
    // one and is shared among its members near the kill by level; the drops are the owner's (the best
    // damager of the group that did the most), its level deciding what is kept (Parties::ShareKill).
    void ShareKill(std::uint32_t monster, const Monsters::Damaged& d) {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(d.refObjId) : nullptr;
        if (row == nullptr) return;
        const auto where = world->Where(monster);
        const bool partyMonster = (row->rarity >> 4) == 1 || (d.rarity >> 4) == 1;
        const Parties::Kill kill = parties->ShareKill(d.damageBy, row->level, partyMonster,
                                                      where ? std::optional<proto::WorldPosition>(where->position) : std::nullopt);
        // Killed by an exp-shared party with two or more members near: its nest may hatch a party monster next.
        if (kill.partyMembers > 1 && monsters) monsters->PartyKilled(monster);
        if (config.drops && where && kill.owner != 0) {
            if (PlayerWorld* killer = PlayerByEntity(kill.owner)) {
                std::vector<DropTable::Drop> loot = config.drops->Roll(*row, d.rarity, killer->level, config.rates, fightRandom);
                // A socket stone's 'gdr' (the keeper's 0xB6): the gold heaps' factor + it / 100; not on
                // a unique (the stone's own text: SN_ITEM_ETC_SOCKET_STONE_GOLD_TT_DESC).
                const bool unique = (row->rarity & 0x0F) == 3 || (d.rarity & 0x0F) == 3;
                if (const float gdr = killer->combat.Sheet().Value(0xB6, 0.0f); gdr > 0.0f && !unique) {
                    for (DropTable::Drop& drop : loot) {
                        if (drop.gold > 0) drop.gold = std::uint64_t(std::trunc(double(drop.gold) * (1.0 + double(gdr) / 100.0)));
                    }
                }
                DropLoot(where->position, killer->selected, std::move(loot), d.rarity);
            }
        }
        for (const Parties::KillShare& share : kill.shares) {
            PlayerWorld* other = PlayerByEntity(share.entity);
            if (other == nullptr || other->dead) continue;
            if (Peer* peer = peers->Find(world->PeerOf(share.entity))) Reward(*peer, *other, monster, *row, d.rarity, share.share);
            // A share of an exp-shared party's experience puts its member in combat.
            if (kill.shares.size() > 1 && parties && (parties->SettingsOf(other->entity) & game::kPartyExpShare) != 0) {
                other->combat.battleUntil = Clock::now() + std::chrono::seconds(20);
            }
        }
        // The quests' kill and drops: the killing hit's attacker's (a pet's owner's), shared in its party by each quest's
        // DivideType (Quests::Killed).
        if (const std::uint32_t killer = d.killer != 0 ? d.killer : (d.damageBy.empty() ? 0u : d.damageBy.back().first);
            quests && killer != 0 && PlayerByEntity(killer) != nullptr) {
            quests->Killed(monster, row->codename, d.rarity, killer);
        }
        // A unique's death for the quests that wait for it, whoever killed it (the original's UNIQUE_DEAD event).
        if (quests && ((row->rarity & 0x0F) == 3 || (d.rarity & 0x0F) == 3)) quests->UniqueDied(row->codename);
        // The triggers of its world (an instance's, or the shared world's pillars):
        if (instances && where) instances->Killed(kill.owner, d, where->position);
        // The Pharaoh's tomb cleared at its hardest: Osiris dead in a Master tomb (INS_PHARAOH_MASTER, _RefGame_World
        // 59): a deed of everyone inside (the beta quests' degree 10).
        if (instances && where && where->position.layer != 0 && row->codename.starts_with("MOB_SD_OSIRIS")) {
            constexpr std::uint32_t kTombMaster = 59;
            for (const std::uint32_t player : instances->PlayersIn(where->position.layer))
                if (instances->WorldOf(player) == kTombMaster)
                    if (PlayerWorld* p = PlayerByEntity(player)) AddDeed(p->selected, "tomb-hardest");
        }
        if (flagWorlds && where) flagWorlds->Killed(d, where->position); // a Capture the Flag room's key
        if (arenas && where) arenas->Killed(d, where->position);         // a score map's points
        if (roc && where) roc->Killed(d, where->position, Clock::now()); // the Peak's followers and its Roc
        if (temples && where) temples->Killed(d, where->position);        // Haroeris: Seth's temple
        // A unique of the world's hives: who killed it (the killing hit's player).
        if (d.unique && where) TellUnique(UniqueEvent::Kind::Killed, d.refObjId, where->position, PlayerByEntity(d.killer));
    }

    // One arrow or bolt from the place beside the weapon (7), of the kind the weapon shoots; the
    // place emptied tells those who see the character (the quiver goes).
    bool TakeAmmunition(std::uint64_t peerId, PlayerWorld& player) {
        Peer* peer = peers->Find(peerId);
        const game::ItemInstance* ammo = player.inventory.At(game::slot::kShield);
        const game::ItemInstance* weapon = player.inventory.At(game::slot::kWeapon);
        if (peer == nullptr || ammo == nullptr || weapon == nullptr || !config.rules->items) return false;
        const assets::ItemRow* ammoRow = config.rules->items->Find(ammo->refId);
        const assets::ItemRow* weaponRow = config.rules->items->Find(weapon->refId);
        if (ammoRow == nullptr || weaponRow == nullptr || !game::IsAmmunition(*ammoRow) || ammoRow->typeId[3] != game::AmmunitionFor(*weaponRow)) {
            return false;
        }
        const std::uint32_t ref = ammo->refId;
        TakeOne(*peer, player, game::slot::kShield);
        if (player.inventory.At(game::slot::kShield) == nullptr) {
            // The last one gone: the bag's next stack of the same kind goes in its place.
            if (const int next = player.inventory.FindSameType(*config.rules->items, *ammoRow); next >= 0) {
                MoveItem(*peer, player, proto::ItemMove{std::uint8_t(next), std::uint8_t(game::slot::kShield), 0});
                if (player.inventory.At(game::slot::kShield) != nullptr) return true;
            }
            Broadcast(player.entity, proto::EquipmentChanged{player.entity, std::uint8_t(game::slot::kShield), 0u, std::uint8_t(0), ref});
            world->SetEquipment(player.entity, player.inventory.Worn());
        }
        return true;
    }

    // --- The skills' host (Skills.h) ------------------------------------------------------------
    std::unique_ptr<Skills> skillUnit;
    PlayerWorld* PlayerByEntity(std::uint32_t entity) {
        const auto it = players.find(world->PeerOf(entity));
        return it != players.end() && it->second.entity == entity ? &it->second : nullptr;
    }
    const PlayerWorld* PlayerByEntity(std::uint32_t entity) const {
        const auto it = players.find(world->PeerOf(entity));
        return it != players.end() && it->second.entity == entity ? &it->second : nullptr;
    }
    std::optional<proto::WorldPosition> Position(std::uint32_t entity) const override {
        const auto place = world->Where(entity);
        if (!place) return std::nullopt;
        return place->position;
    }
    float Facing(std::uint32_t entity) const override {
        const auto place = world->Where(entity);
        return place ? place->yaw : 0.0f;
    }
    bool Moving(std::uint32_t entity) const override { return world->Moving(entity); }
    void MoveTo(std::uint32_t entity, const proto::WorldPosition& to) override { world->MoveTo(entity, to); }
    // --- PvP (guild wars) -----------------------------------------------

    // A player who may be fought: alive in the world, its guild at war, or in a fortress war's area
    // on its side (Fortresses::Fighting; who may hit it is Hostile's).
    const PlayerWorld* Fightable(std::uint32_t entity) const {
        const auto it = players.find(world->PeerOf(entity));
        if (it == players.end() || it->second.entity != entity || it->second.dead || !guilds) return nullptr;
        if (guilds->AtAnyWar(it->second.selected) || it->second.freePvp != 0) return &it->second; // at war, or in a free PvP team
        if (flagWorlds && flagWorlds->InMatch(entity)) return &it->second; // a Capture the Flag match: who may strike him is PvpVerdict's
        if (arenas && arenas->InMatch(entity)) return &it->second;         // a Battle Arena match: the same
        if (it->second.jobState != kNoJob && OnBattlefield(entity)) return &it->second;            // a job's suit in the field
        if (PkTarget(it->second)) return &it->second; // the PK rules': who may strike him is PvpVerdict's
        const auto place = fortresses ? world->Where(entity) : std::nullopt;
        return place && fortresses->Fighting(it->second.selected, place->position.region) ? &it->second : nullptr;
    }
    bool IsStructure(std::uint32_t entity) const override { return fortresses && fortresses->IsStructure(entity); }
    bool InFortressWorld(std::uint32_t entity) const override {
        const auto at = world->Where(entity);
        return at && fortresses && fortresses->FortressArea(at->position.region);
    }
    std::optional<proto::WorldPosition> NearestPointOf(std::uint32_t target, const proto::FlatPosition& from) const override {
        return fortresses ? fortresses->NearestPoint(target, from.x, from.z) : std::nullopt;
    }
    std::optional<game::WallSpec> WallOf(std::uint32_t entity) const override {
        const PlayerWorld* player = PlayerByEntity(entity);
        return player != nullptr && skillUnit ? skillUnit->WallOf(player->combat) : std::nullopt;
    }
    void WallTook(std::uint32_t entity, std::uint32_t amount) override {
        PlayerWorld* player = PlayerByEntity(entity);
        if (player == nullptr || !skillUnit) return;
        SkillCaster caster = CasterOf(*player);
        if (skillUnit->WallTook(caster, amount)) StatsChanged(player->entity);
    }
    std::optional<std::uint32_t> StructureDamage(std::uint32_t attacker, std::uint32_t target) override {
        if (!fortresses || !fortresses->IsStructure(target)) return std::nullopt;
        const PlayerWorld* player = PlayerByEntity(attacker);
        if (player == nullptr) return 0u; // only a player's hit counts
        const game::ItemInstance* weapon = player->inventory.At(game::slot::kWeapon);
        const assets::ItemRow* row = weapon != nullptr && config.rules->items ? config.rules->items->Find(weapon->refId) : nullptr;
        if (row == nullptr || (weapon->durability == 0 && game::NewDurability(*row) != 0)) return 1u; // bare hands; a broken weapon adds nothing
        const game::StructureWeapon kind = game::StructureWeaponOf(row->typeId[3]);
        if (kind.factor <= 0.0) return 1u;
        // The keeper's copies 0xBE..0xC1: the weapon's own whites with its total plus (its 'rein', its set's raise), the float
        // ends truncated below.
        const game::ItemStats stats = game::ComputeStats(*row, *weapon, config.magicOptions.get(), player->setRaise[game::slot::kWeapon], true);
        const auto& attack = kind.magic ? stats.magAttack : stats.phyAttack;
        const int low = int(attack.low), high = int(attack.high);
        if (high <= low) return 0u;
        return std::uint32_t(double(std::uniform_int_distribution<int>(low, high)(fightRandom)) * kind.factor);
    }
    // A player, or a player's COS (a transport, a pet): judged as its owner.
    bool IsPlayer(std::uint32_t entity) const override {
        const auto it = players.find(world->PeerOf(entity));
        if (it != players.end() && it->second.entity == entity) return true;
        return coses && coses->OwnerOf(entity) != 0;
    }
    // A fortress weapon's skills: its Param5 mask and the player's fortress position share a bit (a guild member's).
    bool FortressWeaponAllowed(std::uint32_t entity) const override {
        const PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return true;
        const game::ItemInstance* weapon = p->inventory.At(game::slot::kWeapon);
        const assets::ItemRow* row = weapon != nullptr && config.rules->items ? config.rules->items->Find(weapon->refId) : nullptr;
        if (row == nullptr) return true;
        return (row->params[4] & (guilds ? guilds->PositionOf(p->selected) : 0)) != 0;
    }
    // The player a target stands for: itself, or a COS's owner.
    std::uint32_t AsOwner(std::uint32_t entity) const {
        const std::uint32_t owner = coses ? coses->OwnerOf(entity) : 0u;
        return owner != 0 ? owner : entity;
    }
    bool OnBattlefield(std::uint32_t entity) const {
        const auto place = world->Where(entity);
        return place && !BanditTown(place->position.region) && !InTownArea(place->position);
    }
    // In a town's safe area (regioninfo's town rectangles);
    // never in an instance's layer. Town ground is no battlefield: the attack check's first rule refuses any player on
    // a player there (0x3018, game::AttackCheck), and no monster comes in (Monsters::SetTownAreas).
    bool InTownArea(const proto::WorldPosition& at) const {
        return at.layer == 0 && towns && towns->TownAt(at.region, at.x, at.z) >= 0;
    }
    // The jobs' PvP, safe trade, guild wars, free PvP, the fortress war and the PK rules are one check: PvpVerdict.
    proto::SkillUseStatus AttackRefusal(std::uint32_t attacker, std::uint32_t target) const override {
        const PlayerWorld* a = PlayerByEntity(attacker);
        if (a == nullptr) return proto::SkillUseStatus::Ok;
        // An untouchable target (a player, or a COS of one) is not attacked at all (0x3020), not hit for 0.
        {
            const PlayerWorld* t = PlayerByEntity(target);
            if (t == nullptr && coses) t = PlayerByEntity(coses->OwnerOf(target));
            if (t != nullptr && Clock::now() < t->untouchableUntil) return proto::SkillUseStatus::Untouchable;
        }
        // A rider neither attacks nor casts (0x3021).
        if (a->riding) return proto::SkillUseStatus::Riding;
        // A job monster (0x3006): a thief monster not by thieves, a hunter monster not by traders nor
        // hunters, neither by a player without a job.
        if (const assets::ObjectRow* row = monsters && config.characters ? config.characters->Find(MonsterRef(target)) : nullptr;
            row != nullptr && assets::IsBandit(*row)) {
            const bool thiefMonster = row->typeId[3] == 2;
            if (a->jobState == kNoJob || (thiefMonster && a->jobState == std::uint8_t(game::JobType::Thief)) ||
                (!thiefMonster && a->jobState != std::uint8_t(game::JobType::Thief))) {
                return proto::SkillUseStatus::JobMonster;
            }
            return proto::SkillUseStatus::Ok;
        }
        // A player (or its COS): the attack check's order and codes (the jobs' PvP, safe trade, the
        // battlefield, the party, the PK rules); refused without a code: the caller's own refusal.
        if (const auto verdict = PvpVerdict(attacker, target, game::AttackMode::Chosen); verdict && !verdict->allowed && verdict->code != proto::SkillUseStatus::Target) {
            return verdict->code;
        }
        return proto::SkillUseStatus::Ok;
    }
    std::uint32_t MonsterRef(std::uint32_t entity) const { return monsters ? monsters->RefOf(entity) : 0u; }
    // A quest trap's catch (Skills::SetTrap): the monster's reference, and its owner's quests told (the event 0x1D).
    std::uint32_t MonsterRefOf(std::uint32_t monster) const override { return MonsterRef(monster); }
    void QuestTrapped(std::uint32_t owner, std::uint32_t monster) override {
        const assets::ObjectRow* row = quests && config.characters ? config.characters->Find(MonsterRef(monster)) : nullptr;
        if (row != nullptr) quests->Trapped(owner, monster, row->codename);
    }
    // Whether a player may strike a player or his COS: the attack check (guild wars, the fortress
    // war, free PvP, the jobs and the PK rules in its order), a chosen target's mode.
    bool Hostile(std::uint32_t attacker, std::uint32_t target) const override {
        if (attacker == target) return false;
        const auto verdict = PvpVerdict(attacker, target, game::AttackMode::Chosen);
        return verdict && verdict->allowed;
    }
    bool DuplicateBarred(std::uint32_t target, bool murderer) const override {
        const PlayerWorld* p = PlayerByEntity(target);
        if (p == nullptr) return false;
        if (murderer) return p->pvpState == std::uint8_t(game::PvpState::Murderer);
        return ActivityOf(*p).jobSuit || p->freePvp != 0;
    }
    // An area attack's targets (mode 3: no new victims; an innocent's area never on an assaulter).
    bool HostileInArea(std::uint32_t attacker, std::uint32_t target) const override {
        if (attacker == target) return false;
        const auto verdict = PvpVerdict(attacker, target, game::AttackMode::Area);
        return verdict && verdict->allowed;
    }
    // A player's transport or pet a job's foe may strike (judged as its owner).
    bool FightableCos(std::uint32_t entity) const {
        if (!coses || coses->StatsOf(entity) == nullptr) return false;
        const std::uint32_t owner = coses->OwnerOf(entity);
        return owner != 0 && Fightable(owner) != nullptr;
    }
    const game::CombatStats* Monster(std::uint32_t entity) const override {
        if (const game::CombatStats* m = monsters ? monsters->Stats(entity) : nullptr) return m;
        if (FightableCos(entity)) return coses->StatsOf(entity);
        const PlayerWorld* p = Fightable(entity);
        return p != nullptr ? &p->stats : nullptr;
    }
    std::uint32_t MonsterHp(std::uint32_t entity) const override {
        if (const PlayerWorld* p = Fightable(entity)) return p->hp;
        if (FightableCos(entity)) return coses->HpOf(entity);
        return monsters ? monsters->Hp(entity) : 0u;
    }
    std::vector<std::uint32_t> MonstersNear(const proto::WorldPosition& at, float radius) const override {
        return monsters ? monsters->Near(at, radius) : std::vector<std::uint32_t>{};
    }
    std::vector<std::uint32_t> PlayersNear(const proto::WorldPosition& at, float radius) const override {
        std::vector<std::uint32_t> out;
        const proto::FlatPosition c = proto::ToFlat(at);
        for (const auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            const auto place = world->Where(p.entity);
            if (!place) continue;
            const proto::FlatPosition q = proto::ToFlat(place->position);
            if (std::hypot(q.x - c.x, q.z - c.z) <= double(radius)) out.push_back(p.entity);
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    std::optional<Monsters::Target> MonsterTarget(std::uint32_t entity) const override {
        // A player: its level against states, no states held (ours: players take no states yet); its reference's
        // Knockdown bits (every player character's 3: it falls and is pushed) and KO_RecoverTime; down while knocked down.
        if (const PlayerWorld* p = Fightable(entity)) {
            Monsters::Target t;
            t.defence = StateDefenceOf(*p);
            const assets::ObjectRow* row = config.characters ? config.characters->Find(p->model) : nullptr;
            t.canFall = row == nullptr || (row->knockdown & 1) != 0;
            t.canPush = row == nullptr || (row->knockdown & 2) != 0;
            t.recoverMs = row != nullptr ? row->knockdownRecoverMs : 0;
            t.down = p->knocked == game::Knock::Down && Clock::now() < p->knockedUntil;
            return t;
        }
        return monsters ? monsters->TargetOf(entity) : std::nullopt;
    }
    std::vector<proto::StateRecord> MonsterStates(std::uint32_t entity, Clock::time_point now) const override {
        if (const PlayerWorld* p = Fightable(entity)) return StateRecords(*p, now); // a player struck in PvP: its own
        return monsters ? monsters->StatesOf(entity, now) : std::vector<proto::StateRecord>{};
    }
    Monsters::Damaged HitMonster(std::uint32_t monster, std::uint32_t attacker, std::uint32_t damage, Clock::time_point now,
                                 const Monsters::Affliction* affliction) override {
        if (fortresses && fortresses->IsStructure(monster)) return HitStructure(monster, attacker, damage, now);
        if (FightableCos(monster)) return HitCos(monster, attacker, damage);
        if (Fightable(monster) != nullptr) return HitPlayer(monster, attacker, damage, now, affliction);
        Monsters::Damaged d = monsters ? monsters->Damage(monster, attacker, damage, now, affliction) : Monsters::Damaged{};
        if (coses && d.hit && !d.killed) coses->OwnerFights(attacker, monster); // an aggressive attack pet joins in
        if (d.killed) {
            if (PlayerWorld* player = PlayerByEntity(attacker)) RollBerserk(*player, d, monster);
        }
        return d;
    }
    // A player's hit on a player: its guards (the Snow Shield, the links' shares) as a monster's hit
    // meets them, its knock (KnockPlayer); its states and reflection not yet (ours).
    // A player's hit on a COS (its own health; a transport's goods drop with its death: step 7).
    // A hit on a COS: its own health, and its owner in combat for 20 s (the owner **inferred**).
    void HurtCos(std::uint32_t cos, std::uint32_t attacker, const game::HitResult& hit) {
        if (!coses) return;
        coses->Hurt(cos, attacker, hit);
        if (PlayerWorld* owner = PlayerByEntity(coses->OwnerOf(cos))) owner->combat.battleUntil = Clock::now() + std::chrono::seconds(20);
    }
    // A monster's skill's states on a COS it struck (the "magic" thieves' freeze and burn on a transport, the user's
    // report 2026-11-22), as on a player: none on a blocked hit or a dead COS, through the monster's chance history,
    // against the COS's level (Coses::StateDefenceOf).
    void AfflictCos(std::uint32_t cos, std::uint32_t monster, const game::HitResult& hit, const assets::SkillRow* skill, Clock::time_point now) {
        if (!coses || hit.blocked || skill == nullptr || coses->HpOf(cos) == 0) return;
        const std::vector<game::StateGift> gifts = game::StateGiftsOf(*skill);
        const auto defence = coses->StateDefenceOf(cos);
        if (gifts.empty() || !defence) return;
        const int pulse = assets::FindSkillParam(*skill, assets::SkillTag("puls")).Int(0);
        coses->Afflict(cos, game::RollStates(gifts, *defence, coses->SusceptibilityOf(cos), monsterStateRolls[monster], fightRandom, monster, skill->id, pulse, now),
                       now);
    }
    void CosStates(std::uint32_t entity, std::vector<proto::StateRecord> states) override { TellStates(entity, std::move(states), Clock::now()); }
    void CosKilled(std::uint32_t cos, std::uint32_t model, const std::vector<std::pair<std::uint32_t, std::uint64_t>>& damageBy, std::uint32_t killer) override {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(model) : nullptr;
        const auto where = world->Where(cos);
        PlayerWorld* owner = coses ? PlayerByEntity(coses->OwnerOf(cos)) : nullptr;
        PlayerWorld* by = PlayerByEntity(AsOwner(killer));
        if (row == nullptr || !where) return;
        // A riding horse killed by a team's foe (the kill relation 2: an event, the arena, free PvP) credits nobody.
        if (game::CosKindOf(*row) == game::CosKind::Ride && owner != nullptr && by != nullptr && KillRelationOf(*owner, by) == game::KillRelation::Team) return;
        // Its experience by the damage's shares (a COS's damage its owner's; monsters' damage none), a party's split.
        std::vector<std::pair<std::uint32_t, std::uint64_t>> players;
        for (const auto& [attacker, damage] : damageBy) {
            const std::uint32_t who = AsOwner(attacker);
            if (PlayerByEntity(who) == nullptr) continue;
            const auto same = std::find_if(players.begin(), players.end(), [&](const auto& e) { return e.first == who; });
            if (same != players.end()) same->second += damage;
            else players.push_back({who, damage});
        }
        if (parties && !players.empty()) {
            const Parties::Kill kill = parties->ShareKill(players, row->level, false, where->position);
            for (const Parties::KillShare& share : kill.shares) {
                PlayerWorld* other = PlayerByEntity(share.entity);
                if (other == nullptr || other->dead) continue;
                if (Peer* peer = peers->Find(world->PeerOf(share.entity))) Reward(*peer, *other, cos, *row, 0, share.share);
            }
        }
        if (by == nullptr) return;
        // A murderer's penalty points less, as for a monster of its level; the quests' kill event.
        if (by->pkPoints > 0) {
            AddPkPoints(*by, -game::MonsterKillRelief(row->level, by->level));
            TellPk(*by);
        }
        if (quests) quests->Killed(cos, row->codename, 0, by->entity);
        core::LogInfo("{}: {} kills {}'s {} ({})", config.name, by->name, owner != nullptr ? owner->name : std::string("?"), row->codename, cos);
    }
    Monsters::Damaged HitCos(std::uint32_t cos, std::uint32_t attacker, std::uint32_t damage) {
        Monsters::Damaged d;
        if (!coses) return d;
        d.hit = true;
        game::HitResult hit;
        hit.damage = damage;
        HurtCos(cos, attacker, hit);
        d.hp = coses->HpOf(cos);
        d.killed = d.hp == 0;
        return d;
    }
    Monsters::Damaged HitPlayer(std::uint32_t target, std::uint32_t attacker, std::uint32_t damage, Clock::time_point now,
                                const Monsters::Affliction* affliction = nullptr) {
        Monsters::Damaged d;
        PlayerWorld* victim = PlayerByEntity(target);
        if (victim == nullptr || victim->dead) return d;
        // A rider: the hit strikes his mount whole.
        if (coses && victim->riding) {
            if (const std::uint32_t mount = coses->MountOf(victim->entity); mount != 0) {
                game::HitResult hit;
                hit.damage = damage;
                HurtCos(mount, attacker, hit);
                return d; // nothing on the rider himself
            }
        }
        d.hit = true;
        if (now < victim->untouchableUntil) damage = 0;
        if (skillUnit && damage > 0) {
            SkillCaster caster = CasterOf(*victim);
            const Skills::Guarded guard = skillUnit->Guard(caster, attacker, damage, false, 0.0, now);
            if (guard.mpTaken > 0) VitalsChanged(victim->entity);
            damage = guard.damage;
        }
        victim->hp = damage >= victim->hp ? 0 : victim->hp - damage;
        victim->lastHurt = now;
        CancelSuitChange(*victim); // a hit gives up a job suit's change
        HitWhileSitting(*victim, now); // a hit stands a sitting player up
        victim->combat.battleUntil = now + std::chrono::seconds(20);
        world->SetHp(victim->entity, victim->hp);
        if (Peer* peer = peers->Find(world->PeerOf(target))) SendVitals(*peer, *victim);
        d.hp = victim->hp;
        d.killed = victim->hp == 0;
        d.damageBy.push_back({attacker, damage});
        // A player's hit (or his COS's): the hostility records.
        if (PlayerWorld* striker = PlayerByEntity(AsOwner(attacker)); striker != nullptr && striker != victim) RegisterHostility(*striker, *victim, target);
        if (!d.killed && damage > 0 && !victim->combat.states.Damaged(victim->combat.rolls, fightRandom).empty()) d.statesChanged = true;
        if (!d.killed && affliction != nullptr && !affliction->states.empty()) GiveStates(*victim, affliction->states, now);
        if (!d.killed && affliction != nullptr && affliction->knock != game::Knock::None) KnockPlayer(*victim, *affliction, now, d);
        return d;
    }
    // A player against a state's roll: its level, its jewellery's resistance options, its passives' (game::PlayerStateDefence).
    game::StateDefence StateDefenceOf(const PlayerWorld& player) const {
        std::vector<game::WornItem> worn;
        if (config.rules->items) {
            for (int place = 0; place < game::slot::kWorn; ++place) {
                const game::ItemInstance* item = player.inventory.At(std::uint8_t(place));
                if (item == nullptr) continue;
                // Above the character's level, or its masteries short of the piece's (European armour): worn, but nothing of it
                // counts (its stats, its kind, its options, its sockets; the user's call 2026-11-13 for the level, the
                // masteries the same).
                if (const assets::ItemRow* row = config.rules->items->Find(item->refId);
                    row != nullptr && game::RequirementsMet(*row, WearerOf(player)) == game::EquipCheck::Ok) {
                    worn.push_back({place, row, item});
                }
            }
        }
        game::StateDefence defence = game::PlayerStateDefence(player.level, worn, config.magicOptions.get(), &player.combat.passives);
        player.sets.AddResists(defence); // its sets' 'rals' standing on the keeper
        return defence;
    }
    // States a hit gave a player: put on it; frozen, asleep or stunned it stops where it stands and
    // its fight and cast end; told to those who see it and to itself; its numbers again (frostbite's and slow's speed).
    void GiveStates(PlayerWorld& player, const std::vector<game::ActiveState>& took, Clock::time_point now) {
        bool changed = false;
        for (const game::ActiveState& s : took) {
            if (!player.combat.states.Apply(s)) continue;
            changed = true;
            core::LogInfo("{}: {} takes {} level {} for {} ms from {}", config.name, player.name, game::StateName(s.kind), s.level, s.durationMs, s.source);
        }
        if (!changed) return;
        if (player.combat.states.Locked()) {
            if (const auto place = world->Where(player.entity); place && world->Moving(player.entity)) world->MoveTo(player.entity, place->position);
            // A stun, a freeze, a sleep: its command list emptied and its cast given up, told.
            InterruptCast(player, now);
            player.combat.castUntil = {};
            player.traceTarget = 0;
            player.pickTarget = 0;
        }
        TellStates(player.entity, StateRecords(player, now), now);
        StatsChanged(player.entity);
    }
    // Each update: the players' states' damage (burn, poison, shock, bleed: told as a state's hit) and ends.
    void TickPlayerStates(Clock::time_point now) {
        for (auto& [peerId, player] : players) {
            if (player.entity == 0 || player.dead || player.combat.states.Mask() == 0) continue;
            bool vitals = false;
            for (const game::StateTick& t : player.combat.states.Ticks(now, player.hp, player.mp, player.stats.magAbsorption)) {
                player.hp = t.hp >= player.hp ? 0 : player.hp - t.hp;
                player.mp = t.mp >= player.mp ? 0 : player.mp - t.mp;
                vitals = true;
                std::uint8_t flags = proto::kDamageState;
                if (player.hp == 0) flags |= proto::kDamageKilled;
                Broadcast(player.entity, proto::EntityDamaged{player.entity, t.source, t.hp, player.hp, flags});
                if (player.hp == 0) break;
            }
            if (vitals) {
                world->SetHp(player.entity, player.hp);
                VitalsChanged(player.entity);
            }
            if (player.hp == 0) {
                player.combat.states.Clear();
                TellStates(player.entity, {}, now);
                Die(player);
                continue;
            }
            if (!player.combat.states.Expire(now).empty()) {
                TellStates(player.entity, StateRecords(player, now), now);
                StatsChanged(player.entity);
            }
        }
    }
    // A knock on a player (B):
    // pushed away from the attacker, 20 when knocked down, the skill's 'kb' distance when pushed (along the floor: a wall
    // stops it where it stands; ours: a 0.3 s slide, as a monster's), its actions cancelled (the cast, the
    // attack, the walk), down (motion 8) for its recover time and the skill's, back (0x10) for 2 s; the end restores its
    // speed (FightPlayers).
    void KnockPlayer(PlayerWorld& victim, const Monsters::Affliction& a, Clock::time_point now, Monsters::Damaged& result) {
        const auto place = world->Where(victim.entity);
        if (!place) return;
        const proto::FlatPosition from = proto::ToFlat(a.from), here = proto::ToFlat(place->position);
        double dx = here.x - from.x, dz = here.z - from.z;
        const double length = std::hypot(dx, dz);
        if (length < 0.001) {
            dx = 0.0;
            dz = 1.0;
        } else {
            dx /= length;
            dz /= length;
        }
        const float distance = a.knock == game::Knock::Down ? game::kKnockDownDistance : a.distance;
        proto::WorldPosition to = proto::FromFlat({here.x + dx * distance, here.z + dz * distance}, place->position.y);
        InterruptCast(victim, now); // its cast given up, told
        victim.combat.castUntil = {};
        victim.traceTarget = 0;
        victim.pickTarget = 0;
        // A knock-back slides at 250 a second (P5); a knock-down's fall takes 0.3 s.
        const float speed = a.knock == game::Knock::Down ? distance / 0.3f : game::kKnockBackSpeed;
        world->SetSpeedFactor(victim.entity, std::max(1.0f, speed / std::max(1.0f, config.world.runSpeed)));
        if (world->MoveTo(victim.entity, to) != World::MoveRefused::None) {
            to = place->position;
            world->MoveTo(victim.entity, to);
        }
        victim.knocked = a.knock;
        victim.knockedUntil = now + std::chrono::milliseconds(a.knockMs);
        result.knock = a.knock;
        result.knockTo = to;
        result.knockMs = a.knockMs;
    }
    // An entity's states changed: everyone who sees it, and newcomers after its appearance (World).
    void TellStates(std::uint32_t entity, std::vector<proto::StateRecord> states, Clock::time_point now) {
        world->SetStates(entity, states, now);
        Broadcast(entity, proto::EntityStates{entity, std::move(states)});
    }
    void Killed(std::uint32_t monster, const Monsters::Damaged& d) override {
        // A god's linked loot body (Haroeris', Seth's _DROP) dies with him, struck whole in his killer's name: its loot
        // falls for the killer.
        if (const std::uint32_t body = monsters ? monsters->LinkedBody(monster) : 0u; body != 0) {
            const std::uint32_t by = d.killer != 0 ? d.killer : (d.damageBy.empty() ? 0u : d.damageBy.back().first);
            const Monsters::Damaged struck = monsters->Damage(body, by, std::max(1u, monsters->Hp(body)), Clock::now());
            if (struck.killed) Killed(body, struck);
        }
        // A fortress war's guard or tablet monster: the war's (no experience, no drops).
        if (fortresses && fortresses->IsWarObject(monster) && !fortresses->IsStructure(monster)) {
            const PlayerWorld* killer = d.damageBy.empty() ? nullptr : PlayerByEntity(d.killer != 0 ? d.killer : d.damageBy.back().first);
            fortresses->StructureDestroyed(monster, killer != nullptr ? killer->selected : 0);
            return;
        }
        if (const auto it = structureKillers.find(monster); it != structureKillers.end()) {
            const std::int64_t killer = it->second;
            structureKillers.erase(it);
            if (fortresses) fortresses->StructureDestroyed(monster, killer); // no experience, no drops
            return;
        }
        if (PlayerWorld* victim = PlayerByEntity(monster)) {
            // A player killed by a player (or his COS): its death (a player killer's penalty) and the war's count.
            const std::uint32_t striker = d.killer != 0 ? d.killer : (d.damageBy.empty() ? 0u : d.damageBy.back().first);
            PlayerWorld* killer = d.damageBy.empty() ? nullptr : PlayerByEntity(AsOwner(striker));
            // A pet's kill is its owner's, scored in a guild war by the pet's own level (the original's).
            if (!victim->dead) Die(*victim, killer, 0, coses ? coses->LevelOf(striker) : 0);
            return;
        }
        core::LogInfo("{}: monster {} ({}) killed", config.name, monster, d.refObjId);
        monsterStateRolls.erase(monster);
        // The killer's penalty points by the monster's level (from both
        // monster kill paths).
        if (PlayerWorld* killer = PlayerByEntity(AsOwner(d.killer != 0 ? d.killer : (d.damageBy.empty() ? 0u : d.damageBy.back().first)));
            killer != nullptr && killer->pkPoints > 0) {
            if (const assets::ObjectRow* row = config.characters ? config.characters->Find(d.refObjId) : nullptr) {
                AddPkPoints(*killer, -game::MonsterKillRelief(row->level, killer->level));
                TellPk(*killer);
            }
        }
        // A job monster's kill: its job experience.
        if (const assets::ObjectRow* row = config.characters ? config.characters->Find(d.refObjId) : nullptr; row != nullptr && assets::IsBandit(*row)) {
            std::uint32_t killer = d.killer != 0 ? d.killer : (d.damageBy.empty() ? 0u : d.damageBy.back().first);
            if (coses && coses->OwnerOf(killer) != 0) killer = coses->OwnerOf(killer);
            if (killer != 0) BanditKilled(killer, *row);
        }
        if (!coses) return ShareKill(monster, d);
        // An attack pet's damage is its owner's (the kill's experience, drops and quests; Coses.h).
        Monsters::Damaged credited = d;
        credited.damageBy.clear();
        for (const auto& [attacker, damage] : d.damageBy) {
            const std::uint32_t owner = coses->OwnerOf(attacker);
            const std::uint32_t who = owner != 0 ? owner : attacker;
            const auto same = std::find_if(credited.damageBy.begin(), credited.damageBy.end(), [&](const auto& e) { return e.first == who; });
            if (same != credited.damageBy.end()) {
                same->second += damage;
            } else {
                credited.damageBy.push_back({who, damage});
            }
        }
        if (const std::uint32_t owner = coses->OwnerOf(d.killer); owner != 0) credited.killer = owner;
        ShareKill(monster, credited);
    }
    bool InSight(std::uint32_t caster, std::uint32_t target) const override {
        // A fortress's structure stands on its own navigation object (a walk to its middle stops at its side): reached
        // at its contact point, as the attack's reach measures it.
        if (fortresses && fortresses->IsStructure(target)) return true;
        const auto a = world->Where(caster), b = world->Where(target);
        if (!a || !b || !world->HasNavigation()) return true;
        if (a->position.layer != b->position.layer) return false;
        const std::optional<proto::WorldPosition> end = world->WalkEnd(a->position, b->position);
        if (!end) return true; // the caster on no floor (a test, a structure): nothing to check against
        const proto::FlatPosition e = proto::ToFlat(*end), t = proto::ToFlat(b->position);
        return std::hypot(e.x - t.x, e.z - t.z) <= 3.0; // the walk reached it
    }
    int TakeAmmunition(std::uint32_t caster, int count) override {
        // A row's arrows: its count x its hits, as many as there are (one when only one is left).
        PlayerWorld* player = PlayerByEntity(caster);
        if (player == nullptr) return 0;
        int taken = 0;
        while (taken < count && TakeAmmunition(world->PeerOf(caster), *player)) ++taken;
        return taken;
    }
    void VitalsChanged(std::uint32_t caster) override {
        PlayerWorld* player = PlayerByEntity(caster);
        if (player == nullptr) return;
        world->SetVitals(player->entity, player->hp, player->stats.maxHp, player->mp, player->stats.maxMp);
        if (Peer* peer = peers->Find(world->PeerOf(caster))) SendVitals(*peer, *player);
    }
    void StatsChanged(std::uint32_t entity) override {
        PlayerWorld* player = PlayerByEntity(entity);
        if (player == nullptr) return;
        Peer* peer = peers->Find(world->PeerOf(entity));
        UpdateStats(peer, *player);
        if (peer != nullptr) SendProgress(*peer, *player);
        TellBuffs(*player);
    }
    // Its buffs changed (every change of them comes through StatsChanged): those who see it are told
    // their skills, so their effects show on it there too; newcomers are told after its appearance.
    void TellBuffs(PlayerWorld& player) {
        if (player.entity == 0) return;
        std::vector<std::uint32_t> skills;
        for (const auto& b : player.combat.buffs) skills.push_back(b.skill);
        if (skills == player.toldBuffs) return;
        player.toldBuffs = skills;
        world->SetBuffs(player.entity, skills);
        Broadcast(player.entity, proto::EntityBuffs{player.entity, std::move(skills)});
    }
    std::uint32_t SpawnObject(std::uint32_t owner, const assets::SkillRow& skill, const proto::WorldPosition& at) override {
        const auto place = world->Where(owner);
        return world->AddSkillObject(skill.id, owner, at, place ? place->yaw : 0.0f);
    }
    void RemoveObject(std::uint32_t object) override { world->Leave(object); }
    std::optional<Monsters::Corpse> MonsterCorpse(std::uint32_t entity) const override {
        return monsters ? monsters->CorpseOf(entity) : std::nullopt;
    }
    void DropItem(std::uint32_t owner, std::uint32_t item, std::uint32_t data) override {
        PlayerWorld* player = PlayerByEntity(owner);
        const auto place = world->Where(owner);
        if (player == nullptr || !place) return;
        DropTable::Drop drop;
        drop.item.refId = item;
        drop.item.durability = data; // the mask's monster (its _items.data)
        DropLoot(place->position, player->selected, {drop});
    }
    // A transform begun or over: the look shown (a monster's model, the
    // copied player's look, or its own), told to all who see it and kept for newcomers; its speeds.
    void LookChanged(std::uint32_t entity) override {
        PlayerWorld* player = PlayerByEntity(entity);
        if (player == nullptr) return;
        const SkillState::Transform& t = player->combat.transform;
        std::optional<proto::CharacterSummary> shown;
        if (t.mode == 1 && t.model != 0) {
            shown = proto::CharacterSummary{};
            shown->model = t.model;
        } else if (t.mode == 2) {
            shown = world->OwnLook(t.copied);
        }
        world->SetShownLook(entity, shown);
        const proto::CharacterSummary look = shown ? *shown : world->OwnLook(entity).value_or(proto::CharacterSummary{});
        Broadcast(entity, proto::EntityLook{entity, look.model, look.height, look.equipment, look.avatars});
        StatsChanged(entity);
    }
    void AddThreat(std::uint32_t monster, std::uint32_t from, std::int64_t threat) override {
        if (monsters && threat > 0) monsters->AddThreat(monster, from, threat, Clock::now());
    }
    void MonsterBuffs(std::uint32_t monster, std::vector<std::uint32_t> skills) override {
        world->SetBuffs(monster, skills);
        Broadcast(monster, proto::EntityBuffs{monster, std::move(skills)});
    }
    void HealThreat(std::uint32_t healer, std::uint32_t healed, std::int64_t threat) override {
        if (!monsters || threat <= 0) return;
        const auto now = Clock::now();
        for (const std::uint32_t m : monsters->TargetingOf(healed)) monsters->AddThreat(m, healer, threat, now);
    }
    std::optional<proto::WorldPosition> Blink(std::uint32_t entity, const proto::WorldPosition& to, float most) override {
        return world->Blink(entity, to, most);
    }
    // The parties (Parties.h): its players as they are now, and a way to tell them; a player's party
    // for the skills' party areas (its members' entities, itself among them).
    std::unique_ptr<Parties> parties;
    std::uint16_t PartyServer() const override { return config.id; }
    bool PartyOnBus() const override { return !partyChannel.empty(); }
    void PartyPublish(std::string text) override { BusSend(partyChannel, std::move(text)); }
    // Friends, the block list and letters (Community.h).
    std::unique_ptr<Community> community;
    // The Academy (Academies.h).
    std::unique_ptr<Academies> academies;
    std::unique_ptr<OpenMarket> openMarket;
    std::string openMarketChannel; // the sales' lines on the bus (OpenMarket.h)
    // Magic POP: the machine and Gori (MagicPop.h).
    std::unique_ptr<MagicPop> magicPop;
    std::string popChannel; // the shard's wins announced on the bus (sr_mall_<schema>)
    std::mt19937 popRandom{std::random_device{}()};
    std::optional<std::uint32_t> popDrawFixed; // tests: every draw this value
    std::optional<PopHost::Player> PopPlayer(std::uint32_t entity) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return std::nullopt;
        return PopHost::Player{world->PeerOf(entity), p->selected, p->name, &p->inventory};
    }
    std::optional<PopHost::Npc> PopNpc(std::uint32_t npc) const override {
        const auto it = npcs.find(npc);
        if (it == npcs.end()) return std::nullopt;
        return PopHost::Npc{it->second->refObjId, it->second->codename};
    }
    bool PopInReach(std::uint32_t entity, std::uint32_t npc) const override {
        const auto me = world->Where(entity);
        const auto there = world->Where(npc);
        if (!me || !there || me->position.layer != there->position.layer) return false;
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
        return std::hypot(a.x - b.x, a.z - b.z) <= config.gateReach; // the NPC reach, 300
    }
    const assets::ItemRow* PopItem(std::uint32_t refId) const override { return config.rules->items ? config.rules->items->Find(refId) : nullptr; }
    const assets::ItemRow* PopItem(std::string_view codename) const override {
        return config.rules->items ? config.rules->items->Find(codename) : nullptr;
    }
    void PopRandomBlues(const assets::ItemRow& row, game::ItemInstance& item) override {
        if (config.drops) config.drops->RollBlues(row, item, popRandom);
    }
    game::ItemInstance PopFresh(const assets::ItemRow& row, int count) override {
        if (config.drops) return config.drops->Instance(row, count, 0, popRandom);
        game::ItemInstance item;
        item.refId = row.refObjId;
        item.count = std::uint16_t(std::clamp(count, 1, std::max(1, game::MaxStack(row))));
        if (!game::DataIsCount(row)) {
            item.count = 1;
            item.durability = game::NewDurability(row);
        }
        return item;
    }
    const std::vector<assets::MagicOptionRow>& PopOptions() const override {
        static const std::vector<assets::MagicOptionRow> none;
        return config.magicOptions ? *config.magicOptions : none;
    }
    void PopSave(std::uint32_t entity, const std::vector<std::uint8_t>& touched, std::function<void(bool)> done) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return done(false);
        ExchangeSaveSide side = SaveSideOf(p->selected, p->inventory, touched);
        std::vector<std::uint8_t> again;
        for (const StoredItem& i : side.present) again.push_back(i.slot);
        again.insert(again.end(), side.emptied.begin(), side.emptied.end());
        database.SubmitBarrier([this, side = std::move(side), done = std::move(done), again = std::move(again), character = p->selected](Database& db) mutable {
            std::string error;
            const bool ok = db.Transaction([&](Database& tx) { return SaveSidesIn(tx, side, side, error, false); }, error);
            if (!ok) core::LogWarning("magic pop: the bag of character {}: {}", character, error);
            completions->Post([this, ok, done = std::move(done), again = std::move(again), gone = std::move(side.gone), character] {
                done(ok);
                if (!ok) {
                    for (auto& [peerId, player] : players) {
                        if (player.selected == character && player.entity != 0) QueueItemSave(character, player.inventory, again, gone);
                    }
                }
            });
        });
    }
    void PopSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override {
        if (Peer* peer = peers->Find(peerId)) peer->connection->Send(id, payload);
    }
    void PopAnnounce(const proto::PopAnnounce& announce) override {
        if (!popChannel.empty()) return BusSend(popChannel, MagicPop::EncodeAnnounce(announce));
        PopAnnounceHere(announce);
    }
    // A win told to every player of this server (from the bus: this server's own too).
    void PopAnnounceHere(const proto::PopAnnounce& announce) {
        const std::vector<std::byte> bytes = proto::Encode(announce);
        for (auto& [peerId, player] : players) {
            if (player.entity == 0) continue;
            if (Peer* peer = peers->Find(peerId)) peer->connection->Send(proto::PopAnnounce::kId, bytes);
        }
    }
    std::uint32_t PopDraw() override {
        if (popDrawFixed) return *popDrawFixed;
        return std::uniform_int_distribution<std::uint32_t>(0, game::pop::kDrawRange - 1)(popRandom);
    }
    void PopBagChanged(std::uint32_t entity) override {
        if (quests) quests->BagChanged(entity);
    }
    std::int64_t PopNow() const override { return std::int64_t(std::time(nullptr)); }
    // --- The item mall's items: the time tickets and the premium package, the scrolls
    // and balloons, the resurrection scroll, the expansions, the Devil's Spirit's Extension Gear, the pet growth potion.

    std::int64_t ServiceNow() const { return std::int64_t(std::time(nullptr)) + serviceClockShift; }
    std::int64_t serviceClockShift = 0; // tests: the services' clock moved on

    const assets::ItemRow* ItemRowOf(std::uint32_t refId) const {
        return config.rules && config.rules->items ? config.rules->items->Find(refId) : nullptr;
    }
    const assets::ItemRow* ItemRowOf(std::string_view codename) const {
        return config.rules && config.rules->items ? config.rules->items->Find(codename) : nullptr;
    }

    // The bonus the running time services give a kill now (Param4 % experience, Param5 % skill experience): the
    // parameters 0x100 and 0x102 the original's keeper puts on while bonus seconds remain.
    std::pair<int, int> ServiceBonus(const PlayerWorld& player) const {
        int exp = 0, skill = 0;
        for (const game::TimedService& s : player.services) {
            if (!s.BonusOn()) continue;
            exp += s.expPercent;
            skill += s.skillPercent;
        }
        // The bonus scrolls running: the kill reads their parameters beside the tickets'.
        const std::int64_t now = ServiceNow();
        for (const PlayerWorld::BonusRun& b : player.bonuses) {
            if (b.until <= now) continue;
            if (b.kind == game::BonusKind::Experience) exp += b.value;
            if (b.kind == game::BonusKind::SkillExperience) skill += b.value;
        }
        return {exp, skill};
    }
    // The berserk mode's seconds the bonus scrolls add (the hwan-time key '\0pht' on the keeper's 0x3C, the mode's own
    // time).
    int BonusBerserkSeconds(const PlayerWorld& player) const {
        int seconds = 0;
        for (const PlayerWorld::BonusRun& b : player.bonuses) {
            if (b.kind == game::BonusKind::BerserkTime && b.until > ServiceNow()) seconds += b.value;
        }
        return seconds;
    }
    // The `_timedjob` category a bonus scroll's key is kept in (game::kTimedBonusFirst..), also its TimedService's.
    static std::uint8_t BonusCategory(std::uint32_t key) {
        switch (key) {
        case game::kBonusExperienceKey: return kTimedBonusFirst;
        case game::kBonusSkillPointsKey: return kTimedBonusFirst + 1;
        case game::kBonusSkillExperienceKey: return kTimedBonusFirst + 2;
        case game::kBonusBerserkTimeKey: return kTimedBonusFirst + 3;
        default: return 0;
        }
    }
    static constexpr std::uint8_t kTimedBonusFirst = game::kTimedBonusFirst;

    void TellServices(PlayerWorld& player) {
        Peer* peer = peers->Find(world->PeerOf(player.entity));
        if (peer == nullptr) return;
        const std::int64_t now = ServiceNow();
        proto::TimedServices m;
        for (const game::TimedService& s : player.services) {
            m.services.push_back({s.category, s.item, s.SecondsLeft(now), s.Bonus(), std::uint8_t(s.paused)});
        }
        // The bonus scrolls running, each on the buff board by its item (ours: the original's icon for them not traced),
        // where a right click ends one (TimedCancel).
        for (const PlayerWorld::BonusRun& b : player.bonuses) {
            const std::uint8_t category = BonusCategory(b.key);
            if (category != 0 && b.until > now) m.services.push_back({category, b.item, std::uint32_t(b.until - now), 0, 0});
        }
        if (player.premium.Running(now)) {
            m.services.push_back({game::kTimedPremium, player.premium.item, std::uint32_t(player.premium.SecondsLeft(now)), 0, 0});
            if (const assets::ItemRow* package = ItemRowOf(player.premium.item)) {
                std::size_t k = 0;
                for (const game::PremiumPart& p : game::PremiumPartsOf(*package)) {
                    if (p.tag != "UIL1" || k >= player.premium.used.size()) continue;
                    const assets::ItemRow* item = ItemRowOf(p.target);
                    const int left = std::max(0, p.count - player.premium.used[k++]);
                    if (item != nullptr) m.premium.push_back({item->refObjId, std::uint8_t(left), std::uint8_t(p.count)});
                }
            }
        }
        SendMessage(*peer->connection, m);
    }

    // The character's services as `_timedjob` keeps them (the original's records; ours for the premium's uses).
    void SaveServices(const PlayerWorld& player) {
        std::vector<std::pair<std::uint8_t, std::optional<TimedJobRow>>> rows;
        for (std::uint8_t category : {game::kTimedGoldSilver, game::kTimedSkill}) {
            const auto it = std::find_if(player.services.begin(), player.services.end(), [&](const game::TimedService& s) { return s.category == category; });
            if (it == player.services.end()) {
                rows.push_back({category, std::nullopt});
                continue;
            }
            TimedJobRow r;
            r.category = category;
            r.job = it->item;
            r.timeToKeep = it->TimeToKeep();
            r.data[0] = std::int32_t(it->start);
            r.data[1] = it->Data2();
            r.data[2] = std::int32_t(std::min<std::int64_t>(it->pausedSeconds, 0x7FFFFFFF));
            r.data[3] = it->paused ? 1 : 0;
            r.data[4] = std::int32_t(it->pausedAt);
            rows.push_back({category, r});
        }
        if (player.premium.item != 0) {
            TimedJobRow r;
            r.category = game::kTimedPremium;
            r.job = player.premium.item;
            r.timeToKeep = std::int32_t(player.premium.seconds);
            r.data[0] = std::int32_t(player.premium.start);
            for (int k = 0; k < 3; ++k) r.data[1 + k] = player.premium.used[std::size_t(k)];
            r.data[4] = std::int32_t(player.premium.periodStart);
            r.data[5] = player.premiumBooth ? 1 : 0;
            r.data[6] = player.premium.questUsed; // Data7: its UQL1 quest's finishes
            rows.push_back({game::kTimedPremium, r});
        } else {
            rows.push_back({game::kTimedPremium, std::nullopt});
        }
        if (player.growthUntil > 0) {
            TimedJobRow r;
            r.category = kTimedPetGrowth;
            r.job = player.growthItem;
            r.timeToKeep = 0;
            r.data[0] = std::int32_t(player.growthUntil);
            rows.push_back({kTimedPetGrowth, r});
        } else {
            rows.push_back({kTimedPetGrowth, std::nullopt});
        }
        // The bonus scrolls: Data1 the end (Unix seconds), Data2 the key, Data3 the value.
        for (std::uint8_t category = kTimedBonusFirst; category < kTimedBonusFirst + 4; ++category) {
            const auto it = std::find_if(player.bonuses.begin(), player.bonuses.end(), [&](const PlayerWorld::BonusRun& b) { return BonusCategory(b.key) == category; });
            if (it == player.bonuses.end()) {
                rows.push_back({category, std::nullopt});
                continue;
            }
            TimedJobRow r;
            r.category = category;
            r.job = it->item;
            r.data[0] = std::int32_t(it->until);
            r.data[1] = std::int32_t(it->key);
            r.data[2] = it->value;
            rows.push_back({category, r});
        }
        database.Submit([character = player.selected, rows = std::move(rows)](Database& db) {
            for (const auto& [category, row] : rows) SaveTimedJob(db, character, category, row ? &*row : nullptr);
        });
    }

    // Entering the world: the services read and run up to now (the days offline counted, as the original's load does),
    // the premium's skill on again for its time left, the client told.
    void LoadServices(std::uint64_t peerId, PlayerWorld& player) {
        const std::uint32_t entity = player.entity;
        const std::int64_t character = player.selected;
        auto rows = std::make_shared<std::vector<TimedJobRow>>();
        // After every write before it (the last leave's save of these rows among them): what it reads is current.
        database.SubmitBarrier([this, rows, character, entity, peerId](Database& db) {
            LoadTimedJobs(db, character,
                          {game::kTimedGoldSilver, game::kTimedSkill, game::kTimedPremium, kTimedPetGrowth, kTimedBonusFirst, std::uint8_t(kTimedBonusFirst + 1),
                           std::uint8_t(kTimedBonusFirst + 2), std::uint8_t(kTimedBonusFirst + 3)},
                          *rows);
            completions->Post([this, rows, character, entity, peerId] {
                PlayerWorld* p = PlayerByEntity(entity);
                if (p == nullptr || p->selected != character) return;
                const std::int64_t now = ServiceNow();
                bool changed = false;
                // What was used since the entry, before these rows came back, stays: it is newer than they are (a ticket,
                // a premium or a bonus scroll used at once after entering was cleared here: its effect lost, a second of
                // its kind let through). The rows fill in only what is not held already.
                const auto heldService = [&](std::uint8_t category) {
                    return std::any_of(p->services.begin(), p->services.end(), [&](const game::TimedService& x) { return x.category == category; });
                };
                const auto heldBonus = [&](std::uint32_t key) {
                    return std::any_of(p->bonuses.begin(), p->bonuses.end(), [&](const PlayerWorld::BonusRun& b) { return b.key == key; });
                };
                const bool premiumHeld = p->premium.Running(now);
                for (const TimedJobRow& r : *rows) {
                    if (r.category == game::kTimedGoldSilver || r.category == game::kTimedSkill) {
                        if (heldService(r.category)) continue;
                        game::TimedService s = game::TimedService::FromRecord(r.category, r.job, r.timeToKeep, r.data[0], r.data[1], r.data[2], r.data[3],
                                                                              r.data[4], ItemRowOf(r.job));
                        if (s.Advance(now)) {
                            p->services.push_back(s);
                        } else {
                            core::LogInfo("{}: {}'s time service {} ended while away", config.name, p->name, r.job);
                        }
                        changed = true;
                    } else if (r.category == game::kTimedPremium) {
                        if (premiumHeld) continue;
                        p->premium.item = r.job;
                        p->premium.start = r.data[0];
                        p->premium.seconds = r.timeToKeep;
                        for (int k = 0; k < 3; ++k) p->premium.used[std::size_t(k)] = r.data[1 + k];
                        p->premium.periodStart = r.data[4];
                        p->premiumBooth = r.data[5] != 0;
                        p->premium.questUsed = r.data[6];
                        if (!p->premium.Running(now)) {
                            p->premium = {};
                            p->premiumBooth = false;
                        } else {
                            p->premium.Advance(now);
                        }
                        changed = true;
                    } else if (r.category == kTimedPetGrowth) {
                        p->growthItem = r.job;
                        p->growthUntil = r.data[0] > now ? r.data[0] : 0;
                    } else if (r.category >= kTimedBonusFirst && r.category < kTimedBonusFirst + 4) {
                        // A bonus scroll: its time ran on while away (the original's end on the wall clock).
                        const assets::ItemRow* scroll = ItemRowOf(r.job);
                        const game::BonusScroll b = scroll != nullptr ? game::BonusScrollOf(*scroll) : game::BonusScroll{};
                        if (b && r.data[0] > now) {
                            if (!heldBonus(std::uint32_t(r.data[1]))) p->bonuses.push_back({b.kind, std::uint32_t(r.data[1]), r.job, r.data[2], r.data[0]});
                        } else {
                            changed = true;
                        }
                    }
                }
                if (p->premium.Running(now)) PremiumSkillOn(*p);
                p->servicesTick = Clock::now();
                p->servicesSaved = Clock::now();
                if (changed) SaveServices(*p);
                TellServices(*p);
                (void)peerId;
            });
        });
    }

    // The premium's skill (its USU1 part: SKILL_MALL_PRE_APRU_4W_01, cbuf) for the package's time left.
    void PremiumSkillOn(PlayerWorld& player) {
        const assets::ItemRow* package = ItemRowOf(player.premium.item);
        if (package == nullptr || !skillUnit || !config.skills) return;
        for (const game::PremiumPart& part : game::PremiumPartsOf(*package)) {
            if (part.tag != "USU1") continue;
            const assets::SkillRow* skill = config.skills->Find(std::string_view(part.target));
            if (skill == nullptr) continue;
            const std::int64_t left = player.premium.SecondsLeft(ServiceNow());
            SkillCaster caster = CasterOf(player);
            skillUnit->AddBuff(caster, *skill, std::uint32_t(std::clamp<std::int64_t>(left * 1000, 1000, 0xFFFFFFF0)), Clock::now());
        }
    }
    void PremiumSkillOff(PlayerWorld& player, std::uint32_t packageItem) {
        const assets::ItemRow* package = ItemRowOf(packageItem);
        if (package == nullptr || !skillUnit || !config.skills) return;
        std::vector<std::uint32_t> skills;
        for (const game::PremiumPart& part : game::PremiumPartsOf(*package)) {
            if (part.tag != "USU1") continue;
            if (const assets::SkillRow* skill = config.skills->Find(std::string_view(part.target))) skills.push_back(skill->id);
        }
        SkillCaster caster = CasterOf(player);
        if (!skills.empty()) skillUnit->EndBuffsOf(caster, skills);
    }

    // Every second: the days turned (online), the bonus spent in battle, the end;
    // saved every 600 s and at a change; the premium's day and end; the pet growth potion's end.
    void TickServices(Clock::time_point now) {
        if (now < nextServiceTick) return;
        nextServiceTick = now + std::chrono::seconds(1);
        const std::int64_t unix = ServiceNow();
        for (auto& [peerId, player] : players) {
            if (player.entity == 0 || (player.services.empty() && player.premium.item == 0 && player.growthUntil == 0 && player.bonuses.empty())) continue;
            const double dt = std::chrono::duration<double>(now - player.servicesTick).count();
            player.servicesTick = now;
            bool changed = false, tell = false;
            const bool battle = now < player.combat.battleUntil;
            for (auto it = player.services.begin(); it != player.services.end();) {
                const int dayBefore = it->daysLeft;
                const bool bonusBefore = it->BonusOn();
                if (!it->Advance(unix)) {
                    core::LogInfo("{}: {}'s time service {} ends", config.name, player.name, it->item);
                    it = player.services.erase(it);
                    changed = tell = true;
                    continue;
                }
                if (battle) it->SpendBattle(std::min(dt, 5.0));
                // While the bonus is spent the player is told every 30 s (ours: the buff board's tooltip shows the
                // bonus left; the original's premium list window is not traced), saved at the usual 600 s.
                if (battle && it->BonusOn() && now - player.servicesTold >= std::chrono::seconds(30)) tell = true;
                if (it->daysLeft != dayBefore) changed = tell = true;
                if (it->BonusOn() != bonusBefore) changed = tell = true;
                ++it;
            }
            if (player.premium.item != 0) {
                if (!player.premium.Running(unix)) {
                    core::LogInfo("{}: {}'s premium ends", config.name, player.name);
                    PremiumSkillOff(player, player.premium.item);
                    player.premium = {};
                    if (player.premiumBooth) {
                        player.premiumBooth = false;
                        if (stalls) stalls->SkinChanged(player.entity, player.stallSkin);
                    }
                    changed = tell = true;
                } else {
                    const auto before = player.premium.used;
                    player.premium.Advance(unix);
                    if (player.premium.used != before) changed = tell = true;
                }
            }
            if (player.growthUntil > 0 && unix >= player.growthUntil) {
                player.growthUntil = 0;
                changed = true;
            }
            // A bonus scroll's time over (its job ends; the original's keeper takes its parameter off).
            const std::size_t bonuses = player.bonuses.size();
            std::erase_if(player.bonuses, [&](const PlayerWorld::BonusRun& b) {
                if (b.until > unix) return false;
                core::LogInfo("{}: {}'s bonus scroll {} ends", config.name, player.name, b.item);
                return true;
            });
            if (player.bonuses.size() != bonuses) changed = tell = true;
            if (changed || now - player.servicesSaved >= std::chrono::seconds(600)) {
                player.servicesSaved = now;
                SaveServices(player);
            }
            if (tell) {
                player.servicesTold = now;
                TellServices(player);
            }
        }
    }
    Clock::time_point nextServiceTick;

    void ServiceAnswerTo(Peer& peer, proto::ServiceRequest request, std::uint16_t code, std::uint32_t a = 0, std::uint32_t b = 0) {
        SendMessage(*peer.connection, proto::ServiceAnswer{request, code, a, b});
    }
    void RefuseItem(Peer& peer, const PlayerWorld& player, const assets::ItemRow& row, std::uint16_t code, const char* why) {
        SendMessage(*peer.connection, proto::ItemRefused{row.refObjId, code});
        core::LogInfo("{}: {} uses {}: refused ({}, {:#x})", config.name, player.name, row.codename, why, code);
    }

    // Rising by a resurrection scroll (TID4 6): where it fell, the most HP and MP x P2 % (or P1 each when
    // above 0), P3 % of the last death's loss back (rounded up).
    void RiseByScroll(Peer& peer, PlayerWorld& player, const assets::ItemRow& row) {
        // A murderer gets half of it (x 0.5), rounded up too.
        const bool murderer = player.pvpState == std::uint8_t(game::PvpState::Murderer);
        const std::uint64_t share = player.deathLoss * std::uint64_t(std::max(0, row.params[2]));
        const std::uint64_t back = murderer ? (share + 199) / 200 : (share + 99) / 100;
        player.dead = false;
        player.revivalOffer.reset();
        player.deathLoss = 0;
        player.warDeathAt.reset();
        if (row.params[0] > 0) {
            player.hp = std::min<std::uint32_t>(player.stats.maxHp, std::uint32_t(row.params[0]));
            player.mp = std::min<std::uint32_t>(player.stats.maxMp, std::uint32_t(row.params[0]));
        } else {
            player.hp = std::max<std::uint32_t>(1, std::uint32_t(std::uint64_t(player.stats.maxHp) * std::uint64_t(std::max(0, row.params[1])) / 100));
            player.mp = std::uint32_t(std::uint64_t(player.stats.maxMp) * std::uint64_t(std::max(0, row.params[1])) / 100);
        }
        player.untouchableUntil = Clock::now() + std::chrono::seconds(5);
        player.combat.StopFighting();
        world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
        SendVitals(peer, player);
        Broadcast(player.entity, proto::EntityDamaged{player.entity, 0, 0, player.hp, 0});
        Broadcast(player.entity, proto::ItemUsed{player.entity, row.refObjId});
        if (back > 0) GainExperience(peer, player, back, 0);
        core::LogInfo("{}: {} rises by {} ({} experience back)", config.name, player.name, row.codename, back);
    }

    // The mall's own items used from the bag (true: handled here, used or refused).
    bool UseMallService(Peer& peer, PlayerWorld& player, std::uint8_t place, const assets::ItemRow& row) {
        if (row.typeId[0] != 3 || row.typeId[1] != 3 || row.typeId[2] != 13) return false;
        const int kind = row.typeId[3];
        const std::int64_t now = ServiceNow();
        // The resurrection scroll is the one item a dead player may use; it needs death (0x1887).
        if (kind == 6) {
            if (!player.dead) {
                RefuseItem(peer, player, row, 0x1887, "alive");
                return true;
            }
            TakeOne(peer, player, place);
            RiseByScroll(peer, player, row);
            return true;
        }
        if (kind != 4 && kind != 5 && kind != 14 && kind != 17 && kind != 19 && kind != 15 && kind != 16) return false;
        if (player.dead) {
            RefuseItem(peer, player, row, 0x1889, "dead");
            return true;
        }
        if (kind == 4 || kind == 5) {
            // A time ticket: one of its category at a time (0x1894), a gold one not beside the premium's.
            const std::uint8_t category = game::TimedCategoryOf(row);
            if (std::any_of(player.services.begin(), player.services.end(), [&](const game::TimedService& s) { return s.category == category; })) {
                RefuseItem(peer, player, row, 0x1894, "one running");
                return true;
            }
            TakeOne(peer, player, place);
            player.services.push_back(game::TimedService::Begin(row, category, now));
            Broadcast(player.entity, proto::ItemUsed{player.entity, row.refObjId});
            core::LogInfo("{}: {} starts {} ({} days, +{} % / +{} %)", config.name, player.name, row.codename, player.services.back().days,
                          player.services.back().expPercent, player.services.back().skillPercent);
            SaveServices(player);
            TellServices(player);
            return true;
        }
        if (kind == 14) {
            // The premium package: one at a time (category 10), and its gold ticket
            // (UIU1) not beside a running gold or silver one (0x1894).
            const auto parts = game::PremiumPartsOf(row);
            const bool gold = std::any_of(parts.begin(), parts.end(), [](const game::PremiumPart& p) { return p.tag == "UIU1"; });
            const bool running = std::any_of(player.services.begin(), player.services.end(),
                                             [](const game::TimedService& s) { return s.category == game::kTimedGoldSilver; });
            if (player.premium.Running(now) || (gold && running)) {
                RefuseItem(peer, player, row, 0x1894, "a premium or a ticket runs");
                return true;
            }
            TakeOne(peer, player, place);
            player.premium = {};
            player.premium.item = row.refObjId;
            player.premium.start = now;
            player.premium.periodStart = now;
            player.premium.seconds = std::max(0, row.params[0]);
            player.premiumBooth = false;
            for (const game::PremiumPart& p : parts) {
                if (p.tag == "UIU1") {
                    if (const assets::ItemRow* ticket = ItemRowOf(p.target); ticket != nullptr && game::TimedCategoryOf(*ticket) != 0) {
                        player.services.push_back(game::TimedService::Begin(*ticket, game::TimedCategoryOf(*ticket), now));
                    }
                }
            }
            PremiumSkillOn(player);
            Broadcast(player.entity, proto::ItemUsed{player.entity, row.refObjId});
            core::LogInfo("{}: {} starts the premium {}", config.name, player.name, row.codename);
            SaveServices(player);
            TellServices(player);
            return true;
        }
        if (kind == 17) {
            // An expansion ( ->): P1 the places, P2 0 the bag / 1 the storage.
            const int add = std::max(0, row.params[0]);
            if (row.params[1] == 0) {
                bool quest = false;
                if (quests) {
                    for (std::string_view code : game::kBagQuests) {
                        if (const game::QuestDef* q = quests->Find(code); q != nullptr && quests->Record(player.entity, q->id) != nullptr) quest = true;
                    }
                }
                const int limit = quest ? game::kBagExpansionQuestLimit : game::kBagExpansionLimit;
                if (player.inventory.size + add > std::min<int>(limit, game::Inventory::kMaxSize)) {
                    RefuseItem(peer, player, row, 0x18FE, "the bag's limit");
                    return true;
                }
                TakeOne(peer, player, place);
                player.inventory.size = std::uint8_t(player.inventory.size + add);
                database.Submit([character = player.selected, size = player.inventory.size](Database& db) { SaveInventorySize(db, character, size); });
                SendMessage(*peer.connection, proto::StorageGrown{1, player.inventory.size});
                ServiceAnswerTo(peer, proto::ServiceRequest::Expansion, 0, 1, player.inventory.size);
                core::LogInfo("{}: {}'s bag grows to {}", config.name, player.name, player.inventory.size);
            } else if (row.params[1] == 1) {
                if (!player.storage) {
                    RefuseItem(peer, player, row, 2, "the storage not opened this session");
                    return true;
                }
                if (player.storage->size + add > game::kStorageExpansionLimit) {
                    RefuseItem(peer, player, row, 0x18FE, "the storage's limit");
                    return true;
                }
                TakeOne(peer, player, place);
                player.storage->size = std::uint8_t(player.storage->size + add);
                database.Submit([account = peer.account, size = player.storage->size](Database& db) { SaveChestSize(db, account, size); });
                SendMessage(*peer.connection, proto::StorageGrown{2, player.storage->size});
                ServiceAnswerTo(peer, proto::ServiceRequest::Expansion, 0, 2, player.storage->size);
                core::LogInfo("{}: {}'s storage grows to {}", config.name, player.name, player.storage->size);
            } else {
                RefuseItem(peer, player, row, 3, "no such target");
            }
            return true;
        }
        if (kind == 19) {
            // The Open Market ticket (TID4 19): a timed job of category 11 for its Param1 seconds (the
            // original's ticket keeper), one at a time (0x1894); its listings, time and sale fee read from its row.
            const std::uint8_t category = game::openmarket::kTicketCategory;
            if (std::any_of(player.services.begin(), player.services.end(), [&](const game::TimedService& s) { return s.category == category; })) {
                RefuseItem(peer, player, row, 0x1894, "one running");
                return true;
            }
            TakeOne(peer, player, place);
            player.services.push_back(game::TimedService::Begin(row, category, now));
            Broadcast(player.entity, proto::ItemUsed{player.entity, row.refObjId});
            core::LogInfo("{}: {} starts {} ({} days)", config.name, player.name, row.codename, player.services.back().days);
            SaveServices(player);
            TellServices(player);
            return true;
        }
        // The Extension Gear and the growth potion want their target (UseItemOn).
        RefuseItem(peer, player, row, 2, "needs a target");
        return true;
    }

    // An item used on another (the Extension Gear on a Devil's Spirit, the potion of growth on a growth pet's item).
    // False when the tool is not one of these (another's request).
    bool UseMallItemOn(Peer& peer, PlayerWorld& player, const proto::UseItemOn& m) {
        const game::ItemInstance* tool = player.inventory.At(m.place);
        const assets::ItemRow* row = tool != nullptr && m.place >= game::slot::kWorn ? ItemRowOf(tool->refId) : nullptr;
        if (row == nullptr || row->typeId[0] != 3 || row->typeId[1] != 3 || row->typeId[2] != 13 || (row->typeId[3] != 15 && row->typeId[3] != 16)) return false;
        const auto answer = [&](std::uint16_t code, std::uint32_t a = 0, std::uint32_t b = 0) {
            ServiceAnswerTo(peer, proto::ServiceRequest::UseOn, code, a, b);
            if (code != 0) core::LogInfo("{}: {} uses {} on place {}: refused ({:#x})", config.name, player.name, row->codename, m.target, code);
        };
        if (player.dead) return answer(0x1889), true;
        const std::int64_t now = ServiceNow();
        if (row->typeId[3] == 16) {
            // The Extension Gear: a Devil's Spirit in the bag (0x1882 else), awakened (0x18F8), + P1 to its end.
            if (m.target < game::slot::kWorn || m.target >= player.inventory.size) return answer(0x1882), true;
            game::ItemInstance* target = player.inventory.places.contains(m.target) ? &player.inventory.places[m.target] : nullptr;
            const assets::ItemRow* targetRow = target != nullptr ? ItemRowOf(target->refId) : nullptr;
            if (targetRow == nullptr || !game::IsDevilSpirit(*targetRow)) return answer(3), true;
            if (target->durability == 0) return answer(0x18F8), true;
            const std::int64_t from = std::max<std::int64_t>(now, std::int64_t(target->durability));
            target->durability = std::uint32_t(from + std::max(0, row->params[0]));
            proto::ItemMoved changed;
            changed.from = changed.to = m.target;
            changed.changed.push_back(player.inventory.Record(m.target));
            SendMessage(*peer.connection, changed);
            QueueItemSave(player.selected, player.inventory, {m.target}, {});
            TakeOne(peer, player, m.place);
            core::LogInfo("{}: {} extends {}'s awakening to {}", config.name, player.name, targetRow->codename, target->durability);
            return answer(0, targetRow->refObjId, std::uint32_t(std::max<std::int64_t>(0, std::int64_t(target->durability) - now))), true;
        }
        // The pets' watches and skills (TID4 15 but the growth potions): the item's Param2
        // jobs on the pet of the item named (a bag place 13.. else 3; a pet's item
        // with its row and its pet out, else 0x18B8), Coses::UseMallItemOnPet; a watch takes its minutes from the stack.
        if (row->codename.find("GROWTH") == std::string::npos) {
            // The dispatcher's level gate first (their level 5).
            if (player.level < row->requiredLevel) return answer(0x186C), true;
            if (m.target < game::slot::kWorn || m.target >= player.inventory.size) return answer(3), true;
            const game::ItemInstance* pet = player.inventory.At(m.target);
            const assets::ItemRow* petRow = pet != nullptr ? ItemRowOf(pet->refId) : nullptr;
            if (!coses || petRow == nullptr || game::PetOfItem(*petRow) == game::CosKind::None || pet->durability == 0) return answer(0x18B8), true;
            const Coses::PetItemUse use = coses->UseMallItemOnPet(player.entity, *row, *tool, *pet);
            if (use.code != 0) return answer(use.code), true;
            if (use.taken <= 1) {
                TakeOne(peer, player, m.place);
            } else {
                game::ItemInstance& stack = player.inventory.places[m.place];
                proto::ItemMoved moved;
                moved.from = moved.to = m.place;
                std::vector<std::int64_t> gone;
                if (int(stack.count) > use.taken) {
                    stack.count = std::uint16_t(int(stack.count) - use.taken);
                    moved.changed.push_back(player.inventory.Record(m.place));
                } else {
                    gone.push_back(stack.serial);
                    player.inventory.places.erase(m.place);
                    moved.emptied.push_back(m.place);
                }
                SendMessage(*peer.connection, moved);
                QueueItemSave(player.selected, player.inventory, {m.place}, gone);
            }
            return answer(0, row->refObjId, std::uint32_t(use.taken)), true;
        }
        // The potion of growth (TID4 15): on a growth pet's item whose pet is out (0x18B8), one at a
        // time (0x1894); +100 % of the pet's experience (its skill's expi) for P1 seconds.
        const game::ItemInstance* target = player.inventory.At(m.target);
        const assets::ItemRow* targetRow = target != nullptr && m.target >= game::slot::kWorn ? ItemRowOf(target->refId) : nullptr;
        const bool growthPet = targetRow != nullptr && targetRow->typeId == std::array<int, 4>{3, 2, 1, 1};
        if (!growthPet || !coses || coses->AttackPetLevel(player.entity) <= 0) return answer(0x18B8), true;
        if (player.growthUntil > now) return answer(0x1894), true;
        player.growthUntil = now + std::max(1, row->params[0]);
        player.growthItem = row->refObjId;
        TakeOne(peer, player, m.place);
        SaveServices(player);
        core::LogInfo("{}: {}'s pet grows faster for {} s", config.name, player.name, row->params[0]);
        return answer(0, row->refObjId, std::uint32_t(row->params[0])), true;
    }

    // The premium's daily uses (the original's 0x715F): the package's UIL1 part
    // for the item with uses left (0x18C6), the premium running (0x18CC); the item's effect as its use, then one use taken.
    void UsePremium(Peer& peer, PlayerWorld& player, const proto::PremiumUse& m) {
        const auto answer = [&](std::uint16_t code, std::uint32_t left = 0) { ServiceAnswerTo(peer, proto::ServiceRequest::PremiumUse, code, m.item, left); };
        const std::int64_t now = ServiceNow();
        if (!player.premium.Running(now)) return answer(0x18CC);
        player.premium.Advance(now);
        const assets::ItemRow* package = ItemRowOf(player.premium.item);
        const assets::ItemRow* item = ItemRowOf(m.item);
        if (package == nullptr || item == nullptr) return answer(2);
        std::size_t index = 0;
        const game::PremiumPart* part = nullptr;
        const std::vector<game::PremiumPart> parts = game::PremiumPartsOf(*package);
        for (const game::PremiumPart& p : parts) {
            if (p.tag != "UIL1") continue;
            if (p.target == item->codename && index < player.premium.used.size()) {
                part = &p;
                break;
            }
            ++index;
        }
        if (part == nullptr) return answer(2);
        const int count = part->count;
        if (player.premium.used[index] >= count) return answer(0x18C6);
        bool used = false;
        if (item->typeId == std::array<int, 4>{3, 3, 13, 6}) {
            if (!player.dead) return answer(0x1887);
            RiseByScroll(peer, player, *item);
            used = true;
        } else {
            if (player.dead) return answer(0x1889);
            const bool before = player.returnAt.has_value();
            proto::UseItem use;
            use.option = m.option;
            use.point = m.point;
            UseItem(peer, player, use, item);
            used = !before && player.returnAt.has_value();
        }
        if (!used) return answer(2);
        ++player.premium.used[index];
        SaveServices(player);
        TellServices(player);
        core::LogInfo("{}: {} uses the premium's {} ({} of {} today)", config.name, player.name, item->codename, player.premium.used[index], count);
        answer(0, std::uint32_t(count - player.premium.used[index]));
    }

    // The premium's booth (the original's 0x7160): its BFI1 decoration becomes the stall's while it runs.
    void PremiumBoothOn(Peer& peer, PlayerWorld& player, const proto::PremiumBooth& m) {
        const auto answer = [&](std::uint16_t code) { ServiceAnswerTo(peer, proto::ServiceRequest::PremiumBooth, code, m.item); };
        if (!player.premium.Running(ServiceNow())) return answer(0x18CC);
        const std::uint32_t booth = PremiumBoothItem(player);
        if (booth == 0 || (m.item != 0 && m.item != booth)) return answer(2);
        player.premiumBooth = true;
        if (stalls) stalls->SkinChanged(player.entity, booth);
        SaveServices(player);
        answer(0);
    }
    std::uint32_t PremiumBoothItem(const PlayerWorld& player) const {
        const assets::ItemRow* package = player.premium.item != 0 ? ItemRowOf(player.premium.item) : nullptr;
        if (package == nullptr) return 0;
        for (const game::PremiumPart& p : game::PremiumPartsOf(*package)) {
            if (p.tag == "BFI1") {
                if (const assets::ItemRow* booth = ItemRowOf(p.target)) return booth->refObjId;
            }
        }
        return 0;
    }
    // The stall's decoration now: the premium's booth while it runs and was chosen, else its own.
    std::uint32_t StallSkinOf(const PlayerWorld& player) const {
        if (player.premiumBooth && player.premium.Running(ServiceNow())) {
            if (const std::uint32_t booth = PremiumBoothItem(player)) return booth;
        }
        return player.stallSkin;
    }

    // A time service paused or resumed (the original's 0x74BF): not in battle (0x18FB).
    void PauseService(Peer& peer, PlayerWorld& player, const proto::TimedPause& m) {
        const auto answer = [&](std::uint16_t code) { ServiceAnswerTo(peer, proto::ServiceRequest::Pause, code, m.category, m.on); };
        const auto it = std::find_if(player.services.begin(), player.services.end(), [&](const game::TimedService& s) { return s.category == m.category; });
        if (it == player.services.end()) return answer(2);
        if (Clock::now() < player.combat.battleUntil) return answer(0x18FB);
        it->Pause(m.on != 0, ServiceNow());
        SaveServices(player);
        TellServices(player);
        answer(0);
    }

    // A bonus scroll's run ended by its player (TimedCancel; ours, its effect lost as the box says): its key free for
    // another scroll at once.
    void CancelBonus(Peer& peer, PlayerWorld& player, const proto::TimedCancel& m) {
        const auto answer = [&](std::uint16_t code) { ServiceAnswerTo(peer, proto::ServiceRequest::Cancel, code, m.category); };
        if (!game::IsBonusCategory(m.category)) return answer(2);
        const auto it = std::find_if(player.bonuses.begin(), player.bonuses.end(), [&](const PlayerWorld::BonusRun& b) { return BonusCategory(b.key) == m.category; });
        if (it == player.bonuses.end()) return answer(2);
        core::LogInfo("{}: {} ends the bonus scroll {}", config.name, player.name, it->item);
        player.bonuses.erase(it);
        SaveServices(player);
        TellServices(player);
        answer(0);
    }

    // The item mall and silk (Mall.h).
    std::unique_ptr<Mall> mall;
    std::string silkChannel; // the accounts' silk on the bus (SilkStore.h)
    std::optional<MallHost::Buyer> MallBuyer(std::uint32_t entity) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return std::nullopt;
        const std::uint64_t peerId = world->PeerOf(entity);
        const Peer* peer = peers->Find(peerId);
        if (peer == nullptr) return std::nullopt;
        return MallHost::Buyer{peerId, p->selected, peer->account, p->name};
    }
    bool MallBagHeld(std::uint32_t entity) const override {
        return (exchanges && exchanges->Holding(entity)) || (stalls && stalls->Holding(entity)) || (alchemy && alchemy->Busy(entity)) ||
               (openMarket && openMarket->Busy(entity));
    }
    const assets::ItemRow* MallItem(std::string_view codename) const override {
        return config.rules && config.rules->items ? config.rules->items->Find(codename) : nullptr;
    }
    int MallFreePlaces(std::uint32_t entity) const override {
        const PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return 0;
        int free = 0;
        for (int place = game::slot::kWorn; place < p->inventory.size; ++place) {
            if (!p->inventory.places.contains(std::uint8_t(place))) ++free;
        }
        return free;
    }
    std::vector<std::uint8_t> MallPut(std::uint32_t entity, std::vector<game::ItemInstance> items) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr || freeItemIds.size() < items.size()) return {};
        std::vector<std::uint8_t> places;
        for (int place = game::slot::kWorn; place < p->inventory.size && places.size() < items.size(); ++place) {
            if (!p->inventory.places.contains(std::uint8_t(place))) places.push_back(std::uint8_t(place));
        }
        if (places.size() < items.size()) return {};
        for (std::size_t k = 0; k < items.size(); ++k) {
            items[k].serial = freeItemIds.back();
            freeItemIds.pop_back();
            p->inventory.places[places[k]] = std::move(items[k]);
        }
        return places;
    }
    void MallTakeBack(std::uint32_t entity, const std::vector<std::uint8_t>& places) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return;
        for (std::uint8_t place : places) {
            const auto it = p->inventory.places.find(place);
            if (it == p->inventory.places.end()) continue;
            if (it->second.serial != 0) freeItemIds.push_back(it->second.serial); // never written: its id is free again
            p->inventory.places.erase(it);
        }
        // What the transaction would have written (its own saves folded in) is saved again as the bag is now.
        QueueItemSave(p->selected, p->inventory, places, {});
    }
    std::optional<proto::ItemRecord> MallRecord(std::uint32_t entity, std::uint8_t place) const override {
        const PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr || p->inventory.At(place) == nullptr) return std::nullopt;
        return p->inventory.Record(place);
    }
    void MallSave(std::uint32_t entity, const std::vector<std::uint8_t>& touched, std::function<bool(Database&, std::string&)> work,
                  std::function<void(bool)> done) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return done(false);
        ExchangeSaveSide side = SaveSideOf(p->selected, p->inventory, touched);
        std::vector<std::uint8_t> again; // a failure saves these again (the saves the side took from the queue)
        for (const StoredItem& i : side.present) again.push_back(i.slot);
        again.insert(again.end(), side.emptied.begin(), side.emptied.end());
        database.SubmitBarrier([this, side = std::move(side), work = std::move(work), done = std::move(done), again = std::move(again),
                                character = p->selected](Database& db) mutable {
            std::string error;
            const bool ok = db.Transaction([&](Database& tx) { return SaveSidesIn(tx, side, side, error, false) && work(tx, error); }, error);
            if (!ok && !error.empty()) core::LogWarning("mall: the purchase of character {}: {}", character, error);
            completions->Post([this, ok, done = std::move(done), again = std::move(again), gone = std::move(side.gone), character] {
                done(ok);
                if (!ok) {
                    // The bag as it is now (the purchase's places emptied by MallTakeBack), the saves the side took saved again.
                    for (auto& [peerId, player] : players) {
                        if (player.selected == character && player.entity != 0) QueueItemSave(character, player.inventory, again, gone);
                    }
                }
            });
        });
    }
    void MallSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override {
        if (Peer* peer = peers->Find(peerId)) peer->connection->Send(id, payload);
    }
    void MallDatabase(std::function<void(Database&)> work, std::function<void()> done) override {
        database.Submit([this, work = std::move(work), done = std::move(done)](Database& db) {
            work(db);
            completions->Post(done);
        });
    }
    void MallBagChanged(std::uint32_t entity) override {
        if (quests) quests->BagChanged(entity);
        if (PlayerWorld* p = PlayerByEntity(entity)) LoadNewCubes(*p); // a cube bought: its store ready before its first use
    }
    int MallShardId() const override { return config.id; }
    std::int64_t MallNow() const override { return std::int64_t(std::time(nullptr)); }
    // The jobs' leagues at their NPCs (Leagues.h).
    std::unique_ptr<Leagues> leagues;
    std::optional<LeagueHost::Member> LeagueMember(std::uint32_t entity) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return std::nullopt;
        LeagueHost::Member m;
        m.peer = world->PeerOf(entity);
        m.character = p->selected;
        m.level = p->level;
        m.gold = p->inventory.gold;
        m.jobState = p->jobState;
        m.jobQuest = quests && quests->JobQuestUnderWay(entity);
        m.job = &p->job;
        m.alias = &p->alias;
        m.leftAt = &p->jobLeftAt;
        return m;
    }
    game::JobType LeagueKeeperNear(std::uint32_t entity, std::uint32_t npc) const override {
        const auto it = npcs.find(npc);
        const auto me = world->Where(entity);
        const auto there = world->Where(npc);
        if (it == npcs.end() || !me || !there) return game::JobType::None;
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
        if (std::hypot(a.x - b.x, a.z - b.z) > config.gateReach) return game::JobType::None;
        return game::LeagueOfNpc(it->second->codename);
    }
    bool AliasLettersOk(std::string_view alias) const override {
        const game::NameCheck check = config.rules->names.Check(alias);
        return check == game::NameCheck::Ok || check == game::NameCheck::BadLength;
    }
    void LeaguePay(std::int64_t character, std::uint64_t fee, PaymentWork work, PaymentDone done) override {
        Payment(character, fee, std::move(work), std::move(done), true);
    }
    void LeagueSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override {
        if (Peer* peer = peers->Find(peerId)) peer->connection->Send(id, payload);
    }
    void LeagueChanged(std::uint32_t entity) override {
        PlayerWorld* p = PlayerByEntity(entity);
        Peer* peer = peers->Find(world->PeerOf(entity));
        if (p == nullptr || peer == nullptr) return;
        // A trader's safe trade row made with the join (0, 0, today), gone with the leave (JoinLeague, LeaveLeague).
        if (p->job.type != game::JobType::Trader) p->safeTrade.reset();
        else if (!p->safeTrade) p->safeTrade = SafeTradeRow{0, false, TodayLocal()};
        SendJobInfo(*peer, *p);
        TellJob(peer, *p, true);
    }
    void LeagueDatabase(std::function<void(Database&)> work, std::function<void()> done) override {
        database.SubmitBarrier([this, work = std::move(work), done = std::move(done)](Database& db) {
            work(db);
            completions->Post(done);
        });
    }
    std::int64_t LeagueNow() const override { return std::int64_t(std::time(nullptr)) + leagueClockShift; }
    std::int64_t leagueClockShift = 0; // tests: the league's clock moved on
    // --- The fortresses' tax -------------------------------------------------------
    // Which fortress taxes an NPC's or a gate's deals (`_RefObjCommon.AssocFile1_128`), each fortress's holder and rate
    // (`_SiegeFortress`, read again each minute: the whole shard's, whichever server runs the fortress), and the tax
    // gathered and not yet written (written as an increment once at least 10,000, as the original's).
    std::unordered_map<std::uint32_t, std::uint32_t> taxedBy;
    struct TaxRule {
        std::uint32_t guild = 0;
        int rate = 0;
    };
    std::unordered_map<std::uint32_t, TaxRule> taxRules;
    std::unordered_map<std::uint32_t, std::int64_t> taxGathered;
    Clock::time_point nextTaxRead{};
    bool readingTax = false;
    // The fortress tax at the object `refObjId` for `buyer` (fortress 0: none): the rate, waived when positive for
    // the holder's guild and its union. The NPC's and the gate's menus tell it (the client's prices).
    proto::NpcTax NpcTaxFor(std::uint32_t refObjId, const PlayerWorld& buyer) const {
        proto::NpcTax out;
        const auto by = taxedBy.find(refObjId);
        if (by == taxedBy.end()) return out;
        out.fortress = by->second;
        const auto rule = taxRules.find(by->second);
        if (rule == taxRules.end()) return out;
        out.rate = std::int16_t(rule->second.rate);
        if (guilds && rule->second.guild != 0) {
            const std::uint32_t mine = guilds->GuildOf(buyer.selected);
            if (mine != 0 && mine == rule->second.guild) out.exempt = true;
            if (!out.exempt && mine != 0) {
                const auto a = guilds->FactsOfGuild(mine), b = guilds->FactsOfGuild(rule->second.guild);
                out.exempt = a && b && a->alliance != 0 && a->alliance == b->alliance;
            }
        }
        return out;
    }
    // The tax on `price` for `buyer` at the object `refObjId` (0: none), its fortress in `fortress`.
    std::int64_t TaxOn(std::uint32_t refObjId, const PlayerWorld& buyer, std::uint64_t price, std::uint32_t* fortress = nullptr) const {
        const proto::NpcTax t = NpcTaxFor(refObjId, buyer);
        if (t.fortress == 0 || t.rate == 0) return 0;
        if (fortress != nullptr) *fortress = t.fortress;
        return game::FortressTax(price, t.rate, t.exempt);
    }
    void CollectTax(std::uint32_t fortress, std::int64_t tax) {
        if (fortress == 0 || tax == 0) return;
        std::int64_t& gathered = taxGathered[fortress];
        gathered += tax;
        if (std::abs(gathered) < game::kTaxWriteThreshold) return;
        const std::int64_t delta = std::exchange(gathered, 0);
        database.Submit([fortress, delta](Database& db) { AddFortressTax(db, fortress, delta); });
    }
    void TickTax(Clock::time_point now) {
        if (readingTax || now < nextTaxRead) return;
        nextTaxRead = now + std::chrono::seconds(60);
        readingTax = true;
        database.Submit([this](Database& db) {
            std::vector<std::pair<std::uint32_t, TaxRule>> rules;
            QueryResult r = db.Query("SELECT fortressid, guildid, taxratio FROM _siegefortress");
            for (int k = 0; r.Ok() && k < r.Rows(); ++k) rules.push_back({std::uint32_t(r.Int(k, 0)), {std::uint32_t(r.Int(k, 1)), int(r.Int(k, 2))}});
            completions->Post([this, rules = std::move(rules)] {
                readingTax = false;
                taxRules.clear();
                for (const auto& [id, rule] : rules) taxRules[id] = rule;
            });
        });
    }
    std::uint32_t NpcRef(std::uint32_t entity) const {
        const auto it = npcs.find(entity);
        return it != npcs.end() ? it->second->refObjId : 0;
    }
    std::int64_t TradeTaxOn(std::uint32_t entity, std::uint32_t npc, std::uint64_t amount) const override {
        const auto it = players.find(world->PeerOf(entity));
        return it != players.end() ? TaxOn(NpcRef(npc), it->second, amount) : 0;
    }
    void TradeCollectTax(std::uint32_t npc, std::int64_t tax) override {
        const auto by = taxedBy.find(NpcRef(npc));
        if (by != taxedBy.end()) CollectTax(by->second, tax);
    }

    // --- The game's logs (GameLog.h): queued here, written about once a second by a database job -----------------
    std::unique_ptr<GameLog> gameLog;
    Clock::time_point nextLog{};
    int logRounds = 0;
    void FlushLog(bool now = false) {
        if (!gameLog) return;
        if (!now && Clock::now() < nextLog) return;
        nextLog = Clock::now() + std::chrono::seconds(1);
        GameLog::Batch batch = gameLog->Take();
        // A spill left from a refusal is tried every ten seconds even with nothing new.
        if (batch.Empty() && (++logRounds % 10 != 0 || !gameLog->Spilled())) return;
        database.Submit([log = gameLog.get(), batch = std::move(batch)](Database& db) { log->Write(db, batch); });
    }
    // Where a character stands, as the logs' EventPos (ours: region(x,y,z)).
    std::string LogPos(const PlayerWorld& p) const {
        const auto at = p.entity != 0 ? world->Where(p.entity) : std::nullopt;
        return at ? std::format("{}({:.0f},{:.0f},{:.0f})", at->position.region, at->position.x, at->position.y, at->position.z) : std::string();
    }
    void LogChar(const PlayerWorld& p, logs::CharEvent event, std::int32_t data1 = 0, std::int32_t data2 = 0, std::string desc = {}) {
        if (!gameLog || p.selected == 0) return;
        gameLog->Char({p.selected, std::uint8_t(event), data1, data2, LogPos(p), std::move(desc)});
    }
    void LogItem(const PlayerWorld& p, logs::ItemOp op, std::uint32_t refId, std::int64_t serial, std::int32_t count, std::uint8_t from, std::uint8_t to,
                 logs::Storage storage = logs::Storage::Inventory, std::int64_t gold = 0) {
        if (!gameLog || p.selected == 0) return;
        ItemLogRow r;
        r.character = p.selected;
        r.refId = refId;
        r.data = count;
        r.storage = std::uint8_t(storage);
        r.operation = std::uint8_t(op);
        r.from = from;
        r.to = to;
        r.pos = LogPos(p);
        r.serial = serial;
        r.gold = gold;
        gameLog->Item(std::move(r));
    }
    void LogGold(const PlayerWorld& p, logs::ItemOp op, std::int64_t gold) { LogItem(p, op, 0, 0, 0, logs::kNoSlot, logs::kNoSlot, logs::Storage::Inventory, gold); }

    // The jobs' trade: the specialty NPCs' markets, buying into the transport, selling (Trades.h).
    std::unique_ptr<Trades> trades;
    std::unordered_map<std::uint32_t, int> transportBags; // a transport's model -> its trade bag (_RefObjChar.InventorySize)
    int CosTransportBagSize(std::uint32_t model, int level) const override {
        const auto it = transportBags.find(model);
        if (it == transportBags.end()) return 0;
        const int most = it->second;
        if (most <= 0 || jobRules.goldMin.size() < 2) return most;
        // The original's: by its owner's level, int(40 / S x G x 2448 / 11796) at least 1, G = int(GoldMin(level) x 10 x
        // 0.125) (_RefDropGold), S its capacity class by its Rarity (1: 40, 2: 30), a mall transport's S / 1.4; at most
        // its row's InventorySize (a level-20 trader's horse: 24 of its 60).
        const assets::ObjectRow* row = config.characters ? config.characters->Find(model) : nullptr;
        double s = row != nullptr && row->rarity == 2 ? 30.0 : 40.0;
        if (row != nullptr && row->cashItem) s /= 1.4;
        const std::size_t at = std::clamp<std::size_t>(std::size_t(std::max(1, level)), 1, jobRules.goldMin.size() - 1);
        const int g = int(double(jobRules.goldMin[at]) * 10.0 * 0.125);
        const int places = std::max(1, int(40.0 / s * double(g) * 2448.0 / 11796.0));
        return std::min(most, places);
    }
    // A transport's row and bag (_CharCOS, _InvCOS), in its owner's character's order of saves (Coses.h).
    void CosCartCreate(std::int64_t character, const CosRow& row, std::vector<std::uint32_t> older, std::function<void(std::int64_t)> done) override {
        auto id = std::make_shared<std::int64_t>(0);
        CharacterWrite(
            character,
            [this, character, row, older = std::move(older), id](Database& db) {
                // Its older transports (kept, never brought back) go first with their goods: one transport a character.
                for (const CosRow& old : LoadCosOf(db, character, older)) {
                    core::LogWarning("{}: character {}'s older transport {} ({} stacks of goods) gives way to a new one", config.name, character, old.id,
                                     old.bag.size());
                    DeleteCos(db, old.id);
                }
                std::string error;
                *id = AddCos(db, row, 0, error);
                if (*id == 0) core::LogWarning("{}: character {}'s transport has no row: {}", config.name, character, error);
            },
            [id, done = std::move(done)] { done(*id); });
    }
    void CosCartSave(std::int64_t character) override {
        CharacterWrites& w = characterWrites[character];
        w.goldPending = false; // this save writes the gold as it is at its turn
        if (w.cartQueued) return;
        w.cartQueued = true;
        CharacterTask(character, [this, character]() -> std::function<void(Database&)> {
            CharacterWrites& w = characterWrites[character];
            w.cartQueued = false;
            std::uint64_t gold = w.gold;
            std::int64_t row = 0;
            std::vector<StoredItem> present;
            bool short_ = false;
            if (PlayerWorld* p = PlayerOfCharacter(character)) {
                gold = p->inventory.gold;
                GoldNow(character, gold);
                if (const auto cart = coses ? coses->TransportCart(p->entity) : std::nullopt; cart && cart->bag != nullptr && cart->row != 0) {
                    row = cart->row;
                    for (auto& [place, item] : *cart->bag) {
                        // A new good: one of the reserved ids (none left now: written by the next save).
                        if (item.serial == 0 && !freeItemIds.empty()) {
                            item.serial = freeItemIds.back();
                            freeItemIds.pop_back();
                        }
                        if (item.serial == 0) {
                            short_ = true;
                            continue;
                        }
                        StoredItem s = StoredOf(place, item);
                        s.creator = item.creator;
                        present.push_back(std::move(s));
                    }
                    TopUpItemIds();
                }
            }
            if (short_) CosCartSave(character); // again after this one, the ids reserved meanwhile
            return [this, character, gold, row, present = std::move(present)](Database& db) {
                std::string error;
                const bool ok = db.Transaction(
                    [&](Database& tx) {
                        if (row != 0 && !SaveCart(tx, row, present, error)) return false;
                        return tx.Query("UPDATE _char SET remaingold = $2 WHERE charid = $1", {character, std::int64_t(gold)}).Ok();
                    },
                    error);
                if (!ok) completions->Post([this, character] { SaveFailed(character, "the transport's goods"); });
            };
        });
    }
    void CosCartRelease(std::int64_t character, std::int64_t row, std::uint32_t hp, bool ridden, const std::map<std::uint8_t, game::ItemInstance>& bag) override {
        const auto state = std::int64_t(ridden ? game::CosState::Active : game::CosState::Summoned);
        std::vector<StoredItem> present;
        for (const auto& [place, item] : bag) {
            StoredItem s = StoredOf(place, item);
            s.creator = item.creator;
            present.push_back(std::move(s));
        }
        CharacterWrite(character, [this, character, row, hp, state, present = std::move(present)](Database& db) mutable {
            // Its new goods not written yet: ids of their own now.
            int missing = 0;
            for (const StoredItem& s : present) missing += s.id == 0 ? 1 : 0;
            if (missing > 0) {
                std::vector<std::int64_t> ids = ReserveItemIds(db, missing);
                for (StoredItem& s : present) {
                    if (s.id != 0 || ids.empty()) continue;
                    s.id = ids.back();
                    ids.pop_back();
                }
            }
            std::string error;
            const bool ok = db.Transaction(
                [&](Database& tx) {
                    return SaveCart(tx, row, present, error) &&
                           tx.Query("UPDATE _charcos SET hp = $2, state = $3 WHERE id = $1", {row, std::int64_t(hp), state}).Ok();
                },
                error);
            if (!ok) core::LogWarning("{}: character {}'s transport {} not kept: {}", config.name, character, row, error);
        });
    }
    void CosCartDelete(std::int64_t character, std::int64_t row) override {
        CharacterWrite(character, [row](Database& db) { DeleteCos(db, row); });
    }
    void CosCartLoad(std::int64_t character, std::vector<std::uint32_t> models, std::function<void(std::vector<CosRow>)> done) override {
        auto rows = std::make_shared<std::vector<CosRow>>();
        CharacterWrite(
            character, [character, models = std::move(models), rows](Database& db) { *rows = LoadCosOf(db, character, models); },
            [rows, done = std::move(done)] { done(std::move(*rows)); });
    }
    bool CosNpcNear(std::uint32_t owner, std::uint32_t npc) const override {
        const auto it = npcs.find(npc);
        const auto me = world->Where(owner);
        const auto there = world->Where(npc);
        if (it == npcs.end() || !me || !there) return false;
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
        return std::hypot(a.x - b.x, a.z - b.z) <= config.gateReach;
    }
    void CosSellPay(std::uint32_t owner, std::uint64_t price) override {
        PlayerWorld* player = PlayerByEntity(owner);
        Peer* peer = player != nullptr ? peers->Find(world->PeerOf(owner)) : nullptr;
        if (player == nullptr) return;
        player->inventory.gold = player->inventory.gold > UINT64_MAX - price ? UINT64_MAX : player->inventory.gold + price;
        if (peer != nullptr) SendMessage(*peer->connection, proto::GoldChanged{player->inventory.gold});
        QueueGoldSave(player->selected, player->inventory.gold);
    }
    void CosTransportGone(std::uint32_t owner) override {
        if (trades) trades->CargoChanged(owner); // its goods went with it: no stars
    }
    void CosTransportBack(std::uint32_t owner) override {
        if (trades) trades->CargoChanged(owner); // its goods count again: their stars, the bandits' caravan
    }
    bool CosTown(std::uint16_t region) const override { return BanditTown(region); }

    // --- The jobs' experience and rewards ------------------------------------------------
    // Job experience gained: one level a gain at most,
    // the cap 7; a thief's or hunter's gain adds to its contribution; saved, told (0x30E6), its league again.
    void AddJobExp(PlayerWorld& player, std::uint64_t gain, std::uint64_t donated = 0) {
        if (player.job.type == game::JobType::None || (gain == 0 && donated == 0)) return;
        const int before = player.job.level;
        const bool up = gain > 0 && game::GainJobExp(jobRules, player.job, gain);
        if (gain > 0 && (player.job.type == game::JobType::Thief || player.job.type == game::JobType::Hunter)) {
            player.job.contribution = std::uint32_t(std::min<std::uint64_t>(kMostContribution, std::uint64_t(player.job.contribution) + gain));
        }
        if (donated > 0) player.job.contribution = std::uint32_t(std::min<std::uint64_t>(kMostContribution, std::uint64_t(player.job.contribution) + donated));
        TellJobExp(player, std::int64_t(gain), up, donated);
        if (up) core::LogInfo("{}: {}'s job level {} -> {}", config.name, player.name, before, player.job.level);
    }
    void LoseJobExpOf(PlayerWorld& player, std::uint64_t loss) {
        if (player.job.type == game::JobType::None || loss == 0) return;
        const std::uint64_t had = player.job.exp;
        game::LoseJobExp(player.job, loss);
        // A thief's or hunter's contribution follows every change of its job experience, the loss too (not below 0).
        if (player.job.type == game::JobType::Thief || player.job.type == game::JobType::Hunter) {
            player.job.contribution = std::uint32_t(std::uint64_t(player.job.contribution) - std::min<std::uint64_t>(player.job.contribution, had - player.job.exp));
        }
        TellJobExp(player, -std::int64_t(had - player.job.exp), false, 0);
    }
    static constexpr std::uint64_t kMostContribution = 2'000'000'000;
    void TellJobExp(PlayerWorld& player, std::int64_t delta, bool up, std::uint64_t donated) {
        database.Submit([character = player.selected, job = player.job](Database& db) { SaveJob(db, character, job); });
        Peer* peer = peers->Find(world->PeerOf(player.entity));
        if (peer == nullptr) return;
        SendMessage(*peer->connection, proto::JobExpChanged{std::uint8_t(player.job.type), std::uint8_t(player.job.level), delta, player.job.exp, up, donated});
        SendJobInfo(*peer, player);
    }
    // The party members near `entity` (within 1000, alive, in its layer), itself first.
    std::vector<PlayerWorld*> PartyNear(std::uint32_t entity) {
        std::vector<PlayerWorld*> out;
        PlayerWorld* self = PlayerByEntity(entity);
        if (self == nullptr) return out;
        out.push_back(self);
        const auto at = world->Where(entity);
        if (!parties || !at) return out;
        for (const std::uint32_t other : parties->MemberEntities(entity)) {
            if (other == entity) continue;
            PlayerWorld* p = PlayerByEntity(other);
            const auto there = world->Where(other);
            if (p == nullptr || p->dead || !there || there->position.layer != at->position.layer) continue;
            const proto::FlatPosition a = proto::ToFlat(at->position), b = proto::ToFlat(there->position);
            if (std::hypot(a.x - b.x, a.z - b.z) <= 1000.0) out.push_back(p);
        }
        return out;
    }
    // A sale's gold and job experience (the party's; the
    // pools): a thief's party of n thieves shares gold x (1 + 0.05 (n - 1)) evenly, each trunc(reward x that
    // / n) exp; a trader keeps G x 4 / (h + 4) of G = gold x (1 + 0.05 (t - 1)) with his own reward, each hunter near
    // G x 1.05 / (h + 4) and trunc(margin x (1 + 0.05 (h - 1)) / (h + 4)) exp; 10 % of the party's payout to the
    // weekly pool (the thieves' or, a trader's, the hunters' and his contribution).
    void TradeBargainLocked(std::uint32_t entity, std::int64_t until) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return;
        database.Submit([character = p->selected, until](Database& db) { SaveBargainLock(db, character, until); });
        if (Peer* peer = peers->Find(world->PeerOf(entity))) SendMessage(*peer->connection, proto::TradeBargainLock{600});
    }
    void TradeStarsChanged(std::uint32_t owner, int stars) override {
        if (stars == 1) return;
        if (PlayerWorld* p = PlayerByEntity(owner)) ResetSafeTrade(*p); // the scale off one
    }
    std::uint64_t TradeSold(std::uint32_t seller, std::uint64_t gold, std::uint64_t payout, std::uint64_t margin, int stars) override {
        (void)payout;
        PlayerWorld* s = PlayerByEntity(seller);
        if (s == nullptr) return 0;
        ResetSafeTrade(*s); // a sale at a specialty NPC
        const std::vector<PlayerWorld*> near = PartyNear(seller);
        const auto give = [&](PlayerWorld& p, std::uint64_t amount) {
            if (Peer* peer = peers->Find(world->PeerOf(p.entity))) GiveGold(peer->id, amount);
        };
        // No profit (a town's own goods, a quote at or under the goods' price): a plain sale, its gold alone.
        if (margin == 0) {
            give(*s, gold);
            return gold;
        }
        // The pool's share and a trader's donation: 10 % of the payout after the tax, cut, never bonused.
        const std::uint64_t tenth = std::uint64_t(std::trunc(double(gold) * 0.1));
        if (s->jobState == std::uint8_t(game::JobType::Thief)) {
            std::vector<PlayerWorld*> thieves;
            for (PlayerWorld* p : near) {
                if (p->jobState == std::uint8_t(game::JobType::Thief)) thieves.push_back(p);
            }
            const double n = double(thieves.size()), bonus = 1.0 + 0.05 * (n - 1.0);
            const std::uint64_t each = std::uint64_t(double(gold) * bonus / n);
            const std::uint64_t reward = game::SaleJobExp(game::JobType::Thief, margin, stars);
            for (PlayerWorld* p : thieves) {
                give(*p, each);
                AddJobExp(*p, std::uint64_t(double(reward) * bonus / n));
            }
            AddToPool(game::JobType::Thief, tenth);
            return each;
        }
        // A trader: the profit (the margin M) bonused by the traders near, G = trunc(M x (1 + 0.05 (t - 1))); he gets his
        // share of G and his outlay back (trunc(G x 4 / (h + 4)) - M + P), each hunter near trunc(G x 1.05 / (h + 4)); the
        // job experience on G (his own reward; each hunter's trunc(reward x (1 + 0.05 (h - 1)) / (h + 4))).
        std::vector<PlayerWorld*> hunters;
        double traders = 0;
        for (PlayerWorld* p : near) {
            if (p->jobState == std::uint8_t(game::JobType::Hunter)) hunters.push_back(p);
            if (p->jobState == std::uint8_t(game::JobType::Trader)) traders += 1;
        }
        const double h = double(hunters.size());
        const std::uint64_t g = std::uint64_t(std::trunc(double(margin) * (1.0 + 0.05 * (std::max(1.0, traders) - 1.0))));
        const std::int64_t ownSigned = std::int64_t(std::trunc(double(g) * 4.0 / (h + 4.0))) - std::int64_t(margin) + std::int64_t(gold);
        const std::uint64_t own = std::uint64_t(std::max<std::int64_t>(0, ownSigned));
        give(*s, own);
        AddJobExp(*s, game::SaleJobExp(game::JobType::Trader, g, stars), tenth);
        for (PlayerWorld* p : hunters) {
            give(*p, std::uint64_t(std::trunc(double(g) * 1.05 / (h + 4.0))));
            AddJobExp(*p, std::uint64_t(std::trunc(double(game::SaleJobExp(game::JobType::Hunter, g, stars)) * (1.0 + 0.05 * (h - 1.0)) / (h + 4.0))));
        }
        AddToPool(game::JobType::Hunter, tenth);
        return own;
    }
    // The week's pools (`_TrijobRewards`, the original's `_AddTrijobReward`): shared out by SroDb --job-week.
    void AddToPool(game::JobType type, std::uint64_t amount) {
        if (amount == 0) return;
        database.Submit([type, amount](Database& db) { AddTrijobReward(db, type, amount); });
    }
    // A job's kill (the party's): exp = A x clamp(A / B, 0.5, 1.5) for
    // the killer's level, halved for a trader; shared by the party near (only hunters when a hunter kills; those whose
    // job may take it), each trunc(exp x (1 + 0.05 (n - 1)) x its level / the levels' sum).
    void JobKill(PlayerWorld& killer, int victimLevel, const std::function<bool(const PlayerWorld&)>& may) {
        std::vector<PlayerWorld*> takers;
        for (PlayerWorld* p : PartyNear(killer.entity)) {
            if (!may(*p)) continue;
            if (killer.jobState == std::uint8_t(game::JobType::Hunter) && p->jobState != std::uint8_t(game::JobType::Hunter)) continue;
            takers.push_back(p);
        }
        if (takers.empty()) return;
        // Each taker's K from his own level, halved when he is a trader; his share by his level, the party's bonus.
        double levels = 0;
        for (const PlayerWorld* p : takers) levels += double(std::max(1, p->level));
        const double bonus = 1.0 + 0.05 * double(takers.size() - 1);
        for (PlayerWorld* p : takers) {
            const std::uint64_t k = game::KillJobExp(jobRules, victimLevel, p->level, p->jobState == std::uint8_t(game::JobType::Trader));
            AddJobExp(*p, std::uint64_t(std::trunc(double(k) * double(std::max(1, p->level)) / levels * bonus)));
        }
    }
    // A job monster's kill: a thief monster's by traders and hunters, a hunter monster's by thieves.
    void BanditKilled(std::uint32_t killerEntity, const assets::ObjectRow& row) {
        PlayerWorld* killer = PlayerByEntity(killerEntity);
        if (killer == nullptr || !assets::IsBandit(row)) return;
        const bool thiefMonster = row.typeId[3] == 2;
        const auto may = [thiefMonster](const PlayerWorld& p) {
            return thiefMonster ? (p.jobState == std::uint8_t(game::JobType::Trader) || p.jobState == std::uint8_t(game::JobType::Hunter))
                                : p.jobState == std::uint8_t(game::JobType::Thief);
        };
        if (!may(*killer)) return;
        JobKill(*killer, row.level, may);
    }
    // A transport's cargo on the ground: each slot whole, around where it
    // fell, no owner's mark (skipped); the goods keep their buyer's alias.
    void CosCargoDropped(std::uint32_t owner, const proto::WorldPosition& at, std::map<std::uint8_t, game::ItemInstance> bag) override {
        std::vector<DropTable::Drop> drops;
        for (auto& [place, item] : bag) {
            DropTable::Drop d;
            item.serial = 0;
            d.item = std::move(item);
            drops.push_back(std::move(d));
        }
        const PlayerWorld* o = PlayerByEntity(owner);
        core::LogInfo("{}: {}'s transport drops {} stacks of goods", config.name, o != nullptr ? o->name : std::string("?"), drops.size());
        DropLoot(at, 0, std::move(drops));
    }

    // --- The bandits (Bandits.h) ---------------------------------------------------------
    std::unique_ptr<Bandits> bandits;
    std::unordered_map<std::uint16_t, RegionKind> regionKinds; // `_RefRegion`: towns, the bandits' lists by continent
    void TradeCaravan(std::uint32_t owner) override {
        if (bandits) bandits->Register(owner, Clock::now());
    }
    std::optional<BanditHost::Caravan> BanditCaravan(std::uint32_t entity) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr || p->entity == 0 || !coses) return std::nullopt;
        const auto cart = coses->TransportCart(entity);
        const auto me = world->Where(entity);
        const auto at = cart ? world->Where(cart->entity) : std::nullopt;
        if (!cart || cart->bag == nullptr || !me || !at) return std::nullopt;
        BanditHost::Caravan c;
        c.name = p->name;
        c.jobState = p->jobState;
        int mastery = 0;
        for (const auto& [id, level] : p->book.masteries) mastery = std::max(mastery, int(level));
        c.level = std::max(p->level, mastery);
        c.valueLevel = std::max(p->level, p->maxLevel);
        const assets::ObjectRow* row = config.characters ? config.characters->Find(cart->model) : nullptr;
        c.transportCodename = row != nullptr ? row->codename : std::string();
        c.at = at->position;
        c.playerRegion = me->position.region;
        const assets::ItemTable* items = config.rules->items.get();
        for (const auto& [place, item] : *cart->bag) {
            const assets::ItemRow* good = items != nullptr ? items->Find(item.refId) : nullptr;
            if (good == nullptr || !game::IsSpecialtyGood(*good)) continue;
            c.goods += item.count;
            c.bargain = c.bargain || game::IsBargainGood(*good);
        }
        if (c.goods == 0) return std::nullopt;
        return c;
    }
    bool BanditTown(std::uint16_t region) const override {
        const auto it = regionKinds.find(region);
        return it != regionKinds.end() && it->second.town;
    }
    int BanditZone(std::uint16_t region) const override {
        const auto it = regionKinds.find(region);
        return it != regionKinds.end() ? it->second.banditZone : -1;
    }
    std::optional<proto::WorldPosition> BanditGround(const proto::WorldPosition& from, const proto::WorldPosition& to) const override {
        return world->WalkEnd(from, to);
    }
    bool BanditSpawn(const MonsterNest& nest) override {
        if (!monsters) return false;
        const std::size_t n = monsters->AddNest(nest);
        if (n == SIZE_MAX) return false;
        if (monsters->NestCounts(n).first == 0) { // no ground there (the original's "Invalid Region"): that one lost
            monsters->RemoveNest(n);
            return false;
        }
        return true;
    }
    const game::JobRules& BanditRules() const override { return jobRules; }
    std::optional<TradeHost::Trader> TradeTrader(std::uint32_t entity) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return std::nullopt;
        TradeHost::Trader t;
        t.peer = world->PeerOf(entity);
        t.character = p->selected;
        t.name = p->name;
        t.level = p->level;
        t.highestLevel = std::max(p->level, p->maxLevel);
        t.gold = p->inventory.gold;
        t.jobState = p->jobState;
        t.alias = p->alias;
        return t;
    }
    std::optional<std::pair<std::string, std::uint32_t>> TradeNpcNear(std::uint32_t entity, std::uint32_t npc) const override {
        const auto it = npcs.find(npc);
        const auto me = world->Where(entity);
        const auto there = world->Where(npc);
        if (it == npcs.end() || !me || !there) return std::nullopt;
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
        if (std::hypot(a.x - b.x, a.z - b.z) > config.gateReach) return std::nullopt;
        return std::pair<std::string, std::uint32_t>{it->second->codename, it->second->refObjId};
    }
    bool TradeThreeStarCap() const override { return config.pkProhibited; } // the PK switch's cap (the original's +0x42404)
    bool TradeQuestDone(std::uint32_t entity, std::string_view quest) const override {
        if (!quests) return false;
        const game::QuestDef* q = quests->Find(quest);
        const QuestRecord* r = q != nullptr ? quests->Record(entity, q->id) : nullptr;
        return r != nullptr && r->status == proto::QuestStatus::Done && r->times > 0;
    }
    std::optional<TradeHost::Cart> TradeCart(std::uint32_t owner) override {
        if (!coses) return std::nullopt;
        const auto cart = coses->TransportCart(owner);
        if (!cart) return std::nullopt;
        const assets::ObjectRow* row = config.characters ? config.characters->Find(cart->model) : nullptr;
        return TradeHost::Cart{cart->entity, row != nullptr ? row->codename : std::string(), cart->bag, cart->size};
    }
    void TradeCartChanged(std::uint32_t cart) override {
        if (coses) coses->CartChanged(cart);
    }
    double TradeDistance(std::uint32_t a, std::uint32_t b) const override {
        const auto pa = world->Where(a), pb = world->Where(b);
        if (!pa || !pb) return -1.0;
        const proto::FlatPosition fa = proto::ToFlat(pa->position), fb = proto::ToFlat(pb->position);
        return std::hypot(fa.x - fb.x, fa.z - fb.z);
    }
    bool TradeTakeGold(std::uint64_t peer, std::uint64_t amount) override { return TakeGold(peer, amount); }
    void TradeGiveGold(std::uint64_t peer, std::uint64_t amount) override { GiveGold(peer, amount); }
    void TradeSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override {
        if (Peer* peer = peers->Find(peerId)) peer->connection->Send(id, payload);
    }
    const assets::ItemTable* TradeItems() const override { return config.rules->items.get(); }
    const game::JobRules& TradeRules() const override { return jobRules; }
    void TradeSaveStocks(std::vector<std::pair<std::uint32_t, std::int64_t>> stocks) override {
        database.Submit([stocks = std::move(stocks)](Database& db) { SaveMarketStocks(db, stocks); });
    }
    void TradeLoadMarket(std::uint32_t npcRef, std::function<void(std::vector<game::MarketRow>)> done) override {
        database.Submit([this, npcRef, done = std::move(done)](Database& db) {
            std::vector<game::MarketRow> rows;
            std::string error;
            if (!LoadMarkets(db, {npcRef}, rows, error)) core::LogWarning("{}: a market: {}", config.name, error);
            completions->Post([done, rows = std::move(rows)] { done(rows); });
        });
    }
    // The exchanges between two players (Exchanges.h).
    std::unique_ptr<Exchanges> exchanges;
    std::optional<ExchangeHost::Trader> ExchangeTrader(std::uint32_t entity) override {
        const auto it = players.find(world->PeerOf(entity));
        const auto place = world->Where(entity);
        if (it == players.end() || it->second.entity != entity || !place) return std::nullopt;
        PlayerWorld& p = it->second;
        // Free of other dealings: a stall (its owner, a visitor, a buyer from afar) and the Open Market's window.
        const bool free = (!stalls || (!stalls->Owner(entity) && !stalls->Visiting(entity) && !stalls->Holding(entity))) &&
                          !(openMarket && openMarket->InSession(it->first));
        return ExchangeHost::Trader{it->first, p.selected, p.name, !p.dead, free, place->position, &p.inventory, stalls && stalls->Visiting(entity)};
    }
    void ExchangeSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override { GuildSend(peerId, id, payload); }
    bool ExchangePetOut(std::uint32_t entity, std::uint8_t place) const override { return OpenMarketPetOut(world->PeerOf(entity), place); }
    const assets::ItemTable* ExchangeItems() const override { return config.rules->items.get(); }

    // --- COS (Coses.h) ---------------------------------------------------------------
    std::unique_ptr<Coses> coses;
    std::optional<CosHost::Owner> CosOwner(std::uint32_t entity) override {
        const PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return std::nullopt;
        CosHost::Owner o;
        o.peer = world->PeerOf(entity);
        o.character = p->selected;
        o.name = p->name;
        o.level = p->level;
        o.alive = !p->dead;
        o.murderer = p->pvpState == std::uint8_t(game::PvpState::Murderer);
        o.free = !(exchanges && exchanges->Busy(entity)) && !(stalls && (stalls->Owner(entity) || stalls->Visiting(entity)));
        // In a fight: the battle mark (20 s from a hit given or taken), as everything else judges it.
        o.fought = Clock::now() < p->combat.battleUntil;
        // The job's suit worn (itemdata TID 3/1/7/x: x 1 trader, 2 thief, 3 hunter).
        if (const game::ItemInstance* suit = p->inventory.At(game::slot::kJobSuit); suit != nullptr && config.rules->items) {
            if (const assets::ItemRow* row = config.rules->items->Find(suit->refId); row != nullptr && row->typeId[0] == 3 && row->typeId[1] == 1 && row->typeId[2] == 7) {
                o.job = std::uint8_t(row->typeId[3]);
            }
        }
        const auto place = world->Where(entity);
        // A town by the region's battlefield byte (the original's thief test; _RefRegion.IsBattleField 0).
        o.inTown = place && place->position.layer == 0 && BanditTown(place->position.region);
        o.loggingOut = p->loggingOut;
        o.travelling = p->moving || p->entering;
        o.freePvpChanging = p->freePvpAt.has_value();
        o.freePvp = p->freePvp != 0;
        o.hidden = p->hidden;
        o.inArena = arenas && arenas->InMatch(entity);
        o.sitting = p->motion != proto::kMotionStand;
        return o;
    }
    void CosSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override { GuildSend(peerId, id, payload); }
    void CosBroadcast(std::uint32_t entity, proto::MessageId id, const std::vector<std::byte>& payload) override {
        world->ForEachObserver(entity, [&](std::uint64_t peerId) { GuildSend(peerId, id, payload); });
    }
    void CosRiding(std::uint32_t owner, bool riding) override {
        if (PlayerWorld* p = PlayerByEntity(owner)) {
            p->riding = riding;
            StatsChanged(owner); // its speed: the mount's alone while it rides
        }
    }
    World& CosWorld() override { return *world; }
    const assets::CharacterTable* CosCharacters() const override { return config.characters.get(); }
    void CosLoad(std::int64_t id, std::function<void(std::optional<CosRow>)> done) override {
        database.Submit([this, id, done = std::move(done)](Database& db) {
            std::optional<CosRow> row = LoadCos(db, id);
            completions->Post([done, row = std::move(row)] { done(row); });
        });
    }
    void CosCreate(std::uint32_t owner, const CosRow& row, std::int64_t serial, std::function<void(std::int64_t id)> done) override {
        database.Submit([this, owner, row, serial, done = std::move(done)](Database& db) {
            std::string error;
            const std::int64_t id = AddCos(db, row, serial, error);
            if (id == 0) core::LogWarning("{}: a pet's row not made: {}", config.name, error);
            completions->Post([this, owner, serial, id, done] {
                // The item in the bag names its pet now, as its _Items.Data does (its later saves keep it).
                if (PlayerWorld* p = id != 0 && serial != 0 ? PlayerByEntity(owner) : nullptr) {
                    for (auto& [place, item] : p->inventory.places) {
                        if (item.serial == serial) item.durability = std::uint32_t(id);
                    }
                }
                done(id);
            });
        });
    }
    // A COS row's saves in their order: the pool runs a task on whichever thread is free, so two saves of one row could
    // commit out of order (a summon's "summoned" after its dismissal's "inactive"). One save of a row in flight at a time,
    // the newest waiting behind it (those between are passed over: the newest holds them).
    struct CosSaveQueue {
        std::mutex mutex;
        std::unordered_map<std::int64_t, std::optional<CosRow>> rows; // a row in flight -> the newest to write after it
    };
    std::shared_ptr<CosSaveQueue> cosSaves = std::make_shared<CosSaveQueue>();
    static void SubmitCosSave(DatabasePool& pool, std::shared_ptr<CosSaveQueue> queue, CosRow row) {
        pool.Submit([&pool, queue, row](Database& db) {
            SaveCos(db, row);
            std::optional<CosRow> next;
            {
                std::lock_guard lock(queue->mutex);
                const auto it = queue->rows.find(row.id);
                if (it != queue->rows.end() && it->second) next = std::exchange(it->second, std::nullopt);
                else if (it != queue->rows.end()) queue->rows.erase(it);
            }
            if (next) SubmitCosSave(pool, queue, *next);
        });
    }
    void CosSave(const CosRow& row) override {
        if (row.id == 0) return; // no row (a scroll's COS): SaveCos updates by id
        {
            std::lock_guard lock(cosSaves->mutex);
            const auto [it, first] = cosSaves->rows.try_emplace(row.id);
            if (!first) {
                it->second = row; // one in flight: this one after it
                return;
            }
        }
        SubmitCosSave(database, cosSaves, row);
    }
    bool CosNameAllowed(std::string_view name) const override { return config.rules->names.Check(name) == game::NameCheck::Ok; }
    const assets::SkillTable* CosSkills() const override { return config.skills.get(); }
    std::span<const std::uint64_t> CosLevels() const override { return config.levels; }
    std::span<const std::uint32_t> CosMonsterExperience() const override { return config.monsterExperience; }
    std::optional<CosHost::Foe> CosFoe(std::uint32_t monster) override {
        if (!monsters || (fortresses && fortresses->IsStructure(monster))) return std::nullopt;
        const game::CombatStats* stats = monsters->Stats(monster);
        const auto where = world->Where(monster);
        if (stats == nullptr || !where) return std::nullopt;
        return CosHost::Foe{stats, monsters->Hp(monster), where->position, monsters->TargetOf(monster)};
    }
    void CosStrike(std::uint32_t pet, std::uint32_t monster, const game::HitResult& hit, const Monsters::Affliction* affliction,
                   std::uint32_t imbue) override {
        if (!monsters) return;
        const auto now = Clock::now();
        const Monsters::Damaged d = monsters->Damage(monster, pet, hit.damage, now, affliction);
        if (!d.hit) return;
        std::uint8_t flags = hit.critical ? proto::kDamageCritical : 0;
        if (hit.blocked) flags |= proto::kDamageBlocked;
        if (d.killed) flags |= proto::kDamageKilled;
        Broadcast(monster, proto::EntityDamaged{monster, pet, hit.damage, d.hp, flags, imbue});
        if (d.statesChanged) Broadcast(monster, proto::EntityStates{monster, monsters->StatesOf(monster, now)});
        if (d.killed) Killed(monster, d);
    }
    std::vector<std::uint32_t> CosMonstersNear(const proto::WorldPosition& at, float radius) override {
        return monsters ? monsters->Near(at, radius) : std::vector<std::uint32_t>{};
    }
    // The pets' timed jobs: the wall clock, and their rows in _TimedJobForPet.
    std::int64_t CosNow() const override { return WallClock(); }
    void CosSavePetJob(std::int64_t cos, const PetJobRow& job) override {
        // After every write before it (a job of the same item or skill ended just before is gone first).
        database.SubmitBarrier([cos, job](Database& db) {
            if (!AddPetJob(db, cos, job)) core::LogWarning("pet {}: its job {} not written", cos, job.job);
        });
    }
    void CosEndPetJob(std::int64_t cos, std::uint8_t category, std::uint32_t job) override {
        database.Submit([cos, category, job](Database& db) { EndPetJob(db, cos, category, job); });
    }
    void CosTakeOne(std::uint32_t owner, std::uint8_t place) override {
        PlayerWorld* p = PlayerByEntity(owner);
        Peer* peer = p != nullptr ? peers->Find(world->PeerOf(owner)) : nullptr;
        if (p == nullptr || peer == nullptr || !p->inventory.At(place)) return;
        TakeOne(*peer, *p, place);
    }
    void CosName(std::int64_t id, const std::string& name, std::function<void(CosNaming)> done) override {
        database.Submit([this, id, name, done = std::move(done)](Database& db) {
            const CosNaming result = NameCos(db, id, name);
            completions->Post([done, result] { done(result); });
        });
    }
    std::vector<CosHost::GroundSeen> CosGroundNear(std::uint32_t owner, float range) override {
        std::vector<CosHost::GroundSeen> out;
        const PlayerWorld* p = PlayerByEntity(owner);
        const auto me = world->Where(owner);
        if (p == nullptr || !me) return out;
        const proto::FlatPosition at = proto::ToFlat(me->position);
        const auto now = Clock::now();
        for (const auto& [entity, g] : ground) {
            // As its own pick-up decides (TakeFromGround): not a quest item of another's, not kept for another.
            if (g.ownerOnly && g.owner != p->selected) continue;
            // Specialty goods go only into a transport, by its trader or a thief's own hand (TakeFromGround's rules):
            // never a pet's (inferred: the original's pickup of them needs a transport; the user's report 2026-11-22).
            if (const assets::ItemRow* good = g.item.refId != 0 && config.rules->items ? config.rules->items->Find(g.item.refId) : nullptr;
                good != nullptr && game::IsSpecialtyGood(*good)) {
                continue;
            }
            if (g.owner != 0 && g.owner != p->selected && now < g.ownerUntil &&
                !(parties && parties->SharesItems(std::uint32_t(g.owner), std::uint32_t(p->selected)))) {
                continue;
            }
            // A quest item goes to its owner's bag (PetQuestItem): not while it has no room there.
            if (const assets::ItemRow* r = g.item.refId != 0 && config.rules->items ? config.rules->items->Find(g.item.refId) : nullptr;
                r != nullptr && PetQuestItem(*r) && PlaceFor(p->inventory, *r, r->refObjId, g.item.count) < 0) {
                continue;
            }
            const auto there = world->Where(entity);
            if (!there) continue;
            const proto::FlatPosition q = proto::ToFlat(there->position);
            if (std::hypot(q.x - at.x, q.z - at.z) > range) continue;
            const assets::ItemRow* row = g.item.refId != 0 && config.rules->items ? config.rules->items->Find(g.item.refId) : nullptr;
            CosHost::GroundSeen s;
            s.entity = entity;
            s.position = there->position;
            s.kind = g.item.refId == 0 ? proto::kGrabGold : row != nullptr && row->typeId[1] == 1 ? proto::kGrabEquipment : proto::kGrabOther;
            s.mine = g.owner == 0 || g.owner == p->selected; // its owner's, or nobody's (ours: "my items" takes these too)
            out.push_back(s);
        }
        return out;
    }
    CosHost::Picked CosPickUp(std::uint32_t owner, std::uint32_t pet, std::uint32_t entity, const std::function<bool(game::ItemInstance&)>& put) override {
        using Picked = CosHost::Picked;
        PlayerWorld* p = PlayerByEntity(owner);
        Peer* peer = p != nullptr ? peers->Find(world->PeerOf(owner)) : nullptr;
        const auto it = ground.find(entity);
        if (p == nullptr || peer == nullptr || it == ground.end()) return Picked::Gone;
        GroundItem& g = it->second;
        if (const assets::ItemRow* good = g.item.refId != 0 && config.rules->items ? config.rules->items->Find(g.item.refId) : nullptr;
            good != nullptr && game::IsSpecialtyGood(*good)) {
            return Picked::Gone; // specialty goods: never a pet's (CosGroundNear)
        }
        // Who it goes to (the original's recipient): gold and quest items its owner's bag (the Roc's alchemy items
        // shared); in a party sharing items the member whose turn it is, within 1000; else the pet's own bag.
        const assets::ItemRow* row = g.item.refId != 0 && config.rules->items ? config.rules->items->Find(g.item.refId) : nullptr;
        const bool quest = row != nullptr && PetQuestItem(*row) && !row->codename.starts_with("ITEM_QNO_RM_ARCHEMY");
        const std::uint32_t to = row != nullptr && !quest && parties ? parties->LootRecipient(owner) : owner;
        const std::uint16_t count = std::uint16_t(std::min<std::uint32_t>(g.item.count, 0xFFFF));
        // Into a player's bag (its owner's, or the turn's member's): false when it has no room.
        const auto intoBag = [&](std::uint32_t entity) {
            PlayerWorld* r = PlayerByEntity(entity);
            Peer* rp = r != nullptr ? peers->Find(world->PeerOf(entity)) : nullptr;
            const int place = r != nullptr && rp != nullptr ? PlaceFor(r->inventory, *row, row->refObjId, g.item.count) : -1;
            if (place < 0 || !Put(*r, place, g.item)) return false;
            proto::ItemMoved answer;
            answer.from = answer.to = std::uint8_t(place);
            answer.changed.push_back(r->inventory.Record(std::uint8_t(place)));
            SendMessage(*rp->connection, answer);
            QueueItemSave(r->selected, r->inventory, {std::uint8_t(place)}, {});
            if (parties && parties->PartyOf(entity) != 0) {
                parties->TellLoot(entity, g.item.refId, count, 0);
            } else {
                SendMessage(*rp->connection, proto::ItemGained{g.item.refId, count, 0});
            }
            if (quests) quests->BagChanged(entity);
            if (const game::ItemInstance* got = r->inventory.At(std::uint8_t(place)))
                LogItem(*r, logs::ItemOp::PickUp, got->refId, got->serial, g.item.count, logs::kNoSlot, std::uint8_t(place));
            return true;
        };
        if (g.item.refId == 0) {
            p->inventory.gold += g.gold; // gold to its owner
            SendMessage(*peer->connection, proto::GoldChanged{p->inventory.gold});
            QueueGoldSave(p->selected, p->inventory.gold);
            SendMessage(*peer->connection, proto::ItemGained{0, 0, g.gold}); // its owner's line, as for its own pick-up
        } else if (row != nullptr && (quest || to != owner) && intoBag(quest ? owner : to)) {
            // (into a player's bag)
        } else if (quest) {
            return Picked::Gone; // its owner's bag full: it stays on the ground (the original's 0x18B5)
        } else {
            if (freeItemIds.empty()) return Picked::Gone; // no id for it yet: it stays for now
            game::ItemInstance item = g.item;
            item.serial = freeItemIds.back();
            if (!put(item)) return Picked::NoRoom;
            freeItemIds.pop_back();
            SendMessage(*peer->connection, proto::ItemGained{g.item.refId, count, 0});
        }
        core::LogInfo("{}: {}'s pet picks up ground item {}", config.name, p->name, entity);
        ground.erase(it);
        world->Leave(entity);
        const std::vector<std::byte> bytes = proto::Encode(proto::EntityAction{pet, proto::EntityActionKind::PickUp});
        world->ForEachObserver(pet, [&](std::uint64_t to) {
            if (Peer* o = peers->Find(to)) o->connection->Send(proto::EntityAction::kId, bytes);
        });
        return Picked::Taken;
    }
    // A quest item (itemdata TID 3/3/9): a pet's pick-up of one goes to its owner's bag.
    static bool PetQuestItem(const assets::ItemRow& row) { return row.typeId[0] == 3 && row.typeId[1] == 3 && row.typeId[2] == 9; }
    const assets::ItemTable* CosItems() const override { return config.rules->items.get(); }
    game::Inventory* CosInventory(std::uint32_t owner) override {
        PlayerWorld* p = PlayerByEntity(owner);
        return p != nullptr ? &p->inventory : nullptr;
    }
    void CosInventoryChanged(std::uint32_t owner, const std::vector<std::uint8_t>& touched) override {
        PlayerWorld* p = PlayerByEntity(owner);
        Peer* peer = p != nullptr ? peers->Find(world->PeerOf(owner)) : nullptr;
        if (peer == nullptr) return;
        proto::ItemMoved answer;
        answer.from = answer.to = touched.empty() ? 0 : touched.front();
        for (std::uint8_t place : touched) {
            if (p->inventory.At(place) != nullptr) {
                answer.changed.push_back(p->inventory.Record(place));
            } else {
                answer.emptied.push_back(place);
            }
        }
        SendMessage(*peer->connection, answer);
        if (quests) quests->BagChanged(owner); // a quest's gathered item counted
    }
    game::ItemInstance CosItemOf(const StoredItem& s) override {
        game::ItemInstance item;
        item.serial = s.id;
        item.refId = s.refId;
        item.plus = s.plus;
        item.variance = s.variance;
        item.rentUntil = s.rentUntil;
        item.bound = s.bound;
        FromData(s, item);
        for (std::uint64_t m : s.magic) item.options.push_back({std::uint32_t(m & 0xFFFFFFFFu), std::uint32_t(m >> 32)});
        return item;
    }
    void CosSaveBag(std::uint32_t owner, std::int64_t cos, const std::map<std::uint8_t, game::ItemInstance>& bag, const std::vector<std::uint8_t>& bagTouched,
                    const std::vector<std::uint8_t>& inventoryTouched) override {
        PlayerWorld* p = PlayerByEntity(owner);
        if (p == nullptr) return;
        std::vector<StoredItem> present;
        std::vector<std::uint8_t> emptied;
        for (std::uint8_t place : std::set<std::uint8_t>(bagTouched.begin(), bagTouched.end())) {
            if (const auto it = bag.find(place); it != bag.end()) {
                present.push_back(StoredOf(place, it->second));
            } else {
                emptied.push_back(place);
            }
        }
        database.SubmitBarrier([this, side = SaveSideOf(p->selected, p->inventory, inventoryTouched), cos, present = std::move(present),
                                emptied = std::move(emptied)](Database& db) {
            if (!SaveCosItems(db, side, cos, present, emptied)) completions->Post([this, character = side.character] { SaveFailed(character, "a pet's bag"); });
        });
    }
    bool CosHasItem(std::uint32_t owner, std::int64_t serial) override {
        const PlayerWorld* p = PlayerByEntity(owner);
        if (p == nullptr || serial == 0) return false;
        for (const auto& [place, item] : p->inventory.places) {
            if (item.serial == serial && place >= game::slot::kWorn) return true;
        }
        return false;
    }
    // Both sides in one transaction, after everything submitted before it (a barrier), with what each
    // side's own saves still waited to write: neither side's earlier saves can land after it.
    void ExchangeSave(std::int64_t a, const game::Inventory& bagA, const std::vector<std::uint8_t>& touchedA, std::int64_t b, const game::Inventory& bagB,
                      const std::vector<std::uint8_t>& touchedB) override {
        database.SubmitBarrier([this, sa = SaveSideOf(a, bagA, touchedA), sb = SaveSideOf(b, bagB, touchedB)](Database& db) {
            if (SaveExchange(db, sa, sb)) return;
            completions->Post([this, a = sa.character, b = sb.character] {
                SaveFailed(a, "an exchange");
                SaveFailed(b, "an exchange");
            });
        });
    }
    // A character's inventory as one transaction saves it (an exchange, a pet's bag): its places `touched` as they are
    // now, with its item saves still waiting folded in (taken from its queue: this save writes them).
    ExchangeSaveSide SaveSideOf(std::int64_t character, const game::Inventory& bag, const std::vector<std::uint8_t>& touched) {
        ExchangeSaveSide s;
        s.character = character;
        s.gold = bag.gold;
        GoldNow(character, bag.gold);
        std::set<std::uint8_t> places(touched.begin(), touched.end());
        if (const auto q = itemSaves.find(character); q != itemSaves.end()) {
            places.insert(q->second.dirty.begin(), q->second.dirty.end());
            s.gone = std::move(q->second.gone);
            q->second.gone.clear();
            q->second.dirty.clear();
        }
        for (std::uint8_t place : places) {
            const game::ItemInstance* i = bag.At(place);
            if (i == nullptr) {
                s.emptied.push_back(place);
                continue;
            }
            s.present.push_back(StoredOf(place, *i));
        }
        return s;
    }
    StoredItem StoredOf(std::uint8_t place, const game::ItemInstance& i) const {
        StoredItem stored{i.serial, place, i.refId, i.plus, i.variance, i.durability, i.count, {}, Counted(i.refId, i.durability)};
        stored.advanced = i.advanced;
        stored.bound = i.bound;
        stored.advancedRecipe = i.advancedRecipe;
        stored.sockets = i.sockets;
        for (const game::ItemInstance::Option& o : i.options) stored.magic.push_back(std::uint64_t(o.id) | std::uint64_t(o.value) << 32);
        return stored;
    }
    // Alchemy (Alchemy.h): the stones a dismantle picks among, by degree (the drop table's).
    std::unique_ptr<Alchemy> alchemy;
    std::unordered_map<int, game::alchemy::StoneBucket> stoneBuckets;
    std::optional<Alchemist> AlchemyPlayer(std::uint32_t entity) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return std::nullopt;
        const bool free = !(exchanges && exchanges->Busy(entity)) &&
                          !(stalls && (stalls->Owner(entity) || stalls->Visiting(entity) || stalls->Holding(entity)));
        // The character's bonuses from its buffs: `luck` (0xAD, the strengthening's) and `alcu` (0xAC, the stones'), the
        // premium's +5 % each (SKILL_MALL_PRE_APRU_4W_01). They were left 0 (the audit,
        // 2026-11-14): the premium's two rates never reached the rolls.
        const int luck = int(p->combat.Sheet().Value(0xAD, 0.0f) + p->itemLuck), alchemy = int(p->combat.Sheet().Value(0xAC, 0.0f));
        return Alchemist{world->PeerOf(entity), !p->dead, free, &p->inventory, luck, alchemy};
    }
    void AlchemySend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override { GuildSend(peerId, id, payload); }
    const assets::ItemTable* AlchemyItems() const override { return config.rules->items.get(); }
    const std::vector<assets::MagicOptionRow>* AlchemyOptions() const override { return config.magicOptions.get(); }
    const std::vector<assets::MagicOptGroupRow>* AlchemySocketGroups() const override { return &config.magicOptGroups; }
    const std::vector<assets::MagicOptionAssignRow>* AlchemyAssign() const override { return config.magicOptionAssign.get(); }
    bool AlchemyAtNpc(std::uint32_t entity, std::uint32_t npcEntity, std::uint32_t service) const override {
        const auto it = npcs.find(npcEntity);
        if (it == npcs.end() || (NpcServices(it->second->codename, it->second->refObjId) & service) == 0) return false;
        const auto me = world->Where(entity), npc = world->Where(npcEntity);
        if (!me || !npc) return false;
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(npc->position);
        return std::hypot(a.x - b.x, a.z - b.z) <= config.gateReach;
    }
    const game::alchemy::StoneBucket* AlchemyStones(int degree) const override {
        const auto it = stoneBuckets.find(degree);
        return it != stoneBuckets.end() ? &it->second : nullptr;
    }
    bool AlchemyGive(std::uint32_t entity, const std::vector<game::ItemInstance>& items, std::vector<std::uint8_t>& touched) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr || !config.rules->items) return false;
        const auto before = p->inventory.places;
        const auto ids = freeItemIds;
        const std::size_t touchedBefore = touched.size();
        for (const game::ItemInstance& item : items) {
            const assets::ItemRow* row = config.rules->items->Find(item.refId);
            // Stacks merge only with the same plus (a stone's is its assimilation rate).
            int place = -1;
            if (row != nullptr && game::MaxStack(*row) > 1) {
                for (const auto& [at, held] : p->inventory.places) {
                    if (at >= game::slot::kWorn && held.refId == item.refId && held.plus == item.plus &&
                        int(held.count) + int(item.count) <= game::MaxStack(*row)) {
                        place = at;
                        break;
                    }
                }
            }
            if (place < 0) place = p->inventory.FreeBagPlace();
            if (row == nullptr || place < 0 || !Put(*p, place, item)) {
                p->inventory.places = before;
                freeItemIds = ids;
                touched.resize(touchedBefore);
                return false;
            }
            touched.push_back(std::uint8_t(place));
        }
        return true;
    }
    void AlchemyBag(std::uint32_t entity, std::vector<std::uint8_t> touched, std::vector<std::int64_t> gone) override {
        if (PlayerWorld* p = PlayerByEntity(entity)) TellBag(*p, std::move(touched), gone);
    }
    // Stalls and the stall network (Stalls.h): the towns (regioninfo.txt) and the
    // network's tables, read at the start.
    std::unique_ptr<Stalls> stalls;
    // --- The quests (Quests.h) -------------------------------------------------
    std::unique_ptr<Quests> quests;
    Clock::time_point nextQuestTick;
    const Clock::time_point serverStarted = Clock::now();
    std::optional<QuestHost::Player> QuestPlayer(std::uint32_t entity) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return std::nullopt;
        QuestHost::Player who;
        who.peer = world->PeerOf(entity);
        who.character = p->selected;
        who.name = p->name;
        who.level = p->level;
        who.race = p->wearer.country == 1 ? "europe" : "china";
        who.hwan = p->hwanLevel;
        switch (game::JobType(JobState(*p))) {
        case game::JobType::Trader: who.job = "trader"; break;
        case game::JobType::Thief: who.job = "thief"; break;
        case game::JobType::Hunter: who.job = "hunter"; break;
        default: break;
        }
        who.gold = p->inventory.gold;
        // A party sharing experience and items (a `start party` quest; the party's settings mirrored on every server).
        constexpr std::uint8_t kBoth = game::kPartyExpShare | game::kPartyItemShare;
        who.sharedParty = parties && (parties->SettingsOf(entity) & kBoth) == kBoth;
        who.free = !p->dead && !(stalls && (stalls->Owner(entity) || stalls->Visiting(entity))) && !(exchanges && exchanges->Busy(entity));
        // A quest's trial (QuestDef::Trial): in berserk, a job's suit worn (the job state not 4), dead.
        who.berserk = p->berserkUntil.has_value();
        who.jobSuit = p->jobState != game::kNoJob;
        who.dead = p->dead;
        who.deeds = p->deeds;
        if (const auto at = world->Where(entity)) {
            who.region = at->position.region;
            who.position = at->position;
            who.town = at->position.layer == 0 && BanditTown(at->position.region); // the region's IsBattleField 0
        }
        // The running premium's quests (its UQL1 parts): the part's count less the finishes this package saw
        // (QSP_ALL_POTION_3's start).
        if (p->premium.Running(ServiceNow())) {
            if (const assets::ItemRow* package = ItemRowOf(p->premium.item)) {
                for (const game::PremiumPart& part : game::PremiumPartsOf(*package)) {
                    if (part.tag == "UQL1") who.premiumQuests[part.target] = std::max(0, part.count - p->premium.questUsed);
                }
            }
        }
        return who;
    }
    void QuestPremiumSpent(std::uint32_t entity, const std::string& quest) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr || !p->premium.Running(ServiceNow())) return;
        ++p->premium.questUsed;
        core::LogInfo("{}: {} finishes the premium's {} ({} times)", config.name, p->name, quest, p->premium.questUsed);
        SaveServices(*p);
    }
    int QuestMonsterLevel(const std::string& codename) override {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(std::string_view(codename)) : nullptr;
        return row != nullptr ? row->level : 0;
    }
    // --- The quests' and events' item uses (Quests::UseItem, SummonEvents) ---------------
    std::unique_ptr<SummonEvents> summons;
    bool QuestFreePlace(std::uint32_t entity) override {
        const PlayerWorld* p = PlayerByEntity(entity);
        return p != nullptr && p->inventory.FreeBagPlace() >= 0;
    }
    // The original's 0x30EC (a text key): on the client's quest notice (proto::InstanceNotice::quest).
    void QuestSay(std::uint32_t entity, const std::string& key) override {
        if (Peer* peer = peers->Find(world->PeerOf(entity))) SendMessage(*peer->connection, proto::InstanceNotice{key, false, false, true});
    }
    void SummonSay(std::uint32_t entity, const std::string& key) override { QuestSay(entity, key); }
    // The original's 0x30DA {quest, seconds}: the client's casting gauge of kind 1 (the collection's).
    void QuestSearchStarted(std::uint32_t entity, std::uint32_t quest, std::uint32_t seconds) override {
        if (Peer* peer = peers->Find(world->PeerOf(entity))) {
            SendMessage(*peer->connection, proto::ReturnStarted{0, seconds * 1000, proto::CastingKind::QuestSearch, quest});
        }
    }
    // A character's line (the original's 0x3026 kind 13: its entity and the text key; the client names it).
    void QuestSpeak(std::uint32_t entity, std::uint32_t speaker, const std::string& key) override {
        if (Peer* peer = peers->Find(world->PeerOf(entity))) SendMessage(*peer->connection, proto::ChatLine{proto::ChatKind::Speech, speaker, {}, key});
    }
    std::optional<SummonHost::User> SummonUser(std::uint32_t entity) override {
        const PlayerWorld* p = PlayerByEntity(entity);
        const auto at = world->Where(entity);
        if (p == nullptr || !at) return std::nullopt;
        SummonHost::User u;
        u.name = p->name;
        u.level = p->level;
        u.position = at->position;
        u.town = at->position.layer == 0 && BanditTown(at->position.region); // the region's IsBattleField 0
        u.arena = arenas && arenas->InMatch(entity);
        u.party = parties ? int(parties->MembersOf(entity).size()) : 0;
        return u;
    }
    std::optional<std::size_t> SummonSpawn(std::string_view codename, const proto::WorldPosition& from, const proto::WorldPosition& at, float spread,
                                           bool kept) override {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(codename) : nullptr;
        if (row == nullptr || !monsters) return std::nullopt;
        MonsterNest nest;
        nest.refObjId = row->refObjId;
        nest.position = from;
        if (spread <= 0.0f) {
            const auto ground = world->WalkEnd(from, at); // on ground the user's place reaches, as the bandits'
            if (!ground) return std::nullopt;
            nest.position = *ground;
        }
        nest.position.layer = from.layer;
        nest.spawnRadius = spread;
        nest.radius = 400.0f; // ours: an instance trigger's summons' leash
        nest.maxCount = 1;
        nest.aggressive = false; // Tab_RefTactics' btAggressType 1 for every MOB_EVE_STRONG_* (the box has no row)
        nest.sight = game::kSummonSight;
        nest.respawn = false;
        nest.temporary = !kept;
        const std::size_t n = monsters->AddNest(nest);
        if (n == SIZE_MAX) return std::nullopt;
        if (monsters->NestCounts(n).first == 0) { // no ground there: that one lost (as a bandit)
            monsters->RemoveNest(n);
            return std::nullopt;
        }
        return n;
    }
    int SummonStanding(std::size_t nest) override { return monsters ? monsters->NestCounts(nest).first : 0; }
    void SummonRemove(std::size_t nest) override {
        if (monsters) monsters->RemoveNest(nest);
    }
    // The box's cast: its swing of the skill at no target, for those around.
    void SummonCast(std::size_t nest, std::string_view skill) override {
        const assets::SkillRow* row = config.skills ? config.skills->Find(skill) : nullptr;
        if (!monsters || row == nullptr) return;
        for (const std::uint32_t box : monsters->NestMonsters(nest)) Broadcast(box, proto::EntitySwing{box, 0, row->id});
    }
    // --- The quests' other item handlers (Quests::UseItem) -----------------------------
    // The selected monster: the one the client's selection names in the use (UseItem::target; the original reads its player's
    // the selection its client told it).
    std::uint32_t questUseTarget = 0;
    std::optional<QuestHost::Selected> QuestSelected(std::uint32_t entity) override {
        const PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr || !monsters || !config.characters) return std::nullopt;
        const std::uint32_t target = questUseTarget;
        const assets::ObjectRow* row = target != 0 && monsters->Hp(target) > 0 ? config.characters->Find(monsters->RefOf(target)) : nullptr;
        const auto at = row != nullptr ? world->Where(target) : std::nullopt;
        if (!at) return std::nullopt;
        return QuestHost::Selected{target, row->codename, at->position};
    }
    std::optional<std::size_t> QuestSpawn(std::uint32_t entity, const std::string& codename, const proto::WorldPosition& at) override {
        const auto from = world->Where(entity);
        const assets::ObjectRow* row = config.characters ? config.characters->Find(std::string_view(codename)) : nullptr;
        if (!from || row == nullptr || !monsters) return std::nullopt;
        const auto ground = world->WalkEnd(from->position, at); // on ground the player's place reaches
        if (!ground) return std::nullopt;
        MonsterNest nest;
        nest.refObjId = row->refObjId;
        nest.position = *ground;
        nest.position.layer = from->position.layer;
        nest.radius = 400.0f;
        nest.maxCount = 1;
        // Tab_RefTactics of every quest spawn (MOB_QT_01_LADON 2540, _ROGUE 2570, _ROGUE_CLON 2580, _CRAB 2590, _ONG 2680,
        // MOB_QT_CH_TOMBSTONE_01 2926): btAggressType 0, nSightRange 120.
        nest.aggressive = true;
        nest.sight = 120.0f;
        nest.respawn = false;
        nest.temporary = true;
        const std::size_t n = monsters->AddNest(nest);
        if (n == SIZE_MAX) return std::nullopt;
        if (monsters->NestCounts(n).first == 0) {
            monsters->RemoveNest(n);
            return std::nullopt;
        }
        return n;
    }
    void QuestDespawn(std::size_t nest) override {
        if (monsters && monsters->NestSpec(nest) != nullptr) monsters->RemoveNest(nest);
    }
    // A caught monster: out of the world without dying (its nest refills as after a death), the quest's COS in its place,
    // kept by its owner (FollowQuestCoses). It has its row's health and numbers: the bandits go for it as for a transport
    // (MonsterTurn), and it dies of their hits.
    struct QuestCos {
        std::uint32_t owner = 0;
        std::uint32_t hp = 0;
        game::CombatStats stats;
    };
    std::unordered_map<std::uint32_t, QuestCos> questCoses; // by the COS's entity
    std::uint32_t PlaceQuestCos(std::uint32_t entity, const assets::ObjectRow& row, const proto::WorldPosition& at, float yaw) {
        World::MonsterLook look;
        look.model = row.refObjId;
        look.hp = row.maxHp;
        look.walkSpeed = row.walkSpeed;
        look.runSpeed = row.runSpeed;
        const std::uint32_t c = world->AddCos(look, entity, at, yaw);
        if (c != 0) questCoses[c] = {entity, std::uint32_t(std::max<std::int64_t>(1, row.maxHp)), game::MonsterCombatStats(row, 0)};
        return c;
    }
    std::uint32_t QuestCatch(std::uint32_t entity, std::uint32_t monster, const std::string& cos) override {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(std::string_view(cos)) : nullptr;
        const auto at = monster != 0 ? world->Where(monster) : std::nullopt;
        if (row == nullptr || !at || !monsters) return 0;
        monsters->RemoveFixed(monster);
        return PlaceQuestCos(entity, *row, at->position, at->yaw);
    }
    // Brought back beside its owner on the world entry (the quest service's spawn of kind 2 at
    // its owner's place, kept by its COS manager).
    std::uint32_t QuestResummon(std::uint32_t entity, const std::string& cos) override {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(std::string_view(cos)) : nullptr;
        const auto at = world->Where(entity);
        if (row == nullptr || !at) return 0;
        return PlaceQuestCos(entity, *row, at->position, at->yaw);
    }
    // A trapped monster out of the world without dying.
    bool QuestTakeMonster(std::uint32_t monster) override {
        if (!monsters || monsters->Hp(monster) == 0) return false;
        monsters->RemoveFixed(monster);
        return true;
    }
    // The game clock's day, the shard's (the original's from its shard manager's clock; ours from the wall clock:
    // a game second 20 ms, so a day 1,728 real seconds), the same on both game servers and over a restart.
    std::uint16_t QuestDay() override { return std::uint16_t(ServiceNow() / 1728); }
    // A monster's hit on a caught COS: its own health; at none it dies (the quest's `cos-died`).
    void HurtQuestCos(std::uint32_t cos, std::uint32_t attacker, const game::HitResult& hit) {
        const auto it = questCoses.find(cos);
        if (it == questCoses.end() || it->second.hp == 0) return;
        QuestCos& c = it->second;
        c.hp = hit.damage >= c.hp ? 0 : c.hp - hit.damage;
        world->SetHp(cos, c.hp);
        std::uint8_t flags = hit.critical ? proto::kDamageCritical : 0;
        if (hit.blocked) flags |= proto::kDamageBlocked;
        if (c.hp == 0) flags |= proto::kDamageKilled;
        Broadcast(cos, proto::EntityDamaged{cos, attacker, hit.damage, c.hp, flags});
        if (PlayerWorld* owner = PlayerByEntity(c.owner)) owner->combat.battleUntil = Clock::now() + std::chrono::seconds(20);
        if (c.hp > 0) return;
        core::LogInfo("{}: {}'s caught COS {} dies", config.name, c.owner, cos);
        questCoses.erase(it);
        world->Leave(cos);
    }
    int QuestSpawnStanding(std::size_t nest) override { return monsters && monsters->NestSpec(nest) != nullptr ? monsters->NestCounts(nest).first : 0; }
    std::uint32_t QuestSpawnEntity(std::size_t nest) override {
        if (!monsters || monsters->NestSpec(nest) == nullptr) return 0;
        const std::vector<std::uint32_t> standing = monsters->NestMonsters(nest);
        return standing.empty() ? 0 : standing.front();
    }
    // A quest's instance (Quests::OpenWorld): the party's members here, its master, a transport out, the world's layer.
    std::vector<std::uint32_t> QuestPartyHere(std::uint32_t entity) override { return parties ? parties->MemberEntities(entity) : std::vector<std::uint32_t>{}; }
    bool QuestPartyMaster(std::uint32_t entity) override { return parties && parties->Leads(entity); }
    QuestHost::KillParty QuestKillParty(std::uint32_t killer) override {
        QuestHost::KillParty out;
        constexpr std::uint8_t kBoth = game::kPartyExpShare | game::kPartyItemShare;
        if (!parties || (parties->SettingsOf(killer) & kBoth) != kBoth) return out;
        out.id = parties->PartyOf(killer);
        if (out.id == 0) return out;
        for (const std::uint32_t member : parties->MemberEntities(killer)) {
            const auto at = world->Where(member);
            out.members.push_back({member, at ? std::optional<proto::WorldPosition>(at->position) : std::nullopt});
        }
        return out;
    }
    bool QuestTransportOut(std::uint32_t entity) override { return coses && coses->TransportOut(entity); }
    std::uint32_t QuestWorldOpen(std::uint32_t entity, const std::string& worldName) override {
        const PlayerWorld* p = PlayerByEntity(entity);
        return instances && p != nullptr ? instances->OpenQuestWorld(worldName, game::QuestWorldId(worldName), p->name) : 0;
    }
    bool QuestWorldEnter(std::uint32_t entity, std::uint32_t layer) override {
        const auto at = world->Where(entity);
        return instances && at && instances->AdmitAt(entity, layer, at->position);
    }
    bool QuestWorldEnterAt(std::uint32_t entity, std::uint32_t layer, const proto::WorldPosition& at) override {
        return instances && instances->AdmitAt(entity, layer, at);
    }
    void QuestWorldLeave(std::uint32_t entity) override {
        if (instances) instances->ExitSiege(entity);
    }
    void QuestMoveInside(std::uint32_t entity, const proto::WorldPosition& to) override { InstanceTeleport(entity, to); }
    void QuestScene(std::uint32_t entity, const std::string& name) override {
        if (Peer* peer = peers->Find(world->PeerOf(entity))) SendMessage(*peer->connection, proto::QuestScene{name});
    }
    // A quest's way out: the runtime teleport (INS_QUEST_TELEPORT) in the instance's layer, facing a random way, out to `to`
    // or back where its user came in.
    std::uint32_t QuestWayOut(const proto::WorldPosition& at, const std::optional<proto::WorldPosition>& to) override {
        constexpr std::uint32_t kQuestTeleport = 19556;
        if (!instances || at.layer == 0) return 0;
        const float yaw = std::uniform_real_distribution<float>(0.0f, 6.2831853f)(fightRandom);
        return instances->AddPortal(at.layer, kQuestTeleport, at, 0, to.value_or(proto::WorldPosition{}), yaw);
    }
    bool QuestCosStanding(std::uint32_t cos) override { return questCoses.contains(cos) && world->Where(cos).has_value(); }
    void QuestCosRemove(std::uint32_t cos) override {
        if (questCoses.erase(cos) != 0) world->Leave(cos);
    }
    // A caught COS walks back to its owner beyond 60 (game::kCosFollowFar), put beside it on another layer or far away.
    void FollowQuestCoses() {
        for (const auto& [cos, held] : questCoses) {
            const std::uint32_t owner = held.owner;
            const auto o = world->Where(owner), c = world->Where(cos);
            if (!o || !c) continue;
            const proto::FlatPosition a = proto::ToFlat(o->position), b = proto::ToFlat(c->position);
            const double d = std::hypot(a.x - b.x, a.z - b.z);
            if (d <= game::kCosFollowFar && o->position.layer == c->position.layer) continue;
            if (o->position.layer != c->position.layer || d > game::kCosFollowFar * 4.0 ||
                world->MoveTo(cos, o->position, d < game::kCosFollowFar * 2.0) != World::MoveRefused::None) {
                world->Teleport(cos, o->position);
            }
        }
    }
    void QuestSend(std::uint64_t peer, proto::MessageId id, const std::vector<std::byte>& payload) override {
        if (Peer* p = peers->Find(peer)) p->connection->Send(id, payload);
    }
    std::string QuestNpcNear(std::uint32_t player, std::uint32_t npc) override {
        const auto it = npcs.find(npc);
        const auto me = world->Where(player);
        const auto there = world->Where(npc);
        if (it == npcs.end() || !me || !there) return {};
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
        return std::hypot(a.x - b.x, a.z - b.z) <= config.gateReach ? it->second->codename : std::string();
    }
    // The game's clock as the original's runs it (a game second 20 ms; game/GameClock.h), from the server's start.
    int QuestHour() override {
        const double seconds = std::chrono::duration<double>(Clock::now() - serverStarted).count();
        return int(std::fmod(seconds * 50.0 / 3600.0, 24.0));
    }
    std::uint32_t QuestItemCount(std::uint32_t entity, const std::string& item) override {
        PlayerWorld* p = PlayerByEntity(entity);
        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(std::string_view(item)) : nullptr;
        if (p == nullptr || row == nullptr) return 0;
        std::uint32_t n = 0;
        for (const auto& [place, i] : p->inventory.places)
            if (place >= game::slot::kWorn && i.refId == row->refObjId) n += i.count;
        return n;
    }
    // A new item of `row` (a stack, or one piece with its new durability).
    static game::ItemInstance NewItem(const assets::ItemRow& row, std::uint32_t count) {
        game::ItemInstance item;
        item.refId = row.refObjId;
        item.count = std::uint16_t(count);
        if (game::MaxStack(row) <= 1) item.durability = game::NewDurability(row);
        return item;
    }
    // The items into the bag, all or none (the caller restores the bag on false); the places touched.
    bool PutAll(PlayerWorld& p, const std::vector<game::QuestGoal::Items>& items, std::vector<std::uint8_t>& touched) {
        for (const game::QuestGoal::Items& want : items) {
            const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(std::string_view(want.item)) : nullptr;
            if (row == nullptr) return false;
            const int most = std::max(1, game::MaxStack(*row));
            for (std::uint32_t left = want.count; left > 0;) {
                const std::uint32_t n = std::min<std::uint32_t>(left, std::uint32_t(most));
                const int place = PlaceFor(p.inventory, *row, row->refObjId, int(n));
                if (place < 0 || !Put(p, place, NewItem(*row, n))) return false;
                touched.push_back(std::uint8_t(place));
                left -= n;
            }
        }
        return true;
    }
    void TellBag(PlayerWorld& p, std::vector<std::uint8_t> touched, const std::vector<std::int64_t>& gone) {
        LoadNewCubes(p);
        std::sort(touched.begin(), touched.end());
        touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
        // In pieces of four places (ItemMoved's most of each).
        Peer* peer = peers->Find(world->PeerOf(p.entity));
        proto::ItemMoved answer;
        for (std::size_t i = 0; i < touched.size(); ++i) {
            const std::uint8_t place = touched[i];
            if (p.inventory.At(place)) answer.changed.push_back(p.inventory.Record(place));
            else answer.emptied.push_back(place);
            if (peer != nullptr && (answer.changed.size() == 4 || answer.emptied.size() == 4 || i + 1 == touched.size())) {
                SendMessage(*peer->connection, answer);
                answer.changed.clear();
                answer.emptied.clear();
            }
        }
        QueueItemSave(p.selected, p.inventory, touched, gone);
    }
    bool QuestTakeGold(std::uint32_t entity, std::uint64_t gold) override {
        PlayerWorld* p = PlayerByEntity(entity);
        Peer* peer = p != nullptr ? peers->Find(world->PeerOf(entity)) : nullptr;
        if (p == nullptr || peer == nullptr || p->inventory.gold < gold) return false;
        p->inventory.gold -= gold;
        SendMessage(*peer->connection, proto::GoldChanged{p->inventory.gold});
        QueueGoldSave(p->selected, p->inventory.gold);
        return true;
    }
    bool QuestRoom(std::uint32_t entity, const std::vector<game::QuestGoal::Items>& items) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return false;
        const auto before = p->inventory.places;
        const auto ids = freeItemIds;
        std::vector<std::uint8_t> touched;
        const bool fits = PutAll(*p, items, touched);
        p->inventory.places = before;
        freeItemIds = ids;
        return fits;
    }
    bool QuestRoomAfter(std::uint32_t entity, const std::vector<game::QuestGoal::Items>& items, const std::vector<game::QuestGoal::Items>& taken) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr || !config.rules->items) return false;
        const auto before = p->inventory.places;
        const auto ids = freeItemIds;
        // The taken items out of the bag first (the pay's order), then the reward in; all put back as it was.
        for (const game::QuestGoal::Items& want : taken) {
            const assets::ItemRow* row = config.rules->items->Find(std::string_view(want.item));
            if (row == nullptr) continue;
            std::uint32_t left = want.count;
            for (auto it = p->inventory.places.begin(); it != p->inventory.places.end() && left > 0;) {
                if (it->first < game::slot::kWorn || it->second.refId != row->refObjId) {
                    ++it;
                    continue;
                }
                const std::uint32_t n = std::min<std::uint32_t>(left, it->second.count);
                left -= n;
                if (n == it->second.count) {
                    it = p->inventory.places.erase(it);
                } else {
                    it->second.count = std::uint16_t(it->second.count - n);
                    ++it;
                }
            }
        }
        std::vector<std::uint8_t> touched;
        const bool fits = PutAll(*p, items, touched);
        p->inventory.places = before;
        freeItemIds = ids;
        return fits;
    }
    bool QuestGive(std::uint32_t entity, const std::vector<game::QuestGoal::Items>& items) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return false;
        const auto before = p->inventory.places;
        const auto ids = freeItemIds;
        std::vector<std::uint8_t> touched;
        if (!PutAll(*p, items, touched)) {
            p->inventory.places = before;
            freeItemIds = ids;
            return false;
        }
        TellBag(*p, std::move(touched), {});
        return true;
    }
    // The beta quests' weapons (QuestReward::bound): given as QuestGive, the reward's items at the places touched marked
    // bound (saved with them: sr_bound_items).
    bool QuestGiveBound(std::uint32_t entity, const std::vector<game::QuestGoal::Items>& items) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr || !config.rules->items) return false;
        const auto before = p->inventory.places;
        const auto ids = freeItemIds;
        std::vector<std::uint8_t> touched;
        if (!PutAll(*p, items, touched)) {
            p->inventory.places = before;
            freeItemIds = ids;
            return false;
        }
        std::set<std::uint32_t> refs;
        for (const game::QuestGoal::Items& i : items)
            if (const assets::ItemRow* row = config.rules->items->Find(std::string_view(i.item))) refs.insert(row->refObjId);
        for (std::uint8_t place : touched)
            if (auto it = p->inventory.places.find(place); it != p->inventory.places.end() && refs.contains(it->second.refId)) it->second.bound = true;
        TellBag(*p, std::move(touched), {});
        return true;
    }
    bool QuestTake(std::uint32_t entity, const std::vector<game::QuestGoal::Items>& items) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr || !config.rules->items) return false;
        for (const game::QuestGoal::Items& want : items)
            if (QuestItemCount(entity, want.item) < want.count) return false;
        std::vector<std::uint8_t> touched;
        std::vector<std::int64_t> gone;
        for (const game::QuestGoal::Items& want : items) {
            const assets::ItemRow* row = config.rules->items->Find(std::string_view(want.item));
            std::uint32_t left = want.count;
            for (auto it = p->inventory.places.begin(); it != p->inventory.places.end() && left > 0;) {
                if (it->first < game::slot::kWorn || it->second.refId != row->refObjId) {
                    ++it;
                    continue;
                }
                const std::uint32_t n = std::min<std::uint32_t>(left, it->second.count);
                left -= n;
                touched.push_back(it->first);
                if (n == it->second.count) {
                    gone.push_back(it->second.serial);
                    it = p->inventory.places.erase(it);
                } else {
                    it->second.count = std::uint16_t(it->second.count - n);
                    ++it;
                }
            }
        }
        TellBag(*p, std::move(touched), gone);
        return true;
    }
    void QuestReward(std::uint32_t entity, const QuestHost::Reward& r) override {
        PlayerWorld* p = PlayerByEntity(entity);
        Peer* peer = peers->Find(world->PeerOf(entity));
        if (p == nullptr || peer == nullptr) return;
        // The temple quests' points: the reputation of the player's alliance at that temple (the
        // game server's PostTempleReputationSetReq to the shard manager, SM). The quests'
        // agents stand in the temples' area, on the server running the temples.
        for (const game::QuestReward::Points& points : r.points) {
            const int temple = points.name == "TEMPLE_ANUBIS" ? 0 : points.name == "TEMPLE_HAROERIS" ? 1 : -1;
            const game::JobType job = game::JobType(JobState(*p));
            const int alliance = job == game::JobType::Thief ? 2 : (job == game::JobType::Trader || job == game::JobType::Hunter) ? 1 : 0;
            if (temple < 0 || alliance == 0) continue;
            if (temples) temples->AddReputation(temple, alliance, int(points.amount));
            else core::LogWarning("{}: {}'s {} {} points: no temples on this server", config.name, p->name, points.amount, points.name);
            core::LogInfo("{}: {} gives its alliance {} {} points", config.name, p->name, points.amount, points.name);
        }
        if (r.gold != 0) {
            p->inventory.gold += r.gold;
            SendMessage(*peer->connection, proto::GoldChanged{p->inventory.gold});
            QueueGoldSave(p->selected, p->inventory.gold);
        }
        if (r.inventory != 0) {
            p->inventory.size = std::uint8_t(std::min<int>(game::Inventory::kMaxSize, p->inventory.size + int(r.inventory)));
            SendMessage(*peer->connection, proto::InventoryGrown{p->inventory.size});
            database.Submit([character = p->selected, size = p->inventory.size](Database& db) { SaveInventorySize(db, character, size); });
        }
        if (r.hwan > std::uint32_t(p->hwanLevel)) {
            p->hwanLevel = int(r.hwan);
            database.Submit([character = p->selected, level = p->hwanLevel](Database& db) { SaveHwanLevel(db, character, level); });
        }
        if (r.sp != 0) {
            p->skillPoints += r.sp;
            p->progressDirty = true;
        }
        if (r.exp != 0 || r.skillExp != 0) {
            const std::uint32_t skill = std::uint32_t(std::min<std::uint64_t>(r.skillExp, 0xFFFFFFFFu));
            SendMessage(*peer->connection, proto::ExperienceGained{0, r.exp, skill});
            GainExperience(*peer, *p, r.exp, skill);
        }
        SendProgress(*peer, *p);
    }
    void QuestDrop(std::uint32_t entity, std::uint32_t monster, const std::string& item) override {
        PlayerWorld* p = PlayerByEntity(entity);
        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(std::string_view(item)) : nullptr;
        const auto where = world->Where(monster);
        if (p == nullptr || row == nullptr || !where) return;
        std::uniform_real_distribution<float> turn(0.0f, 6.2831853f), spread(-6.0f, 6.0f);
        const proto::FlatPosition c = proto::ToFlat(where->position);
        const proto::WorldPosition there = proto::FromFlat({c.x + spread(groundRandom), c.z + spread(groundRandom)}, where->position.y);
        const std::uint32_t id = world->AddGroundItem(row->refObjId, there, turn(groundRandom), 1, 0, 0, OwnedItemName(*row, p->name), p->entity);
        const Clock::duration life = GroundLife(game::ItemInstance{}, row->refObjId);
        GroundItem g{NewItem(*row, 1), 0, p->selected, Clock::now() + life, Clock::now() + life, true};
        groundExpiry.push({g.expires, id});
        ground[id] = std::move(g);
    }
    void QuestSave(std::int64_t character, const QuestRecord& r) override {
        const auto [d1, d2] = PackQuestProgress(r.progress);
        const CharacterRow::Quest row{r.quest, std::uint8_t(r.status), r.times, r.started, r.ended, d1, d2};
        database.Submit([character, row](Database& db) { SaveCharQuest(db, character, row); });
    }
    bool QuestThemeDone(std::uint32_t entity, const std::string& theme) override { return instances && instances->ThemeComplete(entity, theme); }

    // --- The Forgotten World (Instances.h) --------------------------------------
    std::unique_ptr<Instances> instances;
    // --- Capture the Flag and Event So-Ok (FlagWorld.h) ----------------
    std::unique_ptr<FlagWorlds> flagWorlds;
    // --- The job temples (Temples.h): run by the server holding their area (dungeon region 0x8010).
    std::unique_ptr<Temples> temples;
    static bool InTempleRegion(std::uint16_t region) { return region >= 0x800A && region <= 0x8010; }
    std::optional<TempleHost::Facts> TempleFacts(std::uint32_t entity) override {
        const PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return std::nullopt;
        TempleHost::Facts f;
        f.level = p->level;
        f.jobState = p->jobState;
        f.freePvp = p->freePvp != 0;
        f.riding = p->riding;
        f.dead = p->dead;
        f.league = std::uint8_t(p->job.type);
        return f;
    }
    std::pair<int, int> TempleAllianceCounts() override {
        int traderHunter = 0, thief = 0;
        for (const auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            const auto at = world->Where(p.entity);
            if (!at || !InTempleRegion(at->position.region)) continue;
            if (p.jobState == 1 || p.jobState == 3) ++traderHunter;
            else if (p.jobState == 2) ++thief;
        }
        return {traderHunter, thief};
    }
    void TempleTell(proto::MessageId id, const std::vector<std::byte>& payload) override {
        for (const auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            const auto at = world->Where(p.entity);
            if (!at || !InTempleRegion(at->position.region)) continue;
            if (Peer* peer = peers->Find(peerId)) peer->connection->Send(id, payload);
        }
    }
    void TempleSaveReputation(const std::array<int, 4>& r) override {
        // `_UpdateAssociationReputation`'s four rows (ours: the same UPDATEs).
        database.Submit([r, name = config.name](Database& db) {
            static constexpr std::array<std::pair<const char*, const char*>, 4> kRows{{{"TEMPLE_ANUBIS", "TraderHunter"},
                                                                                      {"TEMPLE_ANUBIS", "Robber"},
                                                                                      {"TEMPLE_HAROERIS", "TraderHunter"},
                                                                                      {"TEMPLE_HAROERIS", "Robber"}}};
            for (std::size_t i = 0; i < kRows.size(); ++i) {
                const QueryResult q = db.Query("UPDATE _associationreputation SET reputation = $1 WHERE associationtypename = $2 AND associationcodename = $3",
                                               {std::to_string(r[i]), kRows[i].first, kRows[i].second});
                if (!q.Ok()) core::LogWarning("{}: the temples' reputation: {}", name, q.Error());
            }
        });
    }
    // The reputation board as every server of the shard shows it: Temples::Board on the temples'
    // server, shared on sr_temples_<shard> when it changes and every minute (B, then each row's numbers), kept by the others.
    std::vector<proto::TempleReputationRow> templeBoard;
    std::string templeChannel, templeBoardSent;
    Clock::time_point nextTempleTick, templeBoardSentAt;
    static std::string EncodeTempleBoard(const std::vector<proto::TempleReputationRow>& rows) {
        std::string text = "B";
        for (const proto::TempleReputationRow& r : rows) {
            text += std::format("\t{},{},{},{},{}", unsigned(r.allowed),r.nextOpening, r.since, r.traderHunter, r.thief);
        }
        return text;
    }
    void TempleBusMessage(const std::string& payload) {
        if (temples || payload.empty() || payload[0] != 'B') return; // the temples' server keeps its own
        std::vector<proto::TempleReputationRow> rows;
        for (std::size_t at = payload.find('\t'); at != std::string::npos; at = payload.find('\t', at + 1)) {
            proto::TempleReputationRow r;
            unsigned allowed = 0;
            long long next = 0, since = 0;
            unsigned th = 0, thief = 0;
            if (std::sscanf(payload.c_str() + at + 1, "%u,%lld,%lld,%u,%u", &allowed, &next, &since, &th, &thief) != 5) return;
            r.allowed = std::uint8_t(allowed);
            r.nextOpening = next;
            r.since = since;
            r.traderHunter = th;
            r.thief = thief;
            rows.push_back(r);
        }
        templeBoard = std::move(rows);
    }
    // The board's icon (proto::TempleIcon; the original's 0x34E1 at the world's entry): for a player of level 105
    // or more in the temples' area or a temple, or with a job quest under way. Ours decides again when the
    // player passes into or out of the temples (the original's every arrival is an entry into the world).
    void TempleIconCheck(std::uint64_t peerId, PlayerWorld& player) {
        if (player.entity == 0) return;
        const auto at = world->Where(player.entity);
        if (!at) return;
        const bool inTemples = InTempleRegion(at->position.region);
        if (player.templeIcon >= 0 && inTemples == player.inTemples) return;
        player.inTemples = inTemples;
        const bool show = player.level >= 105 && (inTemples || (quests && quests->JobQuestUnderWay(player.entity)));
        if (show == (player.templeIcon == 1)) return;
        player.templeIcon = show ? 1 : 0;
        if (Peer* peer = peers->Find(peerId)) SendMessage(*peer->connection, proto::TempleIcon{show});
    }
    // Every second: the board kept (and shared, on the temples' server); each player's icon.
    void TempleTick(Clock::time_point now) {
        if (now < nextTempleTick) return;
        nextTempleTick = now + std::chrono::seconds(1);
        if (temples) {
            templeBoard = temples->Board();
            std::string text = EncodeTempleBoard(templeBoard);
            if (!templeChannel.empty() && (text != templeBoardSent || now - templeBoardSentAt >= std::chrono::minutes(1))) {
                templeBoardSent = text;
                templeBoardSentAt = now;
                BusSend(templeChannel, std::move(text));
            }
        }
        for (auto& [peerId, p] : players) TempleIconCheck(peerId, p);
    }
    // --- The Roc (Roc.h): run by the server holding the Peak; its shard-wide notices on sr_roc_<shard>.
    std::unique_ptr<Roc> roc;
    std::string rocChannel;
    // --- The world's game clock (the original's): day 0 at 00:00 from the start, its hour set by a GM (/gametime, every game
    // server of the shard: sr_clock_<shard>); each player told it about every 10 minutes (the original's round of its sub-regions
    // over 600 s, each player at its place in it), every player at once when a GM sets it. The client restarts its clock from it.
    proto::WorldClock worldClock{};
    std::string clockChannel;
    void TellClock(std::uint64_t peerId) {
        if (Peer* peer = peers->Find(peerId)) peer->connection->Send(proto::WorldClock::kId, proto::Encode(worldClock));
    }
    void TickClock(Clock::time_point now) {
        for (auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            if (p.clockNext == Clock::time_point{}) {
                p.clockNext = now + std::chrono::seconds(p.entity % 600u);
                continue;
            }
            if (now < p.clockNext) continue;
            p.clockNext = now + std::chrono::seconds(600);
            TellClock(peerId);
        }
    }
    // A GM's hour (the original keeps the minute and the day): every player here told at once.
    void SetGameHour(int hour) {
        worldClock.hour = std::uint8_t(std::clamp(hour, 0, 23));
        core::LogInfo("{}: the game time set to {}:{:02}", config.name, worldClock.hour, worldClock.minute);
        for (auto& [peerId, p] : players) {
            if (p.entity != 0) TellClock(peerId);
        }
    }
    // A counted deed of a character (sr_char_records; the beta quests' `start record`, game/BetaQuests.h): saved, and the
    // count kept on the player raised when it plays here.
    void AddDeed(std::int64_t character, std::string name) {
        if (character == 0) return;
        for (auto& [peer, p] : players) {
            if (p.selected != character) continue;
            ++p.deeds[name];
            break;
        }
        database.Submit([character, name](Database& db) {
            std::string error;
            if (!AddCharRecord(db, character, name, 1, error)) core::LogWarning("deeds: {} of {}: {}", name, character, error);
        });
    }
    void FlagDeed(std::int64_t character, std::string_view name) override { AddDeed(character, std::string(name)); }
    void FortressDeed(std::int64_t character, std::string_view name) override { AddDeed(character, std::string(name)); }
    void RocDeed(std::uint32_t entity, std::string_view name) override {
        if (PlayerWorld* p = PlayerByEntity(entity)) AddDeed(p->selected, std::string(name));
    }
    void RocShardNotice(const std::string& key) override {
        database.Submit([channel = rocChannel, key](Database& db) { Notify(db, channel, "N	" + key); });
    }
    void RocSend(std::uint32_t entity, proto::MessageId id, const std::vector<std::byte>& payload) override {
        if (Peer* peer = peers->Find(world->PeerOf(entity))) peer->connection->Send(id, payload);
    }
    std::optional<proto::WorldPosition> RocPoint(std::uint32_t point) override {
        const assets::TeleportPoint* p = config.teleports ? config.teleports->Point(point) : nullptr;
        if (p == nullptr || !p->HasArrival()) return std::nullopt;
        return proto::WorldPosition{p->region, p->x, p->y, p->z};
    }
    void RocDev(const std::string& what) {
        if (!roc) return;
        if (what == "OPEN") roc->Open(Clock::now());
        else if (what == "SHUT") roc->Shut();
        else if (what == "END") roc->End();
        else if (what == "BREAK") roc->Break(Clock::now());
    }
    // A notice of the shard's Roc (N: a text key) to this server's players; a developer's switch (C) for the Peak's server.
    void RocBusMessage(const std::string& payload) {
        if (payload.size() >= 3 && payload[0] == 'C' && payload[1] == '	') return RocDev(payload.substr(2));
        if (payload.size() < 3 || payload[0] != 'N' || payload[1] != '	') return;
        NoticeToPlayers(payload.substr(2));
    }
    // A text key's notice to every player of this server: a chat line and the warning banner.
    void NoticeToPlayers(const std::string& key) {
        proto::InstanceNotice n{key};
        n.system = true;
        n.warning = true;
        const std::vector<std::byte> bytes = proto::Encode(n);
        for (const auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            if (Peer* peer = peers->Find(peerId)) peer->connection->Send(proto::InstanceNotice::kId, bytes);
        }
    }
    // --- The Qin-Shi tomb's snake gate and upper floors (QinTomb.h): run by the server holding the 4th floor; its
    // shard-wide notices (the original's 0x300C 0x0C10 / 0x0C11) and a developer's switches on sr_qin_<shard>.
    std::unique_ptr<QinTomb> qinTomb;
    std::string qinChannel;
    void QinShardNotice(const std::string& key) override {
        database.Submit([channel = qinChannel, key](Database& db) { Notify(db, channel, "N	" + key); });
    }
    std::optional<proto::WorldPosition> QinPoint(std::uint32_t point) override { return RocPoint(point); }
    void QinDev(const std::string& what) {
        if (!qinTomb) return;
        if (what == "OPEN") qinTomb->Open(Clock::now());
        else if (what == "SHUT") qinTomb->Shut(Clock::now());
        else if (what == "END") qinTomb->End();
    }
    void QinBusMessage(const std::string& payload) {
        if (payload.size() >= 3 && payload[0] == 'C' && payload[1] == '	') return QinDev(payload.substr(2));
        if (payload.size() >= 3 && payload[0] == 'N' && payload[1] == '	') NoticeToPlayers(payload.substr(2));
    }
    // --- The Battle Arena (Arena.h) -------------------------------------------------------
    std::unique_ptr<Arenas> arenas;
    std::string arenaChannel;
    ArenaHost::Facts ArenaFactsOf(std::uint32_t entity) override {
        ArenaHost::Facts f;
        const PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return f;
        if (parties) {
            f.party = parties->PartyOf(entity);
            f.partyLeader = f.party != 0 && parties->Leads(entity);
            for (const Parties::MemberView& m : parties->MembersOf(entity)) f.partyMembers.push_back(m.character);
        }
        if (guilds) {
            if (const auto g = guilds->FactsOf(p->selected)) {
                f.guild = g->id;
                f.guildMaster = g->master;
            }
        }
        f.jobState = ActivityOf(*p).jobSuit ? p->jobState : std::uint8_t(4);
        f.riding = p->riding;
        return f;
    }
    void ArenaSkillExperience(std::uint32_t entity, std::uint64_t sp) override {
        PlayerWorld* p = PlayerByEntity(entity);
        Peer* peer = peers->Find(world->PeerOf(entity));
        if (p != nullptr && peer != nullptr) GainExperience(*peer, *p, 0, std::uint32_t(std::min<std::uint64_t>(sp, 0xFFFFFFFFu)));
    }
    std::uint64_t ArenaLevelExperience(int level) override {
        const std::size_t l = std::size_t(std::max(1, level));
        return l < config.monsterExperience.size() ? config.monsterExperience[l] : 0u;
    }
    void ArenaBuff(std::uint32_t entity, bool on) override {
        PlayerWorld* player = PlayerByEntity(entity);
        const assets::SkillRow* skill = config.skills ? config.skills->Find(game::arena::kCarrierSkill) : nullptr;
        if (player == nullptr || skill == nullptr || !skillUnit) return;
        SkillCaster caster = CasterOf(*player);
        if (on) skillUnit->AddBuff(caster, *skill, std::uint32_t(game::flag::kCarryMs), Clock::now());
        else skillUnit->EndBuffsOf(caster, {skill->id});
    }
    std::uint16_t ArenaOwner(std::uint16_t region) override { return OwnerOf(region); }
    void ArenaReturnPoint(std::uint32_t entity) override {
        PlayerWorld* player = PlayerByEntity(entity);
        if (player == nullptr) return;
        if (const assets::TeleportPoint* to = ReturnArrival(*player); to != nullptr && CanTravelTo(to->region)) {
            TravelTo(world->PeerOf(entity), *player, {to->region, to->x, to->y, to->z});
        }
    }
    void ArenaPublish(std::string text) override {
        if (!arenaChannel.empty()) BusSend(arenaChannel, std::move(text));
    }
    std::string eventChannel;
    bool flagReviving = false; // a camp's revival under way (Resurrect's own rules stand aside)
    std::optional<FlagWorldHost::Player> FlagPlayer(std::uint32_t entity) override {
        const auto it = players.find(world->PeerOf(entity));
        if (it == players.end() || it->second.entity != entity) return std::nullopt;
        const PlayerWorld& p = it->second;
        FlagWorldHost::Player out;
        out.peer = it->first;
        out.character = p.selected;
        out.name = p.name;
        out.level = p.level;
        out.dead = p.dead;
        if (Peer* peer = peers->Find(it->first)) out.account = std::int64_t(peer->account);
        const auto place = world->Where(entity);
        if (place) out.at = place->position;
        game::FlagEntrant& e = out.entrant;
        e.maxLevel = std::max(p.maxLevel, p.level);
        e.alive = !p.dead;
        e.interacting = stalls && (stalls->Owner(entity) || stalls->Visiting(entity));
        e.jobSuit = ActivityOf(p).jobSuit;
        e.freePvp = p.freePvp != 0 || p.freePvpAt.has_value();
        e.guildWar = guilds && guilds->AtAnyWar(p.selected);
        e.inTown = place && place->position.layer == 0 &&
                   (BanditTown(place->position.region) || (towns && towns->TownAt(place->position.region, place->position.x, place->position.z) >= 0));
        return out;
    }
    std::uint32_t FlagEntityOf(std::int64_t character) override { return InstanceEntityOf(std::uint32_t(character)); }
    std::uint16_t FlagServerId() override { return config.id; }
    std::uint16_t FlagHostServer() override { return OwnerOf(game::flag::kRegion); }
    void FlagPublish(std::string text) override {
        if (!eventChannel.empty()) BusSend(eventChannel, std::move(text));
    }
    void FlagSend(std::uint32_t entity, proto::MessageId id, const std::vector<std::byte>& payload) override {
        if (const std::uint64_t peer = world->PeerOf(entity)) QuestSend(peer, id, payload);
    }
    void FlagSendAll(proto::MessageId id, const std::vector<std::byte>& payload) override {
        for (const auto& [peerId, p] : players)
            if (p.entity != 0) QuestSend(peerId, id, payload);
    }
    void FlagSendAround(std::uint32_t entity, proto::MessageId id, const std::vector<std::byte>& payload) override {
        world->ForEachObserver(entity, [&](std::uint64_t to) {
            if (Peer* p = peers->Find(to)) p->connection->Send(id, payload);
        });
    }
    void FlagLeaveParty(std::uint32_t entity) override {
        if (parties && parties->PartyOf(entity) != 0) parties->Handle(entity, proto::PartyLeave::kId, proto::Encode(proto::PartyLeave{}), Clock::now());
    }
    void FlagSetTeam(std::uint32_t entity, std::uint8_t team) override {
        const proto::EntityFlagTeam m{entity, team};
        world->SetExtra(entity, proto::EntityFlagTeam::kId, team != 0xFF ? proto::Encode(m) : std::vector<std::byte>{});
        FlagSendAround(entity, proto::EntityFlagTeam::kId, proto::Encode(m));
    }
    void FlagTravel(std::uint32_t entity, const proto::WorldPosition& to) override {
        PlayerWorld* player = PlayerByEntity(entity);
        if (player == nullptr) return;
        if (to.layer != 0) {
            if (exchanges) exchanges->End(entity);
            if (stalls) stalls->Leave(entity);
            if (coses) coses->DismissAll(entity, true);
            world->Teleport(entity, to); // a match is always this server's (the host's)
            return;
        }
        TravelTo(world->PeerOf(entity), *player, to);
    }
    bool FlagGive(std::uint32_t entity, std::string_view item, std::uint32_t count) override {
        return QuestGive(entity, {{std::string(item), count}});
    }
    std::uint32_t FlagCount(std::uint32_t entity, std::string_view item) override { return QuestItemCount(entity, std::string(item)); }
    bool FlagTake(std::uint32_t entity, std::string_view item, std::uint32_t count) override { return QuestTake(entity, {{std::string(item), count}}); }
    bool FlagRoom(std::uint32_t entity) override { return MallFreePlaces(entity) > 0; }
    bool FlagUseUp(std::uint32_t entity, std::uint8_t slot) override { return InstanceTakeOne(entity, slot); }
    // In their order (a barrier each: the pool runs plain tasks on any free thread, so a registration, its cancel and a
    // registration again could commit out of order and leave the shard's queue without a player the server holds
    // registered). The event's writes are few.
    void FlagSubmit(std::function<void(Database&)> task, std::function<void()> done) override {
        database.SubmitBarrier([this, task = std::move(task), done = std::move(done)](Database& db) {
            task(db);
            if (done) completions->Post(done);
        });
    }
    void FlagRevive(std::uint32_t entity, const proto::WorldPosition& at) override {
        PlayerWorld* player = PlayerByEntity(entity);
        Peer* peer = peers->Find(world->PeerOf(entity));
        if (player == nullptr || peer == nullptr || !player->dead) return;
        flagReviving = true;
        Resurrect(*peer, *player, 2);
        flagReviving = false;
        core::LogInfo("{}: {} rises at the flag world's camp", config.name, player->name);
        if (const auto where = world->Where(entity); !where || where->position.layer != at.layer || where->position.region != at.region ||
                                                    std::abs(where->position.x - at.x) > 1.0f || std::abs(where->position.z - at.z) > 1.0f) {
            if (at.layer != 0) world->Teleport(entity, at);
        }
    }
    void FlagBuff(std::uint32_t entity, bool on) override {
        PlayerWorld* player = PlayerByEntity(entity);
        const assets::SkillRow* skill = config.skills ? config.skills->Find(game::flag::kCarrierSkill) : nullptr;
        if (player == nullptr || skill == nullptr || !skillUnit) return;
        SkillCaster caster = CasterOf(*player);
        if (on) skillUnit->AddBuff(caster, *skill, std::uint32_t(game::flag::kCarryMs), Clock::now());
        else skillUnit->EndBuffsOf(caster, {skill->id});
    }
    void FlagHold(std::uint32_t entity) override {
        PlayerWorld* player = PlayerByEntity(entity);
        if (player == nullptr) return;
        player->combat.StopFighting();
        player->pickTarget = 0;
        if (const auto place = world->Where(entity); place && world->Moving(entity)) world->MoveTo(entity, place->position);
    }
    void FlagUntouchable(std::uint32_t entity, std::chrono::milliseconds time) override {
        if (PlayerWorld* player = PlayerByEntity(entity)) player->untouchableUntil = Clock::now() + time;
    }
    void FlagWalkTo(std::uint32_t entity, const proto::WorldPosition& to) override { world->MoveTo(entity, to); }
    bool FlagWayClear(const proto::WorldPosition& from, const proto::WorldPosition& to) override {
        if (!world->HasNavigation()) return true;
        const auto end = world->WalkEnd(from, to);
        if (!end) return false;
        const proto::FlatPosition a = proto::ToFlat(*end), b = proto::ToFlat(to);
        return std::hypot(a.x - b.x, a.z - b.z) < 5.0;
    }
    std::int64_t FlagUnixNow() override { return WallClock(); }
    // The reverse return scroll's map points (_RefOptionalTeleport in service).
    std::unordered_map<std::uint32_t, OptionalTeleportRow> optionalTeleports;
    // The jobs' constants from the reference tables: the trade scale, the bandits' factor, GoldMin,
    // the job levels' experience.
    game::JobRules jobRules;
    // Where a player stands now as a place kept for it (none in an instance: the original keeps neither place there).
    std::optional<CharacterRow::Spot> KeepableSpot(const PlayerWorld& player) const {
        const auto place = world->Where(player.entity);
        if (!place || place->position.layer != 0 || (instances && instances->LayerOf(player.entity) != 0)) return std::nullopt;
        return CharacterRow::Spot{place->position.region, place->position.x, place->position.y, place->position.z};
    }
    std::optional<InstanceHost::Player> InstancePlayer(std::uint32_t entity) override {
        PlayerWorld* p = PlayerByEntity(entity);
        const auto place = world->Where(entity);
        if (p == nullptr) return std::nullopt;
        InstanceHost::Player who;
        who.peer = world->PeerOf(entity);
        who.character = p->selected;
        who.name = p->name;
        who.level = p->level;
        who.party = parties ? parties->PartyOf(entity) : 0;
        who.partyMaster = parties && parties->Leads(entity);
        who.inTown = place && place->position.layer == 0 && towns && towns->TownAt(place->position.region, place->position.x, place->position.z) >= 0;
        who.dead = p->dead;
        who.riding = p->riding;
        who.inBattle = Clock::now() < p->combat.battleUntil;
        if (const auto a = accountOf.find(who.peer); a != accountOf.end()) who.gm = a->second.gm;
        who.transport = CarriesGoods(*p);
        who.jobSuit = ActivityOf(*p).jobSuit;
        who.dealing = (exchanges && exchanges->Busy(entity)) || (stalls && (stalls->Owner(entity) || stalls->Visiting(entity)));
        who.atWar = guilds && guilds->AtAnyWar(p->selected);
        who.freePvp = p->freePvp != 0;
        return who;
    }
    std::vector<InstanceHost::Member> InstancePartyMembers(std::uint32_t entity) override {
        std::vector<InstanceHost::Member> out;
        if (!parties) return out;
        for (const Parties::MemberView& m : parties->MembersOf(entity)) out.push_back({m.character, m.name, m.level, m.server, m.entity});
        return out;
    }
    std::uint32_t InstanceEntityOf(std::uint32_t character) override {
        for (const auto& [peerId, player] : players) {
            if (player.entity != 0 && std::uint32_t(player.selected) == character) return player.entity;
        }
        return 0;
    }
    std::uint16_t InstanceServerId() override { return config.id; }
    void InstancePublish(std::string text) override {
        if (!instanceChannel.empty()) BusSend(instanceChannel, std::move(text));
    }
    std::string instanceChannel;
    void InstanceSend(std::uint64_t peer, proto::MessageId id, const std::vector<std::byte>& payload) override { QuestSend(peer, id, payload); }
    void InstanceTeleport(std::uint32_t entity, const proto::WorldPosition& to) override {
        PlayerWorld* player = PlayerByEntity(entity);
        if (player == nullptr) return;
        if (exchanges) exchanges->End(entity);
        if (alchemy) alchemy->End(entity);
        if (stalls) stalls->Leave(entity);
        if (coses) coses->DismissAll(entity, true);
        if (to.layer != 0) {
            world->Teleport(entity, to); // an instance is always this server's
        } else {
            TravelTo(world->PeerOf(entity), *player, to); // out: back where it came from, another server's town too (a summoned member)
        }
    }
    std::uint32_t InstanceItemAt(std::uint32_t entity, std::uint8_t slot) override {
        PlayerWorld* p = PlayerByEntity(entity);
        const game::ItemInstance* item = p != nullptr && slot >= game::slot::kWorn ? p->inventory.At(slot) : nullptr;
        return item != nullptr ? item->refId : 0;
    }
    bool InstanceTakeOne(std::uint32_t entity, std::uint8_t slot) override {
        PlayerWorld* p = PlayerByEntity(entity);
        Peer* peer = peers->Find(world->PeerOf(entity));
        if (p == nullptr || peer == nullptr || p->inventory.At(slot) == nullptr) return false;
        TakeOne(*peer, *p, slot);
        return true;
    }
    void InstanceSaveEntry(std::int64_t character, const InstanceEntry& entry) override {
        CharacterWrite(character, [character, entry](Database& db) {
            std::string error;
            if (!SaveInstanceEntry(db, character, entry, error)) core::LogWarning("instances: the entry of {}: {}", character, error);
        });
    }
    void InstanceSaveBook(std::int64_t character, const BookEntry& entry) override {
        database.Submit([character, entry](Database& db) {
            std::string error;
            if (!AddCollectionBookEntry(db, character, entry, error)) core::LogWarning("instances: the book of {}: {}", character, error);
        });
    }
    void InstanceSaveWait(std::int64_t character, const ItemWait& wait) override {
        database.Submit([character, wait](Database& db) {
            std::string error;
            if (!SaveItemWait(db, character, wait, error)) core::LogWarning("instances: the holes' wait of {}: {}", character, error);
        });
    }
    void InstanceBookChanged(std::uint32_t entity) override {
        if (quests) quests->BagChanged(entity); // the collect goals recounted
    }
    int InstanceMonsterLevel(std::uint32_t refObjId) override {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(refObjId) : nullptr;
        return row != nullptr ? row->level : 0;
    }
    // Where a player is kept: in an instance, the place it entered from.
    proto::WorldPosition KeptPlace(std::uint32_t entity, const proto::WorldPosition& at) const {
        if (at.layer == 0) return at;
        if (flagWorlds) {
            if (const auto home = flagWorlds->ReturnPlace(entity)) return *home; // a Capture the Flag match: home
        }
        if (arenas) {
            if (const auto home = arenas->ReturnPlace(entity)) return *home; // a Battle Arena match: home
        }
        if (!instances) return at;
        if (const auto back = instances->ReturnPlace(entity)) return *back;
        return at;
    }
    std::shared_ptr<const assets::RegionAreaTable> towns;
    std::shared_ptr<const assets::StallNetworkData> stallTables;
    std::string stallChannel;
    std::optional<StallHost::Player> StallPlayer(std::uint32_t entity) override {
        const auto it = players.find(world->PeerOf(entity));
        const auto place = world->Where(entity);
        if (it == players.end() || it->second.entity != entity || !place) return std::nullopt;
        PlayerWorld& p = it->second;
        StallHost::Player out;
        out.peer = it->first;
        out.character = p.selected;
        out.name = p.name;
        out.alive = !p.dead;
        out.free = !p.moving && !p.loggingOut && !(exchanges && exchanges->Busy(entity)) && Clock::now() >= p.combat.castUntil;
        out.town = towns ? towns->TownAt(place->position.region, place->position.x, place->position.z) : -1;
        out.position = place->position;
        out.inventory = &p.inventory;
        out.skin = StallSkinOf(p);
        out.riding = p.riding;
        out.murderer = p.pvpState == std::uint8_t(game::PvpState::Murderer);
        out.jobSuit = ActivityOf(p).jobSuit;
        out.partyAsking = parties && parties->Proposing(entity);
        return out;
    }
    void StallSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override { GuildSend(peerId, id, payload); }
    void StallShow(const proto::EntityStall& stall) override {
        world->SetExtra(stall.entity, proto::EntityStall::kId, stall.name.empty() ? std::vector<std::byte>{} : proto::Encode(stall));
        Broadcast(stall.entity, stall);
    }
    const assets::ItemTable* StallItems() const override { return config.rules->items.get(); }
    const assets::StallNetworkData* StallNetworkTables() const override { return towns ? stallTables.get() : nullptr; }
    // The client's names (textdataname.txt's catalog: items, monsters, ...) by their SN_ key, read at the start (Start).
    std::unordered_map<std::string, std::string> textNames;
    void LoadTextNames() {
        std::string why;
        if (config.files == nullptr || !assets::LoadTextCatalog(*config.files, "server_dep\\silkroad\\textdata\\textdataname.txt", textNames, why)) {
            core::LogWarning("{}: the client's names are not read: {}", config.name, why);
        }
    }
    // A name by its SN_ key; empty when unknown.
    std::string TextName(const std::string& key) const {
        const auto it = textNames.find(key);
        return it != textNames.end() ? it->second : std::string();
    }
    // An item's name in lower case (the stall network's search), its codename when it has none.
    std::string StallItemName(std::uint32_t refId) const override {
        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(refId) : nullptr;
        if (row == nullptr) return {};
        std::string name = TextName(row->nameKey);
        if (name.empty()) name = row->codename;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return name;
    }
    bool StallPetOut(std::uint32_t entity, std::uint8_t place) const override { return OpenMarketPetOut(world->PeerOf(entity), place); }
    void StallPlaceMoved(std::uint32_t entity, std::uint8_t place) override {
        if (PlayerWorld* p = PlayerByEntity(entity)) QueueItemSave(p->selected, p->inventory, {place}, {});
    }
    void StallBagLost(std::uint32_t entity) override {
        if (const PlayerWorld* p = PlayerByEntity(entity)) core::LogWarning("{}: {}'s bag filled while a far purchase landed: its session ends", config.name, p->name);
        Disconnect(world->PeerOf(entity), proto::DisconnectReason::Kicked, "your bag changed while a purchase landed; please log in again");
    }
    void StallSaveSale(std::int64_t owner, const game::Inventory& ownerBag, std::uint8_t ownerPlace, std::int64_t buyer, const game::Inventory& buyerBag,
                       std::uint8_t buyerPlace) override {
        ExchangeSave(owner, ownerBag, {ownerPlace}, buyer, buyerBag, {buyerPlace});
    }
    void StallSaveGold(std::int64_t character, std::uint64_t gold, std::function<void()> saved) override {
        GoldNow(character, gold);
        CharacterWrite(character, [character, gold](Database& db) { SaveGold(db, character, gold); }, std::move(saved), true);
    }
    void StallSaveFarSale(const FarSale& sale, std::function<void(bool)> done) override {
        database.SubmitBarrier([this, sale, done = std::move(done)](Database& db) {
            const bool made = SaveFarSale(db, sale.owner, sale.ownerPlace, sale.ownerGold, sale.buyer, sale.buyerPlace, sale.serial, sale.deadlineMs);
            completions->Post([done, made] { done(made); });
        });
    }
    void StallSettle(std::int64_t buyer, std::uint8_t place, std::int64_t serial, std::int64_t deadlineMs, std::function<void(bool)> landed) override {
        database.SubmitBarrier([this, buyer, place, serial, deadlineMs, landed = std::move(landed)](Database& db) {
            const std::optional<bool> outcome = FarSaleLanded(db, buyer, place, serial, deadlineMs);
            // Not yet past the deadline on the database's clock (or the read failed): asked again.
            completions->Post([this, buyer, place, serial, deadlineMs, landed, outcome] {
                if (outcome) return landed(*outcome);
                StallSettle(buyer, place, serial, deadlineMs, landed);
            });
        });
    }
    void StallRefundOffline(std::int64_t character, std::uint64_t gold) override {
        database.SubmitBarrier([character, gold](Database& db) { AddGold(db, character, gold); });
    }
    std::uint16_t StallServer() const override { return config.id; }
    bool StallOnBus() const override { return !stallChannel.empty(); }
    void StallPublish(std::string text) override { BusSend(stallChannel, std::move(text)); }

    // Which game server runs each region (empty: this one runs them all).
    std::unordered_map<std::uint16_t, std::uint16_t> regionOwner;

    // The buffs a logout keeps (a scroll's: its skill has 'cbuf'), with the time each has left.
    std::vector<CharacterRow::Buff> KeptBuffs(const PlayerWorld& player) const {
        std::vector<CharacterRow::Buff> kept;
        const auto now = Clock::now();
        for (const auto& b : player.combat.buffs) {
            const assets::SkillRow* skill = config.skills ? config.skills->Find(b.skill) : nullptr;
            if (skill == nullptr || !Skills::OutlivesLogout(*skill) || b.until <= now || b.until == Clock::time_point::max()) continue;
            kept.push_back({b.skill, std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(b.until - now).count())});
        }
        return kept;
    }
    // A kept buff began or ended: its timed jobs written now, in the character's order (the original adds the row as
    // the buff starts and deletes it as it ends; a crash keeps what was running).
    void KeptBuffsChanged(std::uint32_t entity) override {
        const auto it = players.find(world->PeerOf(entity));
        if (it == players.end() || it->second.entity != entity) return;
        CharacterWrite(it->second.selected, [character = it->second.selected, kept = KeptBuffs(it->second)](Database& db) { SaveBuffs(db, character, kept); });
    }
    void LongCooldownStarted(std::uint32_t entity, std::uint32_t skill) override {
        const auto it = players.find(world->PeerOf(entity));
        if (it == players.end() || it->second.entity != entity) return;
        const std::int64_t started = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        database.Submit([character = it->second.selected, skill, started](Database& db) { SaveLongCooldown(db, character, skill, started); });
    }

    // --- Moves between the shard's game servers ---------------------------------
    struct KnownServer {
        std::string host;
        std::uint16_t port = 0;
        bool up = false;
    };
    std::unordered_map<std::uint16_t, KnownServer> knownServers; // as of the last report
    struct Move {
        std::uint64_t peer = 0;
        std::int64_t account = 0, character = 0;
        std::uint16_t target = 0;
        CharacterPlace place;
        bool fromWorld = false; // it was in the world here (not just choosing its character)
    };
    std::vector<Move> moves; // waiting for their saves
    // Characters that left for another server, until when their arrival is awaited.
    std::unordered_map<std::int64_t, Clock::time_point> movedAway;
    std::string moveChannel;

    std::uint16_t OwnerOf(std::uint16_t region) const {
        const auto it = regionOwner.find(region);
        return it != regionOwner.end() && it->second != 0 ? it->second : config.id;
    }
    bool ServerUp(std::uint16_t id) const {
        const auto it = knownServers.find(id);
        return it != knownServers.end() && it->second.up;
    }
    bool CanTravelTo(std::uint16_t region) const {
        const std::uint16_t owner = OwnerOf(region);
        return owner == config.id || ServerUp(owner);
    }
    void StartMove(std::uint64_t peerId, PlayerWorld& player, const proto::WorldPosition& to, float yaw, std::uint16_t target) {
        Peer* peer = peers->Find(peerId);
        if (peer == nullptr) return;
        Move m;
        m.peer = peerId;
        m.account = peer->account;
        m.character = player.selected;
        m.target = target;
        m.place = {player.selected, {to.region, to.x, to.y, to.z, yaw}};
        m.fromWorld = player.entity != 0;
        player.moving = true;
        if (m.fromWorld) LeaveWorld(peerId, false, true);
        core::LogInfo("{}: {} moves to game server {}", config.name, player.name.empty() ? std::to_string(player.selected) : player.name, target);
        moves.push_back(std::move(m));
    }
    // Takes a player somewhere: on this server a teleport, in another's part of the world a move there.
    // `withTransport`: its transport comes along (the thieves' town scroll, a gate's or a ferry's link that allows one, a
    // teleport zone's; on this server only: a move to another game server leaves it). A transport left behind goes with
    // its goods (only a kill or a termination drops them: Coses::Dismiss).
    void TravelTo(std::uint64_t peerId, PlayerWorld& player, const proto::WorldPosition& to, bool withTransport = false) {
        if (exchanges) exchanges->End(player.entity); // a teleport ends its exchange (ours: the original's busy player takes no gate)
        if (alchemy) alchemy->End(player.entity);
        if (stalls) stalls->Leave(player.entity);       // and its stall
        const std::uint16_t owner = OwnerOf(to.region);
        // Its ride and transport come along (the original's teleport keeps their rows and makes them again at the
        // arrival, the one ridden boarded again), where the teleport took them at all (a return scroll refuses a
        // transport out, `withTransport` the links that allow one); its pets too.
        (void)withTransport;
        if (owner == config.id) {
            if (coses) coses->DismissAll(player.entity, true, true);
            world->Teleport(player.entity, to);
            if (coses) coses->BringTransport(player.entity, to);
            return;
        }
        StartMove(peerId, player, to, 0.0f, owner); // its leave keeps their rows: made again at the other server's entry
    }
    void GoneAfterMove(std::int64_t character) {
        if (guilds) guilds->Left(character);
        if (community) community->GoneAfterMove(character);
        if (parties) parties->MoveFailed(std::uint32_t(character));
    }
    // Each move once what the character has is written (its items' saves, then everything before it):
    // the next server reads only the database.
    void FinishMoves(Clock::time_point now) {
        for (auto it = moves.begin(); it != moves.end();) {
            if (itemSaves.contains(it->character) || characterWrites.contains(it->character)) {
                ++it;
                continue;
            }
            const auto known = knownServers.find(it->target);
            database.SubmitBarrier([this, m = *it, host = known != knownServers.end() ? known->second.host : std::string(),
                                    port = known != knownServers.end() ? known->second.port : std::uint16_t(0)](Database& db) {
                SavePlaces(db, {m.place});
                std::string error;
                std::vector<std::uint8_t> ticket = IssueMoveTicket(db, m.account, m.character, m.target, 30, error);
                if (ticket.empty()) core::LogWarning("{}: no ticket for a move: {}", config.name, error);
                completions->Post([this, m, host, port, ticket = std::move(ticket)] {
                    Peer* peer = peers->Find(m.peer);
                    if (peer == nullptr || ticket.empty()) {
                        // Nobody to tell, or no ticket to give: the character stays out, its session freed.
                        database.Submit([m](Database& db) { ReleaseSession(db, m.account, m.target, 0); });
                        if (m.fromWorld) GoneAfterMove(m.character);
                        if (peer != nullptr) peer->connection->CloseAfterSend();
                        return;
                    }
                    proto::ServerMove move;
                    move.host = host;
                    move.port = port;
                    move.ticket = ticket;
                    SendMessage(*peer->connection, move);
                    if (m.fromWorld) movedAway[m.character] = Clock::now() + config.moveWait;
                });
            });
            it = moves.erase(it);
        }
        for (auto it = movedAway.begin(); it != movedAway.end();) {
            if (now < it->second) {
                ++it;
                continue;
            }
            GoneAfterMove(it->first);
            it = movedAway.erase(it);
        }
    }
    CommunityActor CommunityActorOf(std::uint64_t peerId, const PlayerWorld& player) const { return {peerId, player.selected, player.name, player.model}; }
    // --- The Academy's host (Academies.h) ---
    AcademyActor AcademyActorOf(std::uint64_t peerId, const PlayerWorld& player) const {
        AcademyActor a;
        a.peer = peerId;
        a.entity = player.entity;
        a.character = player.selected;
        a.name = player.name;
        a.model = player.model;
        a.level = player.level;
        a.maxLevel = std::max(player.maxLevel, player.level);
        a.dead = player.dead;
        a.gold = player.inventory.gold;
        a.busy = player.entity != 0 && ((exchanges && exchanges->Busy(player.entity)) || (stalls && (stalls->Owner(player.entity) || stalls->Visiting(player.entity))));
        a.jobChange = player.suitChange.has_value();
        return a;
    }
    void AcademySend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override { GuildSend(peerId, id, payload); }
    std::optional<AcademyActor> AcademyPlayerAt(std::uint32_t entity) const override {
        const std::uint64_t peerId = world->PeerOf(entity);
        const auto it = peerId != 0 ? players.find(peerId) : players.end();
        if (it == players.end() || it->second.entity != entity) return std::nullopt;
        return AcademyActorOf(peerId, it->second);
    }
    void AcademyPay(std::int64_t character, std::uint64_t fee, PaymentWork work, PaymentDone done) override {
        Payment(character, fee, std::move(work), std::move(done));
    }
    void AcademySubmit(std::function<void(Database&)> task, std::function<void()> done) override { GuildSubmit(std::move(task), std::move(done)); }
    std::uint16_t AcademyServer() const override { return config.id; }
    bool AcademyOnBus() const override { return !academyChannel.empty(); }
    void AcademyPublish(std::string text) override { BusSend(academyChannel, std::move(text)); }
    void AcademyGiveExperience(std::uint64_t peerId, std::uint64_t experience) override {
        const auto it = players.find(peerId);
        Peer* peer = peers->Find(peerId);
        if (it == players.end() || peer == nullptr || it->second.entity == 0) return;
        const std::uint64_t gained = std::uint64_t(std::llround(double(experience) * config.rates.experience));
        if (gained == 0) return;
        SendMessage(*peer->connection, proto::ExperienceGained{0, gained, 0});
        GainExperience(*peer, it->second, gained, 0);
    }
    void AcademyHonorChanged(std::uint64_t peerId, std::int64_t points) override {
        if (const auto it = players.find(peerId); it != players.end()) it->second.honorPoints = std::max<std::int64_t>(0, points);
    }
    // The guardian's honour buffs: every one from copper up to its academy's rank, as buffs
    // that last while it plays (engaged again at each entry).
    void AcademyHonorBuffs(std::uint64_t peerId, std::uint8_t rank) override {
        const auto it = players.find(peerId);
        if (it == players.end() || it->second.entity == 0 || !skillUnit || !config.skills) return;
        std::vector<std::uint32_t> all;
        std::vector<const assets::SkillRow*> rows;
        for (const char* code : game::academy::kHonorBuffs) {
            const assets::SkillRow* row = config.skills->Find(std::string_view(code));
            rows.push_back(row);
            if (row != nullptr) all.push_back(row->id);
        }
        SkillCaster caster = CasterOf(it->second);
        skillUnit->EndBuffsOf(caster, all);
        if (rank < 1 || rank > 4) return;
        for (std::size_t i = std::size_t(rank - 1); i < rows.size(); ++i) {
            if (rows[i] != nullptr) skillUnit->AddBuff(caster, *rows[i], 86'400'000u, Clock::now());
        }
    }
    // --- The Open Market (OpenMarket.h) ---
    void OpenMarketSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override { GuildSend(peerId, id, payload); }
    void OpenMarketSubmit(std::function<void(Database&)> task, std::function<void()> done) override { GuildSubmit(std::move(task), std::move(done)); }
    void OpenMarketWrite(std::int64_t character, std::function<void(Database&)> task, std::function<void()> done) override {
        CharacterWrite(character, std::move(task), std::move(done));
    }
    std::optional<OpenMarketActor> OpenMarketActorOf(std::uint64_t peerId) const override {
        const auto it = players.find(peerId);
        const Peer* peer = peers->Find(peerId);
        if (it == players.end() || peer == nullptr || it->second.entity == 0) return std::nullopt;
        return OpenMarketActor{peerId, it->second.entity, peer->account, it->second.selected, it->second.name, it->second.dead};
    }
    std::uint64_t OpenMarketPeerOfAccount(std::int64_t account) const override {
        for (const auto& [peerId, p] : players) {
            const Peer* peer = peers->Find(peerId);
            if (peer != nullptr && peer->account == account && p.entity != 0 && !p.moving) return peerId;
        }
        return 0;
    }
    bool OpenMarketNpcNear(std::uint32_t entity, std::uint32_t npc) const override {
        const auto it = npcs.find(npc);
        if (it == npcs.end() || it->second->codename != game::openmarket::kNpcCodename) return false;
        return AcademyNpcNear(entity, npc);
    }
    bool OpenMarketBagBusy(std::uint32_t entity) const override {
        if ((exchanges && exchanges->Holding(entity)) || (stalls && stalls->Holding(entity)) || (alchemy && alchemy->Busy(entity))) return true;
        // A save of the bag still on its way: the market's rows wait for it (one writer of a bag's places at a time).
        const PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return true;
        const auto s = itemSaves.find(p->selected);
        return s != itemSaves.end() && (s->second.busy || !s->second.dirty.empty() || !s->second.gone.empty());
    }
    std::uint64_t OpenMarketGold(std::uint64_t peerId) const override {
        const auto it = players.find(peerId);
        return it != players.end() ? it->second.inventory.gold : 0;
    }
    void OpenMarketSetGold(std::uint64_t peerId, std::uint64_t gold, bool save) override {
        const auto it = players.find(peerId);
        if (it == players.end()) return;
        it->second.inventory.gold = gold;
        if (Peer* peer = peers->Find(peerId)) SendMessage(*peer->connection, proto::GoldChanged{gold});
        if (save) {
            QueueGoldSave(it->second.selected, gold);
        } else {
            GoldNow(it->second.selected, gold); // the market's transaction writes it
        }
    }
    const game::ItemInstance* OpenMarketBagItem(std::uint64_t peerId, std::uint8_t place) const override {
        const auto it = players.find(peerId);
        return it != players.end() ? it->second.inventory.At(place) : nullptr;
    }
    std::uint8_t OpenMarketBagSize(std::uint64_t peerId) const override {
        const auto it = players.find(peerId);
        return it != players.end() ? it->second.inventory.size : 0;
    }
    void OpenMarketBagTold(std::uint64_t peerId, std::uint8_t place) {
        const auto it = players.find(peerId);
        Peer* peer = peers->Find(peerId);
        if (it == players.end() || peer == nullptr) return;
        proto::ItemMoved answer;
        answer.from = answer.to = place;
        if (it->second.inventory.At(place) != nullptr) answer.changed.push_back(it->second.inventory.Record(place));
        else answer.emptied.push_back(place);
        SendMessage(*peer->connection, answer);
        if (quests) quests->BagChanged(it->second.entity);
    }
    std::optional<game::ItemInstance> OpenMarketTakeAside(std::uint64_t peerId, std::uint8_t place) override {
        const auto it = players.find(peerId);
        if (it == players.end()) return std::nullopt;
        const auto item = it->second.inventory.places.find(place);
        if (item == it->second.inventory.places.end()) return std::nullopt;
        game::ItemInstance out = std::move(item->second);
        it->second.inventory.places.erase(item); // its rows the listing's transaction's: no save queued
        OpenMarketBagTold(peerId, place);
        return out;
    }
    // An item into the bag at `place`, or the first free place when that was filled meanwhile (a save of it there: its
    // rows say `place`); none free: the session ends, its next entry reading the database (the original logs out).
    void OpenMarketLand(std::uint64_t peerId, std::uint8_t place, game::ItemInstance item) {
        const auto it = players.find(peerId);
        if (it == players.end()) return;
        PlayerWorld& player = it->second;
        std::uint8_t at = place;
        if (player.inventory.places.contains(place)) {
            const int free = player.inventory.FreeBagPlace();
            if (free < 0) {
                core::LogWarning("{}: {}'s bag place {} was filled while the Open Market worked: its session ends", config.name, player.name, place);
                Disconnect(peerId, proto::DisconnectReason::Kicked, "your bag changed while the Open Market worked; please log in again");
                return;
            }
            at = std::uint8_t(free);
        }
        player.inventory.places[at] = std::move(item);
        if (at != place) QueueItemSave(player.selected, player.inventory, {at}, {});
        OpenMarketBagTold(peerId, at);
    }
    void OpenMarketPutBack(std::uint64_t peerId, std::uint8_t place, game::ItemInstance item) override {
        if (item.refId != 0) OpenMarketLand(peerId, place, std::move(item));
    }
    void OpenMarketPutInBag(std::uint64_t peerId, std::uint8_t place, const StoredItem& s) override {
        const auto it = players.find(peerId);
        if (it == players.end()) return;
        game::ItemInstance item;
        item.serial = s.id;
        item.refId = s.refId;
        item.plus = s.plus;
        item.variance = s.variance;
        item.rentUntil = s.rentUntil;
        item.advanced = s.advanced;
        item.bound = s.bound;
        item.advancedRecipe = s.advancedRecipe;
        item.sockets = s.sockets;
        FromData(s, item);
        for (std::uint64_t m : s.magic) item.options.push_back({std::uint32_t(m & 0xFFFFFFFFu), std::uint32_t(m >> 32)});
        OpenMarketLand(peerId, place, std::move(item)); // its rows moved already
    }
    std::optional<std::uint8_t> OpenMarketFreePlace(std::uint64_t peerId, const std::set<std::uint8_t>& taken) const override {
        const auto it = players.find(peerId);
        if (it == players.end()) return std::nullopt;
        for (int place = game::openmarket::kFirstBagSlot; place < it->second.inventory.size; ++place) {
            if (!it->second.inventory.places.contains(std::uint8_t(place)) && !taken.contains(std::uint8_t(place))) return std::uint8_t(place);
        }
        return std::nullopt;
    }
    bool OpenMarketPetOut(std::uint64_t peerId, std::uint8_t place) const override {
        const auto it = players.find(peerId);
        if (it == players.end() || !coses) return false;
        const game::ItemInstance* item = it->second.inventory.At(place);
        return item != nullptr && coses->Out(it->second.entity, item->serial);
    }
    game::openmarket::Ticket OpenMarketTicket(std::uint64_t peerId, std::uint32_t& item) const override {
        item = 0;
        const auto it = players.find(peerId);
        if (it == players.end()) return {};
        for (const game::TimedService& s : it->second.services) {
            if (s.category != game::openmarket::kTicketCategory) continue;
            const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(s.item) : nullptr;
            if (row == nullptr) continue;
            item = s.item;
            return {std::max(0, row->params[1]), std::max<std::int64_t>(0, row->params[2]), std::max(0, row->params[3])};
        }
        return {};
    }
    const assets::ItemRow* OpenMarketItem(std::uint32_t refId) const override { return config.rules->items ? config.rules->items->Find(refId) : nullptr; }
    std::uint32_t OpenMarketGroupOf(const assets::ItemRow& row) const override {
        const assets::StallNetworkData* tables = StallNetworkTables();
        return tables != nullptr ? tables->GroupOf(std::uint8_t(row.typeId[0]), std::uint8_t(row.typeId[1]), std::uint8_t(row.typeId[2]), std::uint8_t(row.typeId[3])) : 0;
    }
    std::uint16_t OpenMarketServer() const override { return config.id; }
    bool OpenMarketOnBus() const override { return !openMarketChannel.empty(); }
    void OpenMarketPublish(std::string text) override { BusSend(openMarketChannel, std::move(text)); }

    bool AcademyNpcNear(std::uint32_t entity, std::uint32_t npc) const override {
        const auto it = npcs.find(npc);
        const auto me = world->Where(entity);
        const auto there = world->Where(npc);
        if (it == npcs.end() || !me || !there) return false;
        const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(there->position);
        return std::hypot(a.x - b.x, a.z - b.z) <= config.gateReach;
    }
    bool AcademyKeeperNear(std::uint32_t entity, std::uint32_t npc) const override {
        const PlayerWorld* player = PlayerByEntity(entity);
        return player != nullptr && AtStorageKeeper(*player, npc) == proto::GateStatus::Ok;
    }
    bool AcademyTextAllowed(std::string_view text) const override { return config.rules->names.ChatAllowed(text); }
    std::optional<proto::WorldPosition> AcademyPositionOf(std::uint32_t entity) const override {
        if (const auto place = world->Where(entity)) return place->position;
        return std::nullopt;
    }
    void CommunitySend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override { GuildSend(peerId, id, payload); }
    void CommunitySubmit(std::function<void(Database&)> task, std::function<void()> done) override { GuildSubmit(std::move(task), std::move(done)); }
    std::uint16_t CommunityServer() const override { return config.id; }
    bool CommunityOnBus() const override { return !communityChannel.empty(); }
    void CommunityPublish(std::string text) override { BusSend(communityChannel, std::move(text)); }
    bool CommunityNameAllowed(std::string_view name) const override { return GuildNameAllowed(name); }
    bool CommunityTextAllowed(std::string_view text) const override { return config.rules->names.ChatAllowed(text); }
    bool CommunityGuildMember(std::int64_t character, std::string_view name) const override { return guilds && guilds->MemberNamed(character, name); }
    int CommunityGuildLevel(std::int64_t character) const override {
        if (!guilds) return 0;
        const auto facts = guilds->FactsOf(character);
        return facts ? facts->level : 0;
    }
    // Published one at a time, so the others hear them in this server's order.
    void BusSend(std::string channel, std::string text) {
        busQueue.emplace_back(std::move(channel), std::move(text));
        if (!busSending) BusNext();
    }
    void BusNext() {
        if (busQueue.empty()) {
            busSending = false;
            return;
        }
        busSending = true;
        auto next = std::move(busQueue.front());
        busQueue.pop_front();
        database.Submit([this, next = std::move(next)](Database& db) {
            Notify(db, next.first, next.second);
            completions->Post([this] { BusNext(); });
        });
    }

    std::optional<PartyHost::Player> PlayerAt(std::uint32_t entity) const override {
        const auto it = players.find(world->PeerOf(entity));
        if (it == players.end() || it->second.entity != entity) return std::nullopt;
        const PlayerWorld& p = it->second;
        PartyHost::Player out;
        out.entity = p.entity;
        out.character = std::uint32_t(p.selected);
        out.name = p.name;
        out.level = p.level;
        out.race = std::uint8_t(p.wearer.country);
        out.alive = !p.dead;
        out.hp = p.hp;
        out.maxHp = p.stats.maxHp;
        out.mp = p.mp;
        out.maxMp = p.stats.maxMp;
        out.model = p.model;
        std::tie(out.mastery1, out.mastery2) = game::DominantMasteries(p.book.masteries);
        if (const auto place = world->Where(entity)) out.position = place->position;
        out.jobState = JobState(p);
        out.alias = !p.alias.empty();
        out.jobChanging = p.suitChange.has_value();
        if (const std::uint8_t team = arenas && arenas->InMatch(entity) ? arenas->TeamOf(entity) : std::uint8_t(0xFF); team != 0xFF) out.arenaTeam = std::uint8_t(team + 1);
        return out;
    }
    void Tell(std::uint32_t entity, const PartyMessage& message) override {
        Peer* peer = peers->Find(world->PeerOf(entity));
        if (peer == nullptr) return;
        std::visit([&](const auto& m) { SendMessage(*peer->connection, m); }, message);
    }
    std::vector<std::uint32_t> PartyOf(std::uint32_t player) const override {
        return parties ? parties->MemberEntities(player) : std::vector<std::uint32_t>{};
    }
    std::optional<SkillCaster> Player(std::uint32_t entity) override {
        PlayerWorld* player = PlayerByEntity(entity);
        if (player == nullptr || player->entity == 0) return std::nullopt;
        return CasterOf(*player);
    }
    void ToCaster(std::uint32_t caster, const SkillMessage& message) override {
        Peer* peer = peers->Find(world->PeerOf(caster));
        if (peer == nullptr) return;
        std::visit([&](const auto& m) { SendMessage(*peer->connection, m); }, message);
    }
    void ToObservers(std::uint32_t entity, const SkillMessage& message) override {
        if (const auto* states = std::get_if<proto::EntityStates>(&message)) return TellStates(entity, states->states, Clock::now());
        std::visit([&](const auto& m) { Broadcast(entity, m); }, message);
    }

    // The monsters' turn: they get the players as they are now, and what they did comes back as
    // swings to show, hits to take and those that went home healed.
    void MonsterTurn(Clock::time_point now) {
        std::vector<Monsters::Player> list;
        std::unordered_map<std::uint32_t, std::uint64_t> byEntity;
        list.reserve(players.size());
        for (auto& [peerId, player] : players) {
            if (player.entity == 0) continue;
            const auto place = world->Where(player.entity);
            if (!place) continue;
            list.push_back({player.entity, place->position, !player.dead && now >= player.untouchableUntil, &player.stats});
            list.back().hidden = player.hidden;
            // The jobs: its state, its transport the other a bandit goes between.
            list.back().job = player.jobState;
            // A fortress war's guards and tablet monsters go for those fighting against their side there.
            if (fortresses && fortresses->WarOn()) {
                if (const std::optional<FortressGuild> g = FortressGuildOf(player.selected)) {
                    list.back().guild = g->id;
                    list.back().alliance = g->alliance;
                    list.back().atWar = fortresses->Fighting(player.selected, place->position.region);
                }
            }
            if (coses) {
                if (const auto cart = coses->TransportCart(player.entity)) {
                    list.back().partner = cart->entity;
                    if (player.riding && coses->MountOf(player.entity) == cart->entity) list.back().mount = cart->entity; // never struck on it
                }
            }
            if (skillUnit) {
                const Skills::Protection guard = skillUnit->ProtectionOf(player.combat);
                list.back().protectedGrades = guard.grades;
                list.back().protectedLevel = guard.level;
                list.back().wall = skillUnit->WallOf(player.combat);
            }
            byEntity[player.entity] = peerId;
        }
        if (coses) coses->Fighters(list); // the attack pets out: fought as players are; the transports, by the bandits
        // The caught COSes: as a transport, sought only by the bandits (the tactics flags 4 / 0x80 ask players and COSes, mask 5;
        // every other monster's sight query asks players alone, mask 1), answering for their owner's job. A COS itself starts no fight.
        for (const auto& [cos, held] : questCoses) {
            const auto at = world->Where(cos);
            if (!at || held.hp == 0) continue;
            Monsters::Player c;
            c.entity = cos;
            c.position = at->position;
            c.stats = &held.stats;
            c.partner = held.owner;
            c.banditsOnly = true;
            if (const PlayerWorld* owner = PlayerByEntity(held.owner)) c.job = owner->jobState;
            list.push_back(c);
        }
        Monsters::Events events;
        std::vector<Monsters::Tick> ticks;
        monsters->Update(now, list, events, &ticks);
        for (const Monsters::Events::Unique& u : events.uniques) TellUnique(UniqueEvent::Kind::Appeared, u.refObjId, u.position, nullptr);
        for (const proto::EntitySwing& swing : events.swings) Broadcast(swing.attacker, swing);
        // The states' ticks (burn, poison, bleed: K3), their kills shared as a hit's; the states that ended.
        for (const Monsters::Tick& t : ticks) {
            // A time bomb's burst: a skill's strike of its own (its S_RETURN on the holder; B0BC form 4).
            const bool burst = t.tick.kind == game::StateKind::TimeBomb && !t.self;
            // Its own skill's HP cost (a bomb monster's end): a plain hit with no attacker.
            std::uint8_t flags = t.self ? std::uint8_t(0) : burst ? proto::kDamageBurst : proto::kDamageState;
            if (t.damaged.killed) flags |= proto::kDamageKilled;
            Broadcast(t.monster, proto::EntityDamaged{t.monster, t.tick.source, t.tick.hp, t.damaged.hp, flags, burst ? t.tick.skill : 0u});
            if (t.damaged.killed) Killed(t.monster, t.damaged);
        }
        for (const std::uint32_t entity : events.statesChanged) TellStates(entity, monsters->StatesOf(entity, now), now);
        for (Monsters::Events::Hit& h : events.hits) {
            // A hit on a caught COS: its own health.
            if (questCoses.contains(h.target)) {
                HurtQuestCos(h.target, h.monster, h.hit);
                continue;
            }
            // A hit on a COS (a pet, a ride, a transport: Coses.h): its own health, and its skill's states on it.
            if (coses && coses->OwnerOf(h.target) != 0) {
                HurtCos(h.target, h.monster, h.hit);
                AfflictCos(h.target, h.monster, h.hit, h.skill, now);
                continue;
            }
            const auto who = byEntity.find(h.target);
            if (who == byEntity.end()) continue;
            PlayerWorld& player = players[who->second];
            if (player.dead || now < player.untouchableUntil) continue;
            // A rider: the hit strikes their mount whole, a ride's, a transport's or a pet's.
            if (coses && player.riding) {
                if (const std::uint32_t mount = coses->MountOf(player.entity); mount != 0) {
                    HurtCos(mount, h.monster, h.hit);
                    AfflictCos(mount, h.monster, h.hit, h.skill, now);
                    continue;
                }
            }
            if (coses) coses->OwnerHurt(player.entity, h.monster); // its attack pet defends it
            // Its wall (an Ice Wall, a Fire Wall) took its part first; covering the whole hit, nothing reaches the holder.
            if (h.hit.wallTaken > 0) WallTook(player.entity, h.hit.wallTaken);
            if (h.hit.wallOnly) {
                player.combat.battleUntil = now + std::chrono::seconds(20);
                continue;
            }
            // Its Snow Shield and its reflection.
            Skills::Guarded guard{h.hit.damage};
            if (skillUnit && !h.hit.blocked) {
                SkillCaster caster = CasterOf(player);
                const auto a = world->Where(h.monster), b = world->Where(player.entity);
                double distance = 1e9;
                if (a && b) {
                    const proto::FlatPosition p = proto::ToFlat(a->position), q = proto::ToFlat(b->position);
                    distance = std::sqrt((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z) + double(a->position.y - b->position.y) * (a->position.y - b->position.y));
                }
                guard = skillUnit->Guard(caster, h.monster, h.hit.damage, h.hit.magical, distance, now);
                if (guard.mpTaken > 0) VitalsChanged(player.entity);
            }
            h.hit.damage = guard.damage;
            player.hp = h.hit.damage >= player.hp ? 0 : player.hp - h.hit.damage;
            player.lastHurt = now;
            // The states of the monster's skill (the White-Face Spider's freeze, a burn, a stun): a
            // damage first ends those a hit ends (sleep), then its own are rolled on the player through the monster's
            // chance history (the user's report 2026-11-13: monsters gave none).
            if (h.hit.damage > 0 && !player.combat.states.Damaged(player.combat.rolls, fightRandom).empty()) TellStates(player.entity, StateRecords(player, now), now);
            if (!h.hit.blocked && h.skill != nullptr && player.hp > 0) {
                const std::vector<game::StateGift> gifts = game::StateGiftsOf(*h.skill);
                if (!gifts.empty()) {
                    const game::StateDefence defence = StateDefenceOf(player);
                    const int pulse = assets::FindSkillParam(*h.skill, assets::SkillTag("puls")).Int(0);
                    GiveStates(player, game::RollStates(gifts, defence, player.combat.states.Susceptibility(), monsterStateRolls[h.monster], fightRandom,
                                                        h.monster, h.skill->id, pulse, now),
                               now);
                }
            }
            HitWhileSitting(player, now); // a hit stands a sitting player up
            player.combat.battleUntil = now + std::chrono::seconds(20);
            // Its equipment's wear: the shield for a block, an armour piece for a hit taken.
            if (h.hit.blocked) WearDown(player, game::Wear::Shield);
            else if (h.hit.damage > 0) WearDown(player, game::Wear::Armour);
            if (!h.hit.blocked && skillUnit) {
                SkillCaster caster = CasterOf(player);
                if (skillUnit->EndOnHit(caster, 1 | 4)) StatsChanged(player.entity); // ours: a monster's hit as melee physical
            }
            world->SetHp(player.entity, player.hp);
            std::uint8_t flags = h.hit.critical ? proto::kDamageCritical : 0;
            if (h.hit.blocked) flags |= proto::kDamageBlocked;
            if (player.hp == 0) flags |= proto::kDamageKilled;
            Broadcast(player.entity, proto::EntityDamaged{player.entity, h.monster, h.hit.damage, player.hp, flags});
            // The returned share on the attacker, its reflection skill's strike (ours: at once; the
            // original's on the attacker's next skill tick, B0BC form 1).
            // The links' shares on the guards and the party (33 4.2, 4.3), their skill's strike on each.
            for (const Skills::Guarded::Moved& m : guard.moved) {
                PlayerWorld* other = PlayerByEntity(m.player);
                if (other == nullptr || other->dead) continue;
                other->hp = m.damage >= other->hp ? 0 : other->hp - m.damage;
                world->SetHp(other->entity, other->hp);
                std::uint8_t moved = proto::kDamageReturn;
                if (other->hp == 0) moved |= proto::kDamageKilled;
                Broadcast(other->entity, proto::EntityDamaged{other->entity, h.monster, m.damage, other->hp, moved, m.skill});
                if (other->hp == 0) Die(*other);
            }
            if (guard.returned > 0) {
                const Monsters::Damaged d = monsters->Damage(h.monster, player.entity, guard.returned, now, nullptr);
                if (d.hit) {
                    std::uint8_t back = proto::kDamageReturn;
                    if (d.killed) back |= proto::kDamageKilled;
                    Broadcast(h.monster, proto::EntityDamaged{h.monster, player.entity, guard.returned, d.hp, back, guard.reflectSkill});
                    if (d.killed) Killed(h.monster, d);
                }
            }
            if (player.hp == 0) Die(player, nullptr, monsters->RefOf(h.monster));
        }
        for (std::uint32_t healed : events.healed) {
            if (const game::CombatStats* s = monsters->Stats(healed)) Broadcast(healed, proto::EntityDamaged{healed, 0, 0, s->maxHp, 0});
        }
    }

    // A player's death: `killer` the player who killed it (a COS's owner), `monster` the killing monster's reference (0:
    // none known). Its kill's relation (game::KillRelation) decides the penalty, the PK counts and the killer's reward.
    void Die(PlayerWorld& player, PlayerWorld* killer = nullptr, std::uint32_t monster = 0, int strikerLevel = 0) {
        if (killer == &player) killer = nullptr;
        // Its log (the original's 20): by whom (ours: the killer's name, or the monster's codename).
        {
            std::string by;
            if (killer != nullptr) {
                by = std::format("by {} (PC)", killer->name);
            } else if (const assets::ObjectRow* row = monster != 0 && config.characters ? config.characters->Find(monster) : nullptr) {
                by = "by " + row->codename; // `monster` its refObjId
            }
            LogChar(player, logs::CharEvent::Died, 0, 0, std::move(by));
        }
        const game::KillRelation relation = KillRelationOf(player, killer);
        const bool victimMurderer = player.pkPoints > 0;
        const bool murder = relation == game::KillRelation::Open && killer != nullptr && !killer->dead &&
                            game::KillIsMurder(PvpPairOf(*killer, player, player.entity, game::AttackMode::Chosen));
        player.dead = true;
        // Its states end with it (told: their marks off it).
        if (player.combat.states.Mask() != 0) {
            player.combat.states.Clear();
            TellStates(player.entity, {}, Clock::now());
        }
        if (exchanges) exchanges->End(player.entity); //, siege 0x182B
        if (alchemy) alchemy->End(player.entity);     // a death stops it at the next tick
        if (quests) quests->Died(player.entity);      // a caught COS runs away
        if (stalls) stalls->Leave(player.entity);
        if (coses) coses->OwnerDied(player.entity); // what it rides goes (a transport's goods with it); its pets stay out
        if (player.freePvpAt) {
            player.freePvpAt.reset();
            ShowFreePvp(player);
        }
        EndBerserk(player);
        player.combat.StopFighting();
        player.pickTarget = 0;
        player.returnAt.reset();
        player.recallTo.reset();
        player.reverseOption = 0;
        player.revivalOffer.reset();
        // Where it died, for the reverse return scroll (every death: from ProcessNormalDeath and
        // ProcessSiegeDeath; not in an instance).
        if (const auto spot = KeepableSpot(player)) {
            player.deathSpot = *spot;
            database.Submit([character = player.selected, s = *spot](Database& db) { SaveDeathSpot(db, character, s); });
        }
        if (const auto place = world->Where(player.entity); place && world->Moving(player.entity)) world->MoveTo(player.entity, place->position);
        // Its death penalty (K9): the job's for a job kill,
        // none for a team's (free PvP), the special world's in a fortress at war, else the death penalty with the
        // murderer's rows when it has points and the relation is not 5. Experience off this level's (a level down when it
        // takes more), what it took kept for a resurrection.
        const std::size_t level = std::size_t(std::max(1, player.level));
        player.deathLoss = 0;
        // A job's kill (relation 1): a thief and a trader or hunter in their
        // suits. The victim loses 3 x GoldUnit(level) job exp and 6 x GUST_Mob_Exp of the lower level instead of the
        // ordinary penalty; the killer gains its job's kill and 3 x
        // GUST_Mob_Exp of the lower level.
        const bool jobKill = relation == game::KillRelation::Job;
        const std::size_t lower = std::size_t(std::max(1, killer != nullptr ? std::min(killer->level, player.level) : player.level));
        if (jobKill) {
            LoseJobExpOf(player, game::JobDeathLoss(jobRules, player.level));
            // Its party's takers: a thief's the thieves; a trader's or hunter's the traders and hunters (a hunter's only
            // hunters: JobKill).
            const bool thiefKiller = killer->jobState == std::uint8_t(game::JobType::Thief);
            JobKill(*killer, player.level, [thiefKiller](const PlayerWorld& p) {
                const bool thief = p.jobState == std::uint8_t(game::JobType::Thief);
                return thiefKiller ? thief : (!thief && p.jobState != game::kNoJob);
            });
            if (lower < config.monsterExperience.size()) {
                if (Peer* kp = peers->Find(world->PeerOf(killer->entity))) GainExperience(*kp, *killer, 3ull * config.monsterExperience[lower], 0);
            }
        }
        // The killer's reward for a player's kill (relations 3 and 5): the PvP experience, doubled for a murderer
        // outside relation 5, and
        // his attack pet's.
        if (killer != nullptr && !killer->dead && (relation == game::KillRelation::GuildWar || relation == game::KillRelation::Open)) {
            RewardPlayerKill(*killer, player, relation, victimMurderer);
        }
        const bool murdererNumbers = game::MurdererNumbers(player.pkPoints, relation);
        const bool byPlayer = killer != nullptr;
        if (level < config.levels.size() && level < config.monsterExperience.size() && relation != game::KillRelation::Team) {
            const auto where = world->Where(player.entity);
            const bool fortressWar = where && fortresses && fortresses->WarArea(where->position.region);
            if (fortressWar) player.warDeathAt = Clock::now(); // 180 s to rise, none in the first 15
            game::DeathFacts facts;
            facts.level = player.level;
            facts.levelExperience = config.levels[level];
            facts.monsterExperience = config.monsterExperience[level];
            facts.reducePercent = player.combat.Sheet().Value(0x101, 0.0f);
            facts.byPlayer = byPlayer;
            facts.murderer = murdererNumbers;
            facts.fortressWar = fortressWar;
            if (const assets::ObjectRow* row = monster != 0 && config.characters ? config.characters->Find(monster) : nullptr) {
                facts.thiefMonster = assets::IsBandit(*row) && row->typeId[3] == 2; // a thief monster halves the cap
            }
            std::uint64_t penalty = game::DeathExperienceLoss(facts);
            if (jobKill && lower < config.monsterExperience.size()) {
                const double cut = player.combat.Sheet().Value(0x101, 0.0f);
                std::uint64_t lost = 6ull * config.monsterExperience[lower];
                lost -= std::min<std::uint64_t>(lost, std::uint64_t(std::trunc(cut / 100.0 * double(lost))));
                penalty = lost;
            }
            // Taken from the experience; more than the level holds takes it a level down, the rest off the level below's
            // (ApplyExpDelta with the negative, its virtual 0x170: the user's 64 to 63 after a death, 2026-11-13). What it
            // wears above its new level then gives nothing (UpdateStats, game::LevelFits).
            player.deathLoss = penalty;
            if (penalty <= player.experience || player.level <= 1 || level - 1 >= config.levels.size()) {
                player.deathLoss = std::min(penalty, player.experience);
                player.experience -= player.deathLoss;
            } else {
                const std::uint64_t below = config.levels[level - 1];
                const std::uint64_t rest = penalty - player.experience;
                --player.level;
                player.wearer.level = player.level;
                player.experience = below > rest ? below - rest : 0;
                core::LogInfo("{}: {} goes down to level {}", config.name, player.name, player.level);
                UpdateStats(nullptr, player);
                QueueProgressSave(player);
            }
            // A murderer's skill points too: 20 to a player, 60 to a monster, at most what it has.
            std::uint32_t spLost = 0;
            if (murdererNumbers && !jobKill && !fortressWar) {
                spLost = std::min(player.skillPoints, game::MurdererSkillPointLoss(byPlayer));
                player.skillPoints -= spLost;
            }
            if (player.deathLoss > 0 || spLost > 0) {
                player.progressDirty = true;
                if (Peer* peer = peers->Find(world->PeerOf(player.entity))) SendProgress(*peer, player);
            }
            if (spLost > 0) core::LogInfo("{}: {} loses {} skill points (a murderer's death)", config.name, player.name, spLost);
        }
        // An item to the ground (every death of relations 3, 4, 5 above level 10), by its points.
        if (game::DeathMayDrop(relation, player.level)) DropOnDeath(player, player.pkPoints > 0 && relation != game::KillRelation::Open);
        // The counts' relief by the death: 600 points to a monster, 200 in a guild war, and 1 DailyPK.
        if (const std::int32_t relief = game::DeathRelief(relation); relief > 0 || game::DeathTakesDaily(relation)) {
            const std::int32_t points = player.pkPoints;
            const std::uint8_t daily = player.dailyPk;
            if (relief > 0) AddPkPoints(player, -relief);
            if (game::DeathTakesDaily(relation) && player.dailyPk > 0) --player.dailyPk;
            if (player.pkPoints != points || player.dailyPk != daily) TellPk(player);
        }
        // A murder (relation 5 on one not his natural enemy): TotalPK + 1, the
        // points by it, DailyPK by the level gap; a murderer.
        if (murder) Murdered(*killer, player);
        // A Capture the Flag match's death: his flag dropped, his camp's revival in 30 s, the killer's trophy.
        if (flagWorlds) flagWorlds->Died(player.entity, killer != nullptr ? killer->entity : 0u);
        if (arenas) arenas->Died(player.entity, killer != nullptr ? killer->entity : 0u); // the gate's revival in 10 s, the points
        core::LogInfo("{}: {} dies ({} experience lost){}", config.name, player.name, player.deathLoss, killer != nullptr ? ", killed by " + killer->name : "");
        if (killer != nullptr && guilds) guilds->PlayerKilled(killer->selected, player.selected, strikerLevel > 0 ? strikerLevel : killer->level, player.level);
        if (killer != nullptr && fortresses) {
            if (const auto at = world->Where(player.entity)) fortresses->PlayerKilled(killer->selected, player.selected, at->position.region);
        }
        // The temple quests' kill-player goals: the victim's level, region and job.
        if (killer != nullptr && quests) {
            const auto at = world->Where(player.entity);
            const game::JobType job = game::JobType(JobState(player));
            quests->PlayerKilled(killer->entity, player.level, at ? at->position.region : std::uint16_t(0),
                                 job == game::JobType::Trader ? "trader" : job == game::JobType::Thief ? "thief" : job == game::JobType::Hunter ? "hunter" : "");
        }
    }

    // The kill's relation: 6 the fortress world during a war, 2 free PvP foes
    // (ours has no timed event nor arena yet), 1 a job kill, 4 a monster (or no killer), 3 guilds at war, else 5.
    game::KillRelation KillRelationOf(const PlayerWorld& victim, const PlayerWorld* killer) const {
        // Two foes of a Capture the Flag match (relation 0x20): no penalty, no PK.
        if (killer != nullptr && flagWorlds && flagWorlds->Hostile(killer->entity, victim.entity)) return game::KillRelation::Team;
        // The Battle Arena's foes (relation 0x40): the same.
        if (killer != nullptr && arenas && arenas->Hostile(killer->entity, victim.entity)) return game::KillRelation::Team;
        if (const auto where = world->Where(victim.entity); where && fortresses && fortresses->WarArea(where->position.region)) {
            return game::KillRelation::FortressWar;
        }
        if (killer == nullptr) return game::KillRelation::Monster;
        if (FreePvpHostile(*killer, victim)) return game::KillRelation::Team;
        if (game::IsHostileJob(killer->jobState, victim.jobState)) return game::KillRelation::Job;
        if (guilds && guilds->AtWar(killer->selected, victim.selected)) return game::KillRelation::GuildWar;
        return game::KillRelation::Open;
    }
    void Murdered(PlayerWorld& killer, const PlayerWorld& victim) {
        killer.totalPk = std::uint16_t(std::min(game::kTotalPkMost, int(killer.totalPk) + 1));
        killer.dailyPk = std::uint8_t(std::min(game::kDailyPkMost, int(killer.dailyPk) + game::DailyForMurder(killer.level, victim.level)));
        AddPkPoints(killer, game::PenaltyForMurder(killer.totalPk));
        TellPk(killer);
        core::LogInfo("{}: {} murders {} (TotalPK {}, DailyPK {}, {} points)", config.name, killer.name, victim.name, killer.totalPk, killer.dailyPk,
                      killer.pkPoints);
    }
    void RewardPlayerKill(PlayerWorld& killer, const PlayerWorld& victim, game::KillRelation relation, bool victimMurderer) {
        const std::size_t lower = std::size_t(std::max(1, std::min(killer.level, victim.level)));
        if (lower >= config.monsterExperience.size()) return;
        const bool shared = parties && parties->PartyOf(killer.entity) != 0 && (parties->SettingsOf(killer.entity) & game::kPartyExpShare) != 0;
        const bool doubled = victimMurderer && relation != game::KillRelation::Open;
        const std::uint64_t experience = game::PvpExperience(config.monsterExperience[lower], victim.level, killer.level, config.rates.experience, shared,
                                                             killer.wearer.country == 1, doubled);
        Peer* peer = peers->Find(world->PeerOf(killer.entity));
        if (peer == nullptr) return;
        if (experience > 0) {
            SendMessage(*peer->connection, proto::ExperienceGained{victim.entity, experience, 0});
            GainExperience(*peer, killer, experience, 0);
        }
        // Their attack pet: the lower level's monster experience, doubled for a murderer.
        if (coses && coses->AttackPetLevel(killer.entity) > 0) {
            coses->GainExperience(killer.entity, std::uint64_t(config.monsterExperience[lower]) * (doubled ? 2 : 1));
        }
        core::LogInfo("{}: {} gains {} experience for killing {}", config.name, killer.name, experience, victim.name);
    }
    // Whether an item may drop at a death: stored, not a quest item (TID
    // 3/3/9), not a mall item, its reference's CanDrop, not a pet's summoning item, not equipment of rarity 2 (the seals)
    // or 6 (the rare sets). Ours has no item lock.
    bool CanDropOnDeath(const game::ItemInstance& item) const {
        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(item.refId) : nullptr;
        if (row == nullptr || item.serial == 0) return false;
        if (row->typeId[0] == 3 && row->typeId[1] == 3 && row->typeId[2] == 9) return false;
        if (row->cashItem || !game::CanDropAtDeath(*row) || game::IsCosSummoner(*row)) return false;
        const game::ItemKind kind = game::KindOf(*row);
        const bool equipment = kind == game::ItemKind::Weapon || kind == game::ItemKind::Shield || kind == game::ItemKind::Armour ||
                               kind == game::ItemKind::Accessory || kind == game::ItemKind::JobSuit;
        return !(equipment && (row->rarity == 2 || row->rarity == 6));
    }
    std::mt19937 deathDropRandom{std::random_device{}()};
    // The death's item: drawn by the points' chance;
    // a murderer (relation not 5) rolls a worn place first (the weapon, the suit and a quiver's place again among 0..5)
    // and that piece goes when it may; otherwise one of the bag's filled places at random, refused when it may not. To
    // the ground at his feet (the item move of kind 0x17; inferred), anyone's.
    void DropOnDeath(PlayerWorld& player, bool murderer) {
        // The shard's PK switch refuses the death's drop (the original's forced move 0x17 answers 2 under it).
        if (config.pkProhibited) return;
        if (int(deathDropRandom() % 101u) > game::DeathItemChance(player.pkPoints)) return;
        int place = -1;
        if (murderer) {
            const game::ItemInstance* shield = player.inventory.At(game::slot::kShield);
            const assets::ItemRow* shieldRow = shield != nullptr && config.rules->items ? config.rules->items->Find(shield->refId) : nullptr;
            const int worn = game::MurdererEquipmentPlace(int(deathDropRandom() % 13u), int(deathDropRandom() % 6u),
                                                          shieldRow != nullptr && game::IsAmmunition(*shieldRow));
            if (const game::ItemInstance* item = player.inventory.At(std::uint8_t(worn)); item != nullptr && CanDropOnDeath(*item)) place = worn;
        }
        if (place < 0) {
            std::vector<int> filled;
            for (const auto& [slot, item] : player.inventory.places) {
                if (slot >= game::slot::kWorn && !item.Empty()) filled.push_back(slot);
            }
            if (filled.empty()) return;
            const int pick = filled[deathDropRandom() % filled.size()];
            if (!CanDropOnDeath(player.inventory.places[std::uint8_t(pick)])) return;
            place = pick;
        }
        const auto it = player.inventory.places.find(std::uint8_t(place));
        if (it == player.inventory.places.end()) return;
        game::ItemInstance item = std::move(it->second);
        const std::int64_t serial = item.serial;
        player.inventory.places.erase(it);
        proto::ItemMoved answer;
        answer.from = answer.to = std::uint8_t(place);
        answer.emptied.push_back(std::uint8_t(place));
        if (Peer* peer = peers->Find(world->PeerOf(player.entity))) SendMessage(*peer->connection, answer);
        QueueItemSave(player.selected, player.inventory, {std::uint8_t(place)}, {serial});
        if (place < game::slot::kWorn) {
            Broadcast(player.entity, proto::EquipmentChanged{player.entity, std::uint8_t(place), 0u, std::uint8_t(0), item.refId});
            world->SetEquipment(player.entity, player.inventory.Worn());
            if (place == game::slot::kJobSuit) TellJob(peers->Find(world->PeerOf(player.entity)), player, true);
            StatsChanged(player.entity);
        }
        item.serial = 0;
        const std::uint32_t model = item.refId;
        core::LogInfo("{}: {} drops {} x{} at its death (place {})", config.name, player.name, model, item.count, place);
        PutOnGround(player, std::move(item), 0, model);
        if (quests) quests->BagChanged(player.entity);
    }

    // --- Berserk (game/Berserk.h) ---------------------------------------------

    // The gauge changed: 0..5, none gained in berserk; kept, told.
    void AddBerserkPoints(PlayerWorld& player, int points, std::uint32_t from = 0) {
        if (points > 0 && player.berserkUntil) return;
        const std::uint8_t now = std::uint8_t(std::clamp(int(player.berserkPoints) + points, 0, int(game::kBerserkPoints)));
        if (now == player.berserkPoints) return;
        player.berserkPoints = now;
        if (Peer* peer = peers->Find(world->PeerOf(player.entity))) SendMessage(*peer->connection, proto::BerserkPoints{now, from});
        database.Submit([character = player.selected, now](Database& db) { SaveBerserkPoints(db, character, now); });
    }
    // A player's killing hit, made while not in berserk: a roll for points, its share going
    // to every living member of an exp-shared party in the world, else to the player.
    void RollBerserk(PlayerWorld& player, const Monsters::Damaged& d, std::uint32_t monster) {
        if (player.berserkUntil || player.dead) return;
        const assets::ObjectRow* row = config.characters ? config.characters->Find(d.refObjId) : nullptr;
        const game::BerserkGain gain = game::BerserkPointGain(player.level, row != nullptr ? row->level : player.level, d.rarity,
                                                              int(player.combat.Sheet().Value(0xB8, 0.0f)));
        if (int(berserkRandom() % 10000u) >= gain.chance) return;
        if (parties && (parties->SettingsOf(player.entity) & game::kPartyExpShare) != 0) {
            for (const std::uint32_t member : parties->MemberEntities(player.entity)) {
                if (PlayerWorld* m = PlayerByEntity(member); m != nullptr && !m->dead) AddBerserkPoints(*m, gain.points, monster);
            }
            return;
        }
        AddBerserkPoints(player, gain.points, monster);
    }
    // The request: dead or in it already 3, transformed or
    // hidden 5 (ours: the original's body modes 6 and 7 as stealth), the gauge not full 1; then what
    // berserk ends ends (skc event 4: stealth), the five points go, and the mode runs its time.
    void Berserk(Peer& peer, PlayerWorld& player, Clock::time_point now) {
        proto::BerserkStatus status = proto::BerserkStatus::Ok;
        if (player.dead || player.berserkUntil) {
            status = proto::BerserkStatus::Busy;
        } else if (player.combat.transform.mode != 0 || player.hidden) {
            status = proto::BerserkStatus::Transformed;
        } else if (player.berserkPoints < game::kBerserkPoints) {
            status = proto::BerserkStatus::NotFull;
        }
        SendMessage(*peer.connection, proto::BerserkAnswer{status});
        if (status != proto::BerserkStatus::Ok) return;
        if (skillUnit) {
            SkillCaster caster = CasterOf(player);
            skillUnit->EndOnEvent(caster, 4);
        }
        AddBerserkPoints(player, -int(game::kBerserkPoints));
        // The mode's time: the keeper's 0x3C (ours: the base, and the hwan-time
        // bonus scrolls' seconds, which the keeper puts on 0x3C), plus a socket stone's 'hwit' (the keeper's 0xC3)
        // only when that lies in 1..100.
        const float hwit = player.combat.Sheet().Value(0xC3, 0.0f);
        const std::uint32_t ms = config.berserkMs + std::uint32_t(BonusBerserkSeconds(player)) * 1000u +
                                 (hwit > 0.0f && hwit <= 100.0f ? std::uint32_t(hwit) * 1000u : 0u);
        player.berserkUntil = now + std::chrono::milliseconds(ms);
        player.combat.berserk = true;
        StatsChanged(player.entity);
        TellBerserk(player, ms);
        core::LogInfo("{}: {} goes berserk", config.name, player.name);
    }
    void EndBerserk(PlayerWorld& player) {
        if (!player.berserkUntil) return;
        player.berserkUntil.reset();
        player.combat.berserk = false;
        if (player.entity == 0) return;
        StatsChanged(player.entity);
        TellBerserk(player, 0);
    }
    // Those who see it, and newcomers after its appearance (the original's body state 1).
    void TellBerserk(PlayerWorld& player, std::uint32_t ms) {
        const proto::EntityBerserk m{player.entity, std::uint8_t(ms > 0 ? 1 : 0), game::BerserkVariant(player.hwanLevel), ms};
        world->SetExtra(player.entity, proto::EntityBerserk::kId, ms > 0 ? proto::Encode(m) : std::vector<std::byte>{});
        Broadcast(player.entity, m);
    }
    void Berserks(Clock::time_point now) {
        for (auto& [peerId, player] : players) {
            if (player.berserkUntil && now >= *player.berserkUntil) EndBerserk(player);
        }
    }
    std::mt19937 berserkRandom{std::random_device{}()};

    // A resurrection offered (Skills' host): asked of the dead player; the answer raises it.
    void PickUpWaited(std::uint32_t entity, std::uint32_t item) override {
        for (auto& [peerId, player] : players) {
            if (player.entity != entity) continue;
            Peer* peer = peers->Find(peerId);
            if (peer == nullptr || player.dead || player.motion == proto::kMotionChanging) return;
            StandUp(player);
            player.traceTarget = 0;
            PickUp(*peer, player, proto::PickUp{item});
            return;
        }
    }
    void OfferRevival(std::uint32_t caster, std::uint32_t target, const assets::SkillRow& skill) override {
        PlayerWorld* player = PlayerByEntity(target);
        if (player == nullptr || !player->dead) return;
        player->revivalOffer = PlayerWorld::Offer{caster, &skill};
        if (Peer* peer = peers->Find(world->PeerOf(target))) SendMessage(*peer->connection, proto::RevivalOffered{caster, skill.id});
        core::LogInfo("{}: {} is offered {}", config.name, player->name, skill.codename);
    }
    // Its answer: up where it lies with the skill's HP and MP and its share of what the death took.
    void AnswerRevival(Peer& peer, PlayerWorld& player, bool accept) {
        if (!player.dead || !player.revivalOffer) return;
        const PlayerWorld::Offer offer = *player.revivalOffer;
        player.revivalOffer.reset();
        if (!accept || offer.skill == nullptr) {
            core::LogInfo("{}: {} declines the resurrection", config.name, player.name);
            return;
        }
        const game::Revival r = game::RevivalFor(game::ReviveOf(*offer.skill), player.stats.maxHp, player.stats.maxMp, player.deathLoss,
                                                 player.pvpState == std::uint8_t(game::PvpState::Murderer)); // a murderer's half
        player.deathLoss = 0;
        player.dead = false;
        player.hp = std::clamp<std::uint32_t>(r.hp, 1, player.stats.maxHp);
        player.mp = std::min(r.mp, player.stats.maxMp);
        player.untouchableUntil = Clock::now() + std::chrono::seconds(5);
        player.combat.StopFighting();
        world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
        SendVitals(peer, player);
        Broadcast(player.entity, proto::EntityDamaged{player.entity, 0, 0, player.hp, 0});
        Broadcast(player.entity, proto::SkillTook{player.entity, offer.caster, offer.skill->id});
        core::LogInfo("{}: {} is raised by {} ({} HP, {} MP, {} experience back)", config.name, player.name, offer.caster, player.hp, player.mp, r.experience);
        if (r.experience > 0) GainExperience(peer, player, r.experience, 0);
        else SendProgress(peer, player);
    }
    // A developer's death (K9's tests and tries): as a monster's hit kills.
    void DevDie(Peer& peer, PlayerWorld& player) {
        if (!config.devCommands) {
            core::LogInfo("{}: {} asked for a developer death; not a development server", config.name, peer.address);
            return;
        }
        if (player.dead) return;
        player.hp = 0;
        world->SetHp(player.entity, 0);
        Broadcast(player.entity, proto::EntityDamaged{player.entity, 0, 0, 0, proto::kDamageKilled});
        Die(player);
    }

    // The return point's arrival (the gate where it was set; else its country's first town).
    const assets::TeleportPoint* ReturnArrival(const PlayerWorld& player) const {
        if (!config.teleports) return nullptr;
        const std::uint32_t id = player.returnPoint != 0 ? player.returnPoint : player.wearer.country == 1 ? 20u : 1u; // GATE_EU, GATE_CH
        const assets::TeleportPoint* to = config.teleports->Point(id);
        return to == nullptr || !to->HasArrival() || (to->region & 0x8000) != 0 ? nullptr : to;
    }

    // Dead: up again (the death window's choice: 1 at the return
    // point, 2 where it lies, for characters up to level 10) with full health and mana (ours: the
    // original's resurrection amounts and its death penalty come with experience, C10c).
    void Resurrect(Peer& peer, PlayerWorld& player, std::uint8_t kind) {
        // In a Capture the Flag match the camp's revival is the match's (the death box's camp button).
        if (!flagReviving && flagWorlds && player.dead && flagWorlds->Resurrect(player.entity)) return;
        if (!flagReviving && arenas && player.dead && arenas->Resurrect(player.entity)) return; // the arena's gate, automatic
        if (!player.dead || (kind != 1 && kind != 2) || (kind == 2 && std::max(player.level, player.maxLevel) > 10 && !flagReviving)) return;
        // Dead in a fortress war: no rising in the first 15 s (silently), and the point by the side.
        std::string warGate;
        std::optional<proto::WorldPosition> warPlace;
        if (player.warDeathAt) {
            if (Clock::now() < *player.warDeathAt + std::chrono::seconds(15)) return;
            if (const auto where = world->Where(player.entity); where && fortresses) {
                warGate = fortresses->RevivalGate(player.selected, where->position.region);
                warPlace = fortresses->RevivalPlace(player.selected, where->position.region); // an attacker's war camp
            }
            player.warDeathAt.reset();
        }
        player.dead = false;
        player.revivalOffer.reset(); // what the death took is gone for good (ours: the original clears it on the way to town)
        player.deathLoss = 0;
        // Rising where it fell (the death window's choice 2): 40 % of its most health and mana, untouchable 6 s (11 s in a
        // fortress's area); else full, 5 s (ours: the town's rise is the return's, not traced).
        const bool inPlace = kind == 2 && !flagReviving;
        const auto fell = world->Where(player.entity);
        const bool fortressArea = fell && fortresses && fortresses->FortressArea(fell->position.region);
        player.hp = inPlace ? std::max<std::uint32_t>(1, std::uint32_t(double(player.stats.maxHp) * 0.4)) : player.stats.maxHp;
        player.mp = inPlace ? std::uint32_t(double(player.stats.maxMp) * 0.4) : player.stats.maxMp;
        player.untouchableUntil = Clock::now() + (inPlace ? std::chrono::milliseconds(fortressArea ? 11000 : 6000) : std::chrono::milliseconds(5000));
        if (inPlace && !player.combat.states.All().empty()) {
            player.combat.states.Clear(); // its states cleared
            TellStates(player.entity, {}, Clock::now());
        }
        player.combat.StopFighting();
        world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
        SendVitals(peer, player);
        // Those who see it see it stand (its health back).
        Broadcast(player.entity, proto::EntityDamaged{player.entity, 0, 0, player.hp, 0});
        const auto start = instances ? instances->StartOf(player.entity) : std::nullopt;
        const assets::TeleportPoint* town = ReturnArrival(player);
        if (kind == 2) {
            core::LogInfo("{}: {} rises where it fell", config.name, player.name);
        } else if (start && town != nullptr && instances->InForgottenWorld(player.entity) && CanTravelTo(town->region)) {
            // Dead in the Forgotten World, "return to town": out of the instance to its return point (the original's).
            core::LogInfo("{}: {} rises at {}, out of its instance", config.name, player.name, town->codename);
            DropHostility(player);
            const proto::WorldPosition to{town->region, town->x, town->y, town->z};
            instances->ExitSiege(player.entity, &to);
        } else if (start) {
            core::LogInfo("{}: {} rises at its instance's start", config.name, player.name);
            world->Teleport(player.entity, *start);
        } else if (warPlace) {
            core::LogInfo("{}: {} rises at its guild's war camp", config.name, player.name);
            world->Teleport(player.entity, *warPlace);
        } else if (const assets::TeleportPoint* gate = !warGate.empty() && config.teleports ? config.teleports->PointNamed(warGate) : nullptr; gate != nullptr && gate->HasArrival()) {
            core::LogInfo("{}: {} rises at {}", config.name, player.name, warGate);
            TravelTo(peer.id, player, {gate->region, gate->x, gate->y, gate->z});
        } else if (const assets::TeleportPoint* to = ReturnArrival(player)) {
            // Its return point's server is down: it rises where it fell.
            if (CanTravelTo(to->region)) {
                DropHostility(player); // a return to town
                core::LogInfo("{}: {} rises at {}", config.name, player.name, to->codename);
                TravelTo(peer.id, player, {to->region, to->x, to->y, to->z});
            }
        }
    }

    // Out of a fight (no hit taken for 10 s), health and mana come back by 3 % of their most every
    // 3 s (ours: the original's recovery is not traced).
    // --- The Action page ---------------------------------------------------------
    // Its motion to those who see it, itself too, and newcomers (none kept while it stands at a run).
    void TellMotion(PlayerWorld& player) {
        const proto::EntityMotion m{player.entity, player.motion, player.motionTo, std::uint8_t(player.walking ? 1 : 0)};
        const bool plain = player.motion == proto::kMotionStand && !player.walking;
        world->SetExtra(player.entity, proto::EntityMotion::kId, plain ? std::vector<std::byte>{} : proto::Encode(m));
        Broadcast(player.entity, m);
    }
    // The original's motion request (0x704F): walk and run set the pace of its moves;
    // sit sits a standing player down and stands a sitting one up, through kMotionChanging for 1.5 s. Nothing
    // while dead, changing or transformed; sitting stops a walk where it is and ends what it was doing.
    void SetMotion(PlayerWorld& player, std::uint8_t wanted, Clock::time_point now) {
        if (player.dead || player.motion == proto::kMotionChanging || player.combat.transform.mode != 0) return;
        if (wanted == proto::kMotionWalk || wanted == proto::kMotionRun) {
            // Not while a cast or a chain holds the motion, nor sitting.
            if (now < player.combat.castUntil || now < player.combat.chainUntil || player.motion == proto::kMotionSit) return;
            const bool walking = wanted == proto::kMotionWalk;
            if (walking == player.walking) return;
            player.walking = walking;
            world->SetWalking(player.entity, walking);
            TellMotion(player);
            return;
        }
        if (wanted != proto::kMotionSit) return;
        player.traceTarget = 0;
        player.combat.StopAttacking();
        player.pickTarget = 0;
        if (player.motion == proto::kMotionStand && world->Moving(player.entity)) {
            if (const auto place = world->Where(player.entity)) world->MoveTo(player.entity, place->position); // stops where it is
        }
        player.motionTo = player.motion == proto::kMotionSit ? proto::kMotionStand : proto::kMotionSit;
        player.motion = proto::kMotionChanging;
        player.motionUntil = now + std::chrono::milliseconds(std::int64_t(proto::kMotionChangeSeconds * 1000.0));
        TellMotion(player);
    }
    // A sitting player up at once (a walk, an attack, a cast of its own).
    void StandUp(PlayerWorld& player) {
        if (player.motion != proto::kMotionSit) return;
        player.motion = player.motionTo = proto::kMotionStand;
        TellMotion(player);
    }
    // Hit while sitting: it stands up, through the change.
    void HitWhileSitting(PlayerWorld& player, Clock::time_point now) {
        if (player.motion != proto::kMotionSit) return;
        player.motion = proto::kMotionChanging;
        player.motionTo = proto::kMotionStand;
        player.motionUntil = now + std::chrono::milliseconds(std::int64_t(proto::kMotionChangeSeconds * 1000.0));
        TellMotion(player);
    }
    // Sitting down or standing up done.
    void Motions(Clock::time_point now) {
        for (auto& [peerId, player] : players) {
            if (player.entity == 0 || player.motion != proto::kMotionChanging || now < player.motionUntil) continue;
            player.motion = player.motionTo;
            TellMotion(player);
        }
    }
    // The marks over its head (0x7402): the client sets the beginner's (only up to
    // level 19) and the helper's; the guild war's is the server's. Saved, and told around.
    void TellMarks(Peer& peer, PlayerWorld& player) {
        const proto::EntityMarks m{player.entity, player.marks};
        world->SetExtra(player.entity, proto::EntityMarks::kId, player.marks != 0 ? proto::Encode(m) : std::vector<std::byte>{});
        Broadcast(player.entity, m);
        SendMessage(*peer.connection, m); // itself, also before it shows in the world
    }
    // --- Murderers (game/Pk.h) ----------------------------------------------------------------

    // The wall clock in Unix seconds (the keeper's periods, the daily reset at local midnight; tests move it).
    std::int64_t WallClock() const {
        if (config.wallClock) return config.wallClock();
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    // The server's local calendar day of a Unix time (the original compares the local day).
    static std::int32_t LocalDay(std::int64_t unix) {
        const std::time_t t = std::time_t(unix);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
    }
    game::PvpSide PvpSideOf(const PlayerWorld& p, std::uint32_t standing) const {
        game::PvpSide s;
        s.level = p.level;
        s.state = game::PvpState(p.pvpState);
        s.hasRecords = !p.hostility.empty();
        s.dailyPk = p.dailyPk;
        s.points = p.pkPoints;
        s.battlefield = OnBattlefield(standing);
        return s;
    }
    // Two players as the attack check reads them (`target` the entity struck: the player, or his COS).
    game::PvpPair PvpPairOf(const PlayerWorld& a, const PlayerWorld& b, std::uint32_t target, game::AttackMode mode) const {
        game::PvpPair p;
        p.striker = PvpSideOf(a, a.entity);
        p.target = PvpSideOf(b, target);
        p.mode = mode;
        p.sameParty = parties && parties->PartyOf(a.entity) != 0 && parties->PartyOf(a.entity) == parties->PartyOf(b.entity);
        p.fortressFoes = FortressHostile(a.entity, b.entity);
        p.freePvpFoes = FreePvpHostile(a, b);
        p.guildWar = guilds && guilds->AtWar(a.selected, b.selected);
        p.pkProhibited = config.pkProhibited;
        p.hostileJobs = game::IsHostileJob(a.jobState, b.jobState);
        p.jobLock = Clock::now() < a.jobLockUntil;
        // Safe trade: between hostile jobs, skipped by free PvP's hostile teams; a
        // thief on the protected trader or his transport 0x3024, the trader on a thief 0x3006.
        if (p.hostileJobs && !p.freePvpFoes) {
            constexpr std::uint8_t thief = std::uint8_t(game::JobType::Thief);
            if (a.jobState == thief && SafeTrader(b)) p.safeTrade = proto::SkillUseStatus::SafeTrade;
            else if (b.jobState == thief && SafeTrader(a)) p.safeTrade = proto::SkillUseStatus::JobMonster;
        }
        p.strikerHolds = a.hostility.contains(b.entity);
        if (flagWorlds && flagWorlds->Hostile(a.entity, b.entity)) p.eventRelation = game::kEventFlagWorld;
        if (arenas && arenas->Hostile(a.entity, b.entity)) p.eventRelation = game::kEventArena;
        return p;
    }
    // A player's attack on a player or his COS: none when either is not a
    // living player of this world.
    std::optional<game::AttackVerdict> PvpVerdict(std::uint32_t attacker, std::uint32_t target, game::AttackMode mode) const {
        const PlayerWorld* a = PlayerByEntity(attacker);
        const PlayerWorld* b = PlayerByEntity(AsOwner(target));
        if (a == nullptr || b == nullptr || a == b || a->dead || b->dead) return std::nullopt;
        // In a Capture the Flag match its own test stands in: the
        // other team of one match, nothing else (relation 0x20).
        if ((flagWorlds && (flagWorlds->InMatch(attacker) || flagWorlds->InMatch(b->entity))) ||
            (arenas && (arenas->InMatch(attacker) || arenas->InMatch(b->entity)))) {
            // The Battle Arena's the same (relation 0x40).
            game::AttackVerdict v;
            v.allowed = (flagWorlds && flagWorlds->Hostile(attacker, b->entity)) || (arenas && arenas->Hostile(attacker, b->entity));
            v.code = v.allowed ? proto::SkillUseStatus::Ok : proto::SkillUseStatus::Target;
            return v;
        }
        // In a fortress's area at war its own test stands in: the war's sides alone.
        if (InWarArea(attacker) || InWarArea(b->entity)) {
            game::AttackVerdict v;
            v.allowed = FortressHostile(attacker, b->entity);
            v.code = v.allowed ? proto::SkillUseStatus::Ok : proto::SkillUseStatus::Target;
            return v;
        }
        return game::AttackCheck(PvpPairOf(*a, *b, target, mode));
    }
    bool InWarArea(std::uint32_t entity) const {
        const auto at = fortresses ? world->Where(entity) : std::nullopt;
        return at && fortresses->WarArea(at->position.region);
    }
    // A player who may be struck at all under the PK rules: on a battlefield, level 20 or more or not an innocent (who may
    // strike him is PvpVerdict's).
    bool PkTarget(const PlayerWorld& p) const { return OnBattlefield(p.entity) && (p.level >= game::kPkLevel || p.pvpState != 0); }

    // Its state changed (only on a change): those near told and newcomers after its
    // appearance (0x30BF channel 7); a murderer's keeper stops (its event 7 with state 2).
    void SetPvpState(PlayerWorld& p, game::PvpState state) {
        if (p.pvpState == std::uint8_t(state)) return;
        p.pvpState = std::uint8_t(state);
        if (p.entity == 0) return;
        const proto::EntityPvpState m{p.entity, p.pvpState};
        world->SetExtra(p.entity, proto::EntityPvpState::kId, p.pvpState != 0 ? proto::Encode(m) : std::vector<std::byte>{});
        Broadcast(p.entity, m);
        core::LogInfo("{}: {} is now {}", config.name, p.name,
                      state == game::PvpState::Murderer ? "a murderer" : state == game::PvpState::Assaulter ? "an assaulter" : "innocent");
        if (state == game::PvpState::Murderer && p.pkKeeper) {
            p.pkKeeper.reset();
            database.Submit([character = p.selected](Database& db) { SavePkKeeper(db, character, std::nullopt); });
        }
    }
    // Its counts to itself (0x30CE / 0x30D3 / 0x30CD) and saved (the save mask 0x200).
    void TellPk(PlayerWorld& p, bool save = true) {
        if (save) {
            database.Submit([character = p.selected, d = p.dailyPk, t = p.totalPk, pts = p.pkPoints](Database& db) {
                if (!SavePkCounts(db, character, d, t, pts)) core::LogWarning("the PK counts of {} not saved", character);
            });
        }
        if (p.entity == 0) return;
        if (Peer* peer = peers->Find(world->PeerOf(p.entity))) SendMessage(*peer->connection, proto::PkCounters{p.dailyPk, p.totalPk, std::uint32_t(p.pkPoints)});
    }
    // The points changed (clamped to 0..200,000): at 0 the state follows its
    // records (a dead player keeps 2 until the tick after he rises) and with TotalPK left the keeper starts its 48 h.
    void AddPkPoints(PlayerWorld& p, std::int32_t delta) {
        const std::int32_t was = p.pkPoints;
        p.pkPoints = std::clamp<std::int32_t>(p.pkPoints + delta, 0, game::kPenaltyMost);
        if (p.pkPoints == was) return;
        if (p.pkPoints > 0) {
            SetPvpState(p, game::PvpState::Murderer);
        } else {
            if (!p.dead) SetPvpState(p, game::StateFor(0, !p.hostility.empty()));
            if (p.totalPk > 0 && !p.pkKeeper) StartPkKeeper(p);
        }
    }
    void StartPkKeeper(PlayerWorld& p) {
        p.pkKeeper = CharacterRow::PkKeeper{game::kKeeperSeconds, WallClock()};
        database.Submit([character = p.selected, k = *p.pkKeeper](Database& db) { SavePkKeeper(db, character, k); });
        core::LogInfo("{}: {}'s PK keeper starts its 48 h (TotalPK {})", config.name, p.name, p.totalPk);
    }
    // A hit of `striker` on `victim`: one party erases the record; otherwise,
    // unless a legal foe or a natural enemy without records, the record set to 20 and an innocent striker an assaulter.
    // A hit taken refreshes the victim's own record on the striker.
    void RegisterHostility(PlayerWorld& striker, PlayerWorld& victim, std::uint32_t target) {
        if (&striker == &victim || InWarArea(striker.entity) || InWarArea(victim.entity)) return; // the fortress war's relation 1
        if (flagWorlds && (flagWorlds->InMatch(striker.entity) || flagWorlds->InMatch(victim.entity))) return; // relation 0x20: no records
        if (arenas && (arenas->InMatch(striker.entity) || arenas->InMatch(victim.entity))) return;             // relation 0x40: none either

        if (const auto it = victim.hostility.find(striker.entity); it != victim.hostility.end()) it->second = config.hostileSeconds;
        const game::PvpPair pair = PvpPairOf(striker, victim, target, game::AttackMode::Chosen);
        if (pair.sameParty) {
            striker.hostility.erase(victim.entity);
            return;
        }
        if (!game::HitMakesRecord(pair)) return;
        striker.hostility[victim.entity] = config.hostileSeconds;
        if (striker.pvpState == std::uint8_t(game::PvpState::Neutral)) SetPvpState(striker, game::PvpState::Assaulter);
    }
    // A return to town (the death's return, a return scroll): its records go; an
    // assaulter's id leaves the others' records too and he is innocent again; a murderer's only his own.
    void DropHostility(PlayerWorld& p) {
        const bool assaulter = p.pvpState == std::uint8_t(game::PvpState::Assaulter);
        p.hostility.clear();
        if (!assaulter) return;
        for (auto& [peerId, other] : players) other.hostility.erase(p.entity);
        SetPvpState(p, game::StateFor(p.pkPoints, false));
    }
    // Once a second: each record loses 1 (on a party member's 0) and goes at 0;
    // with one record left at its last second the state goes back at once; with none and alive, the state follows the
    // points. The keeper's periods; the daily reset at the server's local midnight for those online.
    Clock::time_point nextPvpTick{};
    std::int32_t pkDay = 0;
    void TickPvp(Clock::time_point now) {
        if (now < nextPvpTick) return;
        nextPvpTick = now + std::chrono::seconds(1);
        const std::int64_t unix = WallClock();
        const std::int32_t day = LocalDay(unix);
        const bool midnight = pkDay != 0 && day != pkDay;
        pkDay = day;
        for (auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            if (!p.hostility.empty()) {
                const std::uint32_t party = parties ? parties->PartyOf(p.entity) : 0u;
                for (auto it = p.hostility.begin(); it != p.hostility.end();) {
                    --it->second;
                    if (party != 0 && parties->PartyOf(it->first) == party) it->second = 0;
                    if (it->second <= 0) it = p.hostility.erase(it);
                    else ++it;
                }
                if (p.hostility.size() == 1 && p.hostility.begin()->second == 1 && p.pvpState == std::uint8_t(game::PvpState::Assaulter)) {
                    SetPvpState(p, game::StateFor(p.pkPoints, false));
                }
            }
            if (p.hostility.empty() && !p.dead) SetPvpState(p, game::StateFor(p.pkPoints, false));
            // The keeper runs with no points left: a whole period gone takes one TotalPK off.
            if (p.pkKeeper && p.pkPoints == 0) {
                const game::KeeperStep step = game::KeeperAfter(p.totalPk, p.pkKeeper->secondsLeft, unix - p.pkKeeper->stamp);
                if (step.taken > 0 || step.cleared) {
                    p.totalPk = std::uint16_t(step.totalPk);
                    if (step.cleared) {
                        p.pkKeeper.reset();
                        database.Submit([character = p.selected](Database& db) { SavePkKeeper(db, character, std::nullopt); });
                    } else {
                        p.pkKeeper = CharacterRow::PkKeeper{step.secondsLeft, unix};
                        database.Submit([character = p.selected, k = *p.pkKeeper](Database& db) { SavePkKeeper(db, character, k); });
                    }
                    core::LogInfo("{}: {}'s TotalPK goes down to {} (the keeper)", config.name, p.name, p.totalPk);
                    TellPk(p);
                }
            }
            if (midnight && p.dailyPk != 0) {
                p.dailyPk = 0;
                core::LogInfo("{}: {}'s DailyPK reset (a new day)", config.name, p.name);
                TellPk(p);
            }
        }
    }
    // At the entry: the counts and the keeper; the keeper catches up with
    // the time gone since its stamp; points give state 2; with none, TotalPK above 0 and no keeper the points become 1
    // ("Penalty ( record empty )") and he a murderer. Told to himself; his state kept for those who will see him.
    void EnterPvp(Peer& peer, PlayerWorld& p, const CharacterRow& row) {
        p.dailyPk = row.dailyPk;
        p.totalPk = row.totalPk;
        p.pkPoints = row.pkPoints;
        p.honorPoints = row.honorPoints;
        p.pkKeeper = row.pkKeeper;
        p.hostility.clear();
        p.pvpState = 0;
        bool changed = false;
        if (p.pkKeeper && p.pkPoints == 0) {
            const std::int64_t unix = WallClock();
            const game::KeeperStep step = game::KeeperAfter(p.totalPk, p.pkKeeper->secondsLeft, unix - p.pkKeeper->stamp);
            if (step.taken > 0 || step.cleared) {
                changed = changed || p.totalPk != step.totalPk;
                p.totalPk = std::uint16_t(step.totalPk);
                if (step.cleared) {
                    p.pkKeeper.reset();
                    database.Submit([character = p.selected](Database& db) { SavePkKeeper(db, character, std::nullopt); });
                } else {
                    p.pkKeeper = CharacterRow::PkKeeper{step.secondsLeft, unix};
                    database.Submit([character = p.selected, k = *p.pkKeeper](Database& db) { SavePkKeeper(db, character, k); });
                }
                core::LogInfo("{}: {}'s keeper caught up: TotalPK {}", config.name, p.name, p.totalPk);
            }
        }
        if (p.pkPoints <= 0 && p.totalPk > 0 && !p.pkKeeper) {
            core::LogWarning("{}: Penalty ( record empty ) [ID: {}]", config.name, p.name);
            p.pkPoints = 1;
            changed = true;
        }
        if (p.pkPoints > 0) p.pvpState = std::uint8_t(game::PvpState::Murderer);
        const proto::EntityPvpState m{p.entity, p.pvpState};
        world->SetExtra(p.entity, proto::EntityPvpState::kId, p.pvpState != 0 ? proto::Encode(m) : std::vector<std::byte>{});
        if (p.pvpState != 0) SendMessage(*peer.connection, m);
        if (changed) TellPk(p);
        else SendMessage(*peer.connection, proto::PkCounters{p.dailyPk, p.totalPk, std::uint32_t(p.pkPoints)});
    }

    // --- Free PvP -----------------------------

    // Two players in teams are foes when the teams differ or either is the Giraffe (5); members of one party never.
    bool FreePvpHostile(const PlayerWorld& a, const PlayerWorld& b) const {
        if (a.freePvp == 0 || b.freePvp == 0) return false;
        if (parties) {
            const std::uint32_t pa = parties->PartyOf(a.entity);
            if (pa != 0 && pa == parties->PartyOf(b.entity)) return false;
        }
        return a.freePvp != b.freePvp || a.freePvp == proto::kFreePvpTeams || b.freePvp == proto::kFreePvpTeams;
    }
    // The team as those around see it, kept for those who come into sight.
    void ShowFreePvp(const PlayerWorld& player) {
        const proto::EntityFreePvp m{player.entity, player.freePvp, std::uint8_t(player.freePvpAt ? 1 : 0), player.freePvpTo};
        world->SetExtra(player.entity, proto::EntityFreePvp::kId, player.freePvp != 0 || player.freePvpAt ? proto::Encode(m) : std::vector<std::byte>{});
        Broadcast(player.entity, m);
    }
    // A team asked for (0x7516): refused as (entering, leaving or changing
    // by the team it has), else the change starts: the player stops and stands for 10 s and those around
    // are told (0x3041); asking 0 while a change runs gives it up.
    // The kinds of place (game/Places.h): the instance worlds' regions from the original's tables, read at the start.
    game::PlaceTable places;
    // A player as the activity rules see it: its place, its fortress's war, its dealings and state.
    game::ActivityState ActivityOf(const PlayerWorld& player) const {
        game::ActivityState s;
        const auto place = world->Where(player.entity);
        if (place) {
            s.place = places.Of(place->position.region);
            s.fortressWar = fortresses && fortresses->WarArea(place->position.region);
        }
        s.alive = !player.dead;
        s.dealing = (stalls && (stalls->Owner(player.entity) || stalls->Visiting(player.entity))) || (exchanges && exchanges->Busy(player.entity));
        s.casting = Clock::now() < player.combat.castUntil;
        s.riding = player.riding;
        // A job's suit worn (the special dress's place).
        if (const game::ItemInstance* suit = player.inventory.At(game::slot::kJobSuit); suit != nullptr && config.rules->items) {
            const assets::ItemRow* row = config.rules->items->Find(suit->refId);
            s.jobSuit = row != nullptr && game::IsJobSuit(*row); // a league's own (the cape and the others are not)
        }
        return s;
    }
    void SetFreePvp(Peer& peer, PlayerWorld& player, std::uint8_t team) {
        const auto refuse = [&](proto::FreePvpCode code) {
            SendMessage(*peer.connection, proto::FreePvpAnswer{code});
            core::LogInfo("{}: {} free PvP {} refused (0x{:X})", config.name, player.name, team, std::uint16_t(code));
        };
        const auto refusal = [&] {
            if (player.freePvp == 0) return refuse(team != 0 ? proto::FreePvpCode::CannotEnter : proto::FreePvpCode::CannotChange);
            refuse(team == 0 ? proto::FreePvpCode::CannotLeave : proto::FreePvpCode::CannotChange);
        };
        if (team > proto::kFreePvpTeams) return refusal();
        if (player.freePvpAt) {
            if (team != 0) return refusal();
            return CancelFreePvp(peer, player);
        }
        if (team == player.freePvp) return refusal();
        // The place and the state (game::FreePvpRefusal): the original's refusals, a job's suit its own code (0x8007).
        if (const game::Refusal why = game::FreePvpRefusal(ActivityOf(player)); why != game::Refusal::None) {
            core::LogInfo("{}: {} may not change its free PvP team: {}", config.name, player.name, game::RefusalName(why));
            if (why == game::Refusal::JobSuit) return refuse(proto::FreePvpCode::JobState);
            return refusal();
        }
        if (const auto place = world->Where(player.entity); place && world->Moving(player.entity)) world->MoveTo(player.entity, place->position);
        player.freePvpTo = team;
        player.freePvpAt = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(config.freePvpSeconds));
        ShowFreePvp(player);
        core::LogInfo("{}: {} goes to free PvP team {} in {:.1f} s", config.name, player.name, team, config.freePvpSeconds);
    }
    // A change under way given up (the player's own 0, a walk, an attack, a cast, a pick-up, a death).
    void CancelFreePvp(Peer& peer, PlayerWorld& player) {
        if (!player.freePvpAt) return;
        player.freePvpAt.reset();
        ShowFreePvp(player);
        SendMessage(*peer.connection, proto::FreePvpAnswer{proto::FreePvpCode::Cancelled});
        core::LogInfo("{}: {} gives up its free PvP change", config.name, player.name);
    }
    // The changes whose 10 s ran out: the team taken.
    void FreePvps(Clock::time_point now) {
        for (auto& [peerId, player] : players) {
            if (player.entity == 0 || !player.freePvpAt || now < *player.freePvpAt) continue;
            player.freePvpAt.reset();
            player.freePvp = player.freePvpTo;
            ShowFreePvp(player);
            core::LogInfo("{}: {} is in free PvP team {}", config.name, player.name, player.freePvp);
        }
    }

    void SetMarks(Peer& peer, PlayerWorld& player, std::uint8_t wanted) {
        constexpr std::uint8_t kClients = proto::kMarkBeginner | proto::kMarkHelper;
        if ((wanted & proto::kMarkBeginner) != 0 && player.level > 19) return;
        const std::uint8_t marks = std::uint8_t((player.marks & ~kClients) | (wanted & kClients));
        if (marks == player.marks) return;
        player.marks = marks;
        database.Submit([character = player.selected, marks](Database& db) { SaveMarks(db, character, marks); });
        core::LogInfo("{}: {} marks {:#04x}", config.name, player.name, marks);
        TellMarks(peer, player);
    }
    // Trace (0x7074 action 3): another player it follows.
    void StartTrace(PlayerWorld& player, std::uint32_t target) {
        player.traceTarget = 0;
        if (target == 0 || target == player.entity || player.dead || PlayerByEntity(target) == nullptr) return;
        if (player.motion == proto::kMotionChanging) return;
        StandUp(player);
        player.combat.StopAttacking();
        player.pickTarget = 0;
        player.traceTarget = target;
        player.nextTrace = {};
        core::LogInfo("{}: {} traces entity {}", config.name, player.name, target);
    }
    // Each trace every 250 ms (ours): nearer when the target is farther than both bodies and kTraceFar,
    // stopping within both bodies and kTraceNear; ended when the target is gone, dead or out of sight.
    void Traces(Clock::time_point now) {
        const double bodies = 2.0 * double(Skills::kPlayerRadius);
        for (auto& [peerId, player] : players) {
            if (player.traceTarget == 0 || player.entity == 0 || now < player.nextTrace) continue;
            player.nextTrace = now + std::chrono::milliseconds(100);
            const PlayerWorld* target = PlayerByEntity(player.traceTarget);
            const auto me = world->Where(player.entity);
            const auto them = world->Where(player.traceTarget);
            if (player.dead || target == nullptr || target->dead || !me || !them) {
                player.traceTarget = 0;
                continue;
            }
            const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(them->position);
            const double d = std::hypot(b.x - a.x, b.z - a.z);
            // Given up beyond 1,000 (3D), as the original's.
            if (std::hypot(d, double(them->position.y - me->position.y)) > 1000.0) {
                player.traceTarget = 0;
                continue;
            }
            if (d <= bodies + double(proto::kTraceNear)) {
                if (world->Moving(player.entity)) world->MoveTo(player.entity, me->position); // there: stops
                continue;
            }
            if (!world->Moving(player.entity) && d <= bodies + double(proto::kTraceFar)) continue; // near enough while it stands
            // Towards the target, to kTraceNear short of its body; a walking tracer runs (as if it asked).
            if (player.walking) SetMotion(player, proto::kMotionRun, now);
            const double k = (d - (bodies + double(proto::kTraceNear) - 5.0)) / d;
            world->MoveTo(player.entity, proto::FromFlat({a.x + (b.x - a.x) * k, a.z + (b.z - a.z) * k}, them->position.y));
        }
    }

    void Recover(Clock::time_point now) {
        for (auto& [peerId, player] : players) {
            // Dead in a fortress war for three minutes: it goes as by its own choice.
            if (player.entity != 0 && player.dead && player.warDeathAt && now >= *player.warDeathAt + std::chrono::seconds(180)) {
                if (Peer* peer = peers->Find(peerId)) Resurrect(*peer, player, 1);
                continue;
            }
            if (player.entity == 0 || player.dead || now < player.nextRegen) continue;
            // The original's natural recovery (every 4 s): only
            // standing still or sitting; each of HP and MP max(1, its most x the rate / 100), at most half its
            // most; the rate 0.8 standing, 8 sitting (set by the motion).
            player.nextRegen = now + std::chrono::seconds(4);
            if (world->Moving(player.entity) || (player.motion != proto::kMotionStand && player.motion != proto::kMotionSit)) continue;
            if (player.hp >= player.stats.maxHp && player.mp >= player.stats.maxMp) continue;
            // The rates are the parameter keeper's 0x19 (HP) and 0x1A (MP), the motion's 0.8 / 8 their base: a toggle's
            // 0 stops MP's, recovery buffs and blues raise them.
            const float base = player.motion == proto::kMotionSit ? 8.0f : 0.8f;
            const double hpRate = std::max(0.0, double(player.combat.Sheet().Value(game::stat::kHpRecovery, base)));
            const double mpRate = std::max(0.0, double(player.combat.Sheet().Value(game::stat::kMpRecovery, base)));
            const auto gain = [](std::uint32_t most, double rate) {
                if (rate <= 0.0) return 0u; // a rate of 0 (a toggle's MP): none
                return std::min(most / 2, std::max(1u, std::uint32_t(double(most) * rate / 100.0)));
            };
            player.hp = std::min(player.stats.maxHp, player.hp + game::RecoveryCut(player.combat.states, gain(player.stats.maxHp, hpRate), true));
            player.mp = std::min(player.stats.maxMp, player.mp + game::RecoveryCut(player.combat.states, gain(player.stats.maxMp, mpRate), false));
            world->SetHp(player.entity, player.hp);
            world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
            if (Peer* peer = peers->Find(peerId)) SendVitals(*peer, player);
        }
    }

    // One of a stack used up: the place as it is now to its owner, and saved.
    // --- The item mall's services ---------------------------------------------------
    // Wear, the NPC's repair and the hammer; the equipment gender switch; the skin change; skill and mastery withdrawal
    // with the skill restoration potion; the magic cubes. The original's rules and codes (game/MallServices.h).

    std::mt19937 wearRandom{std::random_device{}()};
    // What each magic cube holds (by the cube's item id), its places to its items; read when its owner enters.
    std::unordered_map<std::int64_t, std::map<std::uint8_t, game::ItemInstance>> cubeItems;

    void SendService(Peer& peer, proto::ServiceRequest request, std::uint16_t code, std::uint32_t a = 0, std::uint32_t b = 0) {
        SendMessage(*peer.connection, proto::ServiceAnswer{request, code, a, b});
    }
    std::uint32_t MostDurabilityOf(const game::ItemInstance& item, const assets::ItemRow& row) const {
        return game::MostDurability(row, item, config.magicOptions.get());
    }
    // A place's durability told (the original's 0x3052) and saved; crossing 0 its numbers again (a broken item counts
    // for nothing, back above 0 it counts again).
    void TellDurability(PlayerWorld& player, std::uint8_t place, bool crossed) {
        const game::ItemInstance* item = player.inventory.At(place);
        if (item == nullptr) return;
        Peer* peer = peers->Find(world->PeerOf(player.entity));
        if (peer != nullptr) SendMessage(*peer->connection, proto::DurabilityChanged{place, item->durability});
        QueueItemSave(player.selected, player.inventory, {place}, {});
        if (crossed && place < game::slot::kWorn) UpdateStats(peer, player);
    }
    // A use's wear: a point of the weapon, of the
    // first armour place that rolls (none when it is empty), or of the shield, the chance 2 % x the use's hits.
    void WearDown(PlayerWorld& player, game::Wear kind, int count = 1) {
        if (!config.rules->items || player.dead) return;
        const auto worn = [&](int place) -> game::ItemInstance* {
            auto it = player.inventory.places.find(std::uint8_t(place));
            if (it == player.inventory.places.end() || it->second.durability == 0) return nullptr;
            const assets::ItemRow* row = config.rules->items->Find(it->second.refId);
            return row != nullptr && game::HasDurability(*row) ? &it->second : nullptr;
        };
        const int place = game::RollWear(kind, [&](int at) { return worn(at) != nullptr; }, count, wearRandom);
        if (place < 0) return;
        game::ItemInstance* item = worn(place);
        --item->durability;
        TellDurability(player, std::uint8_t(place), item->durability == 0);
    }
    void EquipmentWorn(std::uint32_t entity, int kind, int count) override {
        if (PlayerWorld* p = PlayerByEntity(entity)) WearDown(*p, game::Wear(kind), count);
    }

    // The NPC's repair (the original's 0x703E): one item (kind 1) or
    // everything worn and in the bag (kind 2), for gold.
    void NpcRepair(Peer& peer, PlayerWorld& player, const proto::Repair& r) {
        const auto refuse = [&](std::uint16_t code) { SendService(peer, proto::ServiceRequest::Repair, code); };
        const auto it = npcs.find(r.npc);
        const auto me = world->Where(player.entity), npc = world->Where(r.npc);
        if (it == npcs.end() || !npc || !game::IsRepairNpc(it->second->codename)) return refuse(game::repaircode::kNoNpc);
        const proto::FlatPosition a = me ? proto::ToFlat(me->position) : proto::FlatPosition{}, b = proto::ToFlat(npc->position);
        if (!me || std::hypot(a.x - b.x, a.z - b.z) > config.gateReach) return refuse(game::repaircode::kNoNpc);
        if (player.inventory.gold == 0) return refuse(game::repaircode::kNoGold);
        // One place (the smith's own: RepairPlace): its code (0 mended).
        bool statsChanged = false;
        std::uint64_t spent = 0;
        const auto mend = [&](std::uint8_t place, bool all) -> std::uint16_t {
            bool revived = false;
            const std::uint16_t code = RepairPlace(player, place, !all, spent, revived);
            if (code == kNothingRepaired) return all ? game::repaircode::kNotRepairable : game::repaircode::kNothingToMend;
            if (code != kRepaired) return code;
            TellDurability(player, place, false);
            statsChanged = statsChanged || (revived && place < game::slot::kWorn);
            return 0;
        };
        std::uint16_t code = 0;
        std::uint64_t before = player.inventory.gold;
        if (r.kind == 1) {
            code = mend(r.place, false);
        } else {
            // Every place in turn; it stops at the first that finds no gold.
            bool any = false;
            for (int place = 0; place < player.inventory.size; ++place) {
                if (place >= game::slot::kEarring && place < game::slot::kWorn) continue; // the accessories wear not
                if (player.inventory.At(std::uint8_t(place)) == nullptr) continue;
                const std::uint16_t c = mend(std::uint8_t(place), true);
                if (c == 0) any = true;
                if (c == game::repaircode::kNoGold) {
                    code = c;
                    break;
                }
            }
            if (code == 0 && !any) code = game::repaircode::kNothingToMend;
            if (code == game::repaircode::kNoGold && any) code = 0; // what the gold bought is mended
        }
        if (player.inventory.gold != before) {
            SendMessage(*peer.connection, proto::GoldChanged{player.inventory.gold});
            QueueGoldSave(player.selected, player.inventory.gold);
            core::LogInfo("{}: {} repairs for {} gold", config.name, player.name, before - player.inventory.gold);
        }
        if (statsChanged) UpdateStats(&peer, player);
        SendService(peer, proto::ServiceRequest::Repair, code);
    }

    // The repair hammer (TID 3/3/13/7): everything worn and in the bag that an NPC would repair
    // (game::Repairable: CanRepair, no "repair invalid", an honor item's charge left) mended to its most, free, a broken one
    // too, one Repair charge spent each; nothing to mend: 0x1888.
    bool UseHammer(Peer& peer, PlayerWorld& player, std::uint8_t slot, const assets::ItemRow& hammer) {
        bool any = false, statsChanged = false;
        const std::vector<assets::MagicOptionRow>* options = config.magicOptions.get();
        for (auto& [place, item] : player.inventory.places) {
            if (place >= game::slot::kEarring && place < game::slot::kWorn) continue;
            const assets::ItemRow* row = config.rules->items->Find(item.refId);
            if (row == nullptr || !game::HasDurability(*row) || !game::Repairable(*row, item, options)) continue;
            const std::uint32_t most = MostDurabilityOf(item, *row);
            if (item.durability >= most) continue;
            statsChanged = statsChanged || (item.durability == 0 && place < game::slot::kWorn);
            item.durability = most;
            bool charged = false;
            for (game::ItemInstance::Option& o : item.options) {
                const assets::MagicOptionRow* m = options != nullptr ? game::FindOption(*options, o.id) : nullptr;
                if (m != nullptr && m->optionName == "MATTR_REPAIR" && o.value > 1) {
                    --o.value;
                    charged = true;
                }
            }
            any = true;
            SendMessage(*peer.connection, proto::DurabilityChanged{place, item.durability});
            // A Repair charge spent: the item's options told again (the original's 0x3040 with mask 0x20).
            if (charged) {
                proto::ItemMoved told;
                told.from = told.to = place;
                told.changed.push_back(player.inventory.Record(place));
                SendMessage(*peer.connection, told);
            }
            QueueItemSave(player.selected, player.inventory, {place}, {});
        }
        if (!any) {
            SendService(peer, proto::ServiceRequest::Hammer, game::repaircode::kHammerNothing, hammer.refObjId);
            return false;
        }
        TakeOne(peer, player, slot);
        Broadcast(player.entity, proto::ItemUsed{player.entity, hammer.refObjId});
        if (statsChanged) UpdateStats(&peer, player);
        SendService(peer, proto::ServiceRequest::Hammer, 0);
        core::LogInfo("{}: {} mends everything with {}", config.name, player.name, hammer.codename);
        return true;
    }

    // An item used on another of the bag (UseItemOn): the gender switch tool on an armour; others' items
    // are left to their own handlers (false).
    bool UseItemOnService(Peer& peer, PlayerWorld& player, const proto::UseItemOn& use) {
        const game::ItemInstance* tool = use.place >= game::slot::kWorn ? player.inventory.At(use.place) : nullptr;
        const assets::ItemRow* toolRow = tool != nullptr && config.rules->items ? config.rules->items->Find(tool->refId) : nullptr;
        if (toolRow == nullptr || !game::IsGenderSwitchTool(*toolRow)) return false;
        const auto refuse = [&](std::uint16_t code) {
            SendService(peer, proto::ServiceRequest::UseOn, code, toolRow->refObjId);
            return true;
        };
        if (player.dead) return refuse(0x1889);
        game::ItemInstance* target = nullptr;
        const assets::ItemRow* row = nullptr;
        if (use.target >= game::slot::kWorn && use.target != use.place) {
            if (const auto it = player.inventory.places.find(use.target); it != player.inventory.places.end()) {
                target = &it->second;
                row = config.rules->items->Find(target->refId);
            }
        }
        const assets::ItemRow* other = row != nullptr && !row->linkedCodename.empty() ? config.rules->items->Find(std::string_view(row->linkedCodename)) : nullptr;
        if (const std::uint16_t code = game::GenderSwitchRefusal(*toolRow, use.target, player.inventory.size, row, other); code != 0) return refuse(code);
        // The item becomes its counterpart keeping everything else.
        target->refId = other->refObjId;
        core::LogInfo("{}: {} turns {} into {}", config.name, player.name, row->codename, other->codename);
        proto::ItemMoved moved;
        moved.from = moved.to = use.target;
        moved.changed.push_back(player.inventory.Record(use.target));
        SendMessage(*peer.connection, moved);
        QueueItemSave(player.selected, player.inventory, {use.target}, {});
        TakeOne(peer, player, use.place);
        Broadcast(player.entity, proto::ItemUsed{player.entity, toolRow->refObjId});
        SendService(peer, proto::ServiceRequest::UseOn, 0, other->refObjId);
        return true;
    }

    // The skin change scroll's window's OK: a model of the same race; another
    // sex with no armour, job suit or avatar on; the model and scale kept, the character shown again.
    void ChangeSkin(Peer& peer, PlayerWorld& player, const proto::SkinChange& s) {
        const game::ItemInstance* scroll = s.place >= game::slot::kWorn ? player.inventory.At(s.place) : nullptr;
        const assets::ItemRow* row = scroll != nullptr && config.rules->items ? config.rules->items->Find(scroll->refId) : nullptr;
        const auto refuse = [&](std::uint16_t code) { SendService(peer, proto::ServiceRequest::Skin, code); };
        if (row == nullptr || !game::IsSkinChangeScroll(*row)) return refuse(game::skincode::kWrongModel);
        if (player.dead) return refuse(0x1889);
        const auto now = config.rules->creation.FigureOf(player.model), next = config.rules->creation.FigureOf(s.model);
        if (!now || !next || now->race != next->race) return refuse(game::skincode::kWrongModel);
        if (now->male != next->male) {
            bool dressed = player.inventory.At(game::slot::kJobSuit) != nullptr || !player.inventory.avatars.empty();
            for (int place = game::slot::kHead; place <= game::slot::kFeet; ++place) dressed = dressed || player.inventory.At(std::uint8_t(place)) != nullptr;
            if (dressed) return refuse(game::skincode::kUndress);
        }
        player.model = s.model;
        player.wearer.male = next->male;
        world->SetModelLook(player.entity, s.model, s.scale);
        database.Submit([character = player.selected, model = s.model, scale = s.scale](Database& db) { SaveCharacterModel(db, character, model, scale); });
        TakeOne(peer, player, s.place);
        core::LogInfo("{}: {} changes its looks to model {} scale {}", config.name, player.name, s.model, s.scale);
        SendService(peer, proto::ServiceRequest::Skin, 0, s.model, s.scale);
        // Everyone near (the player too) sees the new one.
        LookChanged(player.entity);
    }

    // A skill or mastery withdrawn with the skill restoration potion (the original's 0x7202 / 0x7203): a potion a level,
    // the SP back, saved.
    void Withdraw(Peer& peer, PlayerWorld& player, const proto::Withdraw& w) {
        const auto refuse = [&](std::uint16_t code) { SendService(peer, proto::ServiceRequest::Withdraw, code, w.id); };
        const game::ItemInstance* potion = w.potion >= game::slot::kWorn ? player.inventory.At(w.potion) : nullptr;
        const assets::ItemRow* row = potion != nullptr && config.rules->items ? config.rules->items->Find(potion->refId) : nullptr;
        const game::WithdrawMode mode = row != nullptr ? game::WithdrawModeOf(*row) : game::WithdrawMode::None;
        if (mode == game::WithdrawMode::None || !config.skills) return refuse(game::withdrawcode::kFailed);
        const int highest = std::clamp(player.maxLevel, 1, int(jobRules.goldMin.empty() ? 1 : jobRules.goldMin.size() - 1));
        const std::uint32_t goldMin = jobRules.goldMin.empty() ? 0 : jobRules.goldMin[std::size_t(highest)];
        const game::Withdrawal out = w.mastery != 0 ? game::WithdrawMastery(player.book, *config.skills, config.masteryCosts, w.id, w.level, mode, goldMin)
                                                    : game::WithdrawSkill(player.book, *config.skills, w.id, w.level, mode, goldMin);
        if (out.code != 0) return refuse(out.code);
        if (potion->count < out.levels) return refuse(game::withdrawcode::kNoPotion);
        if (player.inventory.gold < out.gold) return refuse(game::withdrawcode::kNoGold);
        // The potions, the gold, the levels; the SP back.
        for (int k = 0; k < out.levels; ++k) TakeOne(peer, player, w.potion);
        if (out.gold > 0) {
            player.inventory.gold -= out.gold;
            SendMessage(*peer.connection, proto::GoldChanged{player.inventory.gold});
            QueueGoldSave(player.selected, player.inventory.gold);
        }
        player.skillPoints += out.skillPoints;
        std::uint32_t told = 0;
        if (w.mastery != 0) {
            player.book.masteries[w.id] = w.level;
            told = w.level;
            CharacterWrite(player.selected, [character = player.selected, mastery = w.id, level = w.level, p = ProgressOf(player)](Database& db) {
                SaveMastery(db, character, mastery, level);
                SaveProgress(db, {p});
            });
        } else {
            const assets::SkillRow* skill = config.skills->Find(w.id);
            const assets::SkillRow* had = config.skills->GroupLevel(skill->group, player.book.GroupLevel(skill->group));
            if (w.level == 0) {
                player.book.groups.erase(skill->group);
            } else {
                player.book.groups[skill->group] = w.level;
            }
            told = out.skillNow;
            CharacterWrite(player.selected, [character = player.selected, previous = had != nullptr ? had->id : 0u, now = out.skillNow, p = ProgressOf(player)](Database& db) {
                SaveSkillWithdrawal(db, character, previous, now);
                SaveProgress(db, {p});
            });
        }
        player.progressDirty = false;
        core::LogInfo("{}: {} withdraws {} {} by {} levels, {} SP back", config.name, player.name, w.mastery != 0 ? "mastery" : "skill", w.id, out.levels,
                      out.skillPoints);
        SendService(peer, proto::ServiceRequest::Withdraw, 0, w.id, told);
        SendSkillBook(peer, player);
        UpdateStats(&peer, player); // a passive withdrawn
        SendProgress(peer, player);
    }

    // The magic cubes (collect, extract): an elixir of the cube's kind dropped on it goes in (its
    // stack in one of the cube's places: ours, the original's elixirs do not stack), one taken out to the bag's first
    // free place.
    void SendCube(Peer& peer, std::int64_t cube, std::uint8_t place) {
        proto::CubeContents c;
        c.cube = place;
        if (const auto it = cubeItems.find(cube); it != cubeItems.end()) {
            for (const auto& [slot, item] : it->second) {
                game::Inventory one;
                one.size = game::Inventory::kMaxSize;
                one.places[slot] = item;
                c.items.push_back(one.Record(slot));
            }
        }
        SendMessage(*peer.connection, c);
    }
    void MoveCube(Peer& peer, PlayerWorld& player, const proto::CubeMove& m) {
        const auto refuse = [&](std::uint16_t code) { SendService(peer, proto::ServiceRequest::Cube, code); };
        game::ItemInstance* cube = nullptr;
        if (m.cube >= game::slot::kWorn) {
            if (const auto it = player.inventory.places.find(m.cube); it != player.inventory.places.end()) cube = &it->second;
        }
        const assets::ItemRow* cubeRow = cube != nullptr ? config.rules->items->Find(cube->refId) : nullptr;
        if (cubeRow == nullptr || !game::IsMagicCube(*cubeRow)) return refuse(game::cubecode::kNotTaken);
        const auto stored = cubeItems.find(cube->serial);
        if (stored == cubeItems.end()) {
            // A cube that came into the bag since the entry (the mall, the storage, a gift): its items read now (CubeContents
            // follows); this move refused meanwhile.
            LoadCubes(world->PeerOf(player.entity), player);
            return refuse(game::cubecode::kNoStore);
        }
        auto& inside = stored->second;
        const std::int64_t cubeId = cube->serial;
        if (m.kind == 0) {
            const auto from = m.from >= game::slot::kWorn && m.from != m.cube ? player.inventory.places.find(m.from) : player.inventory.places.end();
            const assets::ItemRow* row = from != player.inventory.places.end() ? config.rules->items->Find(from->second.refId) : nullptr;
            if (row == nullptr || !game::CubeTakes(*cubeRow, *row)) return refuse(game::cubecode::kNotTaken);
            int slot = -1;
            for (int k = 0; k < game::CubePlaces(*cubeRow); ++k) {
                if (!inside.contains(std::uint8_t(k))) {
                    slot = k;
                    break;
                }
            }
            if (slot < 0) return refuse(game::cubecode::kFull);
            game::ItemInstance item = std::move(from->second);
            player.inventory.places.erase(from);
            inside[std::uint8_t(slot)] = item;
            proto::ItemMoved moved;
            moved.from = moved.to = m.from;
            moved.emptied.push_back(m.from);
            SendMessage(*peer.connection, moved);
            StoredItem s = StoredOf(std::uint8_t(slot), item);
            database.SubmitBarrier([side = SaveSideOf(player.selected, player.inventory, {m.from}), cubeId, slot, s](Database& db) {
                SaveCubeChange(db, side, cubeId, std::uint8_t(slot), &s, true);
            });
        } else {
            const auto it = inside.find(m.slot);
            if (it == inside.end()) return refuse(game::cubecode::kNotTaken);
            const int place = player.inventory.FreeBagPlace();
            if (place < 0) return refuse(game::cubecode::kBagFull);
            player.inventory.places[std::uint8_t(place)] = std::move(it->second);
            inside.erase(it);
            proto::ItemMoved moved;
            moved.from = moved.to = std::uint8_t(place);
            moved.changed.push_back(player.inventory.Record(std::uint8_t(place)));
            SendMessage(*peer.connection, moved);
            database.SubmitBarrier([side = SaveSideOf(player.selected, player.inventory, {std::uint8_t(place)}), cubeId, slot = m.slot](Database& db) {
                SaveCubeChange(db, side, cubeId, slot, nullptr, false);
            });
        }
        SendCube(peer, cubeId, m.cube);
        SendService(peer, proto::ServiceRequest::Cube, 0);
        if (quests) quests->BagChanged(player.entity);
    }
    // A magic cube standing for an elixir in a strengthening (U7-03): its store's first elixir; one used out of its last place.
    std::optional<game::ItemInstance> AlchemyCubeElixir(std::uint32_t entity, std::uint8_t cube) override {
        const PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return std::nullopt;
        const auto c = p->inventory.places.find(cube);
        if (c == p->inventory.places.end()) return std::nullopt;
        const auto it = cubeItems.find(c->second.serial);
        if (it == cubeItems.end() || it->second.empty()) return std::nullopt;
        return it->second.begin()->second;
    }
    bool AlchemyCubeUseOne(std::uint32_t entity, std::uint8_t cube) override {
        PlayerWorld* p = PlayerByEntity(entity);
        if (p == nullptr) return false;
        const auto c = p->inventory.places.find(cube);
        if (c == p->inventory.places.end()) return false;
        const std::int64_t cubeId = c->second.serial;
        const auto it = cubeItems.find(cubeId);
        if (it == cubeItems.end() || it->second.empty()) return false;
        auto last = std::prev(it->second.end());
        const std::uint8_t slot = last->first;
        const std::int64_t serial = last->second.serial;
        std::optional<StoredItem> left;
        if (last->second.count > 1) {
            --last->second.count;
            left = StoredOf(slot, last->second);
        } else {
            it->second.erase(last);
        }
        database.SubmitBarrier([cubeId, slot, serial, left](Database& db) { SaveCubeUse(db, cubeId, slot, serial, left ? &*left : nullptr); });
        if (Peer* peer = peers->Find(world->PeerOf(entity))) SendCube(*peer, cubeId, cube);
        return true;
    }
    // A player entering: its cubes' items read and told.
    // Any cube in the bag whose items are not held yet (it came in since the entry): read now.
    void LoadNewCubes(const PlayerWorld& player) {
        if (!config.rules->items) return;
        for (const auto& [place, item] : player.inventory.places) {
            const assets::ItemRow* row = config.rules->items->Find(item.refId);
            if (row != nullptr && game::IsMagicCube(*row) && item.serial != 0 && !cubeItems.contains(item.serial)) {
                LoadCubes(world->PeerOf(player.entity), player);
                return;
            }
        }
    }
    void LoadCubes(std::uint64_t peerId, const PlayerWorld& player) {
        std::vector<std::pair<std::int64_t, std::uint8_t>> cubes;
        for (const auto& [place, item] : player.inventory.places) {
            const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(item.refId) : nullptr;
            if (row != nullptr && game::IsMagicCube(*row) && item.serial != 0) cubes.push_back({item.serial, place});
        }
        if (cubes.empty()) return;
        // After every write before it (a cube's last change saved): what it reads is current.
        database.SubmitBarrier([this, peerId, cubes, character = player.selected](Database& db) {
            std::vector<std::int64_t> ids;
            for (const auto& c : cubes) ids.push_back(c.first);
            bool ok = false;
            auto read = std::make_shared<std::unordered_map<std::int64_t, std::vector<StoredItem>>>(LoadCubeItems(db, ids, ok));
            completions->Post([this, peerId, cubes, character, read] {
                const auto p = players.find(peerId);
                Peer* peer = peers->Find(peerId);
                if (p == players.end() || p->second.selected != character || peer == nullptr) return;
                for (const auto& [id, place] : cubes) {
                    // One already held in memory is newer than the rows (a load for another cube that came in later).
                    const auto [held, fresh] = cubeItems.try_emplace(id);
                    auto& inside = held->second;
                    if (!fresh) {
                        SendCube(*peer, id, place);
                        continue;
                    }
                    if (const auto it = read->find(id); it != read->end()) {
                        for (const StoredItem& s : it->second) inside[s.slot] = CosItemOf(s);
                    }
                    SendCube(*peer, id, place);
                }
            });
        });
    }

    void TakeOne(Peer& peer, PlayerWorld& player, std::uint8_t slot) {
        game::ItemInstance& stack = player.inventory.places[slot];
        proto::ItemMoved answer;
        answer.from = answer.to = slot;
        std::vector<std::int64_t> gone;
        if (stack.count > 1) {
            --stack.count;
            answer.changed.push_back(player.inventory.Record(slot));
        } else {
            gone.push_back(stack.serial);
            player.inventory.places.erase(slot);
            answer.emptied.push_back(slot);
        }
        SendMessage(*peer.connection, answer);
        QueueItemSave(player.selected, player.inventory, {slot}, gone);
    }
    // Item saves, per character, one at a time (each writes the places changed since the last, as
    // they are then); kept past the player's leaving until written.
    struct ItemSaves {
        bool busy = false;
        int failures = 0; // saves in a row that did not land (written again up to kSaveRetries times)
        std::set<std::uint8_t> dirty;
        std::vector<std::int64_t> gone;
        std::map<std::uint8_t, game::ItemInstance> places; // the inventory as of the last move
        // The avatar places as of the last move, and those changed since the last save.
        std::set<std::uint8_t> avatarDirty;
        std::map<std::uint8_t, game::ItemInstance> avatars;
    };
    std::unordered_map<std::int64_t, ItemSaves> itemSaves;

    // --- A character's own saves, in their order (the audit's U20-13) ---
    // The pool runs a task on whichever of its threads is free, so two saves of one character's row could commit older-last
    // (the original runs its jobs on one connection, in turn). Every save of a character's own rows goes through its writer:
    // one task of it in flight at a time, the rest behind it in the order asked. The gold, the progress and the bag's places
    // are written as they are when their task's turn comes (one of each waiting at most: the newest holds those between).
    // A barrier save elsewhere (an exchange, a deal, a storage move) lands after the task in flight and before those
    // waiting, which then write what is newer still. Kept past the player's leaving until done.
    struct CharacterWrites {
        struct Task {
            std::function<std::function<void(Database&)>()> start; // on the server's thread at its turn: the database's part
            std::function<void()> done;
            bool barrier = false;
        };
        std::deque<Task> tasks;
        bool busy = false;
        bool goldQueued = false, progressQueued = false;
        // A trade under way (HoldGold): its gold written with the transport's goods by the transport's save (cartQueued),
        // not alone before it; a gold change meanwhile pending until then.
        bool goldHeld = false, goldPending = false, cartQueued = false;
        std::uint64_t gold = 0;      // the newest the game holds
        CharacterProgress progress;  // the same
        std::vector<std::function<void()>> drained; // once nothing is waiting or in flight
    };
    std::unordered_map<std::int64_t, CharacterWrites> characterWrites;
    void CharacterTask(std::int64_t character, std::function<std::function<void(Database&)>()> start, std::function<void()> done = {},
                       bool barrier = false) {
        CharacterWrites& w = characterWrites[character];
        w.tasks.push_back({std::move(start), std::move(done), barrier});
        if (!w.busy) NextCharacterWrite(character);
    }
    void CharacterWrite(std::int64_t character, std::function<void(Database&)> task, std::function<void()> done = {}, bool barrier = false) {
        CharacterTask(character, [task = std::move(task)]() mutable { return std::move(task); }, std::move(done), barrier);
    }
    void NextCharacterWrite(std::int64_t character) {
        const auto it = characterWrites.find(character);
        if (it == characterWrites.end()) return;
        if (it->second.tasks.empty()) {
            std::vector<std::function<void()>> drained = std::move(it->second.drained);
            characterWrites.erase(it);
            for (auto& then : drained) then();
            return;
        }
        it->second.busy = true;
        CharacterWrites::Task t = std::move(it->second.tasks.front());
        it->second.tasks.pop_front();
        std::function<void(Database&)> work = t.start ? t.start() : nullptr;
        auto finished = [this, character, done = std::move(t.done)] {
            if (done) done();
            NextCharacterWrite(character);
        };
        if (!work) {
            completions->Post(std::move(finished));
            return;
        }
        auto run = [this, work = std::move(work), finished = std::move(finished)](Database& db) {
            work(db);
            completions->Post(finished);
        };
        if (t.barrier) {
            database.SubmitBarrier(std::move(run));
        } else {
            database.Submit(std::move(run));
        }
    }
    // The gold as the game holds it now, written in the character's order.
    void QueueGoldSave(std::int64_t character, std::uint64_t gold) {
        CharacterWrites& w = characterWrites[character];
        w.gold = gold;
        if (w.goldHeld) {
            w.goldPending = true;
            return;
        }
        if (w.goldQueued) return;
        w.goldQueued = true;
        CharacterTask(character, [this, character]() -> std::function<void(Database&)> {
            CharacterWrites& w = characterWrites[character];
            w.goldQueued = false;
            return [this, character, gold = w.gold](Database& db) {
                if (!SaveGold(db, character, gold)) completions->Post([this, character] { SaveFailed(character, "the gold"); });
            };
        });
    }
    // A trade with the transport (a purchase, a sale): its gold changes are not written alone while it runs; the
    // transport's save it asks for writes them with the goods, in one transaction (the original's _STRG_ADD_SPECIALTY and
    // _STRG_DEL_ITEM with RemainGold). Released after it: a change no transport's save took is queued then.
    void HoldGold(std::int64_t character) { characterWrites[character].goldHeld = true; }
    void ReleaseGold(std::int64_t character) {
        const auto it = characterWrites.find(character);
        if (it == characterWrites.end()) return;
        it->second.goldHeld = false;
        if (std::exchange(it->second.goldPending, false)) return QueueGoldSave(character, it->second.gold);
        if (!it->second.busy && it->second.tasks.empty()) NextCharacterWrite(character); // nothing asked: the writer let go
    }
    // A barrier save wrote the gold as the game holds it now: a gold save still waiting writes nothing older.
    void GoldNow(std::int64_t character, std::uint64_t gold) {
        if (const auto it = characterWrites.find(character); it != characterWrites.end()) it->second.gold = gold;
    }
    // The experience, level, points and stats as they are now, written in the character's order.
    void QueueProgressSave(const PlayerWorld& player) {
        CharacterWrites& w = characterWrites[player.selected];
        w.progress = ProgressOf(player);
        if (w.progressQueued) return;
        w.progressQueued = true;
        CharacterTask(player.selected, [this, character = player.selected]() -> std::function<void(Database&)> {
            CharacterWrites& w = characterWrites[character];
            w.progressQueued = false;
            return [this, p = w.progress](Database& db) {
                if (!SaveProgress(db, {p})) completions->Post([this, character = p.character] { SaveFailed(character, "the progress"); });
            };
        });
    }
    // `then` once the character's saves asked so far have landed (none waiting: at once).
    void AfterCharacterWrites(std::int64_t character, std::function<void()> then) {
        const auto it = characterWrites.find(character);
        if (it == characterWrites.end()) return then();
        it->second.drained.push_back(std::move(then));
    }
    // A save of the character's that did not land (the database refused it, or was gone): its session ends, as the
    // original logs the player out when a job fails ("AQ Failed! Log out!!"), so memory and the database cannot part
    // for good; its next entry reads what the database holds (the bag's places are written again first, a few times).
    static constexpr int kSaveRetries = 3;
    void SaveFailed(std::int64_t character, const char* what) {
        core::LogWarning("{}: {} of character {} not saved: its session ends", config.name, what, character);
        std::uint64_t peerId = 0;
        if (PlayerOfCharacter(character, &peerId) == nullptr) return;
        Disconnect(peerId, proto::DisconnectReason::Kicked, "the server could not save your character; please log in again");
    }
    // A payment with its work in one transaction (Payment.h), in the character's order: the fee taken from the gold at
    // its turn, the gold column written with the work (as the game holds it then, less the fee, plus the credit); the
    // fee given back when it does not stand, the credit given once it has. A player gone meanwhile has them in its
    // logout's save still waiting (that reads the writer's gold at its turn).
    void Payment(std::int64_t character, std::uint64_t fee, PaymentWork work, PaymentDone done, bool barrier = false) {
        struct Outcome {
            bool taken = false, ok = false;
            std::uint64_t credit = 0;
        };
        auto outcome = std::make_shared<Outcome>();
        CharacterTask(
            character,
            [this, character, fee, work = std::move(work), outcome]() -> std::function<void(Database&)> {
                std::uint64_t peerId = 0;
                PlayerWorld* p = PlayerOfCharacter(character, &peerId);
                if (p == nullptr || p->inventory.gold < fee) return {};
                p->inventory.gold -= fee;
                outcome->taken = true;
                if (fee != 0) {
                    if (Peer* peer = peers->Find(peerId)) SendMessage(*peer->connection, proto::GoldChanged{p->inventory.gold});
                }
                GoldNow(character, p->inventory.gold);
                return [character, gold = p->inventory.gold, work, outcome](Database& db) {
                    std::string error;
                    std::uint64_t credit = 0;
                    outcome->ok = db.Transaction(
                        [&](Database& tx) {
                            const std::optional<std::uint64_t> got = work(tx);
                            if (!got) return false;
                            credit = *got;
                            const QueryResult q = tx.Query("UPDATE _char SET remaingold = $2 WHERE charid = $1", {character, std::int64_t(gold + credit)});
                            return q.Ok() && q.Affected() == 1;
                        },
                        error);
                    outcome->credit = outcome->ok ? credit : 0;
                };
            },
            [this, character, fee, outcome, done = std::move(done)] {
                const std::uint64_t back = (outcome->taken && !outcome->ok ? fee : 0) + outcome->credit;
                if (back != 0) {
                    std::uint64_t peerId = 0;
                    if (PlayerWorld* p = PlayerOfCharacter(character, &peerId)) {
                        p->inventory.gold += back;
                        if (Peer* peer = peers->Find(peerId)) SendMessage(*peer->connection, proto::GoldChanged{p->inventory.gold});
                        QueueGoldSave(character, p->inventory.gold);
                    } else if (const auto w = characterWrites.find(character); w != characterWrites.end()) {
                        w->second.gold += back;
                    }
                }
                if (done) done(outcome->ok, outcome->credit);
            },
            barrier);
    }
    std::vector<std::int64_t> freeItemIds; // ids for new items, taken from the database in blocks
    bool reservingIds = false;
    std::unordered_map<std::uint64_t, PlayerWorld> players;
    std::unordered_map<std::string, std::uint64_t> byName; // NameKey -> peer, for the players in the world

    // --- The fortress war (FortressHost) --------------------------------

    FortressActor FortressActorOf(std::uint64_t peerId, const PlayerWorld& player) const {
        return {peerId, player.entity, player.selected, player.level, player.name, player.inventory.gold, player.dead};
    }
    static std::optional<FortressGuild> AsFortressGuild(const std::optional<Guilds::Facts>& f) {
        if (!f) return std::nullopt;
        return FortressGuild{f->id, f->name, f->level, f->members, f->alliance, f->master, f->masterName, f->crest, f->points};
    }
    void FortressSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override { GuildSend(peerId, id, payload); }
    void FortressBroadcast(proto::MessageId id, const std::vector<std::byte>& payload) override {
        for (const auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            if (Peer* listener = peers->Find(peerId)) listener->connection->Send(id, payload);
        }
    }
    std::optional<FortressGuild> FortressGuildOf(std::int64_t character) const override {
        return guilds ? AsFortressGuild(guilds->FactsOf(character)) : std::nullopt;
    }
    std::optional<FortressGuild> FortressGuildById(std::uint32_t guild) const override {
        return guilds ? AsFortressGuild(guilds->FactsOfGuild(guild)) : std::nullopt;
    }
    std::string FortressNpc(std::uint32_t entity) const override {
        const auto it = npcs.find(entity);
        if (it != npcs.end()) return it->second->codename;
        // An instance's own NPC (the Cave's teleporter: TID 1/2/2).
        const std::uint32_t ref = world->ModelOf(entity);
        const assets::ObjectRow* row = ref != 0 && config.characters ? config.characters->Find(ref) : nullptr;
        return row != nullptr && row->typeId[0] == 1 && row->typeId[1] == 2 && row->typeId[2] == 2 ? row->codename : std::string();
    }
    double FortressDistance(std::uint32_t a, std::uint32_t b) const override { return GuildDistance(a, b); }
    bool FortressTakeGold(std::uint64_t peerId, std::uint64_t amount) override { return TakeGold(peerId, amount); }
    void FortressPay(std::int64_t character, std::uint64_t fee, std::uint32_t guild, std::uint32_t points, PaymentWork work, PaymentDone done) override {
        if (points == 0) return Payment(character, fee, std::move(work), std::move(done));
        if (!guilds || !guilds->PayWithPoints(guild, points, character, fee, std::move(work), done)) {
            if (done) done(false, 0);
        }
    }
    void FortressGiveGold(std::uint64_t peerId, std::uint64_t amount) override { GiveGold(peerId, amount); }
    void FortressTaxRateChanged(std::uint32_t fortress, std::uint32_t guild, int rate) override { taxRules[fortress] = {guild, rate}; }
    std::uint32_t SpawnStructure(const FortressSpec::Structure& spot, std::uint32_t refObjId, std::uint32_t hp, std::uint32_t maxHp, bool destroyed) override {
        return monsters ? monsters->AddFixed(refObjId, spot.position, spot.yaw, hp, maxHp, StructureShapeOf(refObjId).body, destroyed) : 0u;
    }
    // A structure's size from its model, read once for each kind. The database's radius for structures is 0
    // (BCRadius), so a walk into reach would end inside the tower. Ours: its body the narrower half of the
    // model's footprint and a quarter more (a walk stops at 0.8 of the reach); a click takes it in a cylinder
    // of the wider half and the model's height.
    StructureShape StructureShapeOf(std::uint32_t refObjId) override {
        const auto known = structureShapes.find(refObjId);
        if (known != structureShapes.end()) return known->second;
        StructureShape shape;
        const assets::ObjectRow* row = config.characters ? config.characters->Find(refObjId) : nullptr;
        const std::string path = row != nullptr && config.files != nullptr ? config.characters->ResolveModel(*config.files, *row) : std::string();
        if (!path.empty()) {
            assets::ModelData model;
            std::string error;
            if (assets::LoadModel(*config.files, path, model, error)) {
                const float width = model.bounds.max[0] - model.bounds.min[0], depth = model.bounds.max[2] - model.bounds.min[2];
                shape.minX = model.bounds.min[0];
                shape.maxX = model.bounds.max[0];
                shape.minZ = model.bounds.min[2];
                shape.maxZ = model.bounds.max[2];
                shape.body = std::min(width, depth) / 2.0f * 1.25f + 8.0f;
                shape.pickRadius = std::uint16_t(std::clamp(std::max(width, depth) / 2.0f, 1.0f, 2000.0f));
                shape.pickHeight = std::uint16_t(std::clamp(model.bounds.max[1] - model.bounds.min[1], 1.0f, 4000.0f));
                // Its navigation object's outline: what an attack's reach is measured to.
                assets::ObjectNavMesh nav;
                std::string navError;
                if (assets::LoadObjectNavMesh(*config.files, path, nav, navError)) {
                    for (const assets::ObjectNavMesh::Edge& e : nav.outline) {
                        if (e.a >= nav.vertices.size() || e.b >= nav.vertices.size()) continue;
                        const auto& va = nav.vertices[e.a];
                        const auto& vb = nav.vertices[e.b];
                        shape.outline.push_back({va[0], va[1], va[2], vb[0], vb[1], vb[2]});
                    }
                }
            } else {
                core::LogWarning("fortress war: {}: {}", path, error);
            }
        }
        core::LogInfo("fortress war: the structure {}: body {:.0f}, clicked in {} x {}, {} outline edges", row != nullptr ? row->codename : std::to_string(refObjId),
                      shape.body, shape.pickRadius, shape.pickHeight, shape.outline.size());
        structureShapes[refObjId] = shape;
        return shape;
    }
    std::unordered_map<std::uint32_t, StructureShape> structureShapes;
    std::unordered_map<std::int64_t, std::uint32_t> rankBuffs; // a character -> its battle rank's buff's token
    std::pair<std::string, std::string> FortressRankBuff(std::int64_t character, std::uint32_t oldSkill, std::uint32_t newSkill) override {
        PlayerWorld* player = nullptr;
        for (auto& [peerId, p] : players) {
            if (p.selected == character && p.entity != 0) player = &p;
        }
        if (player == nullptr || !skillUnit) return {};
        SkillCaster caster = CasterOf(*player);
        if (const auto token = rankBuffs.find(character); token != rankBuffs.end()) {
            skillUnit->CancelBuff(caster, token->second);
            rankBuffs.erase(token);
        }
        (void)oldSkill;
        const assets::SkillRow* skill = newSkill != 0 && config.skills ? config.skills->Find(newSkill) : nullptr;
        if (skill != nullptr) {
            if (const SkillState::Buff* buff = skillUnit->AddBuff(caster, *skill, skill->hasDuration ? skill->buffDurationMs : 7'200'000u, Clock::now())) {
                rankBuffs[character] = buff->token;
            }
        }
        return {player->name, skill != nullptr ? skill->nameKey : std::string()};
    }
    void FortressSendCharacter(std::int64_t character, proto::MessageId id, const std::vector<std::byte>& payload) override {
        for (auto& [peerId, p] : players) {
            if (p.selected != character || p.entity == 0) continue;
            if (Peer* peer = peers->Find(peerId)) peer->connection->Send(id, payload);
        }
    }
    void FortressSendGuild(std::uint32_t guild, proto::MessageId id, const std::vector<std::byte>& payload) override {
        const std::optional<FortressGuild> of = FortressGuildById(guild);
        if (!of) return;
        for (auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            const std::optional<FortressGuild> g = FortressGuildOf(p.selected);
            if (!g || (g->id != guild && (of->alliance == 0 || g->alliance != of->alliance))) continue;
            if (Peer* peer = peers->Find(peerId)) peer->connection->Send(id, payload);
        }
    }
    std::uint32_t FortressNpcIn(const std::string& part, const std::vector<std::uint16_t>& regions) const override {
        for (const auto& [entity, npc] : npcs) {
            if (npc == nullptr || npc->codename.find(part) == std::string::npos) continue;
            const auto where = world->Where(entity);
            if (where && std::find(regions.begin(), regions.end(), where->position.region) != regions.end()) return entity;
        }
        return 0;
    }
    std::optional<proto::WorldPosition> FortressEntityPosition(std::uint32_t entity) const override {
        const auto where = world->Where(entity);
        return where ? std::optional<proto::WorldPosition>(where->position) : std::nullopt;
    }
    void RemoveStructure(std::uint32_t entity) override {
        if (monsters) monsters->RemoveFixed(entity);
    }
    void StructureAppearance(std::uint32_t entity, proto::MessageId id, std::vector<std::byte> payload) override {
        GuildToObservers(entity, id, payload);
        world->SetExtra(entity, id, std::move(payload));
    }
    void FortressSubmit(std::function<void(Database&)> task, std::function<void()> done) override { GuildSubmit(std::move(task), std::move(done)); }
    void FortressZoneBlocks(std::uint32_t key, const std::string& name, bool blocks) override { world->SetNavZone(key, name, blocks); }
    std::uint8_t FortressPositionOf(std::int64_t character) const override { return guilds ? guilds->PositionOf(character) : 0; }
    bool FortressTakeGuildPoints(std::uint32_t guild, std::uint32_t points) override { return guilds && guilds->TakeGuildPoints(guild, points); }
    bool FortressHasPoint(const std::string& teleportCode) const override {
        const assets::TeleportPoint* to = config.teleports ? config.teleports->PointNamed(teleportCode) : nullptr;
        return to != nullptr && to->HasArrival();
    }
    bool FortressSendTo(std::uint64_t peerId, const std::string& teleportCode) override {
        const auto it = players.find(peerId);
        const assets::TeleportPoint* to = config.teleports ? config.teleports->PointNamed(teleportCode) : nullptr;
        if (it == players.end() || it->second.entity == 0 || it->second.dead || to == nullptr || !to->HasArrival()) return false;
        core::LogInfo("fortress war: {} goes to {}", it->second.name, teleportCode);
        TravelTo(peerId, it->second, {to->region, to->x, to->y, to->z});
        return true;
    }
    std::size_t FortressCountIn(const std::vector<std::uint16_t>& regions, const std::function<bool(std::int64_t)>& counts) const override {
        std::size_t n = 0;
        for (const auto& [peerId, player] : players) {
            if (player.entity == 0) continue;
            const auto place = world->Where(player.entity);
            if (place && std::find(regions.begin(), regions.end(), place->position.region) != regions.end() && counts(player.selected)) ++n;
        }
        return n;
    }
    bool FortressJobSuit(std::uint64_t peerId) const override {
        const auto it = players.find(peerId);
        return it != players.end() && it->second.entity != 0 && ActivityOf(it->second).jobSuit;
    }
    bool FortressInArena(std::uint64_t peerId) const override {
        const auto it = players.find(peerId);
        return it != players.end() && it->second.entity != 0 && arenas && arenas->InMatch(it->second.entity);
    }
    void FortressEvacuate(const std::vector<std::uint16_t>& regions, const std::string& teleportCode, const std::function<bool(std::int64_t)>& stays) override {
        const assets::TeleportPoint* to = config.teleports ? config.teleports->PointNamed(teleportCode) : nullptr;
        if (to == nullptr || !to->HasArrival() || (to->region & 0x8000) != 0) {
            core::LogWarning("fortress war: no way out to {}", teleportCode);
            return;
        }
        for (auto& [peerId, player] : players) {
            if (player.entity == 0) continue;
            const auto place = world->Where(player.entity);
            if (!place || std::find(regions.begin(), regions.end(), place->position.region) == regions.end() || stays(player.selected)) continue;
            core::LogInfo("fortress war: {} is sent out of the fortress", player.name);
            TravelTo(peerId, player, {to->region, to->x, to->y, to->z});
        }
    }
    void FortressTeleport(std::uint32_t entity, const proto::WorldPosition& to) override {
        // Never the dead: their client would take the arrival for a living character at its last health.
        const PlayerWorld* player = PlayerByEntity(entity);
        if (player == nullptr || player->dead) return;
        if (exchanges) exchanges->End(entity);
        if (alchemy) alchemy->End(entity);
        if (stalls) stalls->Leave(entity);
        if (coses) coses->DismissAll(entity, true);
        world->Teleport(entity, to);
    }

    // A structure the player may not hit (Fortresses::MayHit): told why, the attack not begun.
    bool StructureRefused(Peer& peer, const PlayerWorld& player, std::uint32_t target) {
        if (!fortresses || target == 0 || (!fortresses->IsStructure(target) && !fortresses->IsWarObject(target))) return false;
        const game::FortressError why = fortresses->MayHit(player.selected, target);
        if (why == game::FortressError::None) return false;
        SendMessage(*peer.connection, proto::FortressAnswer{proto::FortressAction::Attack, false, std::uint16_t(why), 0, proto::FortressSide::Attack});
        return true;
    }
    // A hit on a structure: none when the attacker may not hit it (an area's), else the health it took;
    // its fall is the fortress's (Killed).
    std::unordered_map<std::uint32_t, std::int64_t> structureKillers; // a fallen structure -> who felled it
    Monsters::Damaged HitStructure(std::uint32_t structure, std::uint32_t attacker, std::uint32_t damage, Clock::time_point now) {
        PlayerWorld* player = PlayerByEntity(attacker);
        if (player == nullptr || !monsters || fortresses->MayHit(player->selected, structure) != game::FortressError::None) return {};
        const std::uint32_t before = monsters->Hp(structure);
        Monsters::Damaged d = monsters->Damage(structure, attacker, damage, now, nullptr);
        if (!d.hit) return d;
        fortresses->StructureHit(structure, player->selected, before - std::min(before, d.hp), d.hp);
        if (d.killed) structureKillers[structure] = player->selected;
        return d;
    }
    // The fortress war's word on two players: enemies in a fortress's area
    // while its war runs, on its two sides. The game server's hostility (guild wars) asks it too.
    bool FortressHostile(std::uint32_t attacker, std::uint32_t target) const {
        if (!fortresses) return false;
        const auto a = players.find(world->PeerOf(attacker)), b = players.find(world->PeerOf(target));
        if (a == players.end() || b == players.end() || a->second.entity != attacker || b->second.entity != target) return false;
        const auto pa = world->Where(attacker), pb = world->Where(target);
        if (!pa || !pb) return false;
        return fortresses->Hostile(a->second.selected, pa->position.region, b->second.selected, pb->position.region);
    }

    // --- Guilds (GuildHost) --------------------------------------------------------------------------

    GuildActor GuildActorOf(std::uint64_t peerId, const PlayerWorld& player) const {
        return {peerId, player.entity, player.selected, player.name, player.level, player.model, player.dead, player.inventory.gold, player.skillPoints,
                JobState(player) != kNoJob};
    }
    void GuildSend(std::uint64_t peerId, proto::MessageId id, const std::vector<std::byte>& payload) override {
        if (Peer* p = peers->Find(peerId)) p->connection->Send(id, payload);
    }
    std::optional<GuildActor> GuildPlayerAt(std::uint32_t entity) const override {
        const std::uint64_t peerId = world->PeerOf(entity);
        const auto it = peerId != 0 ? players.find(peerId) : players.end();
        if (it == players.end() || it->second.entity != entity) return std::nullopt;
        return GuildActorOf(peerId, it->second);
    }
    double GuildDistance(std::uint32_t a, std::uint32_t b) const override {
        const auto pa = world->Where(a), pb = world->Where(b);
        if (!pa || !pb) return -1.0;
        const proto::FlatPosition fa = proto::ToFlat(pa->position), fb = proto::ToFlat(pb->position);
        return std::hypot(fa.x - fb.x, fa.z - fb.z);
    }
    bool IsNpc(std::uint32_t entity) const override { return npcs.contains(entity); }
    bool GuildHoldsFortress(std::uint32_t guild) const override { return fortresses && fortresses->GuildHoldsFortress(guild); }
    bool GuildInFortressWar(std::uint32_t guild) const override { return fortresses && fortresses->WarOn() && fortresses->GuildInWar(guild); }
    // The fortress items.
    std::optional<std::uint32_t> FortressItemAt(std::uint64_t peerId, std::uint8_t slot) const override {
        const auto it = players.find(peerId);
        const game::ItemInstance* item = it != players.end() && slot >= game::slot::kWorn ? it->second.inventory.At(slot) : nullptr;
        return item != nullptr ? std::optional<std::uint32_t>(item->refId) : std::nullopt;
    }
    void FortressTakeItem(std::uint64_t peerId, std::uint8_t slot) override {
        const auto it = players.find(peerId);
        Peer* peer = peers->Find(peerId);
        if (it == players.end() || peer == nullptr) return;
        if (const game::ItemInstance* item = it->second.inventory.At(slot)) Broadcast(it->second.entity, proto::ItemUsed{it->second.entity, item->refId});
        TakeOne(*peer, it->second, slot);
    }
    std::optional<std::pair<std::uint32_t, std::uint32_t>> FortressCharacter(const std::string& codename) const override {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(std::string_view(codename)) : nullptr;
        return row != nullptr ? std::optional<std::pair<std::uint32_t, std::uint32_t>>({row->refObjId, row->maxHp}) : std::nullopt;
    }
    std::optional<FortressHost::CharacterInfo> FortressCharacterRow(std::uint32_t refObjId) const override {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(refObjId) : nullptr;
        if (row == nullptr) return std::nullopt;
        return FortressHost::CharacterInfo{game::StructureKindOf(row->typeId[0], row->typeId[1], row->typeId[2], row->typeId[3]), row->maxHp, row->level, row->codename};
    }
    // The war's objects and the items on a target.
    std::uint32_t FortressSpawnWarMonster(std::uint32_t refObjId, const proto::WorldPosition& at, float yaw, std::uint32_t guild, std::uint32_t alliance,
                                          std::uint32_t hp) override {
        return monsters ? monsters->AddWarMonster(refObjId, at, yaw, guild, alliance, hp) : 0u;
    }
    std::uint32_t FortressSpawnUnique(std::uint32_t refObjId, const proto::WorldPosition& at) override {
        if (!monsters) return 0;
        MonsterNest nest;
        nest.refObjId = refObjId;
        nest.position = at;
        nest.respawn = false;
        nest.temporary = true;
        nest.maxCount = 1;
        nest.radius = 500.0f;
        nest.spawnRadius = 10.0f;
        return monsters->AddNest(nest) != SIZE_MAX ? 1u : 0u;
    }
    // The siege quests' and the war's rewards.
    PlayerWorld* PlayerOfCharacter(std::int64_t character, std::uint64_t* peerOut = nullptr) {
        for (auto& [peerId, p] : players) {
            if (p.selected != character || p.entity == 0) continue;
            if (peerOut != nullptr) *peerOut = peerId;
            return &p;
        }
        return nullptr;
    }
    std::vector<std::int64_t> FortressPartyNear(std::int64_t character, double reach) const override {
        std::vector<std::int64_t> out;
        const PlayerWorld* me = const_cast<State*>(this)->PlayerOfCharacter(character);
        if (me == nullptr || !parties) return out;
        const auto here = world->Where(me->entity);
        if (!here) return out;
        const proto::FlatPosition c = proto::ToFlat(here->position);
        for (const Parties::MemberView& m : parties->MembersOf(me->entity)) {
            if (m.server != config.id || m.entity == 0 || m.entity == me->entity) continue;
            const PlayerWorld* p = const_cast<State*>(this)->PlayerByEntity(m.entity);
            const auto there = world->Where(m.entity);
            if (p == nullptr || p->dead || !there || there->position.layer != here->position.layer) continue;
            const proto::FlatPosition q = proto::ToFlat(there->position);
            if (std::hypot(q.x - c.x, q.z - c.z) <= reach) out.push_back(p->selected);
        }
        return out;
    }
    std::uint32_t MobExperienceAt(int level) const {
        return level > 0 && std::size_t(level) < config.monsterExperience.size() ? config.monsterExperience[std::size_t(level)] : 0u;
    }
    void GiveSp(std::uint64_t peerId, PlayerWorld& p, std::uint64_t sp) {
        Peer* peer = peers->Find(peerId);
        if (peer == nullptr || sp == 0) return;
        SendMessage(*peer->connection, proto::ExperienceGained{0, 0, std::uint32_t(std::min<std::uint64_t>(sp, 0xFFFFFFFF))});
        GainExperience(*peer, p, 0, std::uint32_t(std::min<std::uint64_t>(sp, 0xFFFFFFFF)));
    }
    std::vector<std::int64_t> FortressQuestSp(std::int64_t character, std::uint32_t value) override {
        std::uint64_t peerId = 0;
        PlayerWorld* me = PlayerOfCharacter(character, &peerId);
        if (me == nullptr) return {};
        const std::vector<Parties::MemberView> party = parties ? parties->MembersOf(me->entity) : std::vector<Parties::MemberView>{};
        if (party.size() < 2) {
            GiveSp(peerId, *me, std::uint64_t(MobExperienceAt(me->level)) * value);
            return {character};
        }
        // The party's level (ours: its members' average, inferred) x value / its size, to each living member near.
        int levels = 0;
        for (const Parties::MemberView& m : party) levels += m.level;
        const int partyLevel = std::max(1, int(std::lround(double(levels) / double(party.size()))));
        const std::uint64_t each = std::uint64_t(MobExperienceAt(partyLevel)) * value / party.size();
        std::vector<std::int64_t> given{character};
        for (const std::int64_t m : FortressPartyNear(character, 1000.0)) given.push_back(m);
        for (const std::int64_t c : given) {
            std::uint64_t to = 0;
            if (PlayerWorld* p = PlayerOfCharacter(c, &to)) GiveSp(to, *p, each);
        }
        return given;
    }
    bool FortressGiveItemTo(std::int64_t character, std::uint32_t item, std::uint32_t count) override {
        std::uint64_t peerId = 0;
        return PlayerOfCharacter(character, &peerId) != nullptr && FortressGiveItem(peerId, item, count);
    }
    int FortressAreaSp(const std::vector<std::uint16_t>& regions, std::uint32_t guild, std::uint32_t value) override {
        int n = 0;
        for (auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            const auto place = world->Where(p.entity);
            const std::optional<FortressGuild> g = FortressGuildOf(p.selected);
            if (!place || !g || g->id != guild || std::find(regions.begin(), regions.end(), place->position.region) == regions.end()) continue;
            GiveSp(peerId, p, std::uint64_t(MobExperienceAt(p.level)) * value);
            ++n;
        }
        return n;
    }
    std::int64_t FortressMasterOf(std::uint32_t guild) const override {
        for (const auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            const std::optional<FortressGuild> g = FortressGuildOf(p.selected);
            if (g && g->id == guild && g->master) return p.selected;
        }
        return 0;
    }
    // The Cave of Discipline: an instance of the siege world (Instances::OpenSiege).
    std::uint32_t FortressDungeonOpen(const std::string& opener) override {
        return instances ? instances->OpenSiege(opener, std::uint32_t(std::max(0, config.fortress.dungeonSeconds))) : 0u;
    }
    bool FortressDungeonFree(std::uint64_t peerId) override {
        const auto it = players.find(peerId);
        if (it == players.end() || it->second.entity == 0) return false;
        const PlayerWorld& p = it->second;
        if (ActivityOf(p).jobSuit || p.freePvp != 0 || p.riding || CarriesGoods(p)) return false;
        return !(stalls && (stalls->Owner(p.entity) || stalls->Visiting(p.entity)));
    }
    bool FortressDungeonAdmit(std::uint64_t peerId, std::uint32_t layer) override {
        const auto it = players.find(peerId);
        if (it == players.end() || it->second.entity == 0 || !instances) return false;
        if (exchanges) exchanges->End(it->second.entity);
        if (coses) coses->DismissAll(it->second.entity, true); // its pets (a ride or transport was refused before)
        // Entering sets the recall point to where the player stands (only in a normal world): the Cave's leave sends
        // them back there.
        const auto spot = KeepableSpot(it->second);
        if (!instances->AdmitSiege(it->second.entity, layer)) return false;
        if (spot) {
            it->second.recallSpot = *spot;
            database.Submit([character = it->second.selected, s = *spot](Database& db) { SaveRecallSpot(db, character, s); });
        }
        return true;
    }
    bool FortressDungeonExit(std::uint64_t peerId) override {
        const auto it = players.find(peerId);
        if (it == players.end() || it->second.entity == 0 || !instances) return false;
        const CharacterRow::Spot& r = it->second.recallSpot;
        if (r.region == 0) return false;
        const proto::WorldPosition to{r.region, r.x, r.y, r.z};
        instances->ExitSiege(it->second.entity, &to);
        return true;
    }
    bool FortressDungeonReadmits(std::uint64_t peerId, std::uint32_t layer) const override {
        const auto it = players.find(peerId);
        return it != players.end() && it->second.entity != 0 && instances && instances->ReadmitsSiege(it->second.entity, layer);
    }
    bool FortressDungeonStands(std::uint32_t layer) const override { return instances && instances->Stands(layer); }
    std::size_t FortressDungeonInside(std::uint32_t layer) const override { return instances ? instances->Inside(layer) : 0u; }
    std::uint32_t FortressDungeonMaxUsers() const override { return instances && instances->SiegeMaxUsers() != 0 ? instances->SiegeMaxUsers() : 30u; }
    std::uint32_t FortressDungeonOf(std::uint64_t peerId) const override {
        const auto it = players.find(peerId);
        return it != players.end() && it->second.entity != 0 && instances ? instances->LayerOf(it->second.entity) : 0u;
    }
    bool FortressGuildHasPosition(std::uint32_t guild, std::uint8_t position) const override { return guilds && guilds->AnyHolds(guild, position); }
    bool FortressGiveItem(std::uint64_t peerId, std::uint32_t item, std::uint32_t count) override {
        const auto it = players.find(peerId);
        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(item) : nullptr;
        if (it == players.end() || row == nullptr || it->second.entity == 0) return false;
        return QuestGive(it->second.entity, {{row->codename, count}});
    }
    std::uint32_t FortressItemMaxStack(std::uint32_t item) const override {
        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(item) : nullptr;
        return row != nullptr ? std::uint32_t(std::max(1, game::MaxStack(*row))) : 0u;
    }
    bool FortressGiveStack(std::uint64_t peerId, std::uint32_t item, std::uint32_t count) override {
        const auto it = players.find(peerId);
        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(item) : nullptr;
        if (it == players.end() || row == nullptr || it->second.entity == 0 || count == 0) return false;
        PlayerWorld& p = it->second;
        const int place = p.inventory.FreeBagPlace();
        if (place < 0) return false;
        const auto ids = freeItemIds;
        if (!Put(p, place, NewItem(*row, count))) {
            freeItemIds = ids;
            return false;
        }
        TellBag(p, {std::uint8_t(place)}, {});
        if (quests) quests->BagChanged(p.entity);
        return true;
    }
    FortressHost::BlessCheck FortressBlessCheck(std::uint64_t peerId, std::uint32_t skillId) const override {
        const auto it = players.find(peerId);
        const assets::SkillRow* skill = config.skills ? config.skills->Find(skillId) : nullptr;
        if (it == players.end() || skill == nullptr) return {};
        std::vector<const assets::SkillRow*> running;
        std::vector<std::uint32_t> skills;
        for (const SkillState::Buff& b : it->second.combat.buffs) {
            running.push_back(config.skills->Find(b.skill));
            skills.push_back(b.skill);
        }
        return {true, game::AdmitBuff(*skill, running).admitted, skill->cooldownMs, std::move(skills)};
    }
    void FortressBlessGive(std::uint64_t peerId, std::uint32_t skillId) override {
        const auto it = players.find(peerId);
        const assets::SkillRow* skill = config.skills ? config.skills->Find(skillId) : nullptr;
        if (it == players.end() || skill == nullptr || !skillUnit) return;
        SkillCaster caster = CasterOf(it->second);
        skillUnit->AddBuff(caster, *skill, skill->hasDuration && skill->buffDurationMs > 0 ? skill->buffDurationMs : 3'600'000u, Clock::now());
    }
    void FortressGpSpent(std::uint32_t guild, const std::string& name, std::uint32_t points, int reason) override {
        database.Submit([guild, name, points, reason](Database& db) { AddGpHistory(db, guild, name, points, reason); });
    }
    void FortressGpHistoryTo(std::uint64_t peerId, std::uint32_t guild) override {
        auto rows = std::make_shared<std::vector<GpHistoryRow>>();
        GuildSubmit([guild, rows](Database& db) { *rows = LoadGpHistory(db, guild); },
                    [this, peerId, rows] {
                        proto::GuildGpHistory answer;
                        for (const GpHistoryRow& row : *rows) {
                            answer.rows.push_back({row.name, row.points, std::uint8_t(row.reason), std::uint16_t(row.year), std::uint8_t(row.month),
                                                   std::uint8_t(row.day), std::uint8_t(row.hour), std::uint8_t(row.minute)});
                        }
                        if (Peer* peer = peers->Find(peerId)) SendMessage(*peer->connection, answer);
                    });
    }
    std::uint32_t FortressHealStructure(std::uint32_t entity, std::uint32_t amount) override {

        const std::uint32_t hp = monsters ? monsters->Heal(entity, amount) : 0u;
        if (hp != 0) {
            const game::CombatStats* stats = monsters->Stats(entity);
            GuildToObservers(entity, proto::StructureHealth::kId, proto::Encode(proto::StructureHealth{entity, hp, stats != nullptr ? stats->maxHp : hp}));
        }
        return hp;
    }
    // A flag's skill (its characterdata's first skill, DefaultSkill_1) and its reach ('efr''s third word).
    const assets::SkillRow* FlagSkill(std::uint32_t refObjId) const {
        const assets::ObjectRow* row = config.characters ? config.characters->Find(refObjId) : nullptr;
        return row != nullptr && row->skills[0] != 0 && config.skills ? config.skills->Find(row->skills[0]) : nullptr;
    }
    int FortressFlagPulseMs(std::uint32_t refObjId) const override {
        const assets::SkillRow* skill = FlagSkill(refObjId);
        if (skill == nullptr || !assets::FindSkillParam(*skill, assets::SkillTag("heal"))) return 0;
        const assets::SkillParam pulse = assets::FindSkillParam(*skill, assets::SkillTag("puls"));
        return pulse ? std::max(1000, pulse.Int(0)) : 0;
    }
    void FortressFlagAura(std::uint32_t flag, std::uint32_t refObjId, std::uint32_t guild, bool pulse) override {
        const assets::SkillRow* skill = FlagSkill(refObjId);
        const auto there = world->Where(flag);
        const std::optional<FortressGuild> of = FortressGuildById(guild);
        if (skill == nullptr || !there || !of || !skillUnit) return;
        const assets::SkillParam area = assets::FindSkillParam(*skill, assets::SkillTag("efr"));
        const float reach = area ? float(std::max(1, area.Int(2))) : 200.0f;
        const assets::SkillParam heal = assets::FindSkillParam(*skill, assets::SkillTag("heal"));
        const proto::FlatPosition c = proto::ToFlat(there->position);
        for (auto& [peerId, p] : players) {
            if (p.entity == 0 || p.dead) continue;
            const auto at = world->Where(p.entity);
            if (!at) continue;
            const proto::FlatPosition q = proto::ToFlat(at->position);
            if (std::hypot(q.x - c.x, q.z - c.z) > double(reach)) continue;
            const std::optional<FortressGuild> g = FortressGuildOf(p.selected);
            if (!g || (g->id != guild && (of->alliance == 0 || g->alliance != of->alliance))) continue;
            if (heal) {
                // The healing flag: 'heal' {HP, HP %, MP, MP %} each pulse.
                if (!pulse) continue;
                const std::uint32_t hp = heal[0] + std::uint32_t(std::uint64_t(p.stats.maxHp) * heal[1] / 100);
                const std::uint32_t mp = heal[2] + std::uint32_t(std::uint64_t(p.stats.maxMp) * heal[3] / 100);
                p.hp = std::min(p.stats.maxHp, p.hp + game::RecoveryCut(p.combat.states, hp, true));
                p.mp = std::min(p.stats.maxMp, p.mp + game::RecoveryCut(p.combat.states, mp, false));
                world->SetHp(p.entity, p.hp);
                world->SetVitals(p.entity, p.hp, p.stats.maxHp, p.mp, p.stats.maxMp);
                if (Peer* peer = peers->Find(peerId)) SendVitals(*peer, p);
                continue;
            }
            // A buff while they stand by it (ours: renewed each second for two).
            SkillCaster caster = CasterOf(p);
            if (const SkillState::Buff* b = skillUnit->AddBuff(caster, *skill, 2000, Clock::now()); b != nullptr) {
                const auto it = std::find_if(p.combat.buffs.begin(), p.combat.buffs.end(), [&](const SkillState::Buff& x) { return x.token == b->token; });
                if (it != p.combat.buffs.end()) it->own = false;
            }
        }
    }
    int FortressStructureSkillMs(std::uint32_t refObjId) const override {
        const assets::SkillRow* skill = FlagSkill(refObjId);
        if (skill == nullptr || skill->aiType != 'S') return 0;
        if (!assets::FindSkillParam(*skill, assets::SkillTag("pdm2"))) return 1000; // an aura, renewed
        const assets::SkillParam pulse = assets::FindSkillParam(*skill, assets::SkillTag("puls"));
        return pulse ? std::max(1000, pulse.Int(0)) : 0;
    }
    void FortressStructureSkill(std::uint32_t structure, std::uint32_t refObjId, std::uint32_t holder) override {
        const assets::SkillRow* skill = FlagSkill(refObjId);
        if (skill == nullptr) return;
        const assets::SkillParam ratio = assets::FindSkillParam(*skill, assets::SkillTag("pdm2"));
        if (!ratio) return FortressFlagAura(structure, refObjId, holder, false); // a position's or the headquarters' aura
        const auto there = world->Where(structure);
        if (!there || !fortresses) return;
        const assets::SkillParam area = assets::FindSkillParam(*skill, assets::SkillTag("efr"));
        const double reach = area ? double(std::max(1, area.Int(2))) : 200.0;
        const std::optional<FortressGuild> side = holder != 0 ? FortressGuildById(holder) : std::nullopt;
        const proto::FlatPosition c = proto::ToFlat(there->position);
        const Clock::time_point now = Clock::now();
        // Its enemies: alive, within reach, fighting in this war (the original's 0x3043 refuses others), not of its side
        // (0x303C).
        std::vector<std::uint64_t> struck;
        for (auto& [peerId, p] : players) {
            if (p.entity == 0 || p.dead || now < p.untouchableUntil) continue;
            const auto at = world->Where(p.entity);
            if (!at || at->position.layer != there->position.layer) continue;
            const proto::FlatPosition q = proto::ToFlat(at->position);
            if (std::hypot(q.x - c.x, q.z - c.z) > reach || !fortresses->Fighting(p.selected, at->position.region)) continue;
            const std::optional<FortressGuild> g = FortressGuildOf(p.selected);
            if (side && g && (g->id == side->id || (side->alliance != 0 && g->alliance == side->alliance))) continue;
            struck.push_back(peerId);
        }
        const std::vector<game::StateGift> gifts = game::StateGiftsOf(*skill);
        const int pulse = assets::FindSkillParam(*skill, assets::SkillTag("puls")).Int(0);
        for (const std::uint64_t peerId : struck) {
            const auto it = players.find(peerId);
            if (it == players.end() || it->second.dead) continue;
            PlayerWorld& p = it->second;
            // Its damage: the percent of the most health (Formulae_CalculateHPRatioDamage), then its states.
            const std::uint32_t damage = std::uint32_t(std::llround(double(ratio.Int(0)) / 100.0 * double(p.stats.maxHp)));
            p.hp = damage >= p.hp ? 0 : p.hp - damage;
            p.lastHurt = now;
            p.combat.battleUntil = now + std::chrono::seconds(20);
            world->SetHp(p.entity, p.hp);
            Broadcast(p.entity, proto::EntityDamaged{p.entity, structure, damage, p.hp, std::uint8_t(p.hp == 0 ? proto::kDamageKilled : 0)});
            if (p.hp > 0 && !gifts.empty()) {
                GiveStates(p, game::RollStates(gifts, StateDefenceOf(p), p.combat.states.Susceptibility(), monsterStateRolls[structure], fightRandom, structure,
                                               skill->id, pulse, now),
                           now);
            }
            if (p.hp == 0) Die(p, nullptr, refObjId);
        }
    }
    std::optional<FortressHost::KitSkill> FortressKitSkill(const std::string& code) const override {
        const assets::SkillRow* skill = config.skills ? config.skills->Find(std::string_view(code)) : nullptr;
        if (skill == nullptr) return std::nullopt;
        const assets::SkillParam heal = assets::FindSkillParam(*skill, assets::SkillTag("heal"));
        const assets::SkillParam pulse = assets::FindSkillParam(*skill, assets::SkillTag("puls"));
        if (!heal) return std::nullopt;
        return FortressHost::KitSkill{heal.Int(1), pulse ? pulse.Int(0) : 0, skill->hasDuration ? int(skill->buffDurationMs) : 0};
    }
    bool FortressCastItemSkill(std::uint64_t peerId, const std::string& code, std::uint32_t target) override {
        const auto it = players.find(peerId);
        Peer* peer = peers->Find(peerId);
        const assets::SkillRow* skill = config.skills ? config.skills->Find(std::string_view(code)) : nullptr;
        if (it == players.end() || peer == nullptr || skill == nullptr || !skillUnit) return false;
        PlayerWorld& player = it->second;
        // The item's skill is the player's for the cast (an item's skill: SkillBook::itemSkills), as a learned one's.
        if (std::find(player.book.itemSkills.begin(), player.book.itemSkills.end(), skill->id) == player.book.itemSkills.end()) {
            player.book.itemSkills.push_back(skill->id);
        }
        SkillCaster caster = CasterOf(player);
        return skillUnit->Use(caster, proto::UseSkill{skill->id, target}, Clock::now());
    }
    bool FortressWalkClear(const proto::WorldPosition& from, const proto::WorldPosition& to) const override {
        const auto end = world->WalkEnd(from, to);
        if (!end) return true; // no navigation where the player stands (tests)
        const proto::FlatPosition a = proto::ToFlat(*end), b = proto::ToFlat(to);
        return std::hypot(a.x - b.x, a.z - b.z) < 1.0;
    }

    // The fortresses' bus.
    std::string fortressChannel;
    std::uint16_t FortressServerId() const override { return config.id; }
    bool FortressOnBus() const override { return !fortressChannel.empty(); }
    void FortressPublish(std::string text) override { BusSend(fortressChannel, std::move(text)); }
    // The NPCs that keep guilds (one in each town: game::IsGuildNpcCodename).
    bool IsGuildNpc(std::uint32_t entity) const override {
        const auto it = npcs.find(entity);
        return it != npcs.end() && game::IsGuildNpcCodename(it->second->codename);
    }
    bool GuildNameAllowed(std::string_view name) const override {
        const game::NameCheck check = config.rules->names.Check(name);
        return check == game::NameCheck::Ok || check == game::NameCheck::BadLength;
    }
    std::int64_t GuildStorageHolder(std::uint32_t guild) const override {
        const auto c = guildChests.find(guild);
        if (c == guildChests.end() || c->second.user == 0) return 0;
        const auto p = players.find(c->second.user);
        return p != players.end() ? p->second.selected : 0;
    }
    bool GuildInEventWorld(std::uint32_t entity) const override { return flagWorlds && flagWorlds->InMatch(entity); }
    bool GuildInArena(std::uint32_t entity) const override { return arenas && arenas->InMatch(entity); }
    void FortressLost(std::uint32_t guild) override {
        if (guilds) guilds->FortressPositionsReset(guild);
    }
    void GuildPay(std::int64_t character, std::uint64_t fee, PaymentWork work, PaymentDone done) override {
        Payment(character, fee, std::move(work), std::move(done));
    }
    bool TakeGold(std::uint64_t peerId, std::uint64_t amount) override {
        const auto it = players.find(peerId);
        Peer* peer = peers->Find(peerId);
        if (it == players.end() || peer == nullptr || it->second.inventory.gold < amount) return false;
        PlayerWorld& player = it->second;
        player.inventory.gold -= amount;
        SendMessage(*peer->connection, proto::GoldChanged{player.inventory.gold});
        QueueGoldSave(player.selected, player.inventory.gold);
        return true;
    }
    bool GiveGold(std::uint64_t peerId, std::uint64_t amount) override {
        const auto it = players.find(peerId);
        Peer* peer = peers->Find(peerId);
        if (it == players.end() || peer == nullptr || it->second.inventory.gold > UINT64_MAX - amount) return false;
        PlayerWorld& player = it->second;
        player.inventory.gold += amount;
        SendMessage(*peer->connection, proto::GoldChanged{player.inventory.gold});
        QueueGoldSave(player.selected, player.inventory.gold);
        return true;
    }
    bool TakeSkillPoints(std::uint64_t peerId, std::uint32_t points) override {
        const auto it = players.find(peerId);
        Peer* peer = peers->Find(peerId);
        if (it == players.end() || peer == nullptr || it->second.skillPoints < points) return false;
        it->second.skillPoints -= points;
        it->second.progressDirty = true;
        SendProgress(*peer, it->second);
        return true;
    }
    bool TakeGuildItem(std::uint64_t peerId, std::uint8_t slot, std::string_view codename) override {
        const auto it = players.find(peerId);
        Peer* peer = peers->Find(peerId);
        if (it == players.end() || peer == nullptr || slot < game::slot::kWorn) return false;
        const game::ItemInstance* item = it->second.inventory.At(slot);
        const assets::ItemRow* row = item != nullptr && config.rules->items ? config.rules->items->Find(item->refId) : nullptr;
        if (row == nullptr || row->codename != codename) return false;
        TakeOne(*peer, it->second, slot);
        return true;
    }
    void GuildToObservers(std::uint32_t entity, proto::MessageId id, const std::vector<std::byte>& payload) override {
        world->ForEachObserver(entity, [&](std::uint64_t to) {
            if (Peer* p = peers->Find(to)) p->connection->Send(id, payload);
        });
    }
    void GuildAppearance(std::uint32_t entity, proto::MessageId id, std::vector<std::byte> payload) override {
        world->SetExtra(entity, id, std::move(payload));
    }
    std::uint16_t GuildServer() const override { return config.id; }
    std::string GuildChannel() const override { return busChannel; }
    // The guild recall: a member travelling is not asked (the original's teleport state).
    bool GuildRecallAskable(std::uint32_t entity) const override {
        const auto it = players.find(world->PeerOf(entity));
        return it != players.end() && it->second.entity == entity && !it->second.returnAt && !it->second.moving;
    }
    // A member's yes: refused 0x18C1 when dead, travelling, a murderer, with
    // a transport out, or busy (an exchange, a stall, alchemy: the original's motion state neither 0 nor 1); ours also in
    // an instance's layer (a teleport would not leave it). Else on its way to its master's place, on this server or the other.
    bool GuildMercenaries() const override { return config.guildMercenaries; }
    // A guild's abilities changed: the soldiers out of its members here raised again.
    void GuildMercenaryChanged(std::uint32_t guild, std::uint8_t abilities) override {
        if (!coses || !guilds) return;
        for (auto& [peerId, player] : players) {
            if (player.entity == 0) continue;
            const auto facts = guilds->FactsOf(player.selected);
            if (facts && facts->id == guild) coses->SoldierAbilities(player.entity, abilities);
        }
    }
    std::uint16_t GuildRecallGo(std::uint32_t entity, const proto::WorldPosition& to, std::uint32_t seconds) override {
        const std::uint64_t peerId = world->PeerOf(entity);
        const auto it = players.find(peerId);
        if (it == players.end() || it->second.entity != entity) return game::kRecallWorld;
        PlayerWorld& player = it->second;
        const auto place = world->Where(entity);
        const bool busy = MallBagHeld(entity); // an exchange, a stall, alchemy
        if (player.dead || player.returnAt || player.moving || player.pvpState == std::uint8_t(game::PvpState::Murderer) ||
            (coses && coses->TransportOut(entity)) || busy || !place || place->position.layer != 0 || (instances && instances->LayerOf(entity) != 0) ||
            !CanTravelTo(to.region)) {
            return game::kRecallWorld;
        }
        // The original's client runs the recall's gauge for the ask's seconds once its yes is taken (kind 2; its cancel is
        // the teleport's, 0x705B), then the member goes: ours keeps the place on the return's clock.
        const std::uint32_t ms = std::max<std::uint32_t>(1000, seconds * 1000);
        core::LogInfo("{}: {} answers the guild recall: to {:#06x} {:.0f},{:.0f},{:.0f} in {} s", config.name, player.name, to.region, to.x, to.y, to.z,
                      ms / 1000);
        player.returnAt = Clock::now() + std::chrono::milliseconds(ms);
        player.recallTo = to;
        player.reverseOption = 0;
        player.toThiefDen = false;
        if (Peer* peer = peers->Find(peerId)) SendMessage(*peer->connection, proto::ReturnStarted{0, ms, proto::CastingKind::GuildRecall, 0});
        return 0;
    }
    void GuildSubmit(std::function<void(Database&)> task, std::function<void()> done) override {
        database.Submit([this, task = std::move(task), done = std::move(done)](Database& db) {
            task(db);
            completions->Post(done);
        });
    }
    // The parties' and the chat's channels on the shard's bus, and what is waiting to be published.
    std::string partyChannel, chatChannel, communityChannel, academyChannel;
    std::deque<std::pair<std::string, std::string>> busQueue;
    bool busSending = false;
    struct PendingWhisper {
        std::uint64_t peer = 0;
        proto::ChatKind kind = proto::ChatKind::Whisper;
        std::uint8_t index = 0;
        Clock::time_point until{};
    };
    std::unordered_map<std::uint32_t, PendingWhisper> whispers;
    std::uint32_t nextWhisper = 1;
    // The shard's bus: its channel and the connection listening on it.
    std::string busChannel;
    std::unique_ptr<Database> busListener;
    // The website's commands and the uniques' news on the bus (AdminStore.h).
    std::string adminChannel, uniqueChannel;
    // The account behind each peer in the lobby or the world: its name (the website's kick names it) and whether it is
    // a game master's (tb_user.sec_primary 1: the GM commands typed in the chat).
    struct AccountOf {
        std::string name;
        bool gm = false;
    };
    std::unordered_map<std::uint64_t, AccountOf> accountOf;
    // The whole shard's NPCs and nests (--dev-commands only): a developer's teleport to one in another server's part
    // of the world (TravelTo hands the player over).
    std::vector<GameServerConfig::NpcSpawn> devNpcs;
    std::vector<MonsterNest> devNests;
    // The skill lab's monsters (DevSpawn): each one's nest and the player who asked for it (they go with that player).
    struct LabMonster {
        std::size_t nest = 0;
        std::uint32_t owner = 0;
    };
    std::unordered_map<std::uint32_t, LabMonster> labMonsters;
    std::vector<std::uint64_t> loggingOut; // peers counting down (checked every poll)
    std::vector<WorldMessage> outbox;
    Clock::time_point nextSave;
    bool saving = false;
    Clock::time_point nextStats = Clock::now();
    // The site's load samples (sr_server_samples): the world's ticks since the last one (the log's reading adds to it too).
    Clock::time_point nextSample = Clock::now();
    WorldStats sampleStats; // since the last load sample
    WorldStats logStats;    // since the last log line
    double deliverMsMax = 0;

    // _items.data holds an item's count (anything but equipment) or its durability (equipment); an
    // item the tables do not know keeps a count when it has no durability.
    bool Counted(std::uint32_t refId, std::uint32_t durability) const {
        const assets::ItemRow* row = config.rules && config.rules->items ? config.rules->items->Find(refId) : nullptr;
        return row != nullptr ? game::DataIsCount(*row) : durability == 0;
    }
    void FromData(const StoredItem& s, game::ItemInstance& item) const {
        const assets::ItemRow* row = config.rules && config.rules->items ? config.rules->items->Find(s.refId) : nullptr;
        const bool counted = row != nullptr && game::DataIsCount(*row);
        item.durability = counted ? 0 : s.durability;
        item.count = row == nullptr ? s.quantity : counted ? s.quantity : 1;
    }

    GameServerInfo Info() const {
        GameServerInfo s;
        s.id = config.id;
        s.name = config.serverName;
        s.group = config.group;
        s.groupName = config.groupName;
        s.host = config.publicHost;
        s.port = listener ? listener->Port() : config.listen.port;
        s.capacity = config.capacity;
        s.shardCapacity = config.shardCapacity;
        s.online = std::uint32_t(online);
        s.schema = config.shardSchema;
        return s;
    }

    void Report() {
        reporting = true;
        std::vector<std::int64_t> connected;
        for (const auto& [peerId, player] : players) {
            if (Peer* p = peers->Find(peerId); p != nullptr && p->account != 0) connected.push_back(p->account);
        }
        database.Submit([this, info = Info(), connected = std::move(connected)](Database& db) {
            std::string error;
            if (!ReportGameServer(db, info, error)) core::LogWarning("{}: report failed: {}", config.name, error);
            // Who else is up (a move needs the address), and sessions of players who never arrived.
            bool listed = false;
            auto servers = ListGameServers(db, config.staleSeconds, listed);
            ReleaseAbandonedSessions(db, config.id, connected, 60);
            completions->Post([this, servers = std::move(servers), listed] {
                reporting = false;
                nextReport = Clock::now() + config.reportInterval;
                if (!listed) return;
                knownServers.clear();
                for (const GameServerInfo& s : servers) knownServers[s.id] = {s.host, s.port, s.reporting};
            });
        });
    }

    void Send(std::uint64_t peerId, proto::LobbyAction action, proto::LobbyStatus status) {
        Peer* peer = peers->Find(peerId);
        if (!peer) return;
        peer->busy = false;
        proto::LobbyResult r;
        r.action = action;
        r.status = status;
        SendMessage(*peer->connection, r);
    }

    // What an NPC's own teleport point offers, as a gate's (teleportdata's points owned by an NPC: the ferries' ticket
    // sellers, NPC_*_FERRY*; the user's report 2026-11-14): its links, its return point.
    std::uint32_t PointServices(std::uint32_t npcRefObjId) const {
        const assets::TeleportPoint* point = config.teleports ? config.teleports->PointOf(npcRefObjId) : nullptr;
        if (point == nullptr) return 0;
        std::uint32_t services = 0;
        if (!config.teleports->LinksFrom(point->id).empty()) services |= proto::kGateTeleport;
        if (point->recall) services |= proto::kGateRecallPoint;
        return services;
    }

    // The gate a traveller stands at and its point, or why not. `gateId` is a gate's reference id, or an
    // NPC's entity whose model owns a teleport point (a ferry's seller; entities are 2^28 and up, never a reference id).
    proto::GateStatus AtGate(const PlayerWorld& player, std::uint32_t gateId, const assets::TeleportPoint*& point) const {
        const assets::TeleportGate* gate = config.teleports ? config.teleports->Gate(gateId) : nullptr;
        point = gate ? config.teleports->PointOf(gate->refObjId) : nullptr;
        std::optional<proto::WorldPosition> stands;
        if (gate != nullptr && gate->region != 0) stands = proto::WorldPosition{gate->region, gate->x, gate->y, gate->z};
        if (gate == nullptr && config.teleports) {
            if (const auto npc = npcs.find(gateId); npc != npcs.end()) {
                point = config.teleports->PointOf(npc->second->refObjId);
                if (const auto at = world->Where(gateId)) stands = at->position;
            }
        }
        if (!point || !stands) return proto::GateStatus::NoSuchGate;
        const auto place = world->Where(player.entity);
        const proto::FlatPosition at = place ? proto::ToFlat(place->position) : proto::FlatPosition{};
        const proto::FlatPosition spot = proto::ToFlat(*stands);
        // The reach by the target's kind (3D): a gate structure 800, an NPC 300 (`gateReach`).
        const double reach = gate != nullptr ? std::max(double(config.gateReach), kGateStructureReach) : double(config.gateReach);
        const double d = place ? std::hypot(std::hypot(at.x - spot.x, at.z - spot.z), double(place->position.y - stands->y)) : -1.0;
        if (!place || d > reach) {
            core::LogInfo("{}: {} is {:.0f} from gate {}", config.name, player.name, d, gateId);
            return proto::GateStatus::TooFar;
        }
        return proto::GateStatus::Ok;
    }

    static constexpr double kGateStructureReach = 800.0;
    // Whether a transport is out: the original asks whether the player's first pet is one (a horse, a trade cart); pets
    // that follow (attack, pick) are not (Coses::TransportOut).
    bool TransportOut(const PlayerWorld& player) const { return coses && player.entity != 0 && coses->TransportOut(player.entity); }
    // The job's state: 1 trader, 2 thief, 3 hunter, 4 no active job. Ours: no jobs yet, so everyone has none.
    static constexpr std::uint8_t kNoJob = game::kNoJob;
    std::uint8_t JobState(const PlayerWorld& player) const { return player.jobState; }

    // Its job as others see it: the state from the suit worn, its job level and alias while the
    // suit is on; kept for newcomers (World::SetExtra), told around and to itself when the state changes (`tell`).
    void TellJob(Peer* peer, PlayerWorld& player, bool tell) {
        const game::ItemInstance* suit = player.inventory.At(game::slot::kJobSuit);
        const assets::ItemRow* row = suit != nullptr && config.rules->items ? config.rules->items->Find(suit->refId) : nullptr;
        const std::uint8_t state = game::JobStateOf(row);
        const bool changed = state != player.jobState;
        player.jobState = state;
        const bool on = state != kNoJob;
        const proto::EntityJob m{player.entity, state, std::uint8_t(on ? player.job.level : 0), on ? player.alias : std::string()};
        world->SetExtra(player.entity, proto::EntityJob::kId, on ? proto::Encode(m) : std::vector<std::byte>{});
        if (!tell || !changed) return;
        Broadcast(player.entity, m); // those near and the player itself (World::ForEachObserverWithin)
        core::LogInfo("{}: {}'s job state {} ({})", config.name, player.name, state, on ? player.alias : std::string("none"));
    }
    // Its own league (JobInfoMessage), as the client shows it.
    void SendJobInfo(Peer& peer, const PlayerWorld& player, bool entry = false) {
        SendMessage(*peer.connection, proto::JobInfoMessage{std::uint8_t(player.job.type), std::uint8_t(player.job.level), player.job.exp,
                                                            player.job.contribution, player.job.reward, player.alias, entry});
    }

    // A link's restrictions: each in turn, the first that fails refuses. Kind 5 checks its item as kind 4 and does not
    // take it yet.
    proto::GateStatus LinkRefusal(const PlayerWorld& player, const assets::TeleportLink& link) const {
        for (const assets::TeleportLink::Restriction& r : link.restrictions) {
            switch (r.kind) {
            case 1: // the level, a to b (0x1C15)
                if (std::uint32_t(player.level) < r.a || std::uint32_t(player.level) > r.b) return proto::GateStatus::WrongLevel;
                break;
            case 2: // no transport out (0x1C10): a horse or a trade cart may not go along; pets that follow may
                if (TransportOut(player)) return proto::GateStatus::Transport;
                break;
            case 4:
            case 5: { // an item in the bag (0x1C23)
                bool has = false;
                for (const auto& [place, item] : player.inventory.places) has |= place >= game::slot::kWorn && item.refId == r.a;
                if (!has) return proto::GateStatus::Condition;
                break;
            }
            case 6: // no active job (0x1C24: the Roc's gate is not for one in a job's suit)
                if (JobState(player) != kNoJob) return proto::GateStatus::Condition;
                break;
            case 7: { // a thief's suit worn (slot 8) with a transport out (0x1C10: the ferries); a trader's or hunter's passes
                const game::ItemInstance* suit = player.inventory.At(game::slot::kJobSuit);
                const assets::ItemRow* row = suit != nullptr && config.rules->items ? config.rules->items->Find(suit->refId) : nullptr;
                if (game::JobStateOf(row) == std::uint8_t(game::JobType::Thief) && TransportOut(player)) return proto::GateStatus::Transport;
                break;
            }
            case 3: // a mask of PvP states, bit 1 << state (0x1C19; unused by any link of the 1.188 data)
                if ((r.a & (1u << player.pvpState)) != 0) return proto::GateStatus::PvpState;
                break;
            default:
                break;
            }
        }
        return proto::GateStatus::Ok;
    }

    // Walked into a teleport zone (the player's zone handler):
    // `_RefEventZone`'s TELEPORT row names the point; its one link, checked (LinkRefusal: the original's
    // error to the player), takes the player to the link's point at once (the original: after 0.01 s).
    void EnterZone(const World::ZoneEntered& entered) {
        std::string zone = entered.zone;
        std::transform(zone.begin(), zone.end(), zone.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const auto row = config.teleportZones.find(zone);
        if (row == config.teleportZones.end() || !config.teleports) return;
        PlayerWorld* player = PlayerByEntity(entered.entity);
        Peer* peer = player ? peers->Find(world->PeerOf(entered.entity)) : nullptr;
        if (player == nullptr || peer == nullptr) return;
        // A rider, or one with a ride or a transport out, is turned back before the link is looked at (the original's notice
        // 0x300C 0x0C04, UIIT_MSG_COS_CAN_NOT_ENTER_DUNGEON; the links' restriction 2 would refuse it anyway).
        if (TransportOut(*player) || (coses && coses->MountOf(player->entity) != 0)) {
            core::LogInfo("{}: {} walked into {}: refused (riding)", config.name, player->name, zone);
            SendMessage(*peer->connection, proto::CosRefused{"UIIT_MSG_COS_CAN_NOT_ENTER_DUNGEON", {}});
            return;
        }
        const assets::TeleportPoint* from = config.teleports->PointNamed(row->second);
        const auto links = from ? config.teleports->LinksFrom(from->id) : std::vector<const assets::TeleportLink*>{};
        if (links.size() != 1) {
            core::LogWarning("{}: zone {} ({}): {} links, the original takes one", config.name, zone, row->second, links.size());
            return;
        }
        const assets::TeleportPoint* to = config.teleports->Point(links.front()->to);
        if (to == nullptr || !to->HasArrival()) return;
        if (const proto::GateStatus status = LinkRefusal(*player, *links.front()); status != proto::GateStatus::Ok) {
            core::LogInfo("{}: {} walked into {}: refused ({}, level {})", config.name, player->name, zone, int(status), player->level);
            SendMessage(*peer->connection, proto::GateRefused{status});
            return;
        }
        if (!CanTravelTo(to->region)) {
            SendMessage(*peer->connection, proto::GateRefused{proto::GateStatus::ServerDown});
            return;
        }
        core::LogInfo("{}: {} walked into {}: to {} ({:#06x} {:.0f},{:.0f},{:.0f})", config.name, player->name, zone, to->codename, to->region, to->x, to->y,
                      to->z);
        TravelTo(peer->id, *player, {to->region, to->x, to->y, to->z}, true); // a transport along where the link allows one
    }

    void RefuseGate(Peer& peer, std::uint32_t gate, const char* what, proto::GateStatus status) {
        core::LogInfo("{}: {} at gate {} ({}): refused ({})", config.name, peer.address, gate, what, int(status));
        SendMessage(*peer.connection, proto::GateRefused{status});
    }

    // An item used (the original's 0x704C; every use that goes through tells ItemUsed, its 0x305C, to
    // those who see the user): the potions and pills (UseRecovery), the speed scrolls, the monster
    // mask, the return scroll (TID 3/3/3/1 whose Param3 text is RESURRECT): one taken from the stack at
    // once, then after its time (Param1, ms) the character goes to its return point (its race's start
    // town until it designates one at a gate).
    // `given`: the item's use without the item (the premium's daily uses, UsePremium): nothing is taken from the bag.
    // An item's COOLTIME (game::CoolTimeOf; the original asks it before any use of an item that has one): its wait still
    // running, by its key (game::CoolTimeKey: its group, or the item for group 0).
    bool CoolTimeRunning(const PlayerWorld& player, const assets::ItemRow& row, Clock::time_point now) const {
        const game::ItemCoolTime cool = game::CoolTimeOf(row);
        if (cool.ms == 0) return false;
        const auto it = player.coolTimes.find(game::CoolTimeKey(row.refObjId, cool.group));
        return it != player.coolTimes.end() && now < it->second;
    }
    // Its wait from a use, the user told (its item and its group's grey).
    void StartCoolTime(Peer& peer, PlayerWorld& player, const assets::ItemRow& row, Clock::time_point now) {
        const game::ItemCoolTime cool = game::CoolTimeOf(row);
        if (cool.ms == 0) return;
        player.coolTimes[game::CoolTimeKey(row.refObjId, cool.group)] = now + std::chrono::milliseconds(cool.ms);
        SendMessage(*peer.connection, proto::Cooldown{proto::CooldownKind::Item, row.refObjId, cool.ms});
    }

    // An item used from a bag place (or `given`, a premium's daily use). One with a COOLTIME is refused 0x185B while its
    // wait runs and starts it when used (its stack at the place gone down), whatever path takes it, as the original's
    // dispatcher; the Dimension Holes keep their own (Instances: kept over a logout), the fortress items theirs
    // (FortressUseItem).
    void UseItem(Peer& peer, PlayerWorld& player, const proto::UseItem& use, const assets::ItemRow* given = nullptr) {
        const game::ItemInstance* item = given != nullptr ? nullptr : player.inventory.At(use.slot);
        const assets::ItemRow* row = item != nullptr && config.rules->items ? config.rules->items->Find(item->refId) : nullptr;
        const bool hole = row != nullptr && row->typeId == std::array<int, 4>{3, 3, 12, 7};
        if (row == nullptr || hole || player.dead || game::CoolTimeOf(*row).ms == 0) return UseItemAt(peer, player, use, given);
        const auto now = Clock::now();
        if (CoolTimeRunning(player, *row, now)) return RefuseItem(peer, player, *row, game::kCoolTimeRefused, "its wait");
        const std::uint32_t refId = item->refId;
        const std::uint16_t count = item->count;
        UseItemAt(peer, player, use, given);
        const game::ItemInstance* after = player.inventory.At(use.slot);
        if (after == nullptr || after->refId != refId || after->count < count) StartCoolTime(peer, player, *row, now);
    }

    void UseItemAt(Peer& peer, PlayerWorld& player, const proto::UseItem& use, const assets::ItemRow* given) {
        const game::ItemInstance* item = given != nullptr ? nullptr : player.inventory.At(use.slot);
        const assets::ItemRow* row = given != nullptr ? given : item != nullptr && config.rules->items ? config.rules->items->Find(item->refId) : nullptr;
        const auto refuse = [&](const char* why) { core::LogInfo("{}: {} uses place {}: refused ({})", config.name, player.name, use.slot, why); };
        if ((given == nullptr && use.slot < game::slot::kWorn) || row == nullptr) return refuse("no item");
        // The item mall's own (UseMallService: tickets, the premium, expansions, the resurrection scroll, ...).
        if (given == nullptr && UseMallService(peer, player, use.slot, *row)) return;
        // A Dimension Hole or a talisman: the instances' own.
        if (item != nullptr && instances && instances->UseItem(player.entity, use.slot, *row)) return;
        // So-Ok's Subscription Card. A quest-type item (3/3/9; TID3 15 too) is refused in a fortress's
        // world before the flag event takes it (0x18F3 first).
        if (item != nullptr && flagWorlds && row->typeId[0] == 3 && row->typeId[1] == 3 && (row->typeId[2] == 9 || row->typeId[2] == 15)) {
            if (const auto at = world->Where(player.entity); at && fortresses && fortresses->FortressArea(at->position.region)) {
                RefuseItem(peer, player, *row, game::kQuestItemInFortress, "in a fortress's world");
                return;
            }
        }
        if (item != nullptr && flagWorlds && flagWorlds->UseItem(player.entity, use.slot, *row)) return;
        // A ride's or a transport's scroll (Coses.h): used up when it summons; a pet's item stays.
        if (item != nullptr && coses) {
            switch (coses->UseItem(player.entity, *row, *item, Clock::now())) {
            case Coses::Use::Summoned:
                TakeOne(peer, player, use.slot);
                Broadcast(player.entity, proto::ItemUsed{player.entity, row->refObjId});
                return;
            case Coses::Use::Kept:
            case Coses::Use::Refused: return;
            case Coses::Use::NotCos: break;
            }
        }
        // A fortress war's item wants its place or zone:.
        if (row->typeId[0] == 3 && row->typeId[1] == 3 &&
            ((row->typeId[2] == 3 && row->typeId[3] >= 6 && row->typeId[3] <= 11 && row->typeId[3] != 10) || (row->typeId[2] == 1 && row->typeId[3] == 10) ||
             (row->typeId[2] == 6 && row->typeId[3] == 2))) {
            return refuse("a fortress item (FortressUseItem)");
        }
        const bool returnScroll = row->typeId == std::array<int, 4>{3, 3, 3, 1} && row->paramTexts[2] == "RESURRECT";
        // The scrolls and balloons (TID 3/3/13/1..3): the skill Desc1 names, started on the user as any buff
        // (the speed scrolls ('hste'), the balloons, the damage +20 % and absorb 20 % scrolls); a second one starts the
        // time again.
        const bool speedScroll = row->typeId[0] == 3 && row->typeId[1] == 3 && row->typeId[2] == 13 && row->typeId[3] >= 1 && row->typeId[3] <= 3;
        if (speedScroll && item != nullptr) {
            const assets::SkillRow* skill = config.skills ? config.skills->Find(std::string_view(row->paramTexts[0])) : nullptr;
            if (skill == nullptr) return refuse("no skill");
            if (!skillUnit) return refuse("no skills");
            if (player.dead) {
                SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, 0x1889});
                return refuse("dead");
            }
            SkillCaster caster = CasterOf(player);
            // A quest's trap or net (its skill's 'trap' and 'qest': GORIA_6's, GENERAL_1's, IVY_2's;
            // 8.2): its object set at the user's feet, its catch the quest's event (its item's COOLTIME, 60 s, as any item's:
            // UseItem).
            if (Skills::IsQuestTrap(*skill)) {
                if (const std::uint16_t code = skillUnit->SetTrap(caster, *skill, Clock::now()); code != 0) {
                    SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, code});
                    return refuse("the trap's place");
                }
                TakeOne(peer, player, use.slot);
                Broadcast(player.entity, proto::ItemUsed{player.entity, row->refObjId});
                core::LogInfo("{}: {} sets {} ({})", config.name, player.name, row->codename, skill->codename);
                return;
            }
            if (skillUnit->AddBuff(caster, *skill, skill->hasDuration ? skill->buffDurationMs : 3'600'000u, Clock::now()) == nullptr) {
                SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, 2}); // the branch's code stays 2 (inferred)
                return refuse("a buff it may not stand beside runs");
            }
            TakeOne(peer, player, use.slot);
            Broadcast(player.entity, proto::ItemUsed{player.entity, row->refObjId});
            core::LogInfo("{}: {} reads {} ({})", config.name, player.name, row->codename, skill->codename);
            return;
        }
        // The monster mask (TID 3/2/2): its skill (Desc1, 'msch 1') as the monster it carries (its data):
        // that monster's shape, its level at most the user's.
        if (item != nullptr && game::IsMonsterMask(*row)) {
            const assets::SkillRow* skill = config.skills ? config.skills->Find(std::string_view(row->paramTexts[0])) : nullptr;
            const assets::ObjectRow* monster = config.characters ? config.characters->Find(item->durability) : nullptr;
            if (skill == nullptr || monster == nullptr || !skillUnit) return refuse("no monster in the mask");
            SkillCaster caster = CasterOf(player);
            if (!skillUnit->TransformInto(caster, *skill, monster->refObjId, monster->level, monster->skills[0], Clock::now())) return refuse("refused");
            TakeOne(peer, player, use.slot);
            Broadcast(player.entity, proto::ItemUsed{player.entity, row->refObjId});
            core::LogInfo("{}: {} takes the shape of {}", config.name, player.name, monster->codename);
            return;
        }
        // Potions (TID 3/3/1/1..3) and the players' pills (3/3/2/1, 6; the mall's pill bag 8 as the universal pill).
        if (item != nullptr && ((game::IsPotion(*row) && row->typeId[3] >= 1 && row->typeId[3] <= 3) ||
                                (game::IsCure(*row) && (row->typeId[3] == 1 || row->typeId[3] == 6 || row->typeId[3] == 8)))) {
            if (const char* why = UseRecovery(peer, player, use.slot, *row, Clock::now(), true)) refuse(why);
            return;
        }
        // The pets' HP recovery kit (TID 3/3/1/4): on its user's COS.
        if (item != nullptr && coses && game::IsPetHpPotion(*row)) {
            if (const char* why = UsePetRecovery(peer, player, use.slot, *row, Clock::now())) refuse(why);
            return;
        }
        // The pets' universal pill (TID 3/3/2/7).
        if (item != nullptr && coses && game::IsCure(*row) && row->typeId[3] == 7) {
            UsePetCure(peer, player, use, *row);
            return;
        }
        // A stall decoration (TID 3/3/3/4, the item mall's booths): the player's stall shows it from now on, permanently
        // (the item's own description); the item is used up (ours: inferred, as any used item).
        if (item != nullptr && row->typeId == std::array<int, 4>{3, 3, 3, 4}) {
            player.stallSkin = row->refObjId;
            TakeOne(peer, player, use.slot);
            Broadcast(player.entity, proto::ItemUsed{player.entity, row->refObjId});
            database.Submit([character = player.selected, skin = player.stallSkin](Database& db) { SaveStallSkin(db, character, skin); });
            if (stalls) stalls->SkinChanged(player.entity, player.stallSkin);
            core::LogInfo("{}: {} decorates its stall with {}", config.name, player.name, row->codename);
            return;
        }
        // Dead: no item but the resurrection scroll (0x1889 `UIIT_SKILL_USE_FAIL_CHAR_DEAD`).
        if (player.dead) {
            SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, 0x1889});
            return refuse("dead");
        }
        // The repair hammer: everything mended, free.
        if (game::IsRepairHammer(*row)) {
            UseHammer(peer, player, use.slot, *row);
            return;
        }
        // The reverse return scroll (TID 3/3/3/3): its checks in the original's order, each refused with its code; then,
        // as a return scroll, the item spent and the wait of its Param1 (1 s) before the travel. By its type alone (the
        // quest's Certificate of Reverse Return, ITEM_QNO_WC_FERRY2_1_01, is one too).
        const bool reverseScroll = row->typeId == std::array<int, 4>{3, 3, 3, 3};
        if (reverseScroll) {
            const auto refuseWith = [&](std::uint16_t code, const char* why) {
                SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, code});
                refuse(why);
            };
            if (instances && instances->LayerOf(player.entity) != 0) return refuseWith(0x18F5, "in an instance");
            if (arenas && arenas->InMatch(player.entity)) return refuseWith(0x187A, "in a Battle Arena match");
            if (player.returnAt) return refuseWith(0x185D, "already travelling");
            // A murderer: 0x1875.
            if (player.pvpState == std::uint8_t(game::PvpState::Murderer)) return refuseWith(0x1875, "a murderer");
            if (coses && coses->TransportOut(player.entity)) return refuseWith(0x185E, "a transport is out");
            if (use.option == proto::kReverseToRecall) {
                if (player.recallSpot.region == 0) return refuseWith(0x1885, "no recall point");
            } else if (use.option == proto::kReverseToDeath) {
                if (player.deathSpot.region == 0) return refuseWith(0x1886, "no place of death");
            } else if (use.option == proto::kReverseToPoint) {
                const auto it = optionalTeleports.find(use.point);
                if (it == optionalTeleports.end()) return refuseWith(2, "no such point");
                const OptionalTeleportRow& p = it->second;
                if (p.levelMin > 0 && p.levelMax > 0 && (player.level < p.levelMin || player.level > p.levelMax)) return refuseWith(0x18FD, "the point's level");
            } else {
                return refuseWith(2, "no such option");
            }
            // Into a fortress's world: not in a job suit (0x18E9), not while a fortress war runs (0x18F2).
            const std::uint16_t destination = use.option == proto::kReverseToRecall ? player.recallSpot.region
                                              : use.option == proto::kReverseToDeath ? player.deathSpot.region
                                                                                     : optionalTeleports.at(use.point).region;
            if (fortresses && fortresses->FortressArea(destination)) {
                if (ActivityOf(player).jobSuit) return refuseWith(0x18E9, "a job suit into a fortress");
                if (fortresses->WarOn()) return refuseWith(0x18F2, "into a fortress in its war");
            }
            if (given == nullptr) TakeOne(peer, player, use.slot);
            Broadcast(player.entity, proto::ItemUsed{player.entity, row->refObjId});
            const int ms = std::max(100, row->params[0]);
            player.returnAt = Clock::now() + std::chrono::milliseconds(ms);
            player.reverseOption = use.option;
            player.reversePoint = use.point;
            SendMessage(*peer.connection, proto::ReturnStarted{row->refObjId, std::uint32_t(ms)});
            core::LogInfo("{}: {} reads {} (option {}, {} ms)", config.name, player.name, row->codename, int(use.option), ms);
            return;
        }
        // The thieves' town scroll (ITEM_ETC_SCROLL_RETURN_THIEFDEN_01, use kind THIEFDEN): a
        // thief's only (0x186A), and unlike the others it works with a transport out, which comes along.
        const bool thiefDen = row->typeId == std::array<int, 4>{3, 3, 3, 1} && row->paramTexts[2] == "THIEFDEN";
        // Fireworks, the berserk potion, the bonus scrolls, the quests' and events' items, the uses the original
        // refuses.
        // A guild's mercenary scroll, on a shard that runs them (else the original's refusal below, 2).
        if (item != nullptr && config.guildMercenaries && game::IsMercenaryScroll(*row)) {
            UseMercenaryScroll(peer, player, use.slot, *row);
            return;
        }
        // The guild recall: its master's, its members asked on both game servers.
        if (item != nullptr && game::IsGuildRecall(*row)) {
            UseGuildRecall(peer, player, use.slot, *row);
            return;
        }
        if (!returnScroll && !thiefDen && UseSpecialItem(peer, player, use, *row, item)) return;
        if (!returnScroll && !thiefDen) return refuse("not usable yet");
        // The original's order: a return under way (0x185D), a murderer (0x1875), a fortress's area in its war (0x18EB,
        // UIIT_MSG_FORT_ITEMUSE_ERR_NOTPORTAL), a Battle Arena match (0x187A); then the den's scroll a thief's only (0x186A)
        // and its building's point (2), the others not with a transport out (0x185E).
        const auto refuseWith = [&](std::uint16_t code, const char* why) {
            SendMessage(*peer.connection, proto::ItemRefused{row->refObjId, code});
            refuse(why);
        };
        if (player.returnAt) return refuseWith(0x185D, "already returning");
        if (player.pvpState == std::uint8_t(game::PvpState::Murderer)) return refuseWith(0x1875, "a murderer");
        if (const auto at = world->Where(player.entity); at && fortresses && fortresses->WarArea(at->position.region)) {
            return refuseWith(0x18EB, "in a fortress at war");
        }
        if (arenas && arenas->InMatch(player.entity)) return refuseWith(0x187A, "in a Battle Arena match");
        // Where the den's scroll leads: the building its Param4 names (ITEM_ETC_SCROLL_RETURN_THIEFDEN_01: STORE_TD_GATE)
        // and that building's teleport point (GATE_TD).
        const assets::TeleportPoint* den = nullptr;
        if (thiefDen) {
            if (player.jobState != std::uint8_t(game::JobType::Thief)) return refuseWith(0x186A, "only a thief reads it");
            if (config.teleports) {
                for (const assets::TeleportGate& g : config.teleports->Gates()) {
                    if (g.codename == row->paramTexts[3]) den = config.teleports->PointOf(g.refObjId);
                }
            }
            if (den == nullptr || !den->HasArrival()) return refuseWith(2, "no point for its building");
        } else if (TransportOut(player)) {
            return refuseWith(0x185E, "a transport is out");
        }
        player.toThiefDen = thiefDen;
        player.thiefDenPoint = den != nullptr ? den->id : 0;
        // Where it read it: the reverse return scroll's recall point (not in an instance).
        if (const auto spot = KeepableSpot(player)) {
            player.recallSpot = *spot;
            database.Submit([character = player.selected, s = *spot](Database& db) { SaveRecallSpot(db, character, s); });
        }
        player.reverseOption = 0;
        if (given == nullptr) TakeOne(peer, player, use.slot);
        Broadcast(player.entity, proto::ItemUsed{player.entity, row->refObjId}); // SYSTEM_RETURNSCROLL on the reader
        player.returnAt = Clock::now() + std::chrono::milliseconds(std::max(100, row->params[0]));
        SendMessage(*peer.connection, proto::ReturnStarted{row->refObjId, std::uint32_t(std::max(100, row->params[0]))});
        core::LogInfo("{}: {} reads {} ({} ms)", config.name, player.name, row->codename, std::max(100, row->params[0]));
    }

    // --- The recovery items and the auto potion -------------------------

    // A potion or a pill in a bag place used, by the player or by its auto potion: refused (why, for
    // the log; the original answers 0xB04C with 0x1889 dead, 0x185B locked) when the user is dead or
    // the item's kind is locked, or when it would give nothing; else one is taken, the kind locked for
    // its time (Cooldown to the user), the HP and MP given or the cure done, and ItemUsed told to
    // everyone who sees the user, the user too (..).
    // `answer`: a use of the player's own, whose refusal it hears with the original's code (0x1889 dead, 0x185B its kind
    // locked, 2 nothing to give); the auto potion's tries are not answered.
    const char* UseRecovery(Peer& peer, PlayerWorld& player, std::uint8_t slot, const assets::ItemRow& row, Clock::time_point now, bool answer = false) {
        const auto refused = [&](std::uint16_t code, const char* why) {
            if (answer) SendMessage(*peer.connection, proto::ItemRefused{row.refObjId, code});
            return why;
        };
        if (player.dead) return refused(0x1889, "dead");
        const game::ReuseLock lock = game::ReuseLockOf(row, player.wearer.country == 1);
        if (const auto it = player.reuseLocks.find(lock.bit); lock.bit != 0 && it != player.reuseLocks.end() && now < it->second) return refused(0x185B, "too soon");
        bool vitals = false;
        if (game::IsPotion(row)) {
            const game::Recovery r = game::RecoveryOf(row, player.level, player.stats.strength, player.stats.intelligence, player.stats.maxHp, player.stats.maxMp);
            if (r.hp == 0 && r.mp == 0) return refused(2, "gives nothing");
            TakeOne(peer, player, slot);
            // A zombie's HP part heals nothing (ours: the original turns it on the drinker,
            // which is not traced further); its MP part works.
            const std::uint32_t hp = player.combat.states.In(game::StateKind::Zombie) ? 0u : r.hp;
            const auto give = [&](std::uint32_t amount, std::uint32_t& value, std::uint32_t most, std::vector<PlayerWorld::PotionSteps>& steps, bool isHp) {
                if (amount == 0) return;
                if (!r.flat) {
                    // A percent potion at once. One that overfills the gauge drops what is queued; else the queue is
                    // walked adding each potion's steps to the room it leaves, the first that passes the most kept and
                    // those after it dropped.
                    if (std::uint64_t(value) + amount > most) {
                        steps.clear();
                    } else {
                        std::uint64_t room = std::uint64_t(value) + amount;
                        for (std::size_t i = 0; i < steps.size(); ++i) {
                            room += std::uint64_t(steps[i].step) * std::uint64_t(std::max(0, steps[i].left));
                            if (room > most) {
                                steps.resize(i + 1);
                                break;
                            }
                        }
                    }
                    value = std::uint32_t(std::min<std::uint64_t>(most, std::uint64_t(value) + game::RecoveryCut(player.combat.states, amount, isHp)));
                    vitals = true;
                    return;
                }
                // A flat one queues when what it has and what is queued are no more than its most (HP)
                // or less than it (MP); its first step now, four to come.
                std::uint64_t queued = value;
                for (const auto& s : steps) queued += std::uint64_t(s.step) * std::uint64_t(s.left);
                if (isHp ? queued > most : queued >= most) return;
                const std::uint32_t step = game::PotionStep(amount);
                value = std::uint32_t(std::min<std::uint64_t>(most, std::uint64_t(value) + game::RecoveryCut(player.combat.states, step, isHp)));
                // Queued behind the others of its kind: one potion's steps at a time (StepPotions).
                steps.push_back({step, game::kPotionSteps - 1, now + kPotionStepPeriod});
                vitals = true;
            };
            give(hp, player.hp, player.stats.maxHp, player.hpSteps, true);
            give(r.mp, player.mp, player.stats.maxMp, player.mpSteps, false);
        } else {
            const game::CureSpec cure = game::PillCureOf(row);
            if (!cure) return refused(2, "no cure");
            TakeOne(peer, player, slot);
            if (player.combat.states.Cure(cure, player.combat.rolls, fightRandom, now)) TellStates(player.entity, StateRecords(player, now), now);
        }
        if (lock.bit != 0) {
            player.reuseLocks[lock.bit] = now + std::chrono::milliseconds(lock.ms);
            SendMessage(*peer.connection, proto::Cooldown{proto::CooldownKind::Item, row.refObjId, std::uint32_t(lock.ms)});
        }
        if (vitals) Healed(peer, player);
        Broadcast(player.entity, proto::ItemUsed{player.entity, row.refObjId});
        return nullptr;
    }
    static constexpr auto kPotionStepPeriod = std::chrono::seconds(1); // ours: inferred

    // The pets' HP recovery kit: its user's lock (0x08, 1.1 s) checked, the amount
    // worked out for its user (reads the owner), given to its COS at once (Coses::HealPet), then one taken and
    // the kind locked.
    const char* UsePetRecovery(Peer& peer, PlayerWorld& player, std::uint8_t slot, const assets::ItemRow& row, Clock::time_point now,
                               game::CosKind kind = game::CosKind::None) {
        if (player.dead) return "dead";
        const game::ReuseLock lock = game::ReuseLockOf(row, player.wearer.country == 1);
        if (const auto it = player.reuseLocks.find(lock.bit); lock.bit != 0 && it != player.reuseLocks.end() && now < it->second) return "too soon";
        const game::Recovery r = game::RecoveryOf(row, player.level, player.stats.strength, player.stats.intelligence, player.stats.maxHp, player.stats.maxMp);
        if (r.hp == 0) return "gives nothing"; // (no pet MP potion is in the data)
        const std::uint32_t healed = coses->HealPet(player.entity, r.hp, kind);
        if (healed == 0) return "no COS";
        TakeOne(peer, player, slot);
        if (lock.bit != 0) {
            player.reuseLocks[lock.bit] = now + std::chrono::milliseconds(lock.ms);
            SendMessage(*peer.connection, proto::Cooldown{proto::CooldownKind::Item, row.refObjId, std::uint32_t(lock.ms)});
        }
        // Its look on the COS it healed (a horse's heal on the horse, not its rider).
        Broadcast(healed, proto::ItemUsed{healed, row.refObjId});
        return nullptr;
    }

    // The pets' universal pill (3/3/2/7): on the COS the request names (the original's
    // tail: its entity; ours `point`, else the user's COS out), its user's, else 3; its bad states shortened by
    // Param1..6 levels (ours: a COS carries no bad states, so nothing is left to
    // cure), no reuse lock; used up and told.
    void UsePetCure(Peer& peer, PlayerWorld& player, const proto::UseItem& use, const assets::ItemRow& row) {
        if (player.dead) return RefuseItem(peer, player, row, 0x1889, "dead");
        const bool target = use.point != 0 ? coses->OwnerOf(use.point) == player.entity
                                           : coses->Health(player.entity, game::CosKind::None).has_value() || coses->MountOf(player.entity) != 0;
        if (!target) return RefuseItem(peer, player, row, 3, "no COS of its own");
        TakeOne(peer, player, use.slot);
        // Its look on the COS (the one asked for, else its mount, else its user's).
        const std::uint32_t on = use.point != 0 ? use.point : coses->MountOf(player.entity) != 0 ? coses->MountOf(player.entity) : player.entity;
        Broadcast(on, proto::ItemUsed{on, row.refObjId});
        core::LogInfo("{}: {} gives {} to its COS", config.name, player.name, row.codename);
    }

    // The guild recall scroll (game/GuildRecall.h), the original's checks in its order
    // (the item use's guild-points gate, then the summon's TID4 3): its
    // guild's master, a shared world, the guild's level, a town but the thieves' village, its own wait (an hour, kept as
    // the original's timed job 2 / 3); then the item spent and every member online on either game server asked to come
    // (Guilds::Recall) to where the master stands.
    std::set<std::int64_t> recallsChecking; // a use whose wait is being looked up (a second one is refused meanwhile)
    // A mercenary scroll (game/GuildMercenary.h; the shard's switch): the original's SummonMercenary in its order where ours
    // keeps the state (a teleport under way, a job suit, free PvP, the scroll's soldiers known and a guild of level 3, its
    // master, its count by level, none out, its wait, the race, a row for the level), then the wait (_TimedJob 2 / 4,
    // 1,200 s) taken, the scroll spent and the soldiers out beside their master for the scroll's minutes.
    void UseMercenaryScroll(Peer& peer, PlayerWorld& player, std::uint8_t slot, const assets::ItemRow& row) {
        if (player.returnAt) return RefuseItem(peer, player, row, game::kMercTravelling, "travelling");
        if (player.jobState == std::uint8_t(game::Job::Thief)) return RefuseItem(peer, player, row, game::kMercThief, "a thief");
        if (player.jobState != game::kNoJob) return RefuseItem(peer, player, row, game::kMercJob, "a job suit");
        if (player.freePvp != 0 || player.freePvpAt) return RefuseItem(peer, player, row, game::kMercFreePvp, "free PvP");
        const assets::ObjectRow* group = config.characters ? config.characters->Find(std::string_view(game::PetCodename(row))) : nullptr;
        const auto facts = guilds ? guilds->FactsOf(player.selected) : std::nullopt;
        if (group == nullptr || !facts || facts->level < game::kMercSummonGuildLevel) return RefuseItem(peer, player, row, game::kMercCannot, "no guild of level 3");
        if (!facts->master) return RefuseItem(peer, player, row, game::kMercSummonNotMaster, "not its guild's master");
        const int count = game::MercenaryCount(facts->level, facts->unionLeader);
        if (count <= 0) return RefuseItem(peer, player, row, game::kMercCannot, "none for its guild's level");
        if (coses && coses->SoldiersOut(player.entity) > 0) return RefuseItem(peer, player, row, game::kMercOut, "soldiers out already");
        if (group->country != player.wearer.country) return RefuseItem(peer, player, row, game::kMercRace, "another race's soldiers");
        const std::optional<std::string> codename = game::MercenaryCodename(row, player.level);
        const assets::ObjectRow* shape = codename ? config.characters->Find(std::string_view(*codename)) : nullptr;
        if (shape == nullptr) return RefuseItem(peer, player, row, game::kMercNoRow, "no soldier for its level");
        if (!recallsChecking.insert(player.selected).second) return RefuseItem(peer, player, row, game::kMercWaiting, "a use under way");
        const auto taken = std::make_shared<bool>(false);
        GuildSubmit([taken, character = player.selected](Database& db) {
            *taken = TakeTimedWait(db, character, game::kMercWaitCategory, game::kMercWaitJob, game::kMercWaitSeconds);
        },
                    [this, taken, peerId = peer.id, character = player.selected, slot, refId = row.refObjId, model = shape->refObjId, count,
                     abilities = facts->mercenaryAttr, minutes = game::MercenaryMinutes(row)] {
                        recallsChecking.erase(character);
                        Peer* peer = peers->Find(peerId);
                        const auto it = players.find(peerId);
                        if (peer == nullptr || it == players.end() || it->second.selected != character || it->second.entity == 0) return;
                        PlayerWorld& player = it->second;
                        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(refId) : nullptr;
                        const assets::ObjectRow* shape = config.characters ? config.characters->Find(model) : nullptr;
                        if (row == nullptr || shape == nullptr) return;
                        if (!*taken) return RefuseItem(*peer, player, *row, game::kMercWaiting, "its wait runs");
                        const game::ItemInstance* item = player.inventory.At(slot);
                        if (item == nullptr || item->refId != refId) return RefuseItem(*peer, player, *row, 2, "the item went meanwhile");
                        TakeOne(*peer, player, slot);
                        Broadcast(player.entity, proto::ItemUsed{player.entity, refId});
                        if (coses) coses->SummonSoldiers(player.entity, *shape, count, minutes, abilities, Clock::now());
                    });
    }

    void UseGuildRecall(Peer& peer, PlayerWorld& player, std::uint8_t slot, const assets::ItemRow& row) {
        const game::GuildRecallItem recall = game::GuildRecallOf(row);
        const auto facts = guilds ? guilds->FactsOf(player.selected) : std::nullopt;
        if (!facts || !facts->master) return RefuseItem(peer, player, row, game::kRecallNotMaster, "not a guild's master");
        const auto place = world->Where(player.entity);
        if (!place) return RefuseItem(peer, player, row, game::kRecallWorld, "not in the world");
        if (place->position.layer != 0 || (instances && instances->LayerOf(player.entity) != 0)) {
            return RefuseItem(peer, player, row, game::kRecallWorld, "in an instance");
        }
        if (facts->level < recall.guildLevel) return RefuseItem(peer, player, row, game::kRecallGuildLevel, "the guild's level");
        if (!BanditTown(place->position.region)) return RefuseItem(peer, player, row, game::kRecallNotTown, "not in a town");
        if (place->position.region == game::kThievesVillageRegion) return RefuseItem(peer, player, row, game::kRecallThievesTown, "the thieves' village");
        if (!recallsChecking.insert(player.selected).second) return RefuseItem(peer, player, row, game::kRecallWaiting, "a use under way");
        // The wait, then the use (the original makes its timed job before the members are asked).
        const auto taken = std::make_shared<bool>(false);
        GuildSubmit([taken, character = player.selected, seconds = recall.waitSeconds](Database& db) {
            *taken = TakeTimedWait(db, character, game::kRecallWaitCategory, game::kRecallWaitJob, seconds);
        },
                    [this, taken, peerId = peer.id, character = player.selected, slot, refId = row.refObjId, to = place->position, recall] {
                        recallsChecking.erase(character);
                        Peer* peer = peers->Find(peerId);
                        const auto it = players.find(peerId);
                        if (peer == nullptr || it == players.end() || it->second.selected != character || it->second.entity == 0) return;
                        PlayerWorld& player = it->second;
                        const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(refId) : nullptr;
                        if (row == nullptr) return;
                        if (!*taken) return RefuseItem(*peer, player, *row, game::kRecallWaiting, "its wait runs");
                        const game::ItemInstance* item = player.inventory.At(slot);
                        if (item == nullptr || item->refId != refId) return RefuseItem(*peer, player, *row, 2, "the item went meanwhile");
                        TakeOne(*peer, player, slot);
                        Broadcast(player.entity, proto::ItemUsed{player.entity, refId});
                        if (guilds) guilds->Recall(GuildActorOf(peerId, player), to, recall.answerSeconds);
                    });
    }

    // The special items: fireworks, the berserk potion, the bonus scrolls, the quests' and
    // events' items, and the uses the original's server refuses; false: none of these. In the original's order: the
    // item's COOLTIME (0x185B), the quests' handlers (2 when none takes it), then
    // the kind's own work; a use that goes through takes one and is told to those around (0x305C, ItemUsed).
    bool UseSpecialItem(Peer& peer, PlayerWorld& player, const proto::UseItem& use, const assets::ItemRow& row, const game::ItemInstance* item) {
        const bool firework = game::IsFirework(row), berserk = game::IsBerserkPotion(row), quest = game::IsQuestUseItem(row);
        const game::BonusScroll bonus = game::BonusScrollOf(row);
        const std::uint16_t refusal = game::OriginalUseRefusal(row);
        if (!firework && !berserk && !quest && !bonus && refusal == 0) return false;
        if (item == nullptr) return true; // a premium's daily use names none of these
        const auto now = Clock::now();
        if (refusal != 0) {
            RefuseItem(peer, player, row, refusal, "the original's server refuses it");
            return true;
        }
        // A quest's or an event's item: never in a
        // fortress's world (its game world's slot 31: 0x18F3); then the player's quests' handlers, then the events';
        // none taking it: refused 2. Taken, it is used up below.
        if (quest) {
            const auto at = world->Where(player.entity);
            if (at && fortresses && fortresses->FortressArea(at->position.region)) {
                RefuseItem(peer, player, row, game::kQuestItemInFortress, "in a fortress's world");
                return true;
            }
            questUseTarget = use.target; // the client's selection (Quests::UseItem's targets)
            const bool taken = (quests && quests->UseItem(player.entity, row.codename)) || (summons && summons->UseItem(player.entity, row.codename, now));
            questUseTarget = 0;
            if (!taken) {
                RefuseItem(peer, player, row, game::kNoQuestTakesIt, "no quest or event takes it");
                return true;
            }
        }
        if (berserk && player.berserkUntil) {
            RefuseItem(peer, player, row, game::kBerserkPotionRefused, "in berserk");
            return true;
        }
        const std::int64_t unix = ServiceNow();
        if (bonus && std::any_of(player.bonuses.begin(), player.bonuses.end(), [&](const PlayerWorld::BonusRun& b) { return b.key == bonus.key && b.until > unix; })) {
            RefuseItem(peer, player, row, game::kBonusRunning, "one of its key runs");
            return true;
        }
        TakeOne(peer, player, use.slot);
        if (berserk) AddBerserkPoints(player, game::kBerserkPotionPoints);
        if (bonus) {
            std::erase_if(player.bonuses, [&](const PlayerWorld::BonusRun& b) { return b.key == bonus.key; });
            player.bonuses.push_back({bonus.kind, bonus.key, row.refObjId, bonus.value, unix + bonus.seconds});
            SaveServices(player);
            TellServices(player); // its buff board entry
        }
        Broadcast(player.entity, proto::ItemUsed{player.entity, row.refObjId});
        core::LogInfo("{}: {} uses {}{}", config.name, player.name, row.codename,
                      bonus ? std::format(" (+{} for {} s)", bonus.value, bonus.seconds) : std::string());
        return true;
    }

    // Its health or mana went up: the world, the player, and its bar for those who see it.
    void Healed(Peer& peer, const PlayerWorld& player) {
        world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
        SendVitals(peer, player);
        Broadcast(player.entity, proto::EntityDamaged{player.entity, 0, 0, player.hp, 0});
    }

    std::vector<proto::StateRecord> StateRecords(const PlayerWorld& player, Clock::time_point now) const {
        std::vector<proto::StateRecord> records;
        for (const game::ActiveState& a : player.combat.states.All()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(a.Ends() - now).count();
            records.push_back({std::uint8_t(a.kind), std::uint8_t(std::clamp(a.level, 0, 255)), std::uint32_t(std::max<long long>(0, left))});
        }
        return records;
    }

    // The flat potions' steps due (one a second after the first); a dead player's are dropped.
    void StepPotions(Clock::time_point now) {
        for (auto& [peerId, player] : players) {
            if (player.entity == 0 || (player.hpSteps.empty() && player.mpSteps.empty())) continue;
            if (player.dead) {
                player.hpSteps.clear();
                player.mpSteps.clear();
                continue;
            }
            bool changed = false;
            // Each kind a queue: only its first potion steps (one step a tick), the next one's steps start on the tick
            // after the first's last, as the original's.
            const auto run = [&](std::vector<PlayerWorld::PotionSteps>& steps, std::uint32_t& value, std::uint32_t most, bool isHp) {
                if (steps.empty()) return;
                PlayerWorld::PotionSteps& s = steps.front();
                if (s.left > 0 && now >= s.next) {
                    value = std::uint32_t(std::min<std::uint64_t>(most, std::uint64_t(value) + game::RecoveryCut(player.combat.states, s.step, isHp)));
                    --s.left;
                    s.next = now + kPotionStepPeriod;
                    changed = true;
                }
                if (s.left <= 0) {
                    steps.erase(steps.begin());
                    if (!steps.empty()) steps.front().next = std::max(steps.front().next, now + kPotionStepPeriod);
                }
            };
            run(player.hpSteps, player.hp, player.stats.maxHp, true);
            run(player.mpSteps, player.mp, player.stats.maxMp, false);
            if (changed) {
                if (Peer* peer = peers->Find(peerId)) Healed(*peer, player);
            }
        }
    }

    // Every player's auto potion (ours: the server uses it, the original's client did;
    // section 4). A row holds while its user is alive and: HP at or under its threshold, not while a
    // zombie; MP at or under its; for the abnormal row any state but while stunned
    // (0x4000). A row that comes to hold is tried at once, then every delay (500 ms when the
    // delay is off) while it holds (the original's timers 0xE, 0xF, 0x10); a try uses the first stack
    // of the row's item in the bag under the manual rules (UseRecovery: the kind's lock, death), so
    // when a stack runs out the next one is used, and none left means nothing is used.
    void AutoPotions(Clock::time_point now) {
        if (!config.rules->items) return;
        for (auto& [peerId, player] : players) {
            if (player.entity == 0) continue;
            if (exchanges && exchanges->Holding(player.entity)) continue; // its bag is held (no item is used, 0x1835)
            if (stalls && stalls->Holding(player.entity)) continue;
            for (int r = 0; r < game::AutoPotion::kRows; ++r) {
                const game::AutoPotion::Entry& e = player.autoPotion.rows[std::size_t(r)];
                bool holds = false;
                if (e.enabled && e.item != 0 && !player.dead) {
                    const std::uint32_t states = player.combat.states.Mask();
                    if (r == game::AutoPotion::kHp) {
                        holds = player.hp <= game::AutoPotionThreshold(player.stats.maxHp, e.percent) && !player.combat.states.In(game::StateKind::Zombie);
                    } else if (r == game::AutoPotion::kMp) {
                        holds = player.mp <= game::AutoPotionThreshold(player.stats.maxMp, e.percent);
                    } else if (r == game::AutoPotion::kVehicle || r == game::AutoPotion::kAttackPet) {
                        // Ours: its vehicle's or attack pet's health at or under the row's share.
                        const auto health = coses ? coses->Health(player.entity, CosKindOfRow(r)) : std::nullopt;
                        holds = health && health->first <= game::AutoPotionThreshold(health->second, e.percent);
                    } else {
                        holds = states != 0 && !player.combat.states.In(game::StateKind::Stun);
                    }
                }
                std::optional<Clock::time_point>& next = player.autoNext[std::size_t(r)];
                if (!holds) {
                    next.reset();
                    continue;
                }
                if (next && now < *next) continue;
                next = now + std::chrono::milliseconds(player.autoPotion.RepeatMs());
                const assets::ItemRow* row = config.rules->items->Find(e.item);
                std::optional<std::uint8_t> place;
                for (const auto& [slot, item] : player.inventory.places) {
                    if (slot >= game::slot::kWorn && item.refId == e.item) {
                        place = slot;
                        break;
                    }
                }
                Peer* peer = peers->Find(peerId);
                if (row == nullptr || !place || peer == nullptr) continue;
                // An item with a COOLTIME (an HP and MP potion of an event's) waits it as a use by hand.
                if (CoolTimeRunning(player, *row, now)) continue;
                const char* why = r >= game::AutoPotion::kVehicle ? UsePetRecovery(*peer, player, *place, *row, now, CosKindOfRow(r))
                                                                   : UseRecovery(*peer, player, *place, *row, now);
                if (why == nullptr) StartCoolTime(*peer, player, *row, now);
                if (why != nullptr) {
                    // A kind still locked is the usual answer between two uses: not logged.
                    if (std::string_view(why) != "too soon") core::LogInfo("{}: {}'s auto potion ({}): {}", config.name, player.name, row->codename, why);
                } else {
                    core::LogInfo("{}: {}'s auto potion uses {} from place {} (HP {}/{}, MP {}/{})", config.name, player.name, row->codename, *place,
                                  player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
                }
            }
        }
    }

    // The COS kind an auto potion row of ours heals (a vehicle: a ride or a transport).
    static game::CosKind CosKindOfRow(int row) { return row == game::AutoPotion::kAttackPet ? game::CosKind::AttackPet : game::CosKind::Transport; }

    // The settings the window kept: within the original's ranges, an item that does not fit its row
    // left out; kept for the character and told back.
    void SetAutoPotion(Peer& peer, PlayerWorld& player, const proto::AutoPotionSettings& m) {
        game::AutoPotion s = game::FromMessage(m);
        s.Clamp();
        for (int r = 0; r < game::AutoPotion::kRows; ++r) {
            game::AutoPotion::Entry& e = s.rows[std::size_t(r)];
            const assets::ItemRow* row = e.item != 0 && config.rules->items ? config.rules->items->Find(e.item) : nullptr;
            if (row == nullptr || !game::AutoPotionFits(r, *row)) e.item = 0;
        }
        player.autoPotion = s;
        player.autoNext = {};
        database.Submit([character = player.selected, s](Database& db) { SaveAutoPotion(db, character, s); });
        SendMessage(*peer.connection, game::ToMessage(s));
    }

    // The return scrolls whose time is up: to the return point's arrival. And the buffs that ended.
    void FinishReturns() {
        const auto now = Clock::now();
        for (auto& [peerId, player] : players) {
            if (skillUnit && player.entity != 0) {
                SkillCaster caster = CasterOf(player);
                skillUnit->ExpireBuffs(caster, now);
            }
            if (!player.returnAt || now < *player.returnAt) continue;
            player.returnAt.reset();
            const std::optional<proto::WorldPosition> recall = std::exchange(player.recallTo, std::nullopt);
            if (player.entity == 0 || player.dead) continue;
            // The guild recall's gauge ran out: to the master's place, if it still may (as at its yes).
            if (recall) {
                const bool busy = MallBagHeld(player.entity);
                if (player.moving || player.pvpState == std::uint8_t(game::PvpState::Murderer) || (coses && coses->TransportOut(player.entity)) || busy ||
                    (instances && instances->LayerOf(player.entity) != 0) || !CanTravelTo(recall->region)) {
                    core::LogInfo("{}: {}'s guild recall no longer goes", config.name, player.name);
                    continue;
                }
                core::LogInfo("{}: {} goes to its master's recall", config.name, player.name);
                TravelTo(peerId, player, *recall);
                continue;
            }
            // A reverse return scroll: its place read again now.
            if (const std::uint8_t option = std::exchange(player.reverseOption, std::uint8_t(0)); option != 0) {
                std::optional<proto::WorldPosition> where;
                if (option == proto::kReverseToRecall && player.recallSpot.region != 0) {
                    where = proto::WorldPosition{player.recallSpot.region, player.recallSpot.x, player.recallSpot.y, player.recallSpot.z};
                } else if (option == proto::kReverseToDeath && player.deathSpot.region != 0) {
                    where = proto::WorldPosition{player.deathSpot.region, player.deathSpot.x, player.deathSpot.y, player.deathSpot.z};
                } else if (const auto it = optionalTeleports.find(player.reversePoint); option == proto::kReverseToPoint && it != optionalTeleports.end()) {
                    where = proto::WorldPosition{it->second.region, it->second.x, it->second.y, it->second.z};
                }
                if (!where) continue;
                if (!CanTravelTo(where->region)) {
                    if (Peer* peer = peers->Find(peerId)) SendMessage(*peer->connection, proto::GateRefused{proto::GateStatus::ServerDown});
                    continue;
                }
                core::LogInfo("{}: {} reverse-returns to {},{} ({:.0f}, {:.0f})", config.name, player.name, where->region & 0xFF, where->region >> 8, where->x, where->z);
                TravelTo(peerId, player, *where);
                continue;
            }
            // The thieves' town scroll's point (its building's: GATE_TD), its transport along; else the return point.
            const bool thiefDen = std::exchange(player.toThiefDen, false);
            const assets::TeleportPoint* to = thiefDen && config.teleports ? config.teleports->Point(player.thiefDenPoint) : ReturnArrival(player);
            if (to == nullptr) continue;
            if (!CanTravelTo(to->region)) {
                if (Peer* peer = peers->Find(peerId)) SendMessage(*peer->connection, proto::GateRefused{proto::GateStatus::ServerDown});
                continue;
            }
            core::LogInfo("{}: {} returns to {}", config.name, player.name, to->codename);
            DropHostility(player); // a return to town
            // A return to town (not the thieves' den's) clears every abnormal state (poison, burn, ...).
            if (!thiefDen && !player.combat.states.All().empty()) {
                player.combat.states.Clear();
                TellStates(player.entity, {}, now);
                StatsChanged(player.entity);
            }
            TravelTo(peerId, player, {to->region, to->x, to->y, to->z}, thiefDen);
        }
    }

    // A gate's menu (C7i): what its point offers, as the original's NPC window lists it (the
    // services mask of its select answer): the teleport when it leads somewhere, the return point
    // when its point may be one (teleportdata column 10).
    void TalkToGate(Peer& peer, const PlayerWorld& player, const proto::GateTalk& talk) {
        const assets::TeleportPoint* point = nullptr;
        if (const auto status = AtGate(player, talk.gate, point); status != proto::GateStatus::Ok) return RefuseGate(peer, talk.gate, "talk", status);
        proto::GateMenu menu{talk.gate, 0};
        if (!config.teleports->LinksFrom(point->id).empty()) menu.services |= proto::kGateTeleport;
        // The snake gate (no link of its own): into floor 5's copy while the tomb's gate is open (QinTomb.h).
        if (point->id == QinTomb::kSnakeGate && qinTomb && qinTomb->GateOpen()) {
            menu.services |= proto::kGateTeleport;
            menu.opened.push_back(QinTomb::kFloor5Gate);
        }
        if (point->recall) menu.services |= proto::kGateRecallPoint;
        menu.tax = NpcTaxFor(point->ownerRefObjId, player);
        SendMessage(*peer.connection, menu);
    }

    // The gate's point as the return point: kept for the character (C7i).
    void DesignateReturn(Peer& peer, PlayerWorld& player, const proto::DesignateReturn& designate) {
        const assets::TeleportPoint* point = nullptr;
        if (const auto status = AtGate(player, designate.gate, point); status != proto::GateStatus::Ok) {
            return RefuseGate(peer, designate.gate, "return point", status);
        }
        if (!point->recall) return RefuseGate(peer, designate.gate, "return point", proto::GateStatus::NoRecall);
        player.returnPoint = point->id;
        database.Submit([character = player.selected, gate = point->ownerRefObjId](Database& db) { SaveReturnGate(db, character, gate); });
        core::LogInfo("{}: {} designates {} as the return point", config.name, player.name, point->codename);
        SendMessage(*peer.connection, proto::ReturnPointSet{point->id});
    }

    // A gate (C6e): the traveller at it, a link from its point to the destination, an
    // arrival there, the link's fee paid (C7i; GoldChanged); then the world takes it (WorldEntered
    // follows).
    void UseGate(Peer& peer, PlayerWorld& player, const proto::UseGate& use) {
        const auto refuse = [&](proto::GateStatus status) { RefuseGate(peer, use.gate, "teleport", status); };
        // A return scroll counting down: 0x1C14, before anything else.
        if (player.returnAt) return refuse(proto::GateStatus::Returning);
        const assets::TeleportPoint* from = nullptr;
        if (const auto status = AtGate(player, use.gate, from); status != proto::GateStatus::Ok) return refuse(status);
        // The shard's PK switch shuts the thieves' gates: the temple's (GATE_TEMPLE_THIEF_GATE) does nothing, the desert's
        // structure (STORE_DESERT_THIEF_GATE, its point GATE_DESERT_THIEF_GATE) cannot even be selected (the original's
        // selection answers 3); ours: no answer to either.
        if (config.pkProhibited && (from->codename == "GATE_TEMPLE_THIEF_GATE" || from->codename == "GATE_DESERT_THIEF_GATE")) return;
        // A transport out farther than 1000 from him (3D), before the link is read: 0x1C13.
        if (coses) {
            if (const auto cart = coses->TransportCart(player.entity)) {
                const auto me = world->Where(player.entity), it = world->Where(cart->entity);
                if (me && it) {
                    const proto::FlatPosition a = proto::ToFlat(me->position), b = proto::ToFlat(it->position);
                    if (std::sqrt(double(a.x - b.x) * (a.x - b.x) + double(me->position.y - it->position.y) * double(me->position.y - it->position.y) + double(a.z - b.z) * (a.z - b.z)) > 1000.0) {
                        return refuse(proto::GateStatus::TransportFar);
                    }
                }
            }
        }
        // Through the snake gate into floor 5's copy (the link the tomb's opening gives it): no transport out (as the tomb's
        // other links), then the copy's admission.
        if (from->id == QinTomb::kSnakeGate && use.destination == QinTomb::kFloor5Gate) {
            if (!qinTomb || !qinTomb->GateOpen()) return refuse(proto::GateStatus::NoSuchLink);
            if (TransportOut(player)) return refuse(proto::GateStatus::Transport);
            if (const proto::GateStatus status = qinTomb->Admit(player.entity); status != proto::GateStatus::Ok) return refuse(status);
            core::LogInfo("{}: {} goes through the snake gate into the tomb's 5th floor", config.name, player.name);
            return;
        }
        const auto links = config.teleports->LinksFrom(from->id);
        const auto link = std::find_if(links.begin(), links.end(), [&](const assets::TeleportLink* l) { return l->to == use.destination; });
        if (link == links.end()) return refuse(proto::GateStatus::NoSuchLink);
        const assets::TeleportPoint* to = config.teleports->Point(use.destination);
        if (!to || !to->HasArrival()) return refuse(proto::GateStatus::Nowhere);
        // The thieves' den (GATE_TD) is not for a trader's or a hunter's league (0x1C18).
        if (to->codename == "GATE_TD" && (player.job.type == game::JobType::Trader || player.job.type == game::JobType::Hunter)) {
            return refuse(proto::GateStatus::ThiefDen);
        }
        if (const proto::GateStatus status = LinkRefusal(player, **link); status != proto::GateStatus::Ok) return refuse(status);
        // The fare, free for one whose highest level is 20 or less, and the fortress tax of the gate's town on it; the gold.
        std::uint32_t taxFortress = 0;
        const std::uint64_t fare = std::max(player.level, player.maxLevel) <= 20 ? 0u : (*link)->fee;
        const std::int64_t tax = fare > 0 ? TaxOn(from->ownerRefObjId, player, fare, &taxFortress) : 0;
        const std::uint64_t fee = tax >= 0 ? fare + std::uint64_t(tax) : fare - std::min<std::uint64_t>(fare, std::uint64_t(-tax));
        if (player.inventory.gold < fee) return refuse(proto::GateStatus::NotEnoughGold);
        // Into a fortress's world (the original's world check): through one of its fortress gates (TID 4/1/1/1) the war's
        // entry, through any other gate there (its revival, charge and glory gates) the holder's side only; the murderer's
        // check is not made there.
        const bool intoFortress = fortresses && fortresses->FortressArea(to->region);
        if (intoFortress) {
            const assets::TeleportGate* building = config.teleports->Gate(from->ownerRefObjId);
            const game::FortressError e = fortresses->EntryRefusal(player.selected, to->region, building != nullptr && building->FortressGate(), ActivityOf(player).jobSuit);
            switch (e) {
            case game::FortressError::None: break;
            case game::FortressError::EntryJobSuit: return refuse(proto::GateStatus::FortressJobSuit);
            case game::FortressError::EntryFull: return refuse(proto::GateStatus::FortressFull);
            case game::FortressError::EntryNotMember: return refuse(proto::GateStatus::FortressNotMember);
            case game::FortressError::EntryClosed: return refuse(proto::GateStatus::FortressClosed);
            default: return refuse(proto::GateStatus::FortressNotParticipant);
            }
        }
        // A murderer through a gate structure (0x1C16: a teleportbuilding gate, not an NPC's teleport), within the main
        // world or into the Tomb of Qin's 6th floor (region 0x8002); not into an instance world (ours: none is reached
        // through a gate yet), nor into a fortress's world.
        if (!intoFortress && config.teleports->Gate(use.gate) != nullptr && player.pvpState == std::uint8_t(game::PvpState::Murderer) &&
            (places.Of(to->region) == game::PlaceKind::Field || to->region == 0x8002)) {
            return refuse(proto::GateStatus::Murderer);
        }
        // Up to the Roc's Peak: its own check (not on a
        // ride, 0x1C30; ours the transport's words), the gate open (ours: shut, nowhere to go), then the link's key used up
        // (kind 5) and the player into the Peak's layer.
        // Into the job temples' area: its own checks (a job suit on, the alliance's share).
        if (temples && (to->id == Temples::kAreaGateTraderHunter || to->id == Temples::kAreaGateThief)) {
            if (const proto::GateStatus status = temples->AreaRefusal(player.entity, to->id); status != proto::GateStatus::Ok) return refuse(status);
        }
        // Into the Pharaoh's tomb: the world's own admission, its party instance.
        if (const GameWorldSpec* tomb = instances ? instances->TombOfPoint(to->id) : nullptr) {
            if (const proto::GateStatus status = instances->EnterTomb(player.entity, *tomb); status != proto::GateStatus::Ok) return refuse(status);
            core::LogInfo("{}: {} goes into {}", config.name, player.name, tomb->codename);
            return;
        }
        if (to->id == Roc::kPeakGate) {
            if (player.riding) return refuse(proto::GateStatus::Riding);
            if (!roc || !roc->GateOpen()) return refuse(proto::GateStatus::Nowhere);
            for (const assets::TeleportLink::Restriction& r : (*link)->restrictions) {
                if (r.kind != 5) continue;
                for (const auto& [place, item] : player.inventory.places) {
                    if (place < game::slot::kWorn || item.refId != r.a) continue;
                    TakeOne(peer, player, place);
                    break;
                }
            }
            if (!roc->Admit(player.entity)) return refuse(proto::GateStatus::Nowhere);
            core::LogInfo("{}: {} goes up to the Roc's Peak", config.name, player.name);
            return;
        }
        if (!CanTravelTo(to->region)) return refuse(proto::GateStatus::ServerDown);
        CollectTax(taxFortress, tax);
        if (fee > 0) {
            player.inventory.gold -= fee;
            QueueGoldSave(player.selected, player.inventory.gold);
            SendMessage(*peer.connection, proto::GoldChanged{player.inventory.gold});
        }
        core::LogInfo("{}: {} through {} to {} ({},{}) for {} gold", config.name, peer.address, from->codename, to->codename, to->region & 0xFF,
                      to->region >> 8, fee);
        // Its transport goes along where the link lets one cross (the ferries;
        // the towns' gates refuse one by their restriction 2, LinkRefusal), a ride too, its rider on it (Coses::BringTransport).
        TravelTo(peer.id, player, {to->region, to->x, to->y, to->z}, true);
    }

    // A developer's teleport to the nearest nest of a monster (by its codename), on this server or the other one.
    void DevGotoMonster(Peer& peer, PlayerWorld& player, const std::string& codename) {
        if (!config.devCommands) {
            core::LogInfo("{}: {} asked for a developer teleport; not a development server", config.name, peer.address);
            return;
        }
        const assets::ObjectRow* row = config.characters ? config.characters->Find(std::string_view(codename)) : nullptr;
        const auto me = world->Where(player.entity);
        const MonsterNest* best = nullptr;
        double bestDistance = 0;
        for (const MonsterNest& n : devNests.empty() ? config.monsterNests : devNests) {
            if (row == nullptr || n.refObjId != row->refObjId || n.position.layer != 0 || !CanTravelTo(n.position.region)) continue;
            const proto::FlatPosition a = me ? proto::ToFlat(me->position) : proto::FlatPosition{}, b = proto::ToFlat(n.position);
            const double d = std::hypot(a.x - b.x, a.z - b.z);
            if (best == nullptr || d < bestDistance) {
                best = &n;
                bestDistance = d;
            }
        }
        // An NPC's place when no monster has that name (the quests' tests: to a giver, a finisher).
        if (best == nullptr) {
            for (const GameServerConfig::NpcSpawn& n : devNpcs.empty() ? config.npcs : devNpcs) {
                if (n.codename != codename || !CanTravelTo(n.position.region)) continue;
                core::LogInfo("{}: {} goes to {} (developer teleport)", config.name, player.name, codename);
                proto::WorldPosition to = n.position;
                to.x += 20.0f;
                return TravelTo(peer.id, player, to, true);
            }
        }
        if (best == nullptr) {
            core::LogInfo("{}: {} asked for a developer teleport to {}: no nest of it", config.name, player.name, codename);
            SendMessage(*peer.connection, proto::GateRefused{proto::GateStatus::Nowhere});
            return;
        }
        core::LogInfo("{}: {} goes to a nest of {} (developer teleport)", config.name, player.name, codename);
        TravelTo(peer.id, player, best->position, true);
    }

    void DevGoto(Peer& peer, PlayerWorld& player, std::uint32_t point) {
        if (!config.devCommands) {
            core::LogInfo("{}: {} asked for a developer teleport; not a development server", config.name, peer.address);
            return;
        }
        const assets::TeleportPoint* to = config.teleports ? config.teleports->Point(point) : nullptr;
        if (!to || !to->HasArrival()) {
            SendMessage(*peer.connection, proto::GateRefused{proto::GateStatus::Nowhere});
            return;
        }
        if (!CanTravelTo(to->region)) {
            SendMessage(*peer.connection, proto::GateRefused{proto::GateStatus::ServerDown});
            return;
        }
        core::LogInfo("{}: {} goes to {} (developer)", config.name, peer.address, to->codename);
        // Ours: its ride or transport along too, its goods with it (a trader's tries; on this server only, as a gate's
        // link that allows one).
        TravelTo(peer.id, player, {to->region, to->x, to->y, to->z}, true);
    }

    // A developer's teleport to a place of the outdoor world: put down on the highest ground there (the drop from above).
    void DevGotoPlace(Peer& peer, PlayerWorld& player, const proto::DevGotoPlace& at) {
        if (!config.devCommands) {
            core::LogInfo("{}: {} asked for a developer teleport; not a development server", config.name, peer.address);
            return;
        }
        const bool dungeon = (at.region & 0x8000) != 0;
        const bool inside = dungeon ? at.y != proto::DevGotoPlace::kNoHeight && std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z)
                                    : at.x >= 0.0f && at.x <= 1920.0f && at.z >= 0.0f && at.z <= 1920.0f;
        if (!inside) {
            SendMessage(*peer.connection, proto::GateRefused{proto::GateStatus::Nowhere});
            return;
        }
        if (!CanTravelTo(at.region)) {
            SendMessage(*peer.connection, proto::GateRefused{proto::GateStatus::ServerDown});
            return;
        }
        core::LogInfo("{}: {} goes to ({},{}) {:.0f},{:.0f} (developer)", config.name, player.name, at.region & 0xFF, at.region >> 8, at.x, at.z);
        proto::WorldPosition to{at.region, at.x, dungeon ? at.y : proto::DevGotoPlace::kNoHeight, at.z};
        // Within the dungeon it stands in: its layer kept (an instance's own copy, e.g. a temple's).
        if (const auto here = world->Where(player.entity); dungeon && here && here->position.region == at.region) to.layer = here->position.layer;
        TravelTo(peer.id, player, to, true);
    }

    // --- The skill lab's setup (L2; --dev-commands only) --------------------------------------

    bool LabAllowed(Peer& peer, const char* what) {
        if (config.devCommands) return true;
        core::LogInfo("{}: {} asked for {} (the skill lab's); not a development server", config.name, peer.address, what);
        return false;
    }

    // A mastery at `level`, every group of its board at the highest level that allows (at most `skillLevel`), the
    // character raised to `level` first; no skill points spent, everything saved as a learning would save it.
    void DevLearn(Peer& peer, PlayerWorld& player, const proto::DevLearn& r) {
        if (!LabAllowed(peer, "a mastery") || !config.skills) return;
        const assets::MasteryRow* mastery = config.skills->Mastery(r.mastery);
        if (mastery == nullptr || !game::MasteryOfRace(*config.skills, r.mastery, player.wearer.country) || r.level == 0) {
            core::LogInfo("{}: {} asked for mastery {}: not its race's", config.name, player.name, r.mastery);
            return;
        }
        if (player.level < r.level) {
            std::uint64_t need = 0;
            for (int l = player.level; l < r.level && std::size_t(l) < config.levels.size(); ++l) need += config.levels[std::size_t(l)];
            if (need > player.experience) GainExperience(peer, player, need - player.experience, 0);
        }
        player.book.masteries[r.mastery] = r.level;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> learned; // the row before, the row now
        for (const assets::SkillRow* first : config.skills->Board(r.mastery)) {
            const assets::SkillRow* best = nullptr;
            for (int level = first->level; level < 256; ++level) {
                const assets::SkillRow* row = config.skills->GroupLevel(first->group, level);
                if (row == nullptr) break;
                if (row->masteryLevels[0] > r.level || (r.skillLevel > 0 && row->level > r.skillLevel)) break;
                if (row->inService) best = row;
            }
            if (best == nullptr || player.book.GroupLevel(best->group) >= best->level) continue;
            const auto had = player.book.groups.find(best->group);
            const assets::SkillRow* before = had != player.book.groups.end() ? config.skills->GroupLevel(best->group, had->second) : nullptr;
            player.book.groups[best->group] = best->level;
            learned.push_back({before != nullptr ? before->id : 0u, best->id});
        }
        CharacterWrite(player.selected, [character = player.selected, mastery = r.mastery, level = r.level, learned, p = ProgressOf(player)](Database& db) {
            SaveMastery(db, character, mastery, level);
            for (const auto& [previous, id] : learned) SaveSkill(db, character, previous, id);
            SaveProgress(db, {p});
        });
        player.progressDirty = false;
        core::LogInfo("{}: {} has mastery {} at {} and {} skills learned (the skill lab)", config.name, player.name, r.mastery, r.level, learned.size());
        SendSkillBook(peer, player);
        UpdateStats(&peer, player); // the passives' numbers
        SendProgress(peer, player);
    }

    // A lab monster `dx`, `dz` from the player: a nest of its own that never refills; its entity answered.
    void DevSpawn(Peer& peer, PlayerWorld& player, const proto::DevSpawn& r) {
        proto::DevSpawned answer{r.tag, 0};
        const auto me = world->Where(player.entity);
        const assets::ObjectRow* row = config.characters ? config.characters->Find(std::string_view(r.monster)) : nullptr;
        if (LabAllowed(peer, "a monster") && monsters && me && row != nullptr) {
            MonsterNest spec;
            spec.refObjId = row->refObjId;
            const proto::FlatPosition here = proto::ToFlat(me->position);
            spec.position = proto::FromFlat({here.x + r.dx, here.z + r.dz}, me->position.y);
            spec.position.layer = me->position.layer;
            spec.radius = 2000.0f;
            spec.maxCount = 1;
            spec.respawn = false;
            spec.temporary = true;
            spec.rarity = 0;
            spec.lab = r.behaviour;
            spec.labFrom = player.entity;
            spec.labHp = r.hp;
            const std::size_t nest = monsters->AddNest(spec);
            const std::vector<std::uint32_t> made = nest != SIZE_MAX ? monsters->NestMonsters(nest) : std::vector<std::uint32_t>{};
            if (!made.empty()) {
                answer.entity = made.front();
                labMonsters[answer.entity] = {nest, player.entity};
            } else if (nest != SIZE_MAX) {
                monsters->RemoveNest(nest); // no ground there (its region's navigation not in)
            }
        }
        core::LogInfo("{}: {} spawns {} (tag {}, behaviour {}) for the skill lab: {}", config.name, player.name, r.monster, r.tag, int(r.behaviour),
                      answer.entity);
        SendMessage(*peer.connection, answer);
    }

    // The player's lab monsters (`entity`, 0: all of them) taken away.
    void DevRemove(PlayerWorld& player, std::uint32_t entity) {
        for (auto it = labMonsters.begin(); it != labMonsters.end();) {
            if (it->second.owner != player.entity || (entity != 0 && it->first != entity)) {
                ++it;
                continue;
            }
            if (monsters) monsters->RemoveNest(it->second.nest);
            it = labMonsters.erase(it);
        }
    }

    // A state and a knock on a monster or a player, from nobody (as a hit would put them).
    void DevAfflict(Peer& peer, PlayerWorld& player, const proto::DevAfflict& r) {
        if (!LabAllowed(peer, "a state")) return;
        const auto now = Clock::now();
        const auto me = world->Where(player.entity);
        Monsters::Affliction a;
        if (r.state != proto::DevAfflict::kNoState && r.state < std::uint8_t(game::kStateSlots)) {
            game::ActiveState state;
            state.kind = game::StateKind(r.state);
            state.level = r.level;
            state.durationMs = r.ms;
            state.value = r.value;
            state.started = state.nextTick = now;
            a.states.push_back(state);
        }
        if (r.knock == 1 || r.knock == 2) {
            a.knock = game::Knock(r.knock);
            a.from = me ? me->position : proto::WorldPosition{};
            a.distance = 30.0f;
        }
        if (PlayerWorld* victim = PlayerByEntity(r.target)) {
            if (victim->dead) return;
            if (!a.states.empty()) GiveStates(*victim, a.states, now);
            if (a.knock != game::Knock::None) {
                a.knockMs = a.knock == game::Knock::Down ? game::KnockDownMs(0, 0) : game::kKnockBackMs;
                Monsters::Damaged d;
                KnockPlayer(*victim, a, now, d);
                Broadcast(victim->entity, proto::EntityKnocked{victim->entity, player.entity, proto::KnockKind(d.knock), d.knockTo, d.knockMs});
            }
        } else if (monsters) {
            const auto target = monsters->TargetOf(r.target);
            if (!target) return;
            if (a.knock == game::Knock::Down && !target->canFall) a.knock = game::Knock::None;
            if (a.knock != game::Knock::None) a.knockMs = a.knock == game::Knock::Down ? game::KnockDownMs(target->recoverMs, 0) : game::kKnockBackMs;
            const Monsters::Damaged d = monsters->Damage(r.target, 0, 0, now, &a);
            if (d.knock != game::Knock::None) {
                Broadcast(r.target, proto::EntityKnocked{r.target, player.entity, proto::KnockKind(d.knock), d.knockTo, d.knockMs});
            }
            if (d.statesChanged) TellStates(r.target, monsters->StatesOf(r.target, now), now);
        }
        core::LogInfo("{}: {} puts state {} level {} ({} ms) and knock {} on {} (the skill lab)", config.name, player.name, int(r.state), r.level, r.ms,
                      int(r.knock), r.target);
    }

    // The player's waits over, its health and mana full, its states gone, its buffs ended (the bits asked).
    void DevReset(Peer& peer, PlayerWorld& player, std::uint8_t what, const proto::WorldPosition& place = {}) {
        if (!LabAllowed(peer, "a reset")) return;
        const auto now = Clock::now();
        if ((what & proto::DevReset::kPlace) != 0) {
            player.combat.StopFighting();
            if (const auto here = world->Where(player.entity); here && world->Moving(player.entity)) world->MoveTo(player.entity, here->position);
            if (const std::optional<proto::WorldPosition> to = world->Blink(player.entity, place, 5000.0f)) {
                Broadcast(player.entity, proto::EntityBlink{player.entity, 0, proto::BlinkKind::Put, *to});
            }
        }
        if ((what & proto::DevReset::kServices) != 0) {
            if (player.premium.Running(ServiceNow())) PremiumSkillOff(player, player.premium.item);
            player.services.clear();
            player.premium = {};
            player.premiumBooth = false;
            player.bonuses.clear();
            player.growthUntil = 0;
            SaveServices(player); // their rows gone
        }
        if ((what & proto::DevReset::kWaits) != 0) {
            if (config.skills) {
                for (const std::uint32_t id : player.book.Skills(*config.skills)) {
                    const assets::SkillRow* row = config.skills->Find(id);
                    if (row != nullptr && player.combat.skillCooling.contains(row->group)) {
                        SendMessage(*peer.connection, proto::Cooldown{proto::CooldownKind::Skill, id, 0});
                    }
                }
            }
            player.combat.skillCooling.clear();
            player.combat.groupCooling.clear();
            // The items' waits too: the potions' kinds and the COOLTIME groups (the item exam's next potion is not held back
            // by the last one's lock).
            player.reuseLocks.clear();
            if (trades) trades->ClearBargainLock(player.entity);
            player.coolTimes.clear();
            player.hpSteps.clear();
            player.mpSteps.clear();
        }
        if ((what & proto::DevReset::kBuffs) != 0 && skillUnit && !player.combat.buffs.empty()) {
            std::vector<std::uint32_t> skills;
            for (const auto& b : player.combat.buffs) skills.push_back(b.skill);
            SkillCaster caster = CasterOf(player);
            skillUnit->EndBuffsOf(caster, skills);
            UpdateStats(&peer, player);
        }
        if ((what & proto::DevReset::kStates) != 0 && !player.combat.states.All().empty()) {
            player.combat.states = {};
            TellStates(player.entity, {}, now);
            StatsChanged(player.entity);
        }
        if ((what & proto::DevReset::kBattle) != 0) player.combat.battleUntil = {};
        if ((what & proto::DevReset::kBag) != 0) {
            std::vector<std::uint8_t> touched;
            std::vector<std::int64_t> gone;
            for (auto it = player.inventory.places.begin(); it != player.inventory.places.end();) {
                if (it->first < game::slot::kWorn) {
                    ++it;
                    continue;
                }
                touched.push_back(it->first);
                gone.push_back(it->second.serial);
                it = player.inventory.places.erase(it);
            }
            if (!touched.empty()) TellBag(player, std::move(touched), gone);
        }
        if ((what & proto::DevReset::kVitals) != 0 && player.dead) {
            // The lab's: risen where it lies (a resurrection's target left dead).
            player.dead = false;
            player.revivalOffer.reset();
            player.deathLoss = 0;
            player.warDeathAt.reset();
            player.combat.StopFighting();
            Broadcast(player.entity, proto::EntityDamaged{player.entity, 0, 0, player.stats.maxHp, 0});
        }
        if ((what & proto::DevReset::kVitals) != 0 && !player.dead) {
            player.hp = player.stats.maxHp;
            player.mp = player.stats.maxMp;
            world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
            SendVitals(peer, player);
        }
        core::LogInfo("{}: {} is reset ({}) for the skill lab", config.name, player.name, int(what));
    }

    // The skill lab's look at a player's passives and numbers (L5).
    void DevSheet(Peer& peer, PlayerWorld& player) {
        proto::DevSheetAnswer a;
        for (const game::Passives::Entry& e : player.combat.passives.entries) {
            a.passives.push_back({e.skill, e.active, std::uint16_t(std::min<std::size_t>(e.changes.size(), 0xFFFF))});
        }
        const game::CombatStats& s = player.stats;
        const auto put = [&](const char* name, double value) { a.numbers.push_back({name, float(value)}); };
        put("strength", s.strength);
        put("intelligence", s.intelligence);
        put("maxHp", s.maxHp);
        put("maxMp", s.maxMp);
        put("phyAttackMin", s.phyAttackMin);
        put("phyAttackMax", s.phyAttackMax);
        put("magAttackMin", s.magAttackMin);
        put("magAttackMax", s.magAttackMax);
        put("phyDefence", s.phyDefence);
        put("magDefence", s.magDefence);
        put("hitRate", s.hitRate);
        put("evasion", s.evasion);
        put("critical", s.critical);
        put("block", s.block);
        put("range", s.range);
        put("phyBalance", s.phyBalance);
        put("magBalance", s.magBalance);
        put("mpCostPercent", s.mpCostPercent);
        // The item exam's sets: the absorptions (the accessories' whites), the state roll's
        // resists (a set's 'rals' among them), and each worn place's set raise that is not 0 ("setPlus9": the earring's).
        put("phyAbsorption", s.phyAbsorption);
        put("magAbsorption", s.magAbsorption);
        const game::StateDefence d = StateDefenceOf(player);
        constexpr const char* kElements[] = {"resistFreeze", "resistFrostbite", "resistShock", "resistBurn", "resistPoison", "resistZombie"};
        for (std::size_t e = 0; e < 6; ++e) put(kElements[e], d.elementResist[e]);
        put("resistSleep", d.percent[0]);
        put("resistFear", d.percent[3]);
        put("resistStun", d.percent[7]);
        put("resistDisease", d.percent[8]);
        put("resistCombustion", d.percent[15]);
        if (config.rules->items) {
            for (int place = 0; place < game::slot::kWorn; ++place) {
                const game::ItemInstance* item = player.inventory.At(std::uint8_t(place));
                const int raise = item != nullptr ? player.sets.RaiseOf(game::SetPiece{item->serial, item->refId, place, 0, false, true, game::SetStamp(*item)}) : 0;
                if (raise != 0) put(std::format("setPlus{}", place).c_str(), raise);
            }
        }
        // The Devil's Spirit's: the damage on a unique (0x103, its UMDU stone's), the chance to
        // ignore a monster's defence (0xBC, its MOBDEFENCEIGNORE buff's), whether its contributions are on, and each skill
        // a worn item gives ("itemSkill31142": the spirit's, the sockets').
        put("uniqueDamage", s.classBonus);
        put("ignoreDefence", s.ignoreDefenceChance);
        put("spiritOn", player.spiritOn ? 1 : 0);
        for (const std::uint32_t id : player.book.itemSkills) {
            if (a.numbers.size() < 64) put(std::format("itemSkill{}", id).c_str(), 1);
        }
        SendMessage(*peer.connection, a);
    }

    // A developer's item: into the bag, then (`wear`) worn in its place by the move's rules.
    void DevGive(Peer& peer, PlayerWorld& player, const proto::DevGive& x) {
        const auto before = player.inventory.places;
        const bool given = QuestGive(player.entity, {{x.item, std::max<std::uint32_t>(1, x.count)}});
        core::LogInfo("{}: {} is given {} x{} by a developer command{}", config.name, player.name, x.item, x.count, given ? "" : ": refused");
        if (!given || !config.rules->items) return;
        const assets::ItemRow* row = config.rules->items->Find(std::string_view(x.item));
        if (row == nullptr) return;
        // Its blues, plus and data word, on the new item (the item exam's).
        if (!x.magic.empty() || x.plus != 0 || x.data != 0) {
            for (auto& [at, item] : player.inventory.places) {
                if (at < game::slot::kWorn || item.refId != row->refObjId) continue;
                const auto was = before.find(at);
                if (was != before.end() && was->second.refId == item.refId && was->second.count == item.count) continue;
                item.plus = x.plus;
                if (x.data != 0) item.durability = x.data; // the data word (a Devil's Spirit's awakening's end)
                item.options.clear();
                for (const std::uint64_t m : x.magic) item.options.push_back({std::uint32_t(m & 0xFFFFFFFFu), std::uint32_t(m >> 32)});
                TellBag(player, {at}, {});
                break;
            }
        }
        if (!x.wear) return;
        const int place = game::EquipPlaceFor(*row, player.inventory.At(game::slot::kRingLeft) != nullptr, player.inventory.At(game::slot::kRingRight) != nullptr);
        if (place < 0) return;
        for (const auto& [at, item] : player.inventory.places) {
            if (at < game::slot::kWorn || item.refId != row->refObjId) continue;
            const auto was = before.find(at);
            if (was != before.end() && was->second.refId == item.refId && was->second.count == item.count) continue; // there before
            MoveItem(peer, player, proto::ItemMove{at, std::uint8_t(place), 0});
            return;
        }
    }

    // A quick slot changed: kept for the character. The slot and the kind are checked here; what a
    // shortcut points at (a skill learned, an inventory place) is checked with those features (C9).
    void SetQuickSlot(Peer& peer, const PlayerWorld& player, const proto::QuickSlotRecord& r) {
        constexpr std::uint8_t kKinds[] = {0, 37, 70, 71, 73, 74, 78}; // game::QuickSlot::Kind
        if (r.slot >= 51 || std::find(std::begin(kKinds), std::end(kKinds), r.kind) == std::end(kKinds)) {
            core::LogInfo("{}: {} sent quick slot {} kind {}; ignored", config.name, peer.address, r.slot, r.kind);
            return;
        }
        const CharacterRow::QuickSlot slot{r.slot, r.kind, r.kind == 0 ? 0u : r.value};
        database.Submit([character = player.selected, slot](Database& db) { SaveQuickSlot(db, character, slot); });
    }

    // Ids for new items (a split stack): a block is asked for before the last ones are used.
    void TopUpItemIds() {
        if (reservingIds || freeItemIds.size() >= 64) return;
        reservingIds = true;
        database.Submit([this](Database& db) {
            std::vector<std::int64_t> ids = ReserveItemIds(db, 256);
            completions->Post([this, ids = std::move(ids)] {
                freeItemIds.insert(freeItemIds.end(), ids.begin(), ids.end());
                reservingIds = false;
            });
        });
    }

    void QueueItemSave(std::int64_t character, const game::Inventory& inventory, const std::vector<std::uint8_t>& touched,
                       const std::vector<std::int64_t>& gone, const std::vector<std::uint8_t>& avatarsTouched = {}) {
        ItemSaves& s = itemSaves[character];
        s.places = inventory.places;
        s.avatars = inventory.avatars;
        s.dirty.insert(touched.begin(), touched.end());
        s.avatarDirty.insert(avatarsTouched.begin(), avatarsTouched.end());
        s.gone.insert(s.gone.end(), gone.begin(), gone.end());
        if (!s.busy) FlushItems(character);
    }

    // The bag's save in the character's order (CharacterTask): what changed, as it is when its turn comes.
    void FlushItems(std::int64_t character) {
        const auto it = itemSaves.find(character);
        if (it == itemSaves.end()) return;
        ItemSaves& s = it->second;
        if (s.dirty.empty() && s.gone.empty() && s.avatarDirty.empty()) {
            itemSaves.erase(it);
            return;
        }
        s.busy = true;
        CharacterTask(character, [this, character] { return ItemSaveTask(character); }, [this, character] {
            if (const auto again = itemSaves.find(character); again != itemSaves.end()) {
                again->second.busy = false;
                FlushItems(character);
            }
        });
    }
    std::function<void(Database&)> ItemSaveTask(std::int64_t character) {
        const auto it = itemSaves.find(character);
        if (it == itemSaves.end()) return {};
        ItemSaves& s = it->second;
        // Taken meanwhile by a save of both sides (SaveSideOf): nothing left to write.
        if (s.dirty.empty() && s.gone.empty() && s.avatarDirty.empty()) return {};
        const auto stored = [this](std::uint8_t place, const game::ItemInstance& i) {
            StoredItem out{i.serial, place, i.refId, i.plus, i.variance, i.durability, i.count, {}, Counted(i.refId, i.durability)};
            out.bound = i.bound;
            out.advanced = i.advanced; // an advanced elixir's binding
            out.advancedRecipe = i.advancedRecipe;
            out.sockets = i.sockets;
            for (const game::ItemInstance::Option& o : i.options) out.magic.push_back(std::uint64_t(o.id) | std::uint64_t(o.value) << 32);
            return out;
        };
        std::vector<StoredItem> present;
        std::vector<std::uint8_t> emptied;
        for (std::uint8_t place : s.dirty) {
            const auto item = s.places.find(place);
            if (item == s.places.end()) {
                emptied.push_back(place);
                continue;
            }
            present.push_back(stored(place, item->second));
        }
        AvatarPlaces avatars;
        for (std::uint8_t place : s.avatarDirty) {
            const auto item = s.avatars.find(place);
            if (item == s.avatars.end()) {
                avatars.emptied.push_back(place);
            } else {
                avatars.present.push_back(stored(place, item->second));
            }
        }
        std::vector<std::int64_t> gone = std::move(s.gone);
        s.gone.clear();
        s.dirty.clear();
        s.avatarDirty.clear();
        return [this, character, present = std::move(present), emptied = std::move(emptied), gone = std::move(gone), avatars = std::move(avatars)](Database& db) {
            const bool saved = SaveItems(db, character, present, emptied, gone, avatars);
            completions->Post([this, character, saved, present, emptied, gone, avatars] {
                const auto it = itemSaves.find(character);
                if (saved) {
                    if (it != itemSaves.end()) it->second.failures = 0;
                    return;
                }
                // Not landed: its places written again (as they are then), a few times; the session ends.
                if (it != itemSaves.end() && ++it->second.failures <= kSaveRetries) {
                    for (const StoredItem& i : present) it->second.dirty.insert(i.slot);
                    it->second.dirty.insert(emptied.begin(), emptied.end());
                    it->second.gone.insert(it->second.gone.end(), gone.begin(), gone.end());
                    for (const StoredItem& i : avatars.present) it->second.avatarDirty.insert(i.slot);
                    it->second.avatarDirty.insert(avatars.emptied.begin(), avatars.emptied.end());
                }
                SaveFailed(character, "the bag");
            });
        };
    }

    // An item moved in the inventory (the original's 0x706D kind 0): the
    // inventory's rules decide; the mover hears the places as they are now, everyone who sees the
    // character (the owner too) what it wears now; the database follows.
    // The job suit's rules for a move into or out of place 8, in the original's order: 0 the move may start its change,
    // else the original's code. The
    // inventory's own rules (sex, country, level, a full bag) are a trial move on a copy of the bag.
    std::uint16_t SuitRefusal(const PlayerWorld& player, const proto::ItemMove& m, bool on, proto::ItemStatus& inventory) const {
        const auto now = Clock::now();
        // The interaction state 7 is the stall network's (its text UIIT_MSG_WARENETWORK_ERROR_TRIANGLE):
        // a stall up or a purchase from afar in flight; an exchange holds the bag as it does for any move.
        const bool busy = stalls && stalls->Holding(player.entity);
        const auto place = world->Where(player.entity);
        // A town region (`_RefRegion.IsBattleField` 0, as the bandits' and the PvP rules); without the region table,
        // regioninfo's town rectangles (ours).
        const bool inTown = place && place->position.layer == 0 &&
                            (!regionKinds.empty() ? BanditTown(place->position.region)
                                                  : towns && towns->TownAt(place->position.region, place->position.x, place->position.z) >= 0);
        const bool party = parties && parties->PartyOf(player.entity) != 0;
        game::Inventory trial = player.inventory;
        inventory = trial.Move(*config.rules->items, WearerOf(player), m.from, m.to, m.quantity).status;
        if (exchanges && exchanges->Holding(player.entity)) {
            inventory = proto::ItemStatus::Exchanging;
            return 0;
        }
        if (busy) return 0x18CD;                  // the stall network's interaction (state 7)
        if (player.riding) return 0x18CA;         // UIIT_MSG_JOBITEM_WEAR_ERR_RIDE
        if (player.suitChange) return 0x1847;     // a change under way
        if (on) {
            const game::ItemInstance* item = player.inventory.At(m.from);
            const assets::ItemRow* row = item != nullptr ? config.rules->items->Find(item->refId) : nullptr;
            if (player.freePvp != 0 || player.freePvpAt) return 0x18A3; // in free PvP
            if (row == nullptr || game::SuitJob(*row) != player.job.type || player.job.type == game::JobType::None) return 0x189E;
            if (player.alias.empty()) return 0x189F;
            if (!inTown) return 0x18A2;
            if (party) return 0x18A0;
            if (inventory != proto::ItemStatus::Ok) return 0; // its own refusal (sex, country, level)
            // The job level the suit asks for (ReqLevelType2 2..4, ReqLevel2; compares only the level).
            if (row->requirementTypes[1] >= 2 && row->requirementTypes[1] <= 4 && row->requirementLevels[1] > player.job.level) return 0x18A3;
            if (player.inventory.At(game::slot::kJobSuit) != nullptr) return 0x1846;
            return 0;
        }
        if (now < player.combat.battleUntil) return 0x1848; // within 20 s of a fight
        if (TransportOut(player)) return 0x185A;
        if (now < player.jobLockUntil) return 0x1847;
        if (party) return 0x18A0;
        if (!inTown) return 0x18A2;
        if (player.inventory.At(m.to) != nullptr || inventory == proto::ItemStatus::BagFull) return 0x1807;
        return 0;
    }

    void MoveItem(Peer& peer, PlayerWorld& player, const proto::ItemMove& m) {
        proto::ItemMoved answer;
        answer.from = m.from;
        answer.to = m.to;
        if (!config.rules->items) {
            answer.status = proto::ItemStatus::Failed;
            SendMessage(*peer.connection, answer);
            return;
        }
        // A weapon, or a shield, put in hand while a cast holds the hands: 0x1834 (the original's motion lock).
        if (const Clock::time_point now = Clock::now(); now < player.combat.castUntil || now < player.combat.chainUntil) {
            const game::ItemInstance* moving = player.inventory.At(m.from);
            const assets::ItemRow* row = moving != nullptr ? config.rules->items->Find(moving->refId) : nullptr;
            const bool shield = row != nullptr && row->typeId[0] == 3 && row->typeId[1] == 1 && row->typeId[2] == 4 && (row->typeId[3] == 1 || row->typeId[3] == 2);
            if (m.to == game::slot::kWeapon || (m.to == game::slot::kShield && shield)) {
                SendMessage(*peer.connection, proto::ItemRefused{row != nullptr ? row->refObjId : 0u, 0x1834});
                answer.status = proto::ItemStatus::Failed;
                SendMessage(*peer.connection, answer);
                return;
            }
        }
        // The job suit's place: its rules, then a 10 s change (the move made by TickSuitChanges). Only a league's own suit
        // has them: the event suit, the PvP cape and the whole-body suits move as any equipment, unless a league's suit
        // is the one worn there.
        const auto jobSuitAt = [&](std::uint8_t place) {
            const game::ItemInstance* at = player.inventory.At(place);
            const assets::ItemRow* row = at != nullptr ? config.rules->items->Find(at->refId) : nullptr;
            return row != nullptr && game::IsJobSuit(*row);
        };
        if ((m.to == game::slot::kJobSuit || m.from == game::slot::kJobSuit) && (jobSuitAt(m.from) || jobSuitAt(game::slot::kJobSuit))) {
            const bool on = m.to == game::slot::kJobSuit;
            proto::ItemStatus inventory = proto::ItemStatus::Ok;
            const std::uint16_t code = SuitRefusal(player, m, on, inventory);
            if (code != 0 || inventory != proto::ItemStatus::Ok) {
                answer.status = code != 0 ? proto::ItemStatus::JobSuit : inventory;
                answer.code = code;
                SendMessage(*peer.connection, answer);
                core::LogInfo("{}: {} {} its job suit: refused (0x{:04X}, {})", config.name, player.name, on ? "puts on" : "takes off", code, int(inventory));
                return;
            }
            player.suitChange = PlayerWorld::SuitChange{m, on, Clock::now() + std::chrono::milliseconds(int(config.jobSuitSeconds * 1000))};
            answer.status = proto::ItemStatus::JobChange;
            answer.code = 0x189B;
            SendMessage(*peer.connection, answer);
            const proto::EntityJobChange change{player.entity, std::uint8_t(on ? 1 : 0), std::uint16_t(std::max(1.0, std::ceil(config.jobSuitSeconds)))};
            Broadcast(player.entity, change); // those near and the player itself
            core::LogInfo("{}: {} {} its job suit (10 s)", config.name, player.name, on ? "puts on" : "takes off");
            return;
        }
        ApplyMove(peer, player, m);
    }

    // A job suit's change under way given up (a walk, an attack, a cast, a hit taken, a free-PvP change): the suit stays
    // where it was, those near told its end (the original's cancel, 0x3042).
    void CancelSuitChange(PlayerWorld& player) {
        if (!player.suitChange) return;
        const bool on = player.suitChange->on;
        player.suitChange.reset();
        if (player.entity != 0) Broadcast(player.entity, proto::EntityJobChange{player.entity, std::uint8_t(on ? 1 : 0), 0});
        core::LogInfo("{}: {} gives up its job suit's change", config.name, player.name);
    }
    // The job suits' changes whose 10 s are over: the move made now (a refusal of the inventory's now: given up); once on,
    // the job lock's 10 s.
    void TickSuitChanges(Clock::time_point now) {
        for (auto& [peerId, player] : players) {
            if (!player.suitChange || now < player.suitChange->at) continue;
            const PlayerWorld::SuitChange change = *player.suitChange;
            player.suitChange.reset();
            Peer* peer = peers->Find(peerId);
            if (peer == nullptr || player.entity == 0) continue;
            const proto::EntityJobChange over{player.entity, std::uint8_t(change.on ? 1 : 0), 0};
            Broadcast(player.entity, over); // those near and the player itself
            if (ApplyMove(*peer, player, change.move)) {
                if (change.on) player.jobLockUntil = now + std::chrono::milliseconds(int(config.jobLockSeconds * 1000));
                else ResetSafeTrade(player); // the suit off
            }
        }
    }

    // The move as the inventory's rules make it; true when it was made.
    bool ApplyMove(Peer& peer, PlayerWorld& player, const proto::ItemMove& m) {
        proto::ItemMoved answer;
        answer.from = m.from;
        answer.to = m.to;
        // A fortress weapon needs a fortress position that fits its mask (Param5; 0x18E0),
        // checked after CanEquip and the hands' rules: only a move they allow.
        if (m.to == game::slot::kWeapon) {
            const game::ItemInstance* item = player.inventory.At(m.from);
            const assets::ItemRow* row = item != nullptr ? config.rules->items->Find(item->refId) : nullptr;
            game::Inventory trial = player.inventory;
            if (row != nullptr && game::KindOf(*row) == game::ItemKind::Weapon && row->typeId[3] == 16 &&
                trial.Move(*config.rules->items, WearerOf(player), m.from, m.to, m.quantity).status == proto::ItemStatus::Ok &&
                (row->params[4] & (guilds ? guilds->PositionOf(player.selected) : 0)) == 0) {
                answer.status = proto::ItemStatus::FortressPosition;
                SendMessage(*peer.connection, answer);
                return false;
            }
        }
        const auto before = player.inventory.places;
        game::Inventory::MoveResult r = player.inventory.Move(*config.rules->items, WearerOf(player), m.from, m.to, m.quantity);
        // A split makes an item: it takes one of the reserved ids (none left: the move is undone).
        for (std::uint8_t place : r.touched) {
            auto it = player.inventory.places.find(place);
            if (r.status != proto::ItemStatus::Ok || it == player.inventory.places.end() || it->second.serial != 0) continue;
            if (freeItemIds.empty()) {
                player.inventory.places = before;
                r.status = proto::ItemStatus::Failed;
                break;
            }
            it->second.serial = freeItemIds.back();
            freeItemIds.pop_back();
        }
        answer.status = r.status;
        if (r.status == proto::ItemStatus::Ok) {
            for (std::uint8_t place : r.touched) {
                if (player.inventory.At(place)) {
                    answer.changed.push_back(player.inventory.Record(place));
                } else {
                    answer.emptied.push_back(place);
                }
            }
        }
        SendMessage(*peer.connection, answer);
        if (r.status != proto::ItemStatus::Ok) return false;
        bool worn = false;
        for (std::uint8_t place : r.touched) {
            if (place >= game::slot::kWorn) continue;
            worn = true;
            const game::ItemInstance* now = player.inventory.At(place);
            const auto was = before.find(place);
            const std::vector<std::byte> bytes = proto::Encode(proto::EquipmentChanged{
                player.entity, place, now ? now->refId : 0u, std::uint8_t(now ? game::TotalPlus(*now) : 0), was != before.end() ? was->second.refId : 0u});
            world->ForEachObserver(player.entity, [&](std::uint64_t to) {
                if (Peer* listener = peers->Find(to)) listener->connection->Send(proto::EquipmentChanged::kId, bytes);
            });
        }
        if (worn) {
            world->SetEquipment(player.entity, player.inventory.Worn());
            TellJob(&peer, player, true);
            UpdateStats(&peer, player);
            SendProgress(peer, player);
        }
        QueueItemSave(player.selected, player.inventory, r.touched, r.gone);
        TopUpItemIds();
        return true;
    }

    // An avatar put on or taken off (the original's 0x7034 kinds 0x24 and 0x23): the inventory's rules decide; the mover
    // hears the places as they are now; the database follows.
    // What it wears does not change its numbers here (the avatars' blues are not added yet) nor its look.
    void MoveAvatar(Peer& peer, PlayerWorld& player, const proto::AvatarMove& m) {
        proto::AvatarMoved answer;
        if (!config.rules->items) {
            answer.status = proto::ItemStatus::Failed;
            SendMessage(*peer.connection, answer);
            return;
        }
        // A Devil's Spirit only while awake (0x18F9).
        if (m.wear) {
            const game::ItemInstance* spirit = player.inventory.At(m.place);
            const assets::ItemRow* spiritRow = spirit != nullptr ? config.rules->items->Find(spirit->refId) : nullptr;
            if (spiritRow != nullptr && game::IsDevilSpirit(*spiritRow) && !game::DevilSpiritAwake(spirit->durability, ServiceNow())) {
                SendMessage(*peer.connection, proto::ItemRefused{spiritRow->refObjId, 0x18F9});
                answer.status = proto::ItemStatus::Failed;
                SendMessage(*peer.connection, answer);
                return;
            }
        }
        const game::Inventory::AvatarResult r = m.wear ? player.inventory.WearAvatar(*config.rules->items, WearerOf(player), m.place)
                                                       : player.inventory.TakeOffAvatar(*config.rules->items, m.place);
        answer.status = r.status;
        if (r.status == proto::ItemStatus::Ok) {
            for (std::uint8_t place : r.bag) {
                if (player.inventory.At(place)) {
                    answer.bag.push_back(player.inventory.Record(place));
                } else {
                    answer.bagEmptied.push_back(place);
                }
            }
            for (std::uint8_t place : r.avatars) {
                if (player.inventory.Avatar(place)) {
                    answer.avatars.push_back(player.inventory.AvatarRecord(place));
                } else {
                    answer.avatarsEmptied.push_back(place);
                }
            }
        }
        SendMessage(*peer.connection, answer);
        if (r.status != proto::ItemStatus::Ok) return;
        core::LogInfo("{}: {} {} an avatar ({} of the bag, avatar place {})", config.name, player.name, m.wear ? "puts on" : "takes off",
                      r.bag.front(), r.avatars.front());
        QueueItemSave(player.selected, player.inventory, r.bag, {}, r.avatars);
        ShowAvatars(player); // its look for those who see it (ours: the original's 0x3038-like look change not traced)
        // The Devil's Spirit on or off: its contributions in or out, the numbers sent again (for the moves 0x23 / 0x24:
        // taken out and its skills detached, or put in; the equipment's re-check ends its skills' buffs; the parameters
        // sent).
        // Any avatar on or off: its blues in or out too.
        if (!r.avatars.empty()) UpdateStats(&peer, player);
    }

    // A worn Devil's Spirit's awakening over: every `spiritCheck` (the original's 10 s timer), a worn spirit that is
    // asleep has its contributions and skills
    // taken off and its wearer is told (the original's 0x3078 {3, place}), each time it is looked at while it stays worn
    // asleep. It stays worn.
    void TickSpirits(Clock::time_point now) {
        if (!config.rules->items) return;
        const std::int64_t unix = ServiceNow();
        for (auto& [peerId, player] : players) {
            if (player.entity == 0 || now < player.spiritCheckAt) continue;
            player.spiritCheckAt = now + config.spiritCheck;
            const game::ItemInstance* item = player.inventory.Avatar(game::avatar::kSpirit);
            const assets::ItemRow* row = item != nullptr ? config.rules->items->Find(item->refId) : nullptr;
            if (row == nullptr || !game::IsDevilSpirit(*row) || game::DevilSpiritAwake(item->durability, unix)) continue;
            Peer* peer = peers->Find(peerId);
            if (player.spiritOn || player.combat.worn.spirit) {
                core::LogInfo("{}: {}'s Devil's Spirit falls asleep: its options and skills off", config.name, player.name);
                UpdateStats(peer, player);
            }
            if (peer != nullptr) SendMessage(*peer->connection, proto::SpiritAsleep{std::uint8_t(game::avatar::kSpirit)});
        }
    }

    // The avatars a player wears as its look (World's summary; an EntityLook to those who see it and itself).
    void ShowAvatars(PlayerWorld& player) {
        if (player.entity == 0) return;
        std::vector<proto::EquippedItem> avatars;
        for (const auto& [place, item] : player.inventory.avatars) avatars.push_back({item.refId, item.plus});
        world->SetAvatars(player.entity, avatars);
        if (player.combat.transform.mode != 0) return; // a transform's look stands until it ends
        const proto::CharacterSummary look = world->OwnLook(player.entity).value_or(proto::CharacterSummary{});
        Broadcast(player.entity, proto::EntityLook{player.entity, look.model, look.height, look.equipment, look.avatars});
    }

    // Too many lines: the original client's own rule for chat with the people around (a fourth
    // line within 4 s or a sixth within 10 s), and 10 lines of any
    // kind within 10 s; then 10 s without chat.
    static bool Flooding(PlayerWorld& player, proto::ChatKind kind, Clock::time_point now) {
        auto& times = player.chatTimes;
        times.emplace_back(now, kind);
        while (times.size() > 10) times.pop_front();
        const auto local = [](proto::ChatKind k) { return k == proto::ChatKind::All || k == proto::ChatKind::Gm; };
        const auto within = [&](std::chrono::seconds span, bool localOnly) {
            int n = 0;
            for (const auto& [at, k] : times) {
                if (now - at < span && (local(k) || !localOnly)) ++n;
            }
            return n;
        };
        return (local(kind) && (within(std::chrono::seconds(4), true) >= 4 || within(std::chrono::seconds(10), true) >= 6)) ||
               within(std::chrono::seconds(10), false) >= 10;
    }

    // A chat line: to the people who see the speaker, to one player by name,
    // or to everyone; parties, guilds and unions come with their features (until then the
    // refusals the original gives a player without one). The speaker gets the answer and prints
    // its own line from it.
    void Chat(Peer& peer, PlayerWorld& player, const proto::ChatSend& send) {
        const auto answer = [&](proto::ChatResult result, std::uint16_t seconds = 0) {
            SendMessage(*peer.connection, proto::ChatAnswer{result, send.kind, send.index, seconds});
        };
        if (!ChatTextOk(send.text)) return answer(proto::ChatResult::InvalidCommand);
        const Clock::time_point now = Clock::now();
        if (now < player.chatAllowedAt) {
            return answer(proto::ChatResult::Restricted,
                          std::uint16_t(std::chrono::ceil<std::chrono::seconds>(player.chatAllowedAt - now).count()));
        }
        if (Flooding(player, send.kind, now)) {
            player.chatAllowedAt = now + std::chrono::seconds(10);
            player.chatTimes.clear();
            return answer(proto::ChatResult::Restricted, 10);
        }
        // A game master's command (GmCommand), its outcome told back to it alone.
        if (!send.text.empty() && send.text.front() == '/') {
            if (const auto gm = accountOf.find(peer.id); gm != accountOf.end() && gm->second.gm) {
                proto::ChatLine said;
                said.kind = proto::ChatKind::Gm;
                said.name = "GM";
                said.text = GmCommand(send.text, player.name);
                core::LogInfo("{}: GM {}: {} -> {}", config.name, player.name, send.text, said.text);
                SendMessage(*peer.connection, said);
                return answer(proto::ChatResult::Ok);
            }
            // Not a game master's: never said to anyone (ours: the client sends the commands it does not know itself).
            if (send.kind == proto::ChatKind::All) return answer(proto::ChatResult::Ok);
        }
        proto::ChatLine line;
        line.kind = send.kind;
        line.name = player.name;
        line.text = send.text;
        switch (send.kind) {
        case proto::ChatKind::All: {
            line.sender = player.entity;
            const std::vector<std::byte> bytes = proto::Encode(line);
            world->ForEachObserver(player.entity, [&](std::uint64_t to) {
                if (to == peer.id) return;
                if (Peer* listener = peers->Find(to)) listener->connection->Send(proto::ChatLine::kId, bytes);
            });
            return answer(proto::ChatResult::Ok);
        }
        case proto::ChatKind::Whisper: {
            const auto it = byName.find(NameKey(send.target));
            Peer* target = it != byName.end() ? peers->Find(it->second) : nullptr;
            if (target != nullptr) {
                // The target's block list: its whisper function is off for this one.
                if (community && community->BlocksWhispersFrom(players[it->second].selected, player.name)) return answer(proto::ChatResult::WhispersOff);
                SendMessage(*target->connection, line);
                return answer(proto::ChatResult::Ok);
            }
            // Not here: perhaps on the shard's other game servers, which say so if they have it.
            if (chatChannel.empty() || !parties || !parties->OthersOnline()) return answer(proto::ChatResult::TargetNotFound);
            const std::uint32_t request = nextWhisper++;
            whispers[request] = {peer.id, send.kind, send.index, now + std::chrono::seconds(1)};
            PublishChat(kChatBusWhisper, 0, request, NameKey(send.target), &line);
            return; // answered when one says so, or when its time is up
        }
        case proto::ChatKind::Global: {
            // A Global chatting item pays for it (TID 3/3/3/5, whichever kind).
            const game::ItemInstance* item = send.slot >= game::slot::kWorn ? player.inventory.At(send.slot) : nullptr;
            const assets::ItemRow* row = item != nullptr && config.rules->items ? config.rules->items->Find(item->refId) : nullptr;
            if (row == nullptr || row->typeId != std::array<int, 4>{3, 3, 3, 5}) return answer(proto::ChatResult::InvalidCommand);
            TakeOne(peer, player, send.slot);
            line.sender = player.entity;
            TellEveryone(line);
            line.sender = 0; // an entity of this server: it means nothing on the others
            PublishChat(kChatBusGlobal, 0, 0, {}, &line);
            return answer(proto::ChatResult::Ok);
        }
        case proto::ChatKind::Party: {
            // The members of the speaker's party.
            if (!parties || !parties->Chat(player.entity, line)) return answer(proto::ChatResult::NotInParty);
            return answer(proto::ChatResult::Ok);
        }
        case proto::ChatKind::Guild:
        case proto::ChatKind::Union: return answer(guilds ? guilds->Chat(GuildActorOf(peer.id, player), line) : proto::ChatResult::InvalidCommand);
        case proto::ChatKind::Academy: return answer(academies ? academies->Chat(AcademyActorOf(peer.id, player), line) : proto::ChatResult::InvalidCommand);
        default: return answer(proto::ChatResult::InvalidCommand); // no game masters yet
        }
    }

    // The bargain goods' sale hours: on while a period of their schedule runs; everyone in the
    // world told 30 minutes before, at its start and at its end.
    bool bargainOn = false;
    std::int64_t bargainReadyFor = 0; // the period whose 30 minutes' notice was given
    Clock::time_point nextBargainCheck{};

    // The weather (game::ClimateWeather): each climate run on the wall clock (the same on every server of the shard),
    // each player told its region's when it differs from what it was told (entering, a region of another climate, a change).
    std::unordered_map<std::uint16_t, std::int32_t> regionClimate;
    std::map<std::int32_t, game::ClimateWeather> climates;
    Clock::time_point nextWeather{};
    std::int64_t WeatherClockMs() const {
        const double ms = double(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        return std::int64_t(ms * std::max(config.weatherSpeed, 0.001));
    }
    std::uint16_t WeatherAt(std::uint16_t region) const {
        const auto climate = regionClimate.find(region);
        if (climate == regionClimate.end()) return 1; // clear where no climate is named
        const auto w = climates.find(climate->second);
        return w != climates.end() ? w->second.Packed() : std::uint16_t(1);
    }
    void TickWeather(Clock::time_point now) {
        if (now < nextWeather) return;
        nextWeather = now + std::chrono::seconds(1);
        const std::int64_t ms = WeatherClockMs();
        for (auto& [id, w] : climates) w.Advance(ms);
        for (auto& [peerId, p] : players) {
            if (p.entity == 0 || !world) continue;
            const auto place = world->Where(p.entity);
            if (!place) continue;
            const std::uint16_t weather = WeatherAt(place->position.region);
            if (weather == p.weatherSent) continue;
            p.weatherSent = weather;
            if (Peer* peer = peers->Find(peerId)) {
                peer->connection->Send(proto::WeatherNow::kId, proto::Encode(proto::WeatherNow{std::uint8_t(weather & 0xFF), std::uint8_t(weather >> 8)}));
            }
        }
    }
    void TickBargain(Clock::time_point now) {
        if (!trades || now < nextBargainCheck) return;
        nextBargainCheck = now + std::chrono::seconds(1);
        const std::int64_t unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::optional<game::Period> period = game::SchedulePeriod(config.bargainSchedule, game::kScheduleSpecialGoods, unix);
        const bool on = period && period->Contains(unix);
        const auto tell = [&](std::uint8_t moment) {
            const std::vector<std::byte> bytes = proto::Encode(proto::TradeBargainNotice{moment});
            for (const auto& [peerId, p] : players) {
                if (p.entity == 0) continue;
                if (Peer* peer = peers->Find(peerId)) peer->connection->Send(proto::TradeBargainNotice::kId, bytes);
            }
        };
        if (on != bargainOn) {
            bargainOn = on;
            trades->SetBargainHours(on);
            tell(on ? 2 : 3);
            core::LogInfo("{}: the bargain goods' hours {}", config.name, on ? "begin" : "end");
        } else if (!on && period && period->start - unix <= game::kBargainNoticeSeconds && bargainReadyFor != period->start) {
            bargainReadyFor = period->start;
            tell(1);
        }
    }

    void TellEveryone(const proto::ChatLine& line) {
        const std::vector<std::byte> bytes = proto::Encode(line);
        for (const auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            if (Peer* listener = peers->Find(peerId)) listener->connection->Send(proto::ChatLine::kId, bytes);
        }
    }

    void PublishChat(std::uint8_t kind, std::uint16_t to, std::uint32_t request, std::string target, const proto::ChatLine* line) {
        if (chatChannel.empty()) return;
        ChatBusEnvelope e;
        e.from = config.id;
        e.to = to;
        e.kind = kind;
        e.request = request;
        e.target = std::move(target);
        if (line != nullptr) {
            const std::vector<std::byte> bytes = proto::Encode(*line);
            e.line.resize(bytes.size());
            std::memcpy(e.line.data(), bytes.data(), bytes.size());
        }
        BusSend(chatChannel, BusToHex(proto::Encode(e)));
    }

    // From the shard's other game servers: a whisper for someone here, its answer, a global line.
    void ChatBusMessage(const std::string& text) {
        const auto bytes = BusFromHex(text);
        ChatBusEnvelope e;
        if (!bytes || !proto::Decode(*bytes, e) || e.from == config.id || (e.to != 0 && e.to != config.id)) return;
        if (e.kind == kChatBusDelivered || e.kind == kChatBusRefused) {
            const auto it = whispers.find(e.request);
            if (it == whispers.end()) return;
            if (Peer* asker = peers->Find(it->second.peer)) SendMessage(*asker->connection, proto::ChatAnswer{e.kind == kChatBusRefused ? proto::ChatResult::WhispersOff : proto::ChatResult::Ok, it->second.kind, it->second.index, 0});
            whispers.erase(it);
            return;
        }
        proto::ChatLine line;
        if (!proto::Decode(std::as_bytes(std::span(e.line)), line)) return;
        if (e.kind == kChatBusGlobal) return TellEveryone(line);
        if (e.kind != kChatBusWhisper) return;
        const auto it = byName.find(e.target);
        Peer* target = it != byName.end() ? peers->Find(it->second) : nullptr;
        if (target == nullptr) return; // not here: another server may have it
        if (community && community->BlocksWhispersFrom(players[it->second].selected, line.name)) {
            PublishChat(kChatBusRefused, e.from, e.request, {}, nullptr);
            return;
        }
        SendMessage(*target->connection, line);
        PublishChat(kChatBusDelivered, e.from, e.request, {}, nullptr);
    }

    // --- Bug reports from the game (SupportProtocol.h) -----------------------------------------

    // A report on its way in: its screenshot's parts until all have come.
    struct BugReportIn {
        proto::BugReport report;
        std::vector<std::uint8_t> image;
    };
    std::unordered_map<std::uint64_t, BugReportIn> bugReports;                     // by peer
    std::unordered_map<std::int64_t, std::deque<Clock::time_point>> bugReportTimes; // by account: the last hour's
    static constexpr std::size_t kBugReportsPerHour = 5;

    void TellNotice(Peer& peer, std::string text) {
        proto::ChatLine line;
        line.kind = proto::ChatKind::Notice;
        line.text = std::move(text);
        SendMessage(*peer.connection, line);
    }

    // The report, then its screenshot's parts in order (the last one completes it). False: a message that does not decode
    // or breaks the order (the connection is closed).
    bool TakeBugReport(Peer& peer, PlayerWorld& player, const net::Message& m) {
#ifndef SR_BUG_REPORTS
        // A build without them (CMake SR_BUG_REPORTS=OFF): the player told once a report, nothing kept.
        (void)player;
        if (m.id == proto::BugReport::kId) TellNotice(peer, "Bug reports are not taken by this server.");
        return m.id == proto::BugReport::kId || m.id == proto::BugReportPart::kId;
#else
        if (m.id == proto::BugReport::kId) {
            proto::BugReport report;
            if (!proto::Decode(m.payload, report) || report.imageBytes > proto::kBugImageMax ||
                report.category >= std::size(proto::kBugCategories)) {
                return false;
            }
            // At most kBugReportsPerHour an account (counted as each comes in).
            auto& times = bugReportTimes[peer.account];
            const Clock::time_point now = Clock::now();
            while (!times.empty() && now - times.front() > std::chrono::hours(1)) times.pop_front();
            if (times.size() >= kBugReportsPerHour) {
                bugReports.erase(peer.id);
                TellNotice(peer, "You have sent several bug reports in the last hour: please wait a while before the next one.");
                return true;
            }
            times.push_back(now);
            BugReportIn& in = bugReports[peer.id];
            in.image.clear();
            in.report = std::move(report);
            in.image.reserve(in.report.imageBytes);
            if (in.report.imageBytes == 0) FinishBugReport(peer, player);
            return true;
        }
        if (m.id != proto::BugReportPart::kId) return false;
        proto::BugReportPart part;
        if (!proto::Decode(m.payload, part)) return false;
        const auto it = bugReports.find(peer.id);
        if (it == bugReports.end()) return true; // its report refused or gone: the rest of it is let go
        if (it->second.image.size() + part.bytes.size() > it->second.report.imageBytes) return false;
        it->second.image.insert(it->second.image.end(), part.bytes.begin(), part.bytes.end());
        if (it->second.image.size() == it->second.report.imageBytes) FinishBugReport(peer, player);
        return true;
#endif
    }

#ifdef SR_BUG_REPORTS
    // Kept as the website's support ticket (source "client", the account's), the screenshot in the database; the player
    // told its number or why not.
    void FinishBugReport(Peer& peer, PlayerWorld& player) {
        const auto it = bugReports.find(peer.id);
        if (it == bugReports.end()) return;
        BugReportIn in = std::move(it->second);
        bugReports.erase(it);
        NewTicket t;
        t.kind = "bug";
        t.category = std::string(proto::kBugCategories[in.report.category]);
        t.subject = std::move(in.report.subject);
        t.body = std::move(in.report.text);
        t.name = player.name;
        t.source = "client";
        t.jid = peer.account;
        t.address = peer.address;
        // What the server knows besides the client's own lines: the character, the server and where it stands.
        std::string info = std::format("character: {}\ngame server: {} ({})", player.name, config.id, config.name);
        if (const auto place = world->Where(player.entity)) {
            info += std::format("\nposition: region {} ({}, {}, {}){}", place->position.region, std::lround(place->position.x),
                                std::lround(place->position.y), std::lround(place->position.z), place->position.layer != 0 ? " in an instance" : "");
        }
        t.clientInfo = info + "\n" + in.report.info;
        if (t.clientInfo.size() > 2000) t.clientInfo.resize(2000);
        if (const auto refusal = CheckTicket(t)) {
            TellNotice(peer, "The bug report was not sent: " + *refusal);
            return;
        }
        std::string mime;
        if (!in.image.empty()) {
            mime = SupportFileType(std::string_view(reinterpret_cast<const char*>(in.image.data()), std::min<std::size_t>(in.image.size(), 16)));
            if (!mime.starts_with("image/")) in.image.clear(); // not a picture: the report goes without it
        }
        database.Submit([this, peerId = peer.id, name = player.name, t = std::move(t), image = std::move(in.image), mime](Database& db) {
            const auto made = CreateTicket(db, t);
            const std::int64_t ticket = made ? made->first : 0;
            if (made && !image.empty()) AddTicketFileData(db, ticket, "screenshot" + SupportFileExtension(mime), mime, image);
            completions->Post([this, peerId, name, ticket] {
                Peer* p = peers->Find(peerId);
                if (ticket == 0) {
                    core::LogWarning("{}: {}'s bug report could not be saved", config.name, name);
                    if (p != nullptr) TellNotice(*p, "The bug report could not be saved; please try again later.");
                    return;
                }
                core::LogInfo("{}: {} sent bug report #{}", config.name, name, ticket);
                if (p != nullptr) {
                    TellNotice(*p, std::format("Thank you! Your bug report was sent (ticket #{}). The answer shows on your account page on the website.", ticket));
                }
            });
        });
    }
#endif

    // --- The website's commands (AdminStore.h) and the GM commands -------------------------

    // The server's notice to every player of its world: the chat's notice line and the notice banner.
    std::size_t Announce(const std::string& text) {
        proto::ChatLine line;
        line.kind = proto::ChatKind::Notice;
        line.text = text;
        TellEveryone(line);
        return std::size_t(std::count_if(players.begin(), players.end(), [](const auto& kv) { return kv.second.entity != 0; }));
    }
    // Out of the game: told why first, then the connection closed (its character saved as at any logout).
    // The character `character` entering from `peerId`: any other connection of this server with it in the world (or
    // on its way in or out) loses it now, before the new one enters, and is closed.
    void ReplaceOtherConnections(std::uint64_t peerId, std::int64_t character, const std::string& name) {
        std::vector<std::uint64_t> others;
        for (const auto& [otherId, other] : players) {
            if (otherId != peerId && other.selected == character && (other.entity != 0 || other.entering || other.moving)) others.push_back(otherId);
        }
        for (const std::uint64_t otherId : others) {
            core::LogWarning("{}: {} enters again from another connection: the old one's character leaves, its connection closed", config.name, name);
            LeaveWorld(otherId, false);
            players[otherId].selected = 0;
            if (players[otherId].dropped) { // its connection already gone: nothing more to close
                if (players[otherId].heldAccount != 0) ReleaseSessionAfter(character, players[otherId].heldAccount, players[otherId].heldSession);
                players.erase(otherId);
                accountOf.erase(otherId);
                continue;
            }
            Disconnect(otherId, proto::DisconnectReason::Replaced, {});
        }
    }
    void Disconnect(std::uint64_t peerId, proto::DisconnectReason reason, std::string text) {
        Peer* peer = peers->Find(peerId);
        if (peer == nullptr) return;
        SendMessage(*peer->connection, proto::Disconnected{reason, std::move(text)});
        peer->connection->CloseAfterSend();
    }
    // The website's kick: the account (blocked by the site, the login refuses it) out of this server, in the world or
    // in the lobby.
    std::string KickAccount(const std::string& account, const std::optional<std::pair<std::int64_t, std::string>>& block) {
        const auto lower = [](std::string s) {
            for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string wanted = lower(account);
        std::vector<std::uint64_t> out;
        for (const auto& [peerId, a] : accountOf)
            if (lower(a.name) == wanted) out.push_back(peerId);
        std::string kicked;
        for (const std::uint64_t peerId : out) {
            const auto p = players.find(peerId);
            const std::string who = p != players.end() && p->second.entity != 0 ? p->second.name : std::string("(in the lobby)");
            // The block's reason and end as the site wrote it (a ban for a time: _BlockedUser's end), told on leaving.
            if (Peer* peer = peers->Find(peerId)) {
                proto::Disconnected bye{proto::DisconnectReason::Blocked, block ? block->second : std::string(), block ? block->first : 0};
                SendMessage(*peer->connection, bye);
                peer->connection->CloseAfterSend();
            }
            kicked += (kicked.empty() ? "" : ", ") + who;
        }
        return kicked.empty() ? "not here" : "kicked " + kicked;
    }
    // A GM command line (ours; typed in the chat by a game master, or the website's console): its outcome.
    std::string GmCommand(std::string_view line, const std::string& by) {
        while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
        while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
        if (line.empty() || line.front() != '/') return "not a command (commands start with /)";
        const std::size_t space = line.find(' ');
        std::string command(line.substr(0, space));
        for (char& c : command) c = char(std::tolower(static_cast<unsigned char>(c)));
        std::string rest(space == std::string_view::npos ? std::string_view() : line.substr(space + 1));
        while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        const auto present = [&](const std::string& name) -> std::uint64_t {
            const auto it = byName.find(NameKey(name));
            return it != byName.end() ? it->second : 0;
        };
        if (command == "/help" || command == "/?") {
            return "commands: /notice TEXT, /kick NAME, /online, /where NAME, /give CODENAME [COUNT] [NAME], /quest done CODENAME [NAME], /academy rank, "
                   "/siege STARTREQ|ENDREQ|STARTWAR|ENDWAR|STARTTAX|ENDTAX, /flagworld OPEN|START|END|CANCEL, /arena OPEN [MODE]|START|END|CANCEL, "
                   "/roc OPEN|SHUT|END|BREAK, /qin OPEN|SHUT|END, /temple OPEN|CLOSE 9|10|11, /temple REP ANUBIS|HAROERIS TRADER|THIEF N, "
                   "/gametime HOUR";
        }
        if (command == "/temple") {
            // The job temples' developer switches (ours): OPEN|CLOSE 9|10|11, REP ANUBIS|HAROERIS TRADER|THIEF N.
            if (!temples) return "no temples here";
            std::string what = rest;
            for (char& c : what) c = char(std::toupper(static_cast<unsigned char>(c)));
            char verb[16]{}, a[16]{}, b[16]{};
            int n = 0;
            if (std::sscanf(what.c_str(), "%15s %d", verb, &n) == 2 && std::string(verb) == "OPEN") return temples->Open(n) ? "temples open" : "no such schedule";
            if (std::sscanf(what.c_str(), "%15s %d", verb, &n) == 2 && std::string(verb) == "CLOSE") {
                temples->Close(n);
                return "temples closed";
            }
            if (std::sscanf(what.c_str(), "%15s %15s %15s %d", verb, a, b, &n) == 4 && std::string(verb) == "REP") {
                temples->AddReputation(std::string(a) == "ANUBIS" ? 0 : 1, std::string(b) == "THIEF" ? 2 : 1, n);
                return "reputation added";
            }
            return "/temple OPEN|CLOSE 9|10|11, /temple REP ANUBIS|HAROERIS TRADER|THIEF N";
        }
        if (command == "/roc") {
            // The Roc's developer switches: the server holding the Peak acts (from another, over the bus).
            std::string what = rest;
            for (char& c : what) c = char(std::toupper(static_cast<unsigned char>(c)));
            if (what != "OPEN" && what != "SHUT" && what != "END" && what != "BREAK") return "/roc OPEN|SHUT|END|BREAK";
            core::LogInfo("{}: {} switches the Roc: {}", config.name, by, what);
            if (roc) {
                RocDev(what);
            } else {
                database.Submit([channel = rocChannel, what](Database& db) { Notify(db, channel, "C	" + what); });
            }
            return "roc " + what;
        }
        if (command == "/qin") {
            // The Qin-Shi tomb's developer switches (ours): the server holding the 4th floor acts (from another, over the bus).
            std::string what = rest;
            for (char& c : what) c = char(std::toupper(static_cast<unsigned char>(c)));
            if (what != "OPEN" && what != "SHUT" && what != "END") return "/qin OPEN|SHUT|END";
            core::LogInfo("{}: {} switches the Qin-Shi tomb's gate: {}", config.name, by, what);
            if (qinTomb) {
                QinDev(what);
            } else {
                database.Submit([channel = qinChannel, what](Database& db) { Notify(db, channel, "C	" + what); });
            }
            return "qin " + what;
        }
        if (command == "/arena") {
            // The Battle Arena's developer switches (ours; ArenaDev): every game server of the shard follows. MODE 0 random,
            // 1 party, 2 guild, 3 job.
            if (!arenas) return "no arena here";
            std::string what = rest.substr(0, rest.find(' '));
            for (char& c : what) c = char(std::toupper(static_cast<unsigned char>(c)));
            const std::uint8_t action = what == "OPEN" ? 1 : what == "START" ? 2 : what == "END" ? 3 : what == "CANCEL" ? 4 : 0;
            if (action == 0) return "/arena OPEN [MODE]|START|END|CANCEL";
            const std::size_t space = rest.find(' ');
            const std::uint8_t mode = space == std::string::npos ? 0 : std::uint8_t(std::clamp(std::atoi(rest.c_str() + space + 1), 0, 3));
            arenas->Dev(action, mode);
            core::LogInfo("{}: {} switches the Battle Arena: {} {}", config.name, by, what, mode);
            return "arena " + what;
        }
        if (command == "/flagworld") {
            // Capture the Flag's developer switches (ours; FlagWorldDev): every game server of the shard follows.
            if (!flagWorlds) return "no flag world here";
            std::string what = rest;
            for (char& c : what) c = char(std::toupper(static_cast<unsigned char>(c)));
            const std::uint8_t action = what == "OPEN" ? 1 : what == "START" ? 2 : what == "END" ? 3 : what == "CANCEL" ? 4 : 0;
            if (action == 0) return "/flagworld OPEN|START|END|CANCEL";
            flagWorlds->Dev(action);
            core::LogInfo("{}: {} switches Capture the Flag: {}", config.name, by, what);
            return "flagworld " + what;
        }
        if (command == "/siege") {
            // The GM's /SIEGE switches of the fortress war's periods (FortressDevAction).
            if (!fortresses) return "no fortress war here";
            std::string what = rest;
            for (char& c : what) c = char(std::toupper(static_cast<unsigned char>(c)));
            using D = proto::FortressDevAction;
            const std::optional<D> action = what == "STARTREQ"   ? std::optional<D>(D::StartRequests)
                                            : what == "ENDREQ"   ? std::optional<D>(D::EndRequests)
                                            : what == "STARTWAR" ? std::optional<D>(D::StartWar)
                                            : what == "ENDWAR"   ? std::optional<D>(D::EndWar)
                                            : what == "STARTTAX" ? std::optional<D>(D::StartTax)
                                            : what == "ENDTAX"   ? std::optional<D>(D::EndTax)
                                                                 : std::nullopt;
            if (!action) return "/siege STARTREQ|ENDREQ|STARTWAR|ENDWAR|STARTTAX|ENDTAX";
            fortresses->Dev(*action);
            core::LogInfo("{}: {} switches the fortress war's periods: {}", config.name, by, what);
            return "siege " + what;
        }
        if (command == "/gametime") {
            // The world's game hour (the original's GM time command), on every game server of the shard (the bus brings it back
            // here too).
            int hour = -1;
            if (std::sscanf(rest.c_str(), "%d", &hour) != 1 || hour < 0 || hour > 23) return "/gametime HOUR (0..23)";
            database.Submit([channel = clockChannel, hour](Database& db) { Notify(db, channel, std::to_string(hour)); });
            return std::format("game time {}:00", hour);
        }
        if (command == "/notice") {
            if (rest.empty()) return "/notice needs a text";
            if (rest.size() > 500) return "the notice is too long (500 at most)";
            return std::format("notice told to {} players", Announce(rest));
        }
        if (command == "/kick") {
            const std::uint64_t peerId = present(rest);
            if (peerId == 0) return rest + ": not here";
            const std::string name = players[peerId].name;
            Disconnect(peerId, proto::DisconnectReason::Kicked, by);
            return "kicked " + name;
        }
        if (command == "/give") {
            // Ours: an item into a player's bag (the GM's own by default), as the developer's DevGive: the item's codename
            // (itemdata), the count (1), the player on this server.
            std::istringstream in(rest);
            std::string code, second, third;
            in >> code >> second >> third;
            if (code.empty()) return "/give CODENAME [COUNT] [NAME]";
            for (char& c : code) c = char(std::toupper(static_cast<unsigned char>(c)));
            std::uint32_t count = 1;
            std::string name = by;
            if (!second.empty() && std::all_of(second.begin(), second.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                count = std::uint32_t(std::clamp(std::atoi(second.c_str()), 1, 10000));
                if (!third.empty()) name = third;
            } else if (!second.empty()) {
                name = second;
            }
            const assets::ItemRow* row = config.rules->items ? config.rules->items->Find(std::string_view(code)) : nullptr;
            if (row == nullptr) return code + ": no such item";
            const std::uint64_t peerId = present(name);
            Peer* peer = peerId != 0 ? peers->Find(peerId) : nullptr;
            if (peer == nullptr || players[peerId].entity == 0) return name + ": not on this server";
            proto::DevGive give;
            give.item = code;
            give.count = count;
            const auto held = [&] {
                int n = 0;
                for (const auto& [at, item] : players[peerId].inventory.places)
                    if (at >= game::slot::kWorn && item.refId == row->refObjId) n += std::max<int>(1, item.count);
                return n;
            };
            const int before = held();
            DevGive(*peer, players[peerId], give);
            const int after = held();
            return after > before ? std::format("{} x{} to {}", code, after - before, players[peerId].name) : std::format("{}: not given (a full bag?)", code);
        }
        if (command == "/academy") {
            // Ours: "/academy rank": the Academy's honour ranking made again now.
            if (rest != "rank" || !academies) return "/academy rank";
            academies->RankNow();
            return "the honour ranking is being made again";
        }
        if (command == "/quest") {
            // Ours: "/quest done CODENAME [NAME]": the quest and every one it needs first marked finished (Quests::DevFinish).
            std::istringstream in(rest);
            std::string what, code, name;
            in >> what >> code >> name;
            for (char& c : what) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (what != "done" || code.empty()) return "/quest done CODENAME [NAME]";
            for (char& c : code) c = char(std::toupper(static_cast<unsigned char>(c)));
            const std::uint64_t peerId = present(name.empty() ? by : name);
            if (peerId == 0 || players[peerId].entity == 0) return (name.empty() ? by : name) + ": not on this server";
            if (!quests) return "no quests here";
            const int marked = quests->DevFinish(players[peerId].entity, code);
            return marked == 0 ? code + ": no such quest (or not played here)" : std::format("{}: {} marked finished for {}", code, marked, players[peerId].name);
        }
        if (command == "/where") {
            // /where [NAME]: where that player (none: the GM) stands and faces, the heading in degrees as `beta-helpers`
            // takes it (REGION,X,Y,Z,HEADING: a place to paste).
            const std::string who = rest.empty() ? by : rest;
            const std::uint64_t peerId = present(who);
            const auto at = peerId != 0 ? world->Where(players[peerId].entity) : std::nullopt;
            if (!at) return who + ": not here";
            // An NPC placed by it faces as the player did: its heading 90 more than the player's yaw's (the user's checks in
            // Jangan, 2026-11-19: the player's own 224 stood the Beta Helper 90 to her right; 314 is right).
            double heading = (std::numbers::pi / 2.0 - double(at->yaw)) * 180.0 / std::numbers::pi + 90.0;
            heading = std::fmod(std::fmod(heading, 360.0) + 360.0, 360.0);
            return std::format("{}: {},{:.1f},{:.1f},{:.1f},{:.0f}", players[peerId].name, at->position.region, at->position.x, at->position.y,
                               at->position.z, heading);
        }
        if (command == "/online") {
            std::vector<std::string> names;
            for (const auto& [peerId, p] : players)
                if (p.entity != 0) names.push_back(p.name);
            std::sort(names.begin(), names.end());
            std::string list = std::format("{} online", names.size());
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (list.size() + names[i].size() > 180) {
                    list += std::format(", and {} more", names.size() - i);
                    break;
                }
                list += (i == 0 ? ": " : ", ") + names[i];
            }
            return list;
        }
        return "unknown command " + command + " (/help lists them)";
    }
    // A notice on the website's channel: the row's id. Read for this shard, done if it is for this server, answered.
    void AdminNotice(const std::string& payload) {
        char* end = nullptr;
        const long long id = std::strtoll(payload.c_str(), &end, 10);
        if (end == payload.c_str() || *end != '\0' || id <= 0) return;
        database.Submit([this, id, shard = config.shardSchema](Database& db) {
            bool ok = false;
            std::optional<AdminCommand> c = LoadAdminCommand(db, id, shard, ok);
            if (!c) {
                if (!ok) core::LogWarning("{}: the website's command {} could not be read", config.name, id);
                return;
            }
            auto block = BlockOf(db, *c);
            completions->Post([this, c = std::move(*c), block = std::move(block)] { RunAdmin(c, block); });
        });
    }
    // A kick's account block (the website wrote it: no end, or a ban for a time), to tell the player.
    static std::optional<std::pair<std::int64_t, std::string>> BlockOf(Database& db, const AdminCommand& c) {
        if (c.kind != "kick" || !ValidAccountName(c.text)) return std::nullopt;
        bool ok = false;
        const std::optional<AccountRow> account = FindAccount(db, c.text, ok);
        return account ? AccountBlock(db, account->id) : std::nullopt;
    }
    void RunAdmin(const AdminCommand& c, const std::optional<std::pair<std::int64_t, std::string>>& block = std::nullopt) {
        if (c.serverId != 0 && c.serverId != config.id) return;
        const auto characters = [](const std::string& s) {
            return std::size_t(std::count_if(s.begin(), s.end(), [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) != 0x80; }));
        };
        std::string outcome;
        if (c.kind == "announce") {
            outcome = c.text.empty() || characters(c.text) > 500 ? std::string("refused: the text must have 1 to 500 characters")
                                                                 : std::format("told to {} players", Announce(c.text));
        } else if (c.kind == "kick") {
            outcome = ValidAccountName(c.text) ? KickAccount(c.text, block) : std::string("refused: not an account name");
        } else if (c.kind == "console") {
            outcome = c.text.empty() || characters(c.text) > 200 ? std::string("refused: the line must have 1 to 200 characters")
                                                                 : GmCommand(c.text, "the website (" + c.createdBy + ")");
        } else {
            outcome = "refused: no such kind";
        }
        core::LogInfo("{}: the website's {} #{} by {}: {}", config.name, c.kind.substr(0, 16), c.id, c.createdBy, outcome);
        database.Submit([id = c.id, server = config.id, outcome](Database& db) { FinishAdminCommand(db, id, server, outcome); });
    }
    // The uniques' news from the shard's game servers (kind, model, name, killer; Monsters' hives): the original's
    // notice to this server's players (CONTROL_NOTIFY_SPAWN_UNIQUE_MONSTER_MSG; 0x300C 0x0C05 came, 0x0C06 gone with the
    // killer's name, worded by the client: proto::UniqueNotice).
    void UniqueBusMessage(const std::string& payload) {
        std::vector<std::string> f;
        for (std::size_t start = 0; start <= payload.size();) {
            const std::size_t tab = std::min(payload.find('\t', start), payload.size());
            f.push_back(payload.substr(start, tab - start));
            start = tab + 1;
        }
        if (f.size() < 3 || f[2].empty()) return;
        proto::UniqueNotice n;
        n.model = std::uint32_t(std::strtoul(f[1].c_str(), nullptr, 10));
        if (f[0] == "1") {
            n.kind = 1;
        } else if (f[0] == "2" || f[0] == "3") {
            n.kind = 2;
            if (f[0] == "2" && f.size() > 3) n.killer = f[3];
        } else {
            return;
        }
        if (n.model == 0) return;
        const std::vector<std::byte> bytes = proto::Encode(n);
        for (const auto& [peerId, p] : players) {
            if (p.entity == 0) continue;
            if (Peer* peer = peers->Find(peerId)) peer->connection->Send(proto::UniqueNotice::kId, bytes);
        }
    }

    // Whispers nobody on the other servers answered for: there is no such player online.
    void FinishWhispers(Clock::time_point now) {
        for (auto it = whispers.begin(); it != whispers.end();) {
            if (now < it->second.until) {
                ++it;
                continue;
            }
            if (Peer* asker = peers->Find(it->second.peer)) SendMessage(*asker->connection, proto::ChatAnswer{proto::ChatResult::TargetNotFound, it->second.kind, it->second.index, 0});
            it = whispers.erase(it);
        }
    }

    // In combat (20 s after the last hit given or taken) and one the original keeps from leaving then: a job suit worn,
    // in a free PvP team, an assaulter or a murderer (PvP states 1 and 2), in the fortress world during a war, or their
    // guild at war.
    bool HeldInCombat(const PlayerWorld& p) const {
        if (Clock::now() >= p.combat.battleUntil) return false;
        if (JobState(p) != kNoJob || p.freePvp != 0 || p.pvpState != 0) return true;
        if (fortresses && fortresses->WarOn() && InFortressWorld(p.entity)) return true;
        return guilds && guilds->AtAnyWar(p.selected);
    }
    // One of its COS carries trade goods: its transport's bag holds any.
    bool CarriesGoods(const PlayerWorld& p) const { return coses && p.entity != 0 && coses->CarriesGoods(p.entity); }

    // --- Safe trade -----------------------------------------------------------------
    static std::int32_t TodayLocal() {
        const std::time_t t = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
    }
    static bool LeagueTrader(const PlayerWorld& p) { return p.job.type == game::JobType::Trader && p.safeTrade.has_value(); }
    // Told (0x34D5): 0x50 begun, 0x41 the limit, 0x51 at log-in, else 0x40 (reset); `active` as the code sets it.
    void TellSafeTrade(PlayerWorld& p, std::uint8_t code) {
        if (!LeagueTrader(p)) return;
        const std::uint8_t limit = std::uint8_t(jobRules.safetyCount & 0xFF);
        const std::uint8_t count = std::uint8_t(std::clamp(p.safeTrade->count, 0, 255));
        proto::SafeTradeState m;
        if (code == 0x50) {
            p.safeTrade->active = true;
            m = {1, 0x50, count, limit};
        } else if (code == 0x51) {
            m = p.safeTrade->active ? proto::SafeTradeState{1, 0x51, count, limit} : proto::SafeTradeState{0, 0x40, 0, 0};
        } else {
            p.safeTrade->active = false;
            m = {0, std::uint8_t(code == 0x41 ? 0x41 : 0x40), 0, 0};
        }
        if (Peer* peer = peers->Find(world->PeerOf(p.entity))) SendMessage(*peer->connection, m);
    }
    void SaveSafeTradeOf(const PlayerWorld& p) {
        if (!p.safeTrade) return;
        const std::uint64_t number = ++p.safeTradeSaves->issued; // the game thread's count
        database.Submit([character = p.selected, row = *p.safeTrade, order = p.safeTradeSaves, number](Database& db) {
            const std::lock_guard lock(order->mutex);
            if (number < order->written) return; // a newer one is written already
            order->written = number;
            SaveSafeTrade(db, character, row);
        });
    }
    // A reset (0x40): a sale at a specialty NPC, the stars off one, the suit off, bargain goods picked up.
    void ResetSafeTrade(PlayerWorld& p) {
        if (!LeagueTrader(p)) return;
        TellSafeTrade(p, 0x40);
        SaveSafeTradeOf(p);
    }
    // The select's tail at a specialty NPC, the day's reset first (option 0: a new calendar day by the
    // server's clock sets the count to 0).
    proto::SafeTradeTalk SafeTradeAt(PlayerWorld& p, const std::string& npcCodename) {
        proto::SafeTradeTalk t;
        if (!LeagueTrader(p) || !game::IsSpecialtyNpc(npcCodename)) return t;
        t.option = std::uint8_t(jobRules.safetyOption);
        if (jobRules.safetyOption != 0) return t;
        if (const std::int32_t today = TodayLocal(); p.safeTrade->day != today) {
            p.safeTrade->count = 0;
            p.safeTrade->day = today;
            SaveSafeTradeOf(p);
        }
        t.limit = std::uint8_t(jobRules.safetyCount & 0xFF);
        t.count = std::uint8_t(std::clamp(p.safeTrade->count, 0, 255));
        return t;
    }
    // The specialty shop put away: admitted only with no bargain goods and
    // exactly one star; option 1 never, 2 always, 0 the limit tested first (0x41), then counted once while not active.
    void CloseShop(PlayerWorld& p, const proto::ShopClose& close) {
        const auto it = npcs.find(close.entity);
        if (it == npcs.end() || !game::IsSpecialtyNpc(it->second->codename)) return;
        bool bargain = false;
        if (const auto cart = coses ? coses->TransportCart(p.entity) : std::nullopt; cart && cart->bag != nullptr && config.rules->items) {
            for (const auto& [place, item] : *cart->bag) {
                if (const assets::ItemRow* row = config.rules->items->Find(item.refId); row != nullptr && game::IsBargainGood(*row)) bargain = true;
            }
        }
        if (trades) trades->ShopClosed(p.entity, bargain, Clock::now()); // the bargain goods' lock from now
        if (!LeagueTrader(p)) return;
        std::uint8_t code = 0x40;
        if (!bargain && trades && trades->StarsOf(p.entity) == 1) {
            if (jobRules.safetyOption == 2) {
                code = 0x50;
            } else if (jobRules.safetyOption == 0) {
                if (p.safeTrade->count >= jobRules.safetyCount) {
                    code = 0x41;
                } else {
                    if (!p.safeTrade->active) p.safeTrade->count = std::min(p.safeTrade->count + 1, jobRules.safetyCount);
                    code = 0x50;
                }
            }
        }
        TellSafeTrade(p, code);
        SaveSafeTradeOf(p);
        core::LogInfo("{}: {} leaves a specialty NPC: safe trade {} ({}/{})", config.name, p.name, code == 0x50 ? "on" : code == 0x41 ? "at its limit" : "off",
                      p.safeTrade->count, jobRules.safetyCount);
    }
    // Protected: in a trader's suit, goods in his transport, safe trade on, at most one star.
    bool SafeTrader(const PlayerWorld& p) const {
        return p.jobState == std::uint8_t(game::JobType::Trader) && LeagueTrader(p) && p.safeTrade->active && CarriesGoods(p) && trades &&
               trades->StarsOf(p.entity) <= 1;
    }
    // The system menu's Restart / Exit (0x7005; both alike): refused in combat for those held (0x801) and
    // while teleporting (0x802); else the count, 5 s or 60 s with goods. Nothing on the server stops it: only the
    // player's cancel (LogoutCancel; a gate's use and a developer's teleport ours too).
    void Logout(Peer& peer, PlayerWorld& player, proto::LogoutKind kind) {
        if (kind != proto::LogoutKind::Exit && kind != proto::LogoutKind::Restart) return;
        if (HeldInCombat(player)) {
            SendMessage(*peer.connection, proto::LogoutAnswer{proto::LogoutStatus::InCombat, kind, 0});
            return;
        }
        if (player.moving || player.entering) {
            SendMessage(*peer.connection, proto::LogoutAnswer{proto::LogoutStatus::Teleporting, kind, 0});
            return;
        }
        const std::chrono::seconds seconds = CarriesGoods(player) ? std::chrono::seconds(60) : config.logoutCountdown;
        player.loggingOut = true;
        player.logoutKind = kind;
        player.logoutAt = Clock::now() + seconds;
        if (std::find(loggingOut.begin(), loggingOut.end(), peer.id) == loggingOut.end()) loggingOut.push_back(peer.id);
        SendMessage(*peer.connection, proto::LogoutAnswer{proto::LogoutStatus::Counting, kind, std::uint8_t(seconds.count())});
    }
    // A dropped connection: the character stays in the world for the count: 20 s in combat in a suit, 60 s
    // with goods, else the logout's own. False: it leaves at once (a count of 0, not in the world, on its way elsewhere).
    bool Linger(std::uint64_t peerId) {
        const auto it = players.find(peerId);
        if (it == players.end()) return false;
        PlayerWorld& player = it->second;
        if (player.entity == 0 || player.moving || player.entering) return false;
        std::chrono::seconds seconds = config.logoutCountdown;
        if (Clock::now() < player.combat.battleUntil && JobState(player) != kNoJob) seconds = std::chrono::seconds(20);
        if (CarriesGoods(player)) seconds = std::chrono::seconds(60);
        if (seconds.count() <= 0) return false;
        player.loggingOut = true;
        player.dropped = true;
        player.logoutAt = Clock::now() + seconds;
        if (std::find(loggingOut.begin(), loggingOut.end(), peerId) == loggingOut.end()) loggingOut.push_back(peerId);
        core::LogInfo("{}: {}'s connection dropped: in the world {} s more", config.name, player.name, seconds.count());
        return true;
    }

    void CancelLogout(Peer& peer, PlayerWorld& player) {
        if (!player.loggingOut) return;
        player.loggingOut = false;
        SendMessage(*peer.connection, proto::LogoutAnswer{proto::LogoutStatus::Cancelled, player.logoutKind, 0});
    }

    // Countdowns at their end: the character leaves the world (the client goes to the select or quits).
    void FinishLogouts() {
        const Clock::time_point now = Clock::now();
        std::vector<std::uint64_t> gone; // dropped connections whose characters left
        std::erase_if(loggingOut, [&](std::uint64_t peerId) {
            const auto it = players.find(peerId);
            if (it == players.end()) return true;
            if (it->second.dropped && (it->second.entity == 0 || now >= it->second.logoutAt)) {
                gone.push_back(peerId);
                return true;
            }
            if (!it->second.loggingOut || it->second.entity == 0) return true;
            if (now < it->second.logoutAt) return false;
            it->second.loggingOut = false;
            LeaveWorld(peerId, true);
            return true;
        });
        for (const std::uint64_t peerId : gone) {
            LeaveWorld(peerId, false);
            const PlayerWorld& player = players[peerId];
            if (player.heldAccount != 0) ReleaseSessionAfter(player.selected, player.heldAccount, player.heldSession);
            players.erase(peerId);
            accountOf.erase(peerId);
        }
    }

    // The requests a GM's punishment stops (the original's): 0 the chat's (chat, letters, the guild's and the academy's
    // notices), 1 the trade's (an exchange's request, a stall's opening and its visit, the Open Market, a drop).
    static std::optional<std::uint8_t> RestrictionOf(proto::MessageId id) {
        switch (id) {
        case proto::ChatSend::kId:
        case proto::LetterWrite::kId:
        case proto::LetterWriteMany::kId:
        case proto::GuildNoticeEdit::kId:
        case proto::AcademyNoticeWrite::kId: return 0;
        case proto::ExchangeAsk::kId:
        case proto::StallOpen::kId:
        case proto::StallVisit::kId:
        case proto::OpenMarketOpen::kId:
        case proto::DropItem::kId: return 1;
        default: return std::nullopt;
        }
    }
    // Whether the account's punishment of that kind runs now (its end passed: lifted).
    static bool Restricted(const PlayerWorld& player, std::uint8_t kind) {
        const std::optional<std::int64_t>& until = kind == 0 ? player.restrictions.chatUntil : player.restrictions.tradeUntil;
        if (!until) return false;
        if (*until == 0) return true;
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() < *until;
    }

    void Handle(Peer& peer, const net::Message& m) {
        if (peer.account == 0) {
            if (m.id != proto::LobbyEnter::kId) {
                core::LogInfo("{}: {} sent 0x{:04x} before its ticket; closing", config.name, peer.address, m.id);
                peer.connection->Close();
                return;
            }
            proto::LobbyEnter enter;
            if (!proto::Decode(m.payload, enter) || peer.busy) {
                peer.connection->Close();
                return;
            }
            return Enter(peer, std::move(enter.ticket));
        }
        const auto decode = [&](auto& message) {
            if (proto::Decode(m.payload, message)) return true;
            peer.connection->Close();
            return false;
        };
        PlayerWorld& player = players[peer.id];
        if (player.moving) return; // on its way to another server: what it still sends here is too late
        if (player.entity != 0 || player.entering) {
            // A GM's punishment of the account (its trade, its chat): the requests it stops are dropped after the notice
            // (the original's 0x3405); the Open Market's opening answered its code.
            if (player.entity != 0) {
                if (const std::optional<std::uint8_t> kind = RestrictionOf(m.id); kind && Restricted(player, *kind)) {
                    const std::optional<std::int64_t>& until = *kind == 0 ? player.restrictions.chatUntil : player.restrictions.tradeUntil;
                    SendMessage(*peer.connection, proto::AccountRestricted{*kind, until.value_or(0)});
                    if (m.id == proto::OpenMarketOpen::kId) SendMessage(*peer.connection, proto::OpenMarketAnswer{m.id, game::openmarket::code::kRestricted});
                    return;
                }
            }
            // Guilds and unions (Guilds.h): a request that does not decode closes the connection.
            if (proto::IsGuildMessage(m.id)) {
                if (player.entity != 0 && guilds && !guilds->Handle(GuildActorOf(peer.id, player), m.id, m.payload)) peer.connection->Close();
                return;
            }
            // Friends, the block list and letters (Community.h): the same.
            if (proto::IsAcademyMessage(m.id)) {
                if (player.entity != 0 && academies && !academies->Handle(AcademyActorOf(peer.id, player), m.id, m.payload, Clock::now())) peer.connection->Close();
                return;
            }
            // A bug report from the game (SupportProtocol.h; the build's SR_BUG_REPORTS): one that does not decode closes it.
            if (proto::IsSupportMessage(m.id)) {
                if (player.entity != 0 && !TakeBugReport(peer, player, m)) peer.connection->Close();
                return;
            }
            // The Open Market (OpenMarket.h): the same.
            if (proto::IsOpenMarketRequest(m.id)) {
                if (player.entity != 0 && openMarket && !openMarket->Handle(peer.id, m.id, m.payload)) peer.connection->Close();
                return;
            }
            if (proto::IsCommunityMessage(m.id)) {
                if (player.entity != 0 && community && !community->Handle(CommunityActorOf(peer.id, player), m.id, m.payload, Clock::now())) peer.connection->Close();
                return;
            }
            // The fortress war (Fortress.h): the same.
            if (proto::IsFortressMessage(m.id)) {
                if (player.entity != 0 && fortresses && !fortresses->Handle(FortressActorOf(peer.id, player), m.id, m.payload, config.devCommands)) {
                    peer.connection->Close();
                }
                return;
            }
            // Alchemy (Alchemy.h): the same.
            if (proto::IsAlchemyMessage(m.id)) {
                if (player.entity != 0 && alchemy && !alchemy->Handle(player.entity, m.id, m.payload, Clock::now())) peer.connection->Close();
                return;
            }
            // A summoned pet's item stays in the bag while its pet is out (Coses::KeepsItem; the original's texts): not
            // dropped, sold, stored, offered in an exchange or a stall (ours: a drop and a stall hear the trade's text).
            if (coses && player.entity != 0) {
                std::optional<std::uint8_t> place;
                const char* text = "UIIT_MSG_COSPETERR_CANT_TRADE";
                if (m.id == proto::DropItem::kId) {
                    if (proto::DropItem d; decode(d)) place = d.place;
                } else if (m.id == proto::ShopSell::kId) {
                    if (proto::ShopSell s; decode(s)) place = s.place;
                    text = "UIIT_MSG_COSPETERR_CANT_SELL_ACTIVATED_COSITEM";
                } else if (m.id == proto::StorageMove::kId) {
                    if (proto::StorageMove s; decode(s) && !s.fromStorage && s.toStorage) place = s.from;
                    text = "UIIT_MSG_COSPETERR_CANT_MOVETO_CHEST2";
                } else if (m.id == proto::ExchangePut::kId) {
                    if (proto::ExchangePut e; decode(e)) place = e.place;
                } else if (m.id == proto::StallPut::kId) {
                    if (proto::StallPut s; decode(s) && s.place != 0) place = s.place;
                }
                if (const game::ItemInstance* item = place ? player.inventory.At(*place) : nullptr; item != nullptr && coses->KeepsItem(player.entity, item->serial, text)) {
                    return; // the item stays where it is; its owner heard why (CosRefused)
                }
            }
            // The exchange (Exchanges.h): the same.
            if (proto::IsExchangeMessage(m.id)) {
                if (player.entity != 0 && exchanges && !exchanges->Handle(player.entity, m.id, m.payload, Clock::now())) peer.connection->Close();
                return;
            }
            // Capture the Flag and Event So-Ok (FlagWorld.h): the same.
            if (proto::IsEventMessage(m.id)) {
                if (player.entity == 0) return;
                const bool arena = m.id >= proto::ArenaRegister::kId;
                if (arena ? (arenas && !arenas->Handle(player.entity, m.id, m.payload, config.devCommands))
                          : (flagWorlds && !flagWorlds->Handle(player.entity, m.id, m.payload, config.devCommands))) {
                    peer.connection->Close();
                }
                return;
            }
            // The temples' reputation board (Temples.h; the original's 0x74DF / 0xB4DF): a league's member of level
            // 105 or more, at an area NPC of its alliance (the traders' and the hunters' for them, the thieves' for the thieves)
            // or by the HUD's icon (no NPC).
            if (m.id == proto::TempleReputationAsk::kId && player.entity != 0) {
                proto::TempleReputationAsk ask;
                if (!proto::Decode(m.payload, ask)) return;
                proto::TempleReputation answer;
                const game::JobType league = player.job.type;
                const bool traderHunter = league == game::JobType::Trader || league == game::JobType::Hunter;
                if (league == game::JobType::None) {
                    answer.code = proto::TempleReputation::kNoLeague;
                } else if (player.level < 105) {
                    answer.code = proto::TempleReputation::kLevel;
                } else if (ask.npc != 0) {
                    const std::string npc = QuestNpcNear(player.entity, ask.npc);
                    if (npc == "NPC_SD_M_AREA_MERCHANT" || npc == "NPC_SD_M_AREA_HUNTER") {
                        if (!traderHunter) answer.code = proto::TempleReputation::kOtherAlliance;
                    } else if (npc == "NPC_SD_T_AREA_THIEF") {
                        if (traderHunter) answer.code = proto::TempleReputation::kOtherAlliance;
                    } else {
                        answer.code = proto::TempleReputation::kNoNpc;
                    }
                }
                answer.ok = answer.code == 0 && !templeBoard.empty();
                if (answer.ok) answer.rows = templeBoard;
                SendMessage(*peer.connection, answer);
                return;
            }
            // The board's numbers again (the original's 0x74E0 / 0xB4E0).
            if (m.id == proto::TemplePointsAsk::kId && player.entity != 0) {
                SendMessage(*peer.connection, Temples::PointsOf(templeBoard));
                return;
            }
            // A temple's door selected (Temples.h): Seth's says its temple's players.
            if (m.id == proto::SelectGap::kId && temples && player.entity != 0) {
                if (proto::SelectGap select; proto::Decode(m.payload, select) && temples->IsDoor(select.entity)) {
                    if (const auto info = temples->DoorInfo(select.entity)) SendMessage(*peer.connection, *info);
                    return;
                }
            }
            // A temple's door (Temples.h): its gap's use.
            if (m.id == proto::EnterGap::kId && temples && player.entity != 0) {
                if (proto::EnterGap use; proto::Decode(m.payload, use) && temples->IsDoor(use.entity)) {
                    if (const proto::GateStatus status = temples->UseDoor(player.entity, use.entity); status != proto::GateStatus::Ok) {
                        SendMessage(*peer.connection, proto::GateRefused{status});
                    }
                    return;
                }
            }
            // The Forgotten World (Instances.h): the same.
            if (proto::IsInstanceMessage(m.id)) {
                if (player.entity != 0 && instances && !instances->Handle(player.entity, m.id, m.payload)) peer.connection->Close();
                return;
            }
            // Quests (Quests.h): the same.
            if (proto::IsQuestMessage(m.id)) {
                if (player.entity != 0 && quests && !quests->Handle(player.entity, m.id, m.payload)) peer.connection->Close();
                return;
            }
            // Stalls and the network (Stalls.h): the same.
            if (proto::IsStallMessage(m.id)) {
                if (player.entity != 0 && stalls && !stalls->Handle(player.entity, m.id, m.payload, Clock::now())) peer.connection->Close();
                return;
            }
            // The jobs' trade (Trades.h): the same.
            if (Trades::IsRequest(m.id)) {
                if (player.entity != 0 && trades) {
                    HoldGold(player.selected); // a sale's payout written with the goods gone (the transport's save)
                    const bool ok = trades->Handle(player.entity, m.id, m.payload);
                    ReleaseGold(player.selected);
                    if (!ok) peer.connection->Close();
                }
                return;
            }
            // The item mall and silk (Mall.h): the same.
            if (Mall::IsRequest(m.id)) {
                if (player.entity != 0 && mall && !mall->Handle(player.entity, m.id, m.payload)) peer.connection->Close();
                return;
            }
            // Magic POP (MagicPop.h): the same.
            if (MagicPop::IsRequest(m.id)) {
                if (player.entity != 0 && magicPop && !magicPop->Handle(player.entity, m.id, m.payload)) peer.connection->Close();
                return;
            }
            // The jobs' leagues (Leagues.h): the same.
            if (Leagues::IsRequest(m.id)) {
                if (player.entity != 0 && leagues && !leagues->Handle(player.entity, m.id, m.payload)) peer.connection->Close();
                return;
            }
            // COS (Coses.h): the same.
            if (proto::IsCosMessage(m.id)) {
                if (player.entity != 0 && coses && !coses->Handle(player.entity, m.id, m.payload, Clock::now())) peer.connection->Close();
                return;
            }
            // A rider neither attacks nor casts (the original's action notice 4): its client gets down first.
            if (coses && player.entity != 0 && (m.id == proto::AttackTarget::kId || m.id == proto::UseSkill::kId) && coses->MountOf(player.entity) != 0) {
                core::LogInfo("{}: {} attacks from its mount: refused", config.name, player.name);
                return;
            }
            // A stall's owner stays put in its booth: no walk, fight, gate, pick-up or motion.
            if (stalls && player.entity != 0 && stalls->Owner(player.entity)) {
                switch (m.id) {
                case proto::MoveTo::kId:
                case proto::MoveToward::kId:
                case proto::PickUp::kId:
                case proto::AttackTarget::kId:
                case proto::UseSkill::kId:
                case proto::Trace::kId:
                case proto::SetMotion::kId:
                case proto::UseGate::kId:
                case proto::Emote::kId: return;
                default: break;
                }
            }
            // A free PvP change under way is given up by a walk, an attack, a cast, a pick-up or a gate.
            if (player.entity != 0 && player.freePvpAt &&
                (m.id == proto::MoveTo::kId || m.id == proto::MoveToward::kId || m.id == proto::AttackTarget::kId || m.id == proto::UseSkill::kId ||
                 m.id == proto::PickUp::kId || m.id == proto::UseGate::kId)) {
                CancelFreePvp(peer, player);
            }
            // A visitor's walk takes it out of the stall.
            if (stalls && player.entity != 0 && (m.id == proto::MoveTo::kId || m.id == proto::MoveToward::kId)) stalls->Walked(player.entity);
            // An open exchange holds the bag and the gold (the original's 0x1835: "Cannot use item during trade or
            // talking to NPC."): what would change them is refused; so does a stall (its owner's, a buyer's from afar).
            // Alchemy at work holds the bag too (its state lets nothing else through).
            // So does the Open Market while its transaction runs (a pick-up too: it may fill the place a purchase takes).
            if (player.entity != 0 && m.id == proto::PickUp::kId && ((openMarket && openMarket->Busy(player.entity)) || (stalls && stalls->Holding(player.entity)))) return;
            if (player.entity != 0 && ((exchanges && exchanges->Holding(player.entity)) || (stalls && stalls->Holding(player.entity)) ||
                                       (alchemy && alchemy->Busy(player.entity)) || (mall && mall->Busy(player.entity)) ||
                                       (magicPop && magicPop->Busy(player.entity)) || (openMarket && openMarket->Busy(player.entity)))) {
                switch (m.id) {
                case proto::ItemMove::kId:
                case proto::DropItem::kId:
                case proto::DropGold::kId:
                case proto::UseItem::kId:
                case proto::UseItemOn::kId:
                case proto::ShopBuy::kId:
                case proto::ShopSell::kId:
                case proto::ShopBuyback::kId:
                case proto::NpcRepair::kId:
                case proto::StorageMove::kId:
                case proto::StorageGold::kId:
                case proto::SkinChange::kId:
                case proto::Withdraw::kId:
                case proto::CubeMove::kId:
                case proto::Repair::kId: {
                    proto::ItemMoved answer;
                    answer.status = proto::ItemStatus::Exchanging;
                    SendMessage(*peer.connection, answer);
                    return;
                }
                case proto::AvatarMove::kId: {
                    proto::AvatarMoved answer;
                    answer.status = proto::ItemStatus::Exchanging;
                    SendMessage(*peer.connection, answer);
                    return;
                }
                default: break;
                }
            }
            // The dead move no item and no gold (the original refuses every kind but the death's own drop and a mall
            // package: 2).
            if (player.entity != 0 && player.dead) {
                switch (m.id) {
                case proto::ItemMove::kId:
                case proto::DropItem::kId:
                case proto::DropGold::kId:
                case proto::StorageMove::kId:
                case proto::StorageGold::kId:
                case proto::CubeMove::kId: {
                    proto::ItemMoved answer;
                    answer.status = proto::ItemStatus::Failed;
                    SendMessage(*peer.connection, answer);
                    return;
                }
                case proto::AvatarMove::kId: {
                    proto::AvatarMoved answer;
                    answer.status = proto::ItemStatus::Failed;
                    SendMessage(*peer.connection, answer);
                    return;
                }
                default: break;
                }
            }
            // In the world (or on the way): only its messages.
            switch (m.id) {
            case proto::MoveTo::kId:
            case proto::MoveToward::kId:
                // A click (MoveTo), or one on the sky (MoveToward: that way until something stops it); the two are laid
                // out alike.
                if (proto::MoveTo move; decode(move) && player.entity != 0) {
                    bool toward = m.id == proto::MoveToward::kId;
                    if (player.dead) return;
                    // A cast going on holds it where it stands until its impact (it is not cancelled), a chain
                    // of rows until its last went off (the client keeps the
                    // click and sends it once its swing ends).
                    if (const Clock::time_point t = Clock::now(); t < player.combat.castUntil || t < player.combat.chainUntil) return;
                    // Nor while knocked down or back (the client keeps the click till it stands), nor frozen,
                    // asleep, stunned or rooted (its states).
                    if (player.knocked != game::Knock::None && Clock::now() < player.knockedUntil) return;
                    if (player.combat.states.Locked() || player.combat.states.Rooted()) return;
                    // Not while sitting down or standing up; a sitting player stands as it
                    // goes, and its own walk ends a trace.
                    if (player.motion == proto::kMotionChanging) return;
                    // Not farther than 100 m from its transport left standing: the walk stops at the line (Coses::Tether).
                    if (coses && !coses->Tether(player.entity, move.target, toward)) return;
                    if (coses && coses->KeptOutOfTown(player.entity, move.target)) return;
                    CancelSuitChange(player);
                    StandUp(player);
                    player.traceTarget = 0;
                    if (skillUnit) {
                        SkillCaster caster = CasterOf(player);
                        if (skillUnit->EndOnEvent(caster, 1)) StatsChanged(player.entity);
                    }
                    player.pickTarget = 0;   // a new walk cancels a pick-up on the way
                    player.combat.StopAttacking(); // and an attack
                    if (flagWorlds) {
                        if (const auto from = world->Where(player.entity)) flagWorlds->Walked(player.entity, from->position, move.target);
                    }
                    if (const World::MoveRefused why = toward ? world->MoveToward(player.entity, move.target) : world->MoveTo(player.entity, move.target);
                        why != World::MoveRefused::None) {
                        static constexpr const char* kWhy[] = {"", "not in the world", "too far", "the navigation there is not in",
                                                               "no floor under the walker", "the walk failed"};
                        core::LogInfo("{}: {} walks to {},{} ({:.0f}, {:.0f}): refused ({})", config.name, player.name, move.target.region & 0xFF,
                                      move.target.region >> 8, move.target.x, move.target.z, kWhy[std::size_t(why)]);
                    }
                }
                return;
            case proto::Turn::kId:
                // A turn where it stands (toward an NPC it talks to, an item it picks up in reach): kept, and told to
                // those who see it. Not from the dead, the knocked down, the frozen, asleep or stunned, nor a walker.
                if (proto::Turn turn; decode(turn) && player.entity != 0 && !player.dead) {
                    if (player.knocked != game::Knock::None && Clock::now() < player.knockedUntil) return;
                    if (player.combat.states.Locked()) return;
                    if (world->Turn(player.entity, proto::YawOf(turn.heading))) Broadcast(player.entity, proto::EntityTurned{player.entity, turn.heading});
                }
                return;
            case proto::WorldLeave::kId: return LeaveWorld(peer.id, true);
            case proto::ChatSend::kId:
                if (proto::ChatSend send; decode(send) && player.entity != 0) Chat(peer, player, send);
                return;
            case proto::ItemMove::kId:
                if (proto::ItemMove move; decode(move) && player.entity != 0) MoveItem(peer, player, move);
                return;
            case proto::AvatarMove::kId:
                if (proto::AvatarMove move; decode(move) && player.entity != 0) MoveAvatar(peer, player, move);
                return;
            case proto::QuickSlotSet::kId:
                if (proto::QuickSlotSet set; decode(set) && player.entity != 0) SetQuickSlot(peer, player, set.record);
                return;
            case proto::LogoutRequest::kId:
                if (proto::LogoutRequest r; decode(r) && player.entity != 0) Logout(peer, player, r.kind);
                return;
            case proto::LogoutCancel::kId:
                if (player.entity != 0) CancelLogout(peer, player);
                return;
            case proto::UseGate::kId:
                if (proto::UseGate use; decode(use) && player.entity != 0) {
                    CancelLogout(peer, player);
                    UseGate(peer, player, use);
                }
                return;
            case proto::UseItem::kId:
                if (proto::UseItem use; decode(use) && player.entity != 0) UseItem(peer, player, use);
                return;
            // The item mall's services.
            case proto::PremiumUse::kId:
                if (proto::PremiumUse u; decode(u) && player.entity != 0) UsePremium(peer, player, u);
                return;
            case proto::PremiumBooth::kId:
                if (proto::PremiumBooth b; decode(b) && player.entity != 0) PremiumBoothOn(peer, player, b);
                return;
            case proto::TimedPause::kId:
                if (proto::TimedPause t; decode(t) && player.entity != 0) PauseService(peer, player, t);
                return;
            case proto::TimedCancel::kId:
                if (proto::TimedCancel t; decode(t) && player.entity != 0) CancelBonus(peer, player, t);
                return;
            case proto::UseItemOn::kId:
                // The Extension Gear and the potion of growth (the mall's items), else the gender switch tool (its services).
                if (proto::UseItemOn u; decode(u) && player.entity != 0 && !UseMallItemOn(peer, player, u)) UseItemOnService(peer, player, u);
                return;
            case proto::AutoPotionSettings::kId:
                if (proto::AutoPotionSettings s; decode(s) && player.entity != 0) SetAutoPotion(peer, player, s);
                return;
            case proto::CancelReturn::kId:
                if (player.returnAt) {
                    player.returnAt.reset();
                    player.reverseOption = 0;
                    const bool recall = std::exchange(player.recallTo, std::nullopt).has_value();
                    core::LogInfo("{}: {} cancels the {}", config.name, player.name, recall ? "guild recall" : "return");
                    SendMessage(*peer.connection, proto::ReturnCancelled{});
                }
                return;
            case proto::CancelCasting::kId:
                // The gauge's cancel of a quest search (kind 1) or of a repair kit's work (kind 4).
                if (proto::CancelCasting c; decode(c) && player.entity != 0) {
                    if (c.kind == proto::CastingKind::QuestSearch && quests) quests->CancelSearch(player.entity, c.id);
                    if (c.kind == proto::CastingKind::SkillItem && fortresses) fortresses->CancelKits(player.entity);
                }
                return;
            case proto::DropItem::kId:
                if (proto::DropItem drop; decode(drop) && player.entity != 0) DropItem(peer, player, drop);
                return;
            // The item mall's services.
            case proto::Repair::kId:
                if (proto::Repair r; decode(r) && player.entity != 0 && !player.dead) NpcRepair(peer, player, r);
                return;
            case proto::SkinChange::kId:
                if (proto::SkinChange c; decode(c) && player.entity != 0) ChangeSkin(peer, player, c);
                return;
            case proto::Withdraw::kId:
                if (proto::Withdraw w; decode(w) && player.entity != 0 && !player.dead) Withdraw(peer, player, w);
                return;
            case proto::CubeMove::kId:
                if (proto::CubeMove c; decode(c) && player.entity != 0) MoveCube(peer, player, c);
                return;
            case proto::DropGold::kId:
                if (proto::DropGold drop; decode(drop) && player.entity != 0) DropGold(peer, player, drop);
                return;
            case proto::SetMotion::kId:
                if (proto::SetMotion m; decode(m) && player.entity != 0) SetMotion(player, m.motion, Clock::now());
                return;
            case proto::SetMarks::kId:
                if (proto::SetMarks m; decode(m) && player.entity != 0) SetMarks(peer, player, m.marks);
                return;
            case proto::Trace::kId:
                if (proto::Trace t; decode(t) && player.entity != 0) StartTrace(player, t.target);
                return;
            case proto::FreePvp::kId:
                if (proto::FreePvp f; decode(f) && player.entity != 0) {
                    CancelSuitChange(player);
                    SetFreePvp(peer, player, f.team);
                }
                return;
            case proto::Emote::kId:
                // A gesture, told as it is to everyone who sees the player (no checks;
                // ours: not while dead, the dead lying down).
                if (proto::Emote e; decode(e) && player.entity != 0 && !player.dead) Broadcast(player.entity, proto::EntityEmote{player.entity, e.gesture});
                return;
            case proto::PickUp::kId:
                if (proto::PickUp pick; decode(pick) && player.entity != 0 && !player.dead) {
                    if (player.motion == proto::kMotionChanging) return;
                    // A command of the action list: behind a swing or a cast it waits.
                    if (skillUnit) {
                        SkillCaster caster = CasterOf(player);
                        if (skillUnit->HoldPickUp(caster, pick.entity, Clock::now())) return;
                    }
                    StandUp(player);
                    player.traceTarget = 0;
                    player.combat.attackTarget = 0;
                    PickUp(peer, player, pick);
                }
                return;
            case proto::AttackTarget::kId:
                if (proto::AttackTarget a; decode(a) && player.entity != 0 && player.motion != proto::kMotionChanging) {
                    CancelSuitChange(player);
                    StandUp(player);
                    player.traceTarget = 0;
                    Attack(peer, player, a);
                }
                return;
            case proto::UseSkill::kId:
                if (proto::UseSkill u; decode(u) && player.entity != 0 && player.motion != proto::kMotionChanging) {
                    CancelSuitChange(player);
                    StandUp(player);
                    player.traceTarget = 0;
                    UseSkill(peer, player, u);
                }
                return;
            case proto::ActionStop::kId:
                if (proto::ActionStop stop; decode(stop) && player.entity != 0 && skillUnit) {
                    SkillCaster caster = CasterOf(player);
                    skillUnit->Stop(caster, Clock::now());
                }
                return;
            case proto::CancelBuff::kId:
                if (proto::CancelBuff c; decode(c) && player.entity != 0 && skillUnit) {
                    SkillCaster caster = CasterOf(player);
                    skillUnit->CancelBuff(caster, c.token);
                }
                return;
            case proto::RaiseMastery::kId:
                if (proto::RaiseMastery r; decode(r) && player.entity != 0) RaiseMastery(peer, player, r);
                return;
            case proto::LearnSkill::kId:
                if (proto::LearnSkill r; decode(r) && player.entity != 0) LearnSkill(peer, player, r);
                return;
            case proto::RaiseStat::kId:
                if (proto::RaiseStat r; decode(r) && player.entity != 0) RaiseStat(peer, player, r);
                return;
            case proto::Resurrect::kId:
                if (proto::Resurrect r; decode(r) && player.entity != 0) Resurrect(peer, player, r.kind);
                return;
            case proto::ShopOpen::kId:
                if (proto::ShopOpen open; decode(open) && player.entity != 0) OpenShop(peer, player, open);
                return;
            case proto::ShopClose::kId:
                if (proto::ShopClose close; decode(close) && player.entity != 0) CloseShop(player, close);
                return;
            case proto::ShopBuy::kId:
                if (proto::ShopBuy buy; decode(buy) && player.entity != 0) BuyGood(peer, player, buy);
                return;
            case proto::ShopSell::kId:
                if (proto::ShopSell sell; decode(sell) && player.entity != 0) SellItem(peer, player, sell);
                return;
            case proto::ShopBuyback::kId:
                if (proto::ShopBuyback back; decode(back) && player.entity != 0) BuyBack(peer, player, back);
                return;
            case proto::NpcRepair::kId:
                if (proto::NpcRepair r; decode(r) && player.entity != 0) RepairItems(peer, player, r);
                return;
            case proto::StorageOpen::kId:
                if (proto::StorageOpen open; decode(open) && player.entity != 0) OpenStorage(peer, player, open);
                return;
            case proto::StorageMove::kId:
                if (proto::StorageMove move; decode(move) && player.entity != 0) MoveStorageItem(peer, player, move);
                return;
            case proto::StorageGold::kId:
                if (proto::StorageGold g; decode(g) && player.entity != 0) MoveStorageGold(peer, player, g);
                return;
            case proto::StorageClose::kId:
                CloseGuildStorage(peer.id, player);
                return;
            case proto::NpcTalk::kId:
                if (proto::NpcTalk talk; decode(talk) && player.entity != 0) TalkToNpc(peer, player, talk);
                return;
            case proto::GateTalk::kId:
                if (proto::GateTalk talk; decode(talk) && player.entity != 0) TalkToGate(peer, player, talk);
                return;
            case proto::DesignateReturn::kId:
                if (proto::DesignateReturn d; decode(d) && player.entity != 0) DesignateReturn(peer, player, d);
                return;
            case proto::DevGoto::kId:
                if (proto::DevGoto go; decode(go) && player.entity != 0) {
                    CancelLogout(peer, player);
                    DevGoto(peer, player, go.point);
                }
                return;
            case proto::DevDie::kId:
                if (player.entity != 0) DevDie(peer, player);
                return;
            case proto::DevStrike::kId:
                if (proto::DevStrike x; decode(x) && player.entity != 0 && !player.dead) {
                    // A monster: one hit as an attack's, its death as any kill's (the quests' tests: a unique's).
                    if (config.devCommands && PlayerByEntity(x.target) == nullptr && monsters && world->Where(x.target)) {
                        const Monsters::Damaged d = HitMonster(x.target, player.entity, x.damage, Clock::now(), nullptr);
                        core::LogInfo("{}: {} strikes monster {} for {} (developer){}", config.name, player.name, x.target, x.damage, d.killed ? ": killed" : "");
                        if (!d.hit) return;
                        Broadcast(x.target, proto::EntityDamaged{x.target, player.entity, x.damage, d.hp, std::uint8_t(d.killed ? proto::kDamageKilled : 0)});
                        if (d.killed) Killed(x.target, d);
                        return;
                    }
                    if (!config.devCommands || !Hostile(player.entity, x.target)) {
                        core::LogInfo("{}: {} asked for a developer strike on {}: refused", config.name, peer.address, x.target);
                        return;
                    }
                    const Monsters::Damaged d = HitPlayer(x.target, player.entity, x.damage, Clock::now());
                    player.combat.battleUntil = Clock::now() + std::chrono::seconds(20); // an attack's: the striker in combat too
                    Broadcast(x.target, proto::EntityDamaged{x.target, player.entity, x.damage, d.hp, std::uint8_t(d.killed ? proto::kDamageKilled : 0)});
                    if (d.killed) Killed(x.target, d);
                }
                return;
            case proto::DevGotoPlace::kId:
                if (proto::DevGotoPlace x; decode(x) && player.entity != 0 && !player.dead) {
                    CancelLogout(peer, player);
                    DevGotoPlace(peer, player, x);
                }
                return;
            case proto::DevGotoMonster::kId:
                if (proto::DevGotoMonster x; decode(x) && player.entity != 0 && !player.dead) DevGotoMonster(peer, player, x.monster);
                return;
            case proto::DevGive::kId:
                if (proto::DevGive x; decode(x) && player.entity != 0) {
                    if (!config.devCommands) {
                        core::LogInfo("{}: {} asked for a developer item; not a development server", config.name, peer.address);
                        return;
                    }
                    DevGive(peer, player, x);
                }
                return;
            case proto::DevLearn::kId:
                if (proto::DevLearn x; decode(x) && player.entity != 0 && !player.dead) DevLearn(peer, player, x);
                return;
            case proto::DevSpawn::kId:
                if (proto::DevSpawn x; decode(x) && player.entity != 0) DevSpawn(peer, player, x);
                return;
            case proto::DevAfflict::kId:
                if (proto::DevAfflict x; decode(x) && player.entity != 0) DevAfflict(peer, player, x);
                return;
            case proto::DevReset::kId:
                if (proto::DevReset x; decode(x) && player.entity != 0) DevReset(peer, player, x.what, x.place);
                return;
            case proto::DevRemove::kId:
                if (proto::DevRemove x; decode(x) && player.entity != 0 && LabAllowed(peer, "a removal")) DevRemove(player, x.entity);
                return;
            case proto::DevSheet::kId:
                if (proto::DevSheet x; decode(x) && player.entity != 0 && LabAllowed(peer, "a sheet")) DevSheet(peer, player);
                return;
            case proto::DevGuild::kId:
                if (proto::DevGuild x; decode(x) && player.entity != 0 && guilds && LabAllowed(peer, "a guild")) guilds->DevCreate(GuildActorOf(peer.id, player), x.name, x.level);
                return;
            case proto::DevVitals::kId:
                if (proto::DevVitals x; decode(x) && player.entity != 0 && !player.dead && LabAllowed(peer, "vitals")) {
                    if (x.maxHp != player.labMaxHp) {
                        player.labMaxHp = x.maxHp;
                        UpdateStats(&peer, player);
                    }
                    player.hp = std::max<std::uint32_t>(1, std::uint32_t(std::uint64_t(player.stats.maxHp) * std::min<int>(x.hpPercent, 100) / 100));
                    player.mp = std::uint32_t(std::uint64_t(player.stats.maxMp) * std::min<int>(x.mpPercent, 100) / 100);
                    world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
                    SendVitals(peer, player);
                }
                return;
            case proto::DevExperience::kId:
                if (proto::DevExperience x; decode(x) && player.entity != 0 && !player.dead) {
                    if (!config.devCommands) {
                        core::LogInfo("{}: {} asked for developer experience; not a development server", config.name, peer.address);
                        return;
                    }
                    GainExperience(peer, player, x.experience, x.skill);
                }
                return;
            case proto::BerserkRequest::kId:
                if (player.entity != 0) Berserk(peer, player, Clock::now());
                return;
            case proto::RevivalAnswer::kId:
                if (proto::RevivalAnswer a; decode(a) && player.entity != 0) AnswerRevival(peer, player, a.accept != 0);
                return;
            case proto::ViewLimits::kId:
                // The player's own view: never more than the shares; the answer says what applies.
                if (proto::ViewLimits limits; decode(limits) && player.entity != 0) {
                    const World::ViewLimitsApplied applied = world->SetViewLimits(player.entity, limits.players, limits.monsters, float(limits.radius) * 10.0f);
                    const WorldConfig& w = world->Config();
                    const auto clamp16 = [](std::size_t v) { return std::uint16_t(std::min<std::size_t>(v, 0xFFFF)); };
                    SendMessage(*peer.connection, proto::ViewLimitsAnswer{clamp16(applied.players), clamp16(applied.monsters),
                                                                          clamp16(w.viewPlayers), clamp16(w.viewMonsters),
                                                                          clamp16(std::size_t(w.viewRadius / 10.0f))});
                }
                return;
            default:
                // The party block (Parties.h).
                if (m.id >= 0x10D0 && m.id <= 0x10EF && player.entity != 0 && parties) {
                    const Parties::Handled handled = parties->Handle(player.entity, m.id, m.payload, Clock::now());
                    if (handled == Parties::Handled::Malformed) peer.connection->Close();
                    if (handled != Parties::Handled::No) return;
                }
                core::LogInfo("{}: {} sent 0x{:04x} in the world; ignored", config.name, peer.address, m.id);
                return;
            }
        }
        switch (m.id) {
        case proto::WorldEnter::kId: return EnterWorld(peer);
        case proto::CharacterListRequest::kId: return List(peer);
        case proto::CharacterCreate::kId:
            if (proto::CharacterCreate c; decode(c)) Create(peer, std::move(c));
            return;
        case proto::CharacterNameCheck::kId:
            if (proto::CharacterNameCheck c; decode(c)) NameCheck(peer, std::move(c.name));
            return;
        case proto::CharacterDelete::kId:
            if (proto::CharacterDelete c; decode(c)) Change(peer, proto::LobbyAction::Delete, std::move(c.name));
            return;
        case proto::CharacterRestore::kId:
            if (proto::CharacterRestore c; decode(c)) Change(peer, proto::LobbyAction::Restore, std::move(c.name));
            return;
        case proto::CharacterSelect::kId:
            if (proto::CharacterSelect c; decode(c)) Change(peer, proto::LobbyAction::Select, std::move(c.name));
            return;
        default:
            core::LogInfo("{}: {} sent 0x{:04x}, which the lobby does not take; closing", config.name, peer.address, m.id);
            peer.connection->Close();
        }
    }

    // The ticket, then the account's one session.
    void Enter(Peer& peer, std::vector<std::uint8_t> ticket) {
        const auto refuse = [&](proto::LobbyEnterStatus status) {
            proto::LobbyEntered r;
            r.status = status;
            SendMessage(*peer.connection, r);
            peer.connection->CloseAfterSend();
        };
        peer.busy = true;
        // The token this connection holds its account's session by (Store.h ClaimSession): never 0, never another's.
        const std::int64_t token = std::int64_t(sessionTokens() >> 1) | 1;
        database.Submit([this, peerId = peer.id, ticket = std::move(ticket), full = online >= config.capacity, token](Database& db) {
            // A moving player's ticket carries its character, and its session has come with it: a world
            // that is full still takes in someone who is already playing.
            std::int64_t character = 0;
            std::optional<std::int64_t> account = ConsumeTicket(db, ticket, config.id, &character);
            bool claimed = false, ok = true;
            if (account && character != 0) {
                claimed = true;
                AdoptSession(db, *account, config.id, token); // the move's session, this connection's now
            } else if (account && !full) {
                claimed = ClaimSession(db, *account, config.id, config.staleSeconds, token, ok);
            }
            std::string accountName;
            int security = 3;
            if (account) AccountSecurity(db, *account, accountName, security);
            completions->Post([this, peerId, account, character, claimed, ok, full, token, accountName = std::move(accountName), security] {
                Peer* peer = peers->Find(peerId);
                proto::LobbyEntered r;
                r.maxCharacters = std::uint8_t(config.maxCharacters);
                if (!ok) {
                    r.status = proto::LobbyEnterStatus::Busy;
                } else if (!account) {
                    r.status = proto::LobbyEnterStatus::InvalidTicket;
                } else if (full && character == 0) {
                    r.status = proto::LobbyEnterStatus::ServerFull;
                } else if (!claimed) {
                    r.status = proto::LobbyEnterStatus::AlreadyLoggedIn;
                } else if (!peer) {
                    // Gone while its ticket was checked: give the session back.
                    database.Submit([account = *account, id = config.id, token](Database& db) { ReleaseSession(db, account, id, token); });
                    return;
                }
                if (!peer) return;
                if (claimed) peer->session = token;
                peer->busy = false;
                SendMessage(*peer->connection, r);
                if (r.status != proto::LobbyEnterStatus::Accepted) {
                    peer->connection->CloseAfterSend();
                    return;
                }
                peer->account = *account;
                accountOf[peerId] = {accountName, security == 1};
                ++online;
                if (character != 0) {
                    // Straight into the world, where its last server said it is.
                    PlayerWorld& player = players[peerId];
                    player.selected = character;
                    player.arrived = true;
                    EnterWorld(*peer);
                }
            });
        });
    }

    void List(Peer& peer) {
        if (peer.busy) return;
        peer.busy = true;
        database.Submit([this, peerId = peer.id, account = peer.account](Database& db) {
            bool ok = false;
            std::vector<CharacterGone> gone;
            std::vector<CharacterRow> rows = ListCharacters(db, account, config.id, config.deleteSeconds, ok, &gone);
            completions->Post([this, peerId, rows = std::move(rows), ok, gone = std::move(gone)] {
                // Those deleted for good left their guild and academy: every server's memory of them loaded again.
                for (const CharacterGone& g : gone) {
                    if (guilds && g.guild != 0) guilds->RowsChanged(g.guild);
                    if (academies && g.academy != 0) academies->RowsChanged(g.academy);
                }
                Peer* peer = peers->Find(peerId);
                if (!peer) return;
                peer->busy = false;
                if (!ok) return Send(peerId, proto::LobbyAction::List, proto::LobbyStatus::Busy);
                proto::CharacterList list;
                list.maxCharacters = std::uint8_t(config.maxCharacters);
                for (const CharacterRow& row : rows) list.characters.push_back(Summary(row));
                SendMessage(*peer->connection, list);
            });
        });
    }

    void Create(Peer& peer, proto::CharacterCreate c) {
        if (peer.busy) return Send(peer.id, proto::LobbyAction::Create, proto::LobbyStatus::Busy);
        if (const auto name = FromNameCheck(config.rules->names.Check(c.name)); name != proto::LobbyStatus::Ok) {
            return Send(peer.id, proto::LobbyAction::Create, name);
        }
        switch (config.rules->creation.Check(c.model, c.chest, c.legs, c.shoes, c.weapon)) {
        case game::CreationCheck::Ok: break;
        case game::CreationCheck::BadWeapon:
            return Send(peer.id, proto::LobbyAction::Create,
                        c.weapon == 0 ? proto::LobbyStatus::NoWeapon : proto::LobbyStatus::InvalidCreation);
        case game::CreationCheck::BadModel:
        case game::CreationCheck::BadArmour: return Send(peer.id, proto::LobbyAction::Create, proto::LobbyStatus::InvalidCreation);
        }
        NewCharacter n;
        n.name = c.name;
        n.model = c.model;
        n.height = c.height;
        // Its return point the race's town gate (STORE_EU_GATE 19554, STORE_CH_GATE 2094), as the original's creation.
        if (const assets::ObjectRow* race = config.characters ? config.characters->Find(c.model) : nullptr) n.returnGate = race->country == 1 ? 19554u : 2094u;
        n.level = game::StartingStats::kLevel;
        n.strength = game::StartingStats::kStrength;
        n.intelligence = game::StartingStats::kIntelligence;
        n.hp = game::StartingStats::kHp;
        n.mp = game::StartingStats::kMp;
        // What the screen chose (a European character may wear no armour), and the shield a one-handed
        // starter weapon comes with.
        const std::pair<EquipSlot, std::uint32_t> worn[] = {{EquipSlot::Chest, c.chest},
                                                           {EquipSlot::Legs, c.legs},
                                                           {EquipSlot::Feet, c.shoes},
                                                           {EquipSlot::Weapon, c.weapon},
                                                           {EquipSlot::Shield, config.rules->creation.ShieldFor(c.weapon)}};
        const assets::ItemTable* table = config.rules->items.get();
        for (const auto& [slot, item] : worn) {
            if (item == 0) continue;
            const assets::ItemRow* row = table ? table->Find(item) : nullptr;
            n.equipment.push_back({slot, item, 0, row ? game::NewDurability(*row) : 0u});
        }
        // A bow comes with 250 arrows, a crossbow with 250 bolts (the original's character creation).
        if (const assets::ItemRow* weapon = table ? table->Find(c.weapon) : nullptr; weapon && game::TwoHanded(*weapon)) {
            const char* ammo = weapon->typeId[3] == 6 ? "ITEM_ETC_AMMO_ARROW_01_DEF" : weapon->typeId[3] == 12 ? "ITEM_ETC_AMMO_BOLT_01_DEF" : nullptr;
            if (const assets::ItemRow* a = ammo ? table->Find(ammo) : nullptr) {
                n.equipment.push_back({EquipSlot::Shield, a->refObjId, 0, 0, std::uint16_t(game::MaxStack(*a))});
            }
        }
        peer.busy = true;
        database.Submit([this, peerId = peer.id, account = peer.account, n = std::move(n)](Database& db) {
            std::string error;
            const CreateCharacterResult result = CreateCharacter(db, account, config.id, n, config.maxCharacters, error);
            if (result == CreateCharacterResult::Failed) core::LogError("{}: creating {} failed: {}", config.name, n.name, error);
            // Its log (the original's character creation, 1).
            std::int64_t made = 0;
            if (result == CreateCharacterResult::Created) {
                const QueryResult id = db.Query("SELECT charid FROM _char WHERE lower(charname16) = lower($1) ORDER BY charid DESC LIMIT 1", {n.name});
                if (id.Ok() && id.Rows() > 0) made = id.Int(0, 0);
            }
            completions->Post([this, peerId, result, made] {
                if (made != 0 && gameLog) gameLog->Char({made, std::uint8_t(logs::CharEvent::Created), 0, 0, {}, {}});
                proto::LobbyStatus status = proto::LobbyStatus::Failed;
                if (result == CreateCharacterResult::Created) status = proto::LobbyStatus::Ok;
                if (result == CreateCharacterResult::NameTaken) status = proto::LobbyStatus::NameTaken;
                if (result == CreateCharacterResult::TooMany) status = proto::LobbyStatus::TooManyCharacters;
                Send(peerId, proto::LobbyAction::Create, status);
            });
        });
    }

    void NameCheck(Peer& peer, std::string name) {
        if (peer.busy) return Send(peer.id, proto::LobbyAction::NameCheck, proto::LobbyStatus::Busy);
        if (const auto status = FromNameCheck(config.rules->names.Check(name)); status != proto::LobbyStatus::Ok) {
            return Send(peer.id, proto::LobbyAction::NameCheck, status);
        }
        peer.busy = true;
        database.Submit([this, peerId = peer.id, name = std::move(name)](Database& db) {
            bool ok = false;
            const bool free = CharacterNameFree(db, config.id, name, ok);
            completions->Post([this, peerId, free, ok] {
                Send(peerId, proto::LobbyAction::NameCheck,
                     !ok ? proto::LobbyStatus::Busy : (free ? proto::LobbyStatus::Ok : proto::LobbyStatus::NameTaken));
            });
        });
    }

    void Change(Peer& peer, proto::LobbyAction action, std::string name) {
        if (peer.busy) return Send(peer.id, action, proto::LobbyStatus::Busy);
        peer.busy = true;
        database.Submit([this, peerId = peer.id, account = peer.account, action, name = std::move(name)](Database& db) {
            std::int64_t character = 0;
            CharacterChange change = CharacterChange::Failed;
            switch (action) {
            case proto::LobbyAction::Delete: change = DeleteCharacter(db, account, config.id, name, config.deleteSeconds); break;
            case proto::LobbyAction::Restore: change = RestoreCharacter(db, account, config.id, name, config.deleteSeconds); break;
            case proto::LobbyAction::Select: change = SelectCharacter(db, account, config.id, name, character); break;
            default: break;
            }
            completions->Post([this, peerId, action, change, character] {
                if (action == proto::LobbyAction::Select && change == CharacterChange::Done) {
                    core::LogInfo("{}: character {} selected", config.name, character);
                    if (peers->Find(peerId)) players[peerId].selected = character;
                }
                Send(peerId, action, FromChange(change));
            });
        });
    }

    // The selected character into the world: its row, then its place (or its race's town).
    void EnterWorld(Peer& peer) {
        PlayerWorld& player = players[peer.id];
        const auto refuse = [&](proto::WorldStatus status) {
            proto::WorldEntered r;
            r.status = status;
            SendMessage(*peer.connection, r);
        };
        if (!world) return refuse(proto::WorldStatus::Busy);
        if (player.selected == 0) return refuse(proto::WorldStatus::NotSelected);
        // The character still in the world from another connection (a dropped one counting down, a client that hung)
        // leaves first, its state saved; the load then waits for those saves (a barrier), so it reads what they wrote.
        bool held = false;
        for (const auto& [otherId, other] : players) held |= otherId != peer.id && other.selected == player.selected && other.entity != 0;
        if (held) ReplaceOtherConnections(peer.id, player.selected, "character " + std::to_string(player.selected));
        player.entering = true;
        auto load = [this, peerId = peer.id, account = peer.account, character = player.selected](Database& db) {
            bool ok = false;
            std::optional<CharacterRow> row = LoadCharacter(db, account, config.id, character, ok);
            const AccountRestrictions restrictions = LoadRestrictions(db, account); // a GM's trade and chat punishments
            completions->Post([this, peerId, row = std::move(row), ok, restrictions] {
                Peer* peer = peers->Find(peerId);
                if (!peer) return;
                PlayerWorld& player = players[peerId];
                player.entering = false;
                player.restrictions = restrictions;
                // Replaced meanwhile by another connection's entry (ReplaceOtherConnections): it stays out.
                if (ok && row && player.selected != row->id) return;
                if (!ok || !row) {
                    proto::WorldEntered r;
                    r.status = ok ? proto::WorldStatus::NotSelected : proto::WorldStatus::Busy;
                    SendMessage(*peer->connection, r);
                    return;
                }
                WorldPlayer p;
                if (row->place) {
                    p.position = {row->place->region, row->place->x, row->place->y, row->place->z};
                    p.yaw = row->place->yaw;
                    p.dropOnGround = true; // on the floor nearest its saved height (where it stood)
                } else {
                    const auto figure = config.rules->creation.FigureOf(row->model);
                    p = StartPlace(figure ? figure->race : game::CreationRules::Race::China);
                }
                // Another server's part of the world: on to it.
                if (const std::uint16_t owner = OwnerOf(p.position.region); owner != config.id) {
                    if (!ServerUp(owner)) {
                        proto::WorldEntered r;
                        r.status = proto::WorldStatus::ServerDown;
                        SendMessage(*peer->connection, r);
                        return;
                    }
                    player.name = row->name;
                    StartMove(peerId, player, p.position, p.yaw, owner);
                    return;
                }
                // One copy of a character in the world: one still here from another connection (a client that hung
                // and logged in again) goes first (the user's report 2026-11-13: a clone and its pets).
                ReplaceOtherConnections(peerId, row->id, row->name);
                p.peer = peerId;
                p.character = row->id;
                p.summary = Summary(*row);
                // The world answers with the character once the ground around it is loaded; the
                // quick slots go now (the client keeps them through the entry).
                player.entity = world->Enter(p);
                player.name = row->name;
                player.model = row->model;
                byName[NameKey(row->name)] = peerId;
                proto::QuickSlotList slots;
                for (const CharacterRow::QuickSlot& q : row->quickSlots) {
                    // A skill's slot on a chain's sub-row (kept from before the heads were found) shows its head.
                    std::uint32_t value = q.value;
                    if (q.kind == 73 && config.skills) { // game::QuickSlot::Kind::Skill
                        if (const assets::SkillRow* s = config.skills->Find(value)) value = config.skills->Head(*s).id;
                    }
                    slots.slots.push_back({q.slot, q.kind, value});
                }
                SendMessage(*peer->connection, slots);
                player.berserkPoints = row->berserkPoints;
                player.hwanLevel = row->hwanLevel;
                player.recallSpot = row->recallSpot;
                player.deathSpot = row->deathSpot;
                // Standing, running, its marks (the beginner's only up to level 19: ours, as the original
                // refuses a request carrying it above that), tracing nobody.
                player.motion = player.motionTo = proto::kMotionStand;
                player.walking = false;
                player.traceTarget = 0;
                player.marks = std::uint8_t(row->marks & (row->level > 19 ? ~proto::kMarkBeginner : 0xFF));
                player.stallSkin = row->stallSkin;
                TellMarks(*peer, player);
                EnterPvp(*peer, player, *row); // its PK counts and PvP state
                player.berserkUntil.reset();
                player.combat.berserk = false;
                SendMessage(*peer->connection, proto::BerserkPoints{player.berserkPoints});
                // Its auto potion, its kinds unlocked and nothing queued.
                player.autoPotion = row->autoPotion;
                player.autoNext = {};
                player.reuseLocks.clear();
                player.hpSteps.clear();
                player.mpSteps.clear();
                SendMessage(*peer->connection, game::ToMessage(player.autoPotion));
                // Its job: its league and alias, and the state its suit gives (at login without the
                // suit's lock).
                player.job = row->job;
                player.alias = row->alias;
                player.jobLeftAt = row->jobLeftAt;
                player.jobState = kNoJob;
                player.safeTrade = row->job.type == game::JobType::Trader ? row->safeTrade : std::nullopt;
                SendJobInfo(*peer, player, true);
                TellSafeTrade(player, 0x51); // an active one told again
                // The bargain goods' lock-out still running (timed job 2/7): kept, and its icon told again.
                if (const std::int64_t left = row->bargainUntil - std::chrono::duration_cast<std::chrono::seconds>(
                                                                      std::chrono::system_clock::now().time_since_epoch()).count();
                    left > 0 && trades) {
                    trades->SetBargainLock(player.entity, std::chrono::seconds(left));
                    SendMessage(*peer->connection, proto::TradeBargainLock{std::uint32_t(std::min<std::int64_t>(left, 600))});
                }
                // The inventory: items stored without durability (before C7e) get a new item's.
                player.inventory = {};
                player.sets.Reset(); // its items made anew: their sets counted at its first numbers (UpdateStats)
                player.inventory.size = std::min(row->inventorySize, game::Inventory::kMaxSize);
                player.inventory.gold = row->gold;
                // The gate it designated (_char keeps the gate's object id) as its teleport point.
                player.returnPoint = 0;
                if (const assets::TeleportPoint* gate = row->returnGate != 0 && config.teleports ? config.teleports->PointOf(row->returnGate) : nullptr) {
                    player.returnPoint = gate->id;
                }
                // The buffs it left running, with the time they had left (their changes worked out once
                // its numbers are in, below).
                player.combat = {};
                std::vector<CharacterRow::Buff> restoring = row->buffs;
                std::vector<std::uint8_t> mended;
                for (const StoredItem& s : row->items) {
                    game::ItemInstance item;
                    item.serial = s.id;
                    item.refId = s.refId;
                    item.plus = s.plus;
                    item.variance = s.variance;
                    item.rentUntil = s.rentUntil;
                    item.advanced = s.advanced;
                    item.bound = s.bound;
                    item.advancedRecipe = s.advancedRecipe;
                    item.sockets = s.sockets;
                    FromData(s, item);
                    for (std::uint64_t m : s.magic) item.options.push_back({std::uint32_t(m & 0xFFFFFFFFu), std::uint32_t(m >> 32)});
                    if (item.durability == 0 && config.rules->items) {
                        if (const assets::ItemRow* r = config.rules->items->Find(item.refId); r && game::NewDurability(*r) > 0) {
                            item.durability = game::NewDurability(*r);
                            mended.push_back(s.slot);
                        }
                    }
                    player.inventory.places[s.slot] = std::move(item);
                }
                // The avatars it wears, in their places.
                for (const StoredItem& s : row->avatars) {
                    if (s.slot >= game::avatar::kPlaces) continue;
                    game::ItemInstance item;
                    item.serial = s.id;
                    item.refId = s.refId;
                    item.plus = s.plus;
                    item.variance = s.variance;
                    item.rentUntil = s.rentUntil;
                    item.bound = s.bound;
                    FromData(s, item);
                    for (std::uint64_t m : s.magic) item.options.push_back({std::uint32_t(m & 0xFFFFFFFFu), std::uint32_t(m >> 32)});
                    player.inventory.avatars[s.slot] = std::move(item);
                }
                TellJob(peer, player, true); // the state its suit gives (place 8 is in now)
                const auto figure = config.rules->creation.FigureOf(row->model);
                player.wearer = {row->level, figure && figure->race == game::CreationRules::Race::Europe ? 1 : 0, figure ? figure->male : true};
                // Its numbers, health and mana (one that left dead comes back at full health).
                player.level = row->level;
                player.maxLevel = std::max<int>(row->maxLevel, row->level);
                player.strength = row->strength;
                player.intelligence = row->intelligence;
                player.experience = row->experience;
                player.skillPoints = row->skillPoints;
                player.skillExperience = row->skillExperience;
                player.statPoints = row->statPoints;
                player.progressDirty = false;
                player.book = {};
                for (const auto& [mastery, level] : row->masteries) player.book.masteries[mastery] = level;
                for (std::uint32_t id : row->skills) {
                    const assets::SkillRow* skill = config.skills ? config.skills->Find(id) : nullptr;
                    if (skill == nullptr) continue;
                    int& level = player.book.groups[skill->group];
                    level = std::max(level, skill->level);
                }
                player.dead = false;
                player.hp = row->hp;
                player.mp = row->mp;
                UpdateStats(nullptr, player);
                // The long reuses it left running, with the time they have left; those that are over, forgotten.
                if (skillUnit && !row->longCooldowns.empty()) {
                    SkillCaster caster = CasterOf(player);
                    const std::int64_t unixNow = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    std::vector<std::uint32_t> over;
                    for (const CharacterRow::LongCooldown& c : row->longCooldowns) {
                        const assets::SkillRow* skill = config.skills ? config.skills->Find(c.skill) : nullptr;
                        const std::int64_t left = skill != nullptr ? c.started + skill->cooldownMs / 1000 - unixNow : 0;
                        if (left > 0) skillUnit->RestoreCooldown(caster, *skill, std::uint32_t(left * 1000), Clock::now());
                        else over.push_back(c.skill);
                    }
                    if (!over.empty()) database.Submit([character = player.selected, over = std::move(over)](Database& db) { DeleteLongCooldowns(db, character, over); });
                }
                if (skillUnit && !restoring.empty()) {
                    SkillCaster caster = CasterOf(player);
                    for (const CharacterRow::Buff& b : restoring) {
                        if (const assets::SkillRow* skill = config.skills ? config.skills->Find(b.skill) : nullptr) {
                            skillUnit->RestoreBuff(caster, *skill, std::uint32_t(std::min<std::uint64_t>(b.remainingMs, 0xFFFFFFFFull)), Clock::now());
                        }
                    }
                    UpdateStats(nullptr, player);
                }
                if (player.hp == 0) player.hp = player.stats.maxHp;
                world->SetVitals(player.entity, player.hp, player.stats.maxHp, player.mp, player.stats.maxMp);
                SendVitals(*peer, player);
                SendProgress(*peer, player);
                SendSkillBook(*peer, player);
                if (!mended.empty()) QueueItemSave(row->id, player.inventory, mended, {});
                proto::InventoryList list;
                list.size = player.inventory.size;
                list.gold = player.inventory.gold;
                for (const auto& [place, item] : player.inventory.places) list.items.push_back(player.inventory.Record(place));
                SendMessage(*peer->connection, list);
                proto::AvatarList avatars;
                for (const auto& [place, item] : player.inventory.avatars) avatars.items.push_back(player.inventory.AvatarRecord(place));
                SendMessage(*peer->connection, avatars);
                LoadCubes(peerId, player); // its magic cubes' items
                // Its pets left out (a teleport, a move from another game server): back out once it stands (Coses.h).
                if (coses && config.rules->items) {
                    std::vector<Coses::PetLink> pets;
                    for (const auto& [place, item] : player.inventory.places) {
                        const assets::ItemRow* r = item.durability != 0 ? config.rules->items->Find(item.refId) : nullptr;
                        if (r != nullptr && game::IsCosSummoner(*r)) pets.push_back({std::int64_t(item.durability), item.serial});
                    }
                    coses->Entered(player.entity, std::move(pets));
                }
                if (guilds) guilds->Entered(GuildActorOf(peerId, player));
                if (community) community->Entered(CommunityActorOf(peerId, player), player.arrived);
                if (academies) academies->Entered(AcademyActorOf(peerId, player), player.arrived);
                if (!player.arrived) LogChar(player, logs::CharEvent::Login); // (a move from another game server is none)
                if (openMarket) openMarket->Entered(peerId);
                player.deeds = row->deeds;
                if (quests) {
                    std::vector<QuestRecord> records;
                    for (const CharacterRow::Quest& q : row->quests) {
                        QuestRecord r;
                        r.quest = q.quest;
                        r.status = proto::QuestStatus(q.status == 0 ? std::uint8_t(proto::QuestStatus::Done) : q.status);
                        r.times = q.times;
                        r.started = q.started;
                        r.ended = q.ended;
                        r.progress = UnpackQuestProgress(q.data1, q.data2);
                        records.push_back(r);
                    }
                    quests->Enter(player.entity, std::move(records));
                }
                if (instances) instances->Enter(player.entity, row->id, row->book, row->instanceEntries, row->itemWaits);
                if (flagWorlds) flagWorlds->Enter(player.entity); // one coming over into its match
                if (arenas) arenas->Enter(player.entity);
                // Come into the temples' area without the right to be there (no job suit, ...): to its league's place
                // (FAILED_ENTER_WORLD_TO_SPECIAL_TARGET_*).
                if (temples) {
                    if (const auto at = world->Where(player.entity); at && at->position.region == 0x8010 && at->position.layer == 0) {
                        if (const auto to = temples->RefusedPlace(player.entity)) {
                            core::LogInfo("{}: {} may not stay in the temples' area: to region {}", config.name, player.name, to->region);
                            TravelTo(peerId, player, *to);
                        }
                    }
                }
                if (mall) mall->Entered(player.entity, peer->account); // its account's silk (SilkBalanceMessage)
                if (!player.inventory.avatars.empty()) ShowAvatars(player); // what it wears on the dress side, seen
                LoadServices(peerId, player); // the mall's time services, the premium
                if (player.arrived) {
                    // Come from another server of the shard: its party and that server hear of it.
                    player.arrived = false;
                    if (parties) parties->Arrived(player.entity);
                    if (!moveChannel.empty()) BusSend(moveChannel, std::to_string(player.selected));
                }
                if (fortresses) {
                    fortresses->Entered(FortressActorOf(peerId, player));
                    // A login inside a fortress's area at war: those who may not be there go out to its linked gate (ours, as
                    // the gates' entry check).
                    if (const auto at = world->Where(player.entity); at && !fortresses->MayEnter(player.selected, at->position.region)) {
                        fortresses->SendOut(FortressActorOf(peerId, player), at->position.region);
                    }
                }
                core::LogInfo("{}: {} enters the world (entity {})", config.name, row->name, player.entity);
                // The website's "Online now": the session's character (an arrival from another server too).
                database.Submit([account = peer->account, server = config.id, character = player.selected](Database& db) {
                    SetSessionCharacter(db, account, server, character);
                });
            });
        };
        // Its own saves still on their way (a logout a moment ago, the old connection's above): the load after them.
        AfterCharacterWrites(player.selected, [this, held, load = std::move(load)]() mutable {
            if (held) {
                database.SubmitBarrier(std::move(load));
            } else {
                database.Submit(std::move(load));
            }
        });
    }

    // Out of the world (back to the lobby, or gone): its place is saved.
    // `moving`: to another game server of the shard, not out of the game: it stays in its party and
    // nobody is told it left; where it arrives is saved by the move. What it takes along is what a
    // logout leaves in the database, as in the original (its server change carries only where to go): the scrolls'
    // buffs and the long reuses.
    void LeaveWorld(std::uint64_t peerId, bool reply, bool moving = false) {
        PlayerWorld& player = players[peerId];
        if (player.entity == 0) return;
        player.suitChange.reset(); // a job suit's change under way is given up (the suit stays where it was)
        if (trades) trades->Left(player.entity);
        // Its magic cubes' items are the database's again (another server may read them next).
        for (const auto& [place, item] : player.inventory.places) cubeItems.erase(item.serial);
        if (mall) mall->Left(player.entity);
        if (magicPop) magicPop->Left(player.entity);
        // The mall's services as they stand (their time runs on while away: the original's wall clock).
        if (!player.services.empty() || player.premium.item != 0 || player.growthUntil != 0 || !player.bonuses.empty()) SaveServices(player);
        player.services.clear();
        player.bonuses.clear(); // its bonus scrolls too (the next entry reads them back; another character must not keep them)
        player.premium = {};
        player.premiumBooth = false;
        player.growthUntil = 0;
        // Back in the lobby: the session plays no character (gone: its session goes; moving: the next server sets it).
        if (reply && !moving) {
            if (Peer* peer = peers->Find(peerId); peer != nullptr && peer->account != 0) {
                database.Submit([account = peer->account, server = config.id](Database& db) { SetSessionCharacter(db, account, server, 0); });
            }
        }
        // Its exchange ends first (the original's disconnect, 0x182B), nothing moved.
        if (exchanges) exchanges->End(player.entity);
        if (alchemy) alchemy->End(player.entity);
        if (stalls) stalls->Leave(player.entity); // its stall closes (its visitors hear 0x3C15)
        // Its pets stay Summoned in their rows: they come out again where it next enters, on the next server or after a
        // logout (the user's report 2026-11-14: summoned pets come back on the next login); its ride goes. Its transport's
        // row stays with its goods (a logout, a dropped connection, a move alike: the original's departure), made again
        // beside it at its next entry (Coses.h).
        if (coses) coses->DismissAll(player.entity, true, false, moving);
        if (quests) quests->Leave(player.entity);
        if (!moving) LogChar(player, logs::CharEvent::Logout);
        if (parties) {
            if (moving) parties->MovedAway(player.entity);
            else parties->LeaveWorld(player.entity); // out of its party while the members still know it
        }
        std::optional<CharacterPlace> leftAt;
        if (const auto place = moving ? std::nullopt : world->Where(player.entity)) {
            const proto::WorldPosition kept = KeptPlace(player.entity, place->position);
            leftAt = CharacterPlace{player.selected, {kept.region, kept.x, kept.y, kept.z, place->yaw}};
            // Dead in an ordinary world (not an instance, not a fortress war's death): kept at its return point, as the
            // original writes the town's recall position at the exit (it rises there, not where it fell).
            if (player.dead && place->position.layer == 0 && !player.warDeathAt) {
                if (const assets::TeleportPoint* to = ReturnArrival(player)) leftAt = CharacterPlace{player.selected, {to->region, to->x, to->y, to->z, place->yaw}};
            }
        }
        if (instances) instances->Leave(player.entity);
        if (flagWorlds) flagWorlds->Leave(player.entity);
        if (arenas) arenas->Leave(player.entity);
        // The buffs the original keeps as timed jobs (a scroll's: its skill has 'cbuf') keep the time they have left
        // until the character is back; the skills' own end with
        // the logout.
        std::vector<CharacterRow::Buff> kept = KeptBuffs(player);
        player.combat.buffs.clear();
        // Its saves as one task in the character's order, after everything submitted before it (a barrier: a SaveAll's
        // batch of places still running lands first): the place, the buffs kept, the vitals, the progress not saved yet and the gold
        // (the original's full save at the exit). The gold is the newest when its turn comes (a payment of its still
        // under way changes it).
        characterWrites[player.selected].gold = player.inventory.gold;
        CharacterTask(player.selected, [this, character = player.selected, leftAt, kept = std::move(kept), hp = player.hp, mp = player.mp,
                                        p = ProgressOf(player), dirty = player.progressDirty]() -> std::function<void(Database&)> {
            return [character, leftAt, kept, hp, mp, p, dirty, gold = characterWrites[character].gold](Database& db) {
                bool saved = !leftAt || SavePlaces(db, {*leftAt});
                saved = SaveBuffs(db, character, kept) && saved;
                saved = SaveVitals(db, character, hp, mp) && saved;
                saved = SaveLogout(db, character) && saved;
                if (dirty) saved = SaveProgress(db, {p}) && saved; // what changed since its last save (every other change is saved as it comes)
                saved = SaveGold(db, character, gold) && saved;
                if (!saved) core::LogWarning("the logout of character {}: a save did not land", character);
            };
        }, {}, true);
        player.progressDirty = false;
        CloseGuildStorage(peerId, player);
        // The guild's points not given yet, given now, dead or alive (the original's exit donates them with no test of life).
        if (guilds && player.guildPointsDue > 0) guilds->PointsEarned(player.selected, player.guildPointsDue);
        player.guildPointsDue = 0;
        if (guilds) {
            if (moving) guilds->MovedAway(player.selected);
            else guilds->Left(player.selected);
        }
        if (openMarket) openMarket->Left(peerId);
        if (academies) {
            if (moving) academies->MovedAway(player.selected);
            else academies->Left(player.selected);
        }
        if (community) {
            if (moving) community->MovedAway(player.selected);
            else community->Left(player.selected);
        }
        if (!labMonsters.empty()) DevRemove(player, 0); // the skill lab's monsters go with their player
        world->Leave(player.entity);
        player.entity = 0;
        player.combat.StopFighting();
        if (const auto it = byName.find(NameKey(player.name)); it != byName.end() && it->second == peerId) byName.erase(it);
        player.loggingOut = false;
        // Back to the lobby once its saves have landed (the original's LOBBYUSER_STATE_WAITFOR_DATA_SAVING): its next
        // entry reads what they wrote.
        if (reply) {
            AfterCharacterWrites(player.selected, [this, peerId] {
                if (Peer* peer = peers->Find(peerId)) SendMessage(*peer->connection, proto::WorldLeft{});
            });
        }
    }

    // Every player's place, now and then (one batch on a database thread).
    // The world's ticks since the last log (the bot benchmark, C6f).
    // The world's ticks since the last reading; the load sample's sum keeps them too.
    // Each reader (the log, the load sample) keeps its own total: one taking them must not cut the other's short.
    void TakeWorldStats() {
        const WorldStats w = world->TakeStats();
        sampleStats.Add(w);
        logStats.Add(w);
    }
    // A load sample for the site: the ticks since the last, the players online, the monsters alive.
    void SampleLoad() {
        nextSample = Clock::now() + config.sampleInterval;
        TakeWorldStats();
        ServerSample sample;
        sample.server = config.id;
        sample.ticks = std::uint32_t(sampleStats.ticks);
        sample.overBudget = std::uint32_t(sampleStats.overBudget);
        sample.tickAvgMs = sampleStats.ticks ? sampleStats.tickMsSum / double(sampleStats.ticks) : 0.0;
        sample.tickMaxMs = sampleStats.tickMsMax;
        sample.budgetMs = 1000.0 / world->TickRate();
        sample.players = std::uint32_t(online);
        sample.monsters = monsters ? std::uint32_t(monsters->Alive()) : 0u;
        sampleStats = {};
        database.Submit([sample](Database& db) { SaveServerSample(db, sample); });
    }

    void LogStats() {
        const double seconds = std::chrono::duration<double>(config.statsInterval).count();
        nextStats = Clock::now() + config.statsInterval;
        TakeWorldStats();
        const WorldStats w = std::exchange(logStats, {});
        if (w.entities == 0 && w.legs == 0) return; // nobody in the world: nothing to say
        const double budget = 1000.0 / world->TickRate();
        core::LogInfo("{}: world: {} online, {} walkers in up to {} zones; {} ticks ({} over the {:.0f} ms budget, {} dropped), tick "
                      "{:.2f} ms average, {:.2f} max; slowest zone step {:.2f} ms; who sees whom {:.2f} ms max; {:.0f} walks/s ({:.0f} "
                      "clicks/s refused, {:.1f} into another zone/s), {:.0f} messages/s; delivering {:.2f} ms max",
                      config.name, online, w.entities, w.zonesMax, w.ticks, w.overBudget, budget, w.dropped,
                      w.ticks ? w.tickMsSum / double(w.ticks) : 0.0, w.tickMsMax, w.zoneMsMax, w.interestMsMax, double(w.legs) / seconds,
                      double(w.refused) / seconds, double(w.zoneChanges) / seconds, double(w.messages) / seconds, deliverMsMax);
        if (w.refused > 0) {
            core::LogInfo("{}: world: {} clicks refused: {} too far, {} navigation not in, {} no floor under the walker, {} walks failed",
                          config.name, w.refused, w.refusedFar, w.refusedNavigation, w.refusedFloor, w.refusedWalk);
        }
        if (w.watcherTicks > 0) {
            const double n = double(w.watcherTicks), worked = double(std::max<std::uint64_t>(w.viewsWorkedOut, 1));
            core::LogInfo("{}: views: {:.0f} entries on average ({:.0f} players, {:.0f} monsters, {:.0f} items), {} at most; {:.0f} % worked out "
                          "again a tick; legs {:.0f}/s told at once, {:.0f}/s held for a farther tier's tick",
                          config.name, double(w.seen) / n, double(w.seenPlayers) / worked, double(w.seenMonsters) / worked,
                          double(w.seenItems) / worked, w.seenMax, 100.0 * double(w.viewsWorkedOut) / n, double(w.legsSent) / seconds,
                          double(w.legsDeferred) / seconds);
        }
        deliverMsMax = 0;
        if (monsters) core::LogInfo("{}: monsters: {} alive in {} awake nests of {}", config.name, monsters->Alive(), monsters->ActiveNests(), monsters->Nests());
    }

    void SaveAll() {
        std::vector<CharacterPlace> places;
        std::unordered_map<std::int64_t, std::uint32_t> entityOf; // a character in an instance is kept where it entered from
        for (const auto& [peerId, player] : players) {
            if (player.entity != 0) entityOf[player.selected] = player.entity;
        }
        world->ForEachPlayer([&](std::int64_t character, const World::Place& p) {
            const auto e = entityOf.find(character);
            const proto::WorldPosition kept = e != entityOf.end() ? KeptPlace(e->second, p.position) : p.position;
            if (kept.layer != 0) return;
            places.push_back({character, {kept.region, kept.x, kept.y, kept.z, p.yaw}});
        });
        // And the experience gained since the last save, each in its character's order; the health and mana with the places
        // (a crash keeps them as of the last save, not the last logout).
        struct Vitals {
            std::int64_t character = 0;
            std::uint32_t hp = 0, mp = 0;
        };
        std::vector<Vitals> vitals;
        for (auto& [peerId, player] : players) {
            if (player.entity == 0) continue;
            vitals.push_back({player.selected, player.hp, player.mp});
            if (!player.progressDirty) continue;
            QueueProgressSave(player);
            player.progressDirty = false;
        }
        saving = true;
        database.Submit([this, places = std::move(places), vitals = std::move(vitals)](Database& db) {
            if (!SavePlaces(db, places)) core::LogWarning("{}: saving {} places failed", config.name, places.size());
            for (const Vitals& v : vitals) SaveVitals(db, v.character, v.hp, v.mp);
            completions->Post([this] {
                saving = false;
                nextSave = Clock::now() + config.saveInterval;
            });
        });
    }

    // What the world's ticks say, to each player.
    // The world's messages of this pass: each player's handed to its connection at once (one lookup, one lock and one
    // write a player, not one a message: a crowd's tick is thousands of them). Its reliable ones first, in their order,
    // then its latest-lane ones (the moves), in theirs: the moves stand together, so the connection packs them into one
    // frame (Framing.h). That order changes nothing a client sees: a tick's moves are of what the player already saw (a
    // newcomer's place is in its appearance, one gone has no move). An entry's own follow-ups (EnteredWorld) right after it.
    void Deliver() {
        if (outbox.empty()) return;
        std::vector<std::uint32_t> order(outbox.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = std::uint32_t(i);
        std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return outbox[a].peer < outbox[b].peer; });
        std::vector<net::OutgoingMessage> batch, moves;
        const auto flush = [&](Peer& peer) {
            batch.insert(batch.end(), moves.begin(), moves.end());
            peer.connection->SendMany(batch);
            batch.clear();
            moves.clear();
        };
        for (std::size_t i = 0; i < order.size();) {
            const std::uint64_t peerId = outbox[order[i]].peer;
            std::size_t end = i;
            while (end < order.size() && outbox[order[end]].peer == peerId) ++end;
            if (Peer* peer = peers->Find(peerId)) {
                for (std::size_t k = i; k < end; ++k) {
                    const WorldMessage& m = outbox[order[k]];
                    (m.latestKey != 0 ? moves : batch).push_back({m.id, m.payload, m.latestKey});
                    if (m.id == proto::WorldEntered::kId) {
                        flush(*peer);
                        EnteredWorld(*peer);
                    }
                }
                flush(*peer);
            }
            i = end;
        }
        outbox.clear();
    }

    // What follows a player's entry (a teleport too) at once: an entry clears the client's buffs, so the ones running,
    // again; its berserk; its job's look.
    void EnteredWorld(Peer& peer) {
        const auto it = players.find(peer.id);
        if (it == players.end()) return;
        PlayerWorld& entered = it->second;
        if (skillUnit) {
            for (const auto& b : entered.combat.buffs) SendMessage(*peer.connection, skillUnit->BuffMessage(b, Clock::now()));
            // Its berserk goes on through a teleport: told again, as the buffs are.
            if (entered.berserkUntil) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*entered.berserkUntil - Clock::now()).count();
                if (left > 0) {
                    const proto::EntityBerserk mode{entered.entity, 1, game::BerserkVariant(entered.hwanLevel), std::uint32_t(left)};
                    SendMessage(*peer.connection, mode);
                    world->SetExtra(mode.entity, proto::EntityBerserk::kId, proto::Encode(mode)); // a new entity: for those who see it
                }
            }
        }
        // Those it appears to are told its buffs after its appearance (World::SetBuffs); a new entity, so nobody sees it
        // yet to be told now.
        // In a job's suit: its own "*Alias" and no guild line from the start (else its own name would show until a suit
        // change).
        if (entered.entity != 0 && entered.jobState != kNoJob) {
            SendMessage(*peer.connection, proto::EntityJob{entered.entity, entered.jobState, std::uint8_t(entered.job.level), entered.alias});
        }
        entered.toldBuffs.clear();
        for (const auto& b : entered.combat.buffs) entered.toldBuffs.push_back(b.skill);
        if (entered.entity != 0) world->SetBuffs(entered.entity, entered.toldBuffs);
    }

    void Closed(Peer& peer) {
        bugReports.erase(peer.id); // a report half sent goes with its connection
        const bool lingering = Linger(peer.id);
        std::int64_t character = 0;
        if (!lingering) {
            if (const auto it = players.find(peer.id); it != players.end()) character = it->second.selected;
            LeaveWorld(peer.id, false);
            players.erase(peer.id);
            accountOf.erase(peer.id);
        }
        if (peer.account == 0) return;
        --online;
        // The session is held while the character still stands in the world (a login elsewhere would load it twice), and
        // until its saves have landed (the next load reads them).
        if (lingering) {
            PlayerWorld& player = players[peer.id];
            player.heldAccount = peer.account;
            player.heldSession = peer.session;
            return;
        }
        ReleaseSessionAfter(character, peer.account, peer.session);
    }
    void ReleaseSessionAfter(std::int64_t character, std::int64_t account, std::int64_t token) {
        const auto release = [this, account, token] {
            database.Submit([account, id = config.id, token](Database& db) { ReleaseSession(db, account, id, token); });
        };
        if (character == 0) return release();
        AfterCharacterWrites(character, release);
    }
};

GameServer::GameServer() = default;
GameServer::~GameServer() { Stop(); }

bool GameServer::Start(const GameServerConfig& config, std::string& error) {
    Stop();
    if (!config.rules) {
        error = "the game server needs its character rules";
        return false;
    }
    if (!ValidSchemaName(config.shardSchema)) {
        error = "not a schema name: " + config.shardSchema;
        return false;
    }
    auto state = std::make_unique<State>();
    state->config = config;
#ifndef SR_DEV_COMMANDS
    // A release build: no developer commands, whatever the settings say (CMake SR_DEV_COMMANDS).
    if (state->config.devCommands) core::LogWarning("{}: dev-commands is set, but this build has no developer commands; it stays off", config.name);
    state->config.devCommands = false;
#endif
    // Names without a schema are this shard's first, then the accounts'.
    state->config.databaseUrl = WithSearchPath(config.databaseUrl, config.shardSchema + ", " + kAccountSchema);
    {
        auto db = Database::Connect(state->config.databaseUrl, error);
        if (!db) {
            error = "database: " + error;
            return false;
        }
        if (!MigrateSchema(*db, config.shardSchema, error)) return false;
        {
            // The weather's climates (the original's _RefClimate, _RefRegion.Climate), started on the shared clock.
            bool ok = false;
            ClimateData climates = LoadClimates(*db, ok);
            if (!ok) core::LogWarning("{}: the climates could not be read: the weather stays clear", config.name);
            const std::int64_t ms = state->WeatherClockMs();
            for (const game::ClimateRow& row : climates.climates) state->climates[row.id].Start(row, ms, 0x5EA50E1Bull);
            state->regionClimate = std::move(climates.regionClimate);
            core::LogInfo("{}: weather: {} climates over {} regions{}", config.name, state->climates.size(), state->regionClimate.size(),
                          config.weatherSpeed != 1.0 ? std::format(" ({}x speed)", config.weatherSpeed) : std::string());
        }
        if (config.layout) {
            // Every area of the game's table has one owner, or nobody would serve part of the world.
            bool ok = false;
            const auto regions = LoadRegionAreas(*db, ok);
            if (!ok) {
                error = "database: cannot read _refregion";
                return false;
            }
            std::vector<std::string> areas;
            for (const auto& [region, area] : regions) {
                if (std::find(areas.begin(), areas.end(), area) == areas.end()) areas.push_back(area);
            }
            if (areas.empty()) core::LogWarning("{}: the database has no _refregion: this server runs the whole world", config.name);
            if (!areas.empty() && !config.layout->Check(areas, error)) {
                error = "the shard's settings: " + error;
                return false;
            }
            for (const auto& [region, area] : regions) state->regionOwner[std::uint16_t(region)] = config.layout->OwnerOf(area);
            if (config.devCommands) {
                state->devNpcs = state->config.npcs;
                state->devNests = state->config.monsterNests;
            }
            // Only what stands in this server's part of the world.
            const auto mine = [&](std::uint16_t region) {
                const auto it = state->regionOwner.find(region);
                return it == state->regionOwner.end() || it->second == config.id;
            };
            std::erase_if(state->config.npcs, [&](const GameServerConfig::NpcSpawn& n) { return !mine(n.position.region); });
            std::erase_if(state->config.monsterNests, [&](const MonsterNest& n) { return !mine(n.position.region); });
            // Every fortress is held here, only those of its part run here.
            for (FortressSpec& f : state->config.fortresses) f.local = f.regions.empty() || mine(f.regions.front());
            std::int64_t spawns = 0;
            for (const MonsterNest& n : state->config.monsterNests) spawns += n.maxCount;
            core::LogInfo("{}: its part of the world: {} NPCs, {} monster nests for {} monsters, {} fortresses", config.name, state->config.npcs.size(),
                          state->config.monsterNests.size(), spawns,
                          std::count_if(state->config.fortresses.begin(), state->config.fortresses.end(), [](const FortressSpec& f) { return f.local; }));
        }
        ReleaseServerSessions(*db, config.id); // left by a previous run that did not stop cleanly
        // Its uniques' last word too: they went with that run (they come again as this one's hives hatch).
        ForgetServerUniques(*db, config.id, std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
        // The record kept short: a month, and each unique's latest and its last kill.
        PruneUniqueEvents(*db, 30);
    }
    state->LoadTextNames(); // the uniques' names (the website's) and the stall network's item names
    if (!state->database.Start(state->config.databaseUrl, config.databaseThreads, error)) {
        error = "database: " + error;
        return false;
    }
    state->net = std::make_unique<net::NetService>(config.networkThreads);
    state->completions = std::make_unique<Completions>(*state->net);
    state->listener = state->net->Listen(config.listen, error);
    if (!state->listener) return false;
    state->peers = std::make_unique<Peers>(config.name, config.peers);
    state->peers->OnClosed([s = state.get()](Peer& peer) { s->Closed(peer); });
    {
        // The first report before anyone is told about this server.
        auto db = Database::Connect(state->config.databaseUrl, error);
        if (!db || !ReportGameServer(*db, state->Info(), error)) {
            error = "database: " + error;
            return false;
        }
    }
    state->nextReport = Clock::now() + config.reportInterval;
    {
        // The first block of ids for new items.
        auto db = Database::Connect(state->config.databaseUrl, error);
        if (db) state->freeItemIds = ReserveItemIds(*db, 256);
    }
    if (config.jobs) {
        WorldConfig worldConfig = config.world;
        worldConfig.server = config.id; // its entity ids from its own range (World.h)
        state->world = std::make_unique<World>(worldConfig, *config.jobs, config.files);
        for (const GameServerConfig::NpcSpawn& n : state->config.npcs) state->npcs[state->world->AddNpc(n.refObjId, n.position, n.yaw, n.name)] = &n;
        if (!state->config.npcs.empty()) core::LogInfo("{}: {} NPCs stand in the world", config.name, state->config.npcs.size());
        state->skillUnit = std::make_unique<Skills>(*state, config.skills.get(), config.skillSeed != 0 ? config.skillSeed : std::random_device{}());
        if (config.skillSeed != 0) state->wearRandom.seed(config.skillSeed); // the wear's rolls repeat too (tests)
        state->parties = std::make_unique<Parties>(*state);
        state->community = std::make_unique<Community>(*state);
        state->academies = std::make_unique<Academies>(*state);
        if (state->config.openMarket) state->openMarket = std::make_unique<OpenMarket>(*state, OpenMarketConfig{state->config.openMarketFees});
        state->exchanges = std::make_unique<Exchanges>(*state);
        state->leagues = std::make_unique<Leagues>(*state);
        if (state->config.mall) state->mall = std::make_unique<Mall>(*state, state->config.mall);
        if (state->config.gacha) state->magicPop = std::make_unique<MagicPop>(*state, state->config.gacha);
        state->popDrawFixed = state->config.popDraw;
        state->trades = std::make_unique<Trades>(*state);
        state->alchemy = std::make_unique<Alchemy>(*state);
        if (state->config.gameLog) {
            const std::filesystem::path spill =
                state->config.gameLogSpill.empty() ? std::filesystem::path(std::format("gamelog-{}.spill", state->config.id)) : state->config.gameLogSpill;
            state->gameLog = std::make_unique<GameLog>(LogSchemaOf(state->config.shardSchema), spill);
        }
        if (state->config.drops) {
            for (int degree = 1; degree <= 12; ++degree) {
                game::alchemy::StoneBucket bucket{state->config.drops->Consumables("magicstone", degree - 1),
                                                  state->config.drops->Consumables("attrstone", degree - 1)};
                if (!bucket.magic.empty() || !bucket.attr.empty()) state->stoneBuckets[degree] = std::move(bucket);
            }
        }
        state->coses = std::make_unique<Coses>(*state);
        state->stalls = std::make_unique<Stalls>(*state);
        state->towns = config.towns;
        state->stallTables = config.stallNetwork;
        if ((!state->towns || !state->stallTables) && config.files) {
            std::string why;
            auto towns = std::make_shared<assets::RegionAreaTable>();
            auto tables = std::make_shared<assets::StallNetworkData>();
            if (towns->Load(*config.files, why) && tables->Load(*config.files, why)) {
                state->towns = std::move(towns);
                state->stallTables = std::move(tables);
            } else {
                core::LogWarning("{}: the stall network is off: {}", config.name, why);
            }
        }
        // Monsters with the game's navigation (an instance's nests come later), or nests, or fortresses.
        if (config.characters && (!state->config.monsterNests.empty() || !state->config.fortresses.empty() || config.files != nullptr)) {
            state->monsters = std::make_unique<Monsters>(*state->world, *config.characters, config.skills.get(), state->config.monsterNests, std::random_device{}(),
                                                         config.monsterResistances);
            core::LogInfo("{}: {} monster nests (they wake where players are)", config.name, state->monsters->Nests());
        }
        core::LogInfo("{}: world at {} ticks a second{}", config.name, config.world.tickRate,
                      config.files ? "" : " (no navigation: walks unchecked)");
    }
    {
        // Every guild and union, in memory from the start (Guilds.h).
        state->guilds = std::make_unique<Guilds>(*state);
        state->guilds->SetElectionSeconds(state->config.guildElectionSeconds);
        state->guilds->SetWarEndSeconds(state->config.guildWarEndSeconds);
        auto db = Database::Connect(state->config.databaseUrl, error);
        GuildData data;
        if (!db || !LoadGuilds(*db, data, error)) {
            error = "database: guilds: " + error;
            return false;
        }
        const std::size_t count = data.guilds.size(), unions = data.alliances.size();
        state->guilds->Load(std::move(data));
        core::LogInfo("{}: {} guilds, {} unions", config.name, count, unions);
        // The instance worlds' regions (game/Places.h): what each place allows.
        {
            std::vector<std::pair<std::uint16_t, std::string>> regions;
            if (!LoadWorldRegions(*db, regions, error)) {
                error = "database: the worlds' regions: " + error;
                return false;
            }
            for (const auto& [region, world] : regions) state->places.Add(region, game::PlaceKindOfWorld(world));
            if (state->places.Size() > 0) core::LogInfo("{}: {} regions of instance worlds", config.name, state->places.Size());
        }
        // The quests: sr_quests's definitions; the original's ones the client lists, a server's own all.
        {
            std::vector<StoredQuest> stored;
            std::vector<std::string> problems;
            if (!LoadQuests(*db, stored, problems, error)) {
                error = "database: the quests: " + error;
                return false;
            }
            for (const std::string& p : problems) core::LogWarning("{}: quest left out: {}", config.name, p);
            // The client's quests, and which of them lock when given up (questcontentsdata col 4: 0, the client's
            // "If you abandon this quest, you can't do it again later.").
            std::map<std::string, bool> client;
            if (config.files != nullptr) {
                assets::QuestTable table;
                std::string why;
                if (table.Load(*config.files, why))
                    for (const assets::QuestRow& row : table.Rows()) client[row.codename] = row.giveUpWarning == 0;
            }
            std::vector<game::QuestDef> defs;
            for (StoredQuest& s : stored) {
                const auto it = client.find(s.def.codename);
                if (s.origin == QuestOrigin::Script && it != client.end() && it->second) s.def.giveUpLocks = true;
                if (s.origin == QuestOrigin::Ours || client.empty() || it != client.end()) defs.push_back(std::move(s.def));
            }
            // The beta quests (game/BetaQuests.h), only on a shard that has them.
            if (config.betaQuests) {
                const std::string text = game::BetaQuestDefinitions();
                std::size_t at = text.find("quest ");
                while (at != std::string::npos) {
                    const std::size_t next = text.find("\nquest ", at);
                    game::QuestDef q;
                    std::string why;
                    if (game::ParseQuest(text.substr(at, next == std::string::npos ? std::string::npos : next + 1 - at), q, why)) defs.push_back(std::move(q));
                    else core::LogWarning("{}: a beta quest does not read: {}", config.name, why);
                    at = next == std::string::npos ? next : next + 1;
                }
                core::LogInfo("{}: the beta quests are on", config.name);
            }
            state->quests = std::make_unique<Quests>(*state, Quests::Rates{config.rates.experience, config.rates.skillExperience, config.rates.gold});
            const Quests::Loaded loaded = state->quests->Load(std::move(defs));
            if (loaded.offered + loaded.waiting > 0)
                core::LogInfo("{}: {} quests offered, {} waiting for what they need", config.name, loaded.offered, loaded.waiting);
        }
        // The two summon events the original always runs: the party summon scroll, the
        // Pandora box.
        if (state->monsters && config.characters) state->summons = std::make_unique<SummonEvents>(*state, std::random_device{}());
        // The Forgotten World: its worlds, their nests, doors and triggers, the holes and
        // the talismans, from the database's original tables and the client's files.
        if (state->monsters && config.files != nullptr && config.characters && config.rules->items) {
            auto catalog = std::make_shared<GameWorldCatalog>();
            std::vector<std::string> notes;
            if (LoadGameWorlds(*db, *config.files, *config.characters, *config.rules->items, *catalog, notes, error)) {
                for (const std::string& n : notes) core::LogWarning("{}: the Forgotten World: {}", config.name, n);
                std::size_t triggers = catalog->sharedTriggers.size();
                for (const GameWorldSpec& w : catalog->worlds) triggers += w.triggers.size();
                core::LogInfo("{}: the Forgotten World: {} worlds, {} holes, {} talismans, {} triggers", config.name, catalog->worlds.size(),
                              catalog->holes.size(), catalog->talismans.size(), triggers);
                std::shared_ptr<const GameWorldCatalog> kept = catalog;
                // The job temples (Temples.h): on the server holding their area, the reputations from the database.
                const bool templesHere = config.temples.enabled && !catalog->temples.empty() && state->OwnerOf(0x8010) == config.id;
                state->instances = std::make_unique<Instances>(*state, *state->world, *state->monsters, *config.characters, std::move(catalog),
                                                               std::random_device{}());
                if (templesHere) {
                    TempleConfig templeConfig = config.temples;
                    if (templeConfig.schedule.empty() && templeConfig.cyclePeriod == 0 && !LoadSchedules(*db, {9, 10, 11}, templeConfig.schedule, error)) {
                        core::LogWarning("{}: the temples' schedule: {}", config.name, error);
                        error.clear();
                    }
                    // `_AssociationReputation`: Anubis's and Haroeris's two each.
                    std::array<int, 4> reputation{};
                    const QueryResult r = db->Query("SELECT associationtypename, associationcodename, reputation FROM _associationreputation");
                    for (int i = 0; r.Ok() && i < r.Rows(); ++i) {
                        const std::string type = r.Text(i, 0), who = r.Text(i, 1);
                        const std::size_t at = (type == "TEMPLE_HAROERIS" ? 2 : 0) + (who == "Robber" ? 1 : 0);
                        if (type == "TEMPLE_ANUBIS" || type == "TEMPLE_HAROERIS") reputation[at] = int(r.Int(i, 2));
                    }
                    core::LogInfo("{}: the job temples: {} temples, {} schedule rows, reputations {} {} {} {}", config.name, kept->temples.size(),
                                  templeConfig.schedule.size(), reputation[0], reputation[1], reputation[2], reputation[3]);
                    state->temples = std::make_unique<Temples>(*state, *state->instances, *state->monsters, *state->world, kept, std::move(templeConfig),
                                                               reputation);
                }
            } else {
                core::LogWarning("{}: the Forgotten World is off: {}", config.name, error);
                error.clear();
            }
        }
        // Capture the Flag and Event So-Ok (FlagWorld.h, 55): every server runs the round; the event map's owner
        // the matches.
        if (state->world) {
            state->flagWorlds = std::make_unique<FlagWorlds>(*state, *state->world, state->monsters.get(), config.characters.get(), config.flagWorld,
                                                             std::random_device{}());
            // The Battle Arena (Arena.h): its worlds from the original's tables (or the test's), its schedule.
            ArenaConfig arenaConfig = config.arena;
            if (arenaConfig.worlds.empty() && !LoadArenaWorlds(*db, config.characters.get(), arenaConfig.worlds, error)) {
                core::LogWarning("{}: the arena's worlds: {}", config.name, error);
                error.clear();
            }
            if (arenaConfig.schedule.empty() && !LoadSchedules(*db, {16, 17, 18, 19, 20, 21, 22, 23}, arenaConfig.schedule, error)) {
                core::LogWarning("{}: the arena's schedule: {}", config.name, error);
                error.clear();
            }
            core::LogInfo("{}: the Battle Arena: {} worlds, {} schedule rows{}", config.name, arenaConfig.worlds.size(), arenaConfig.schedule.size(),
                          arenaConfig.enabled ? "" : " (off: event-arena)");
            state->arenas = std::make_unique<Arenas>(*state, *state, *state->world, state->monsters.get(), config.characters.get(), std::move(arenaConfig),
                                                     std::random_device{}());
        }
        // The Roc (Roc.h): on the server holding the Peak, its schedule SCHEDULE_DEF_MEET_ROC.
        if (state->instances && state->monsters && config.characters && config.roc.enabled && state->OwnerOf(26045) == config.id) {
            RocConfig rocConfig = config.roc;
            if (rocConfig.schedule.empty() && rocConfig.cycleOpen == 0 && !LoadSchedules(*db, {1}, rocConfig.schedule, error)) {
                core::LogWarning("{}: the Roc's schedule: {}", config.name, error);
                error.clear();
            }
            core::LogInfo("{}: the Roc: the Peak is here ({} schedule rows{})", config.name, rocConfig.schedule.size(),
                          rocConfig.cycleOpen > 0 ? std::format(", ours: the gate {} s every {} s", rocConfig.cycleOpen, rocConfig.cyclePeriod) : std::string());
            state->roc = std::make_unique<Roc>(*state, *state->instances, *state->monsters, *config.characters, config.skills.get(), std::move(rocConfig),
                                               std::random_device{}());
        }
        // The Qin-Shi tomb's snake gate (QinTomb.h): on the server holding the 4th floor, its schedule define 2.
        if (state->instances && state->monsters && config.characters && config.qinTomb.enabled && state->OwnerOf(QinTomb::kFloor4) == config.id) {
            QinTombConfig qinConfig = config.qinTomb;
            if (qinConfig.schedule.empty() && qinConfig.cycleOpen == 0 && !LoadSchedules(*db, {2}, qinConfig.schedule, error)) {
                core::LogWarning("{}: the Qin-Shi tomb's schedule: {}", config.name, error);
                error.clear();
            }
            if (state->OwnerOf(QinTomb::kFloor5) != config.id || state->OwnerOf(QinTomb::kFloor6) != config.id) {
                core::LogWarning("{}: the Qin-Shi tomb's upper floors are another server's area: their copies are this one's", config.name);
            }
            core::LogInfo("{}: the Qin-Shi tomb: the snake gate is here ({} schedule rows{})", config.name, qinConfig.schedule.size(),
                          qinConfig.cycleOpen > 0 ? std::format(", ours: the gate {} s every {} s", qinConfig.cycleOpen, qinConfig.cyclePeriod) : std::string());
            state->qinTomb = std::make_unique<QinTomb>(*state, *state->instances, *state->monsters, *config.characters, std::move(qinConfig),
                                                       std::random_device{}());
        }
        // The reverse return scroll's map points.
        {
            std::vector<OptionalTeleportRow> points;
            if (LoadOptionalTeleports(*db, points, error)) {
                for (const OptionalTeleportRow& p : points) state->optionalTeleports[p.id] = p;
            } else {
                core::LogWarning("{}: the reverse return scroll's map points: {}", config.name, error);
                error.clear();
            }
        }
        // The jobs' constants.
        // The specialty NPCs' markets of the NPCs that stand on this server, and the transports' trade bags.
        if (state->trades) {
            std::vector<std::uint32_t> here;
            for (const GameServerConfig::NpcSpawn& n : state->config.npcs) here.push_back(n.refObjId);
            std::vector<game::MarketRow> rows;
            if (!LoadMarkets(*db, here, rows, error)) {
                core::LogWarning("{}: the markets: {}", config.name, error);
                error.clear();
            } else {
                state->trades->AddMarkets(std::move(rows));
                core::LogInfo("{}: {} specialty markets", config.name, state->trades->Markets());
            }
            // The objects whose deals a fortress taxes (their AssocFile1_128 names it).
            std::vector<std::pair<std::uint32_t, std::uint32_t>> taxed;
            if (!LoadTaxedObjects(*db, taxed, error)) {
                core::LogWarning("{}: the taxed objects: {}", config.name, error);
                error.clear();
            }
            for (const auto& [object, fortress] : taxed) state->taxedBy[object] = fortress;
            QueryResult bags = db->Query("SELECT o.id, ch.inventorysize FROM _refobjcommon o JOIN _refobjchar ch ON ch.id = o.link WHERE o.codename128 LIKE 'COS_T_%'");
            for (int k = 0; bags.Ok() && k < bags.Rows(); ++k) state->transportBags[std::uint32_t(bags.Int(k, 0))] = int(bags.Int(k, 1));
        }
        {
            // The bandits: their monsters by zone and level, the regions' towns and continents.
            game::BanditTable table;
            if (!LoadBanditData(*db, table, state->regionKinds, error)) {
                core::LogWarning("{}: the bandits' data: {}", config.name, error);
                error.clear();
            }
            state->bandits = std::make_unique<Bandits>(*state, std::move(table), std::random_device{}());
            state->bandits->SetInterval(config.banditMinSeconds, config.banditSpanSeconds);
            if (state->monsters) {
                state->monsters->SetTowns([s = state.get()](std::uint16_t region) { return s->BanditTown(region); });
                state->monsters->SetTownAreas([s = state.get()](const proto::WorldPosition& at) { return s->InTownArea(at); });
            }
        }
        // The summons' own spots (`_RefGameWorldNPC`'s INS_DEFAULT rows: the gods' loot bodies).
        if (state->monsters) { // (no such table: no rows)
            const QueryResult rows = db->Query("SELECT npccodename128, regionid, posx, posy, posz FROM _refgameworldnpc WHERE worldcodename128 = 'INS_DEFAULT'");
            std::unordered_map<std::string, proto::WorldPosition> places;
            for (int k = 0; rows.Ok() && k < rows.Rows(); ++k) {
                places[rows.Text(k, 0)] = proto::WorldPosition{std::uint16_t(std::int16_t(rows.Int(k, 1))), float(rows.Double(k, 2)), float(rows.Double(k, 3)),
                                                               float(rows.Double(k, 4))};
            }
            state->monsters->SetSummonPlaces(std::move(places));
            // The uniques' conditional skills (Tab_RefAISkill), by their tactics' monster.
            const QueryResult ai = db->Query(
                "SELECT t.dwobjid, a.skillcodename, a.excuteconditiontype, a.excuteconditiondata, a.\"option\" FROM tab_refaiskill a "
                "JOIN tab_reftactics t ON t.dwtacticsid = a.tacticsid ORDER BY t.dwobjid, a.tacticsid");
            std::unordered_map<std::uint32_t, std::vector<Monsters::ConditionalSkill>> conditional;
            std::set<std::pair<std::uint32_t, std::string>> seen;
            for (int k = 0; ai.Ok() && k < ai.Rows(); ++k) {
                const std::uint32_t obj = std::uint32_t(ai.Int(k, 0));
                std::string code = ai.Text(k, 1);
                while (!code.empty() && std::isspace(static_cast<unsigned char>(code.back()))) code.pop_back();
                if (!seen.insert({obj, code}).second) continue; // a monster's tactics rows share them (its champion's)
                conditional[obj].push_back({code, int(ai.Int(k, 2)), std::uint32_t(std::max<std::int64_t>(0, ai.Int(k, 3))), int(ai.Int(k, 4))});
            }
            state->monsters->SetConditionalSkills(std::move(conditional));
        }
        if (!LoadJobRules(*db, state->jobRules, error)) {
            core::LogWarning("{}: the jobs' constants: {}", config.name, error);
            error.clear();
        } else {
            core::LogInfo("{}: the jobs' constants: scale {} / {} / {} / {}, factor {}, {} gold levels", config.name, state->jobRules.scale[0],
                          state->jobRules.scale[1], state->jobRules.scale[2], state->jobRules.scale[3], state->jobRules.monsterFactor,
                          state->jobRules.goldMin.size());
        }
        // The guild storages this server held when it last stopped are free again.
        ReleaseServerGuildStorageLocks(*db, config.id);
        if (config.shardBus) {
            // The shard's other game servers: their guild changes, who of theirs is online.
            state->busChannel = "sr_guilds_" + config.shardSchema;
            state->busListener = Database::Connect(state->config.databaseUrl, error);
            if (!state->busListener || !state->busListener->Listen(state->busChannel)) {
                error = "database: the shard's bus: " + (state->busListener ? std::string("LISTEN refused") : error);
                return false;
            }
            state->guilds->Hello();
            // Its parties and chat, on channels of their own.
            state->partyChannel = "sr_party_" + config.shardSchema;
            state->chatChannel = "sr_chat_" + config.shardSchema;
            state->communityChannel = "sr_community_" + config.shardSchema;
            state->academyChannel = "sr_academy_" + config.shardSchema;
            if (state->openMarket) state->openMarketChannel = "sr_openmarket_" + config.shardSchema;
            state->moveChannel = "sr_moves_" + config.shardSchema;
            if (state->instances) state->instanceChannel = "sr_instances_" + config.shardSchema;
            state->adminChannel = "sr_admin_" + config.shardSchema;
            state->uniqueChannel = "sr_uniques_" + config.shardSchema;
            state->rocChannel = "sr_roc_" + config.shardSchema;
            state->clockChannel = "sr_clock_" + config.shardSchema;
            state->qinChannel = "sr_qin_" + config.shardSchema;
            state->templeChannel = "sr_temples_" + config.shardSchema;
            if (state->stalls) state->stallChannel = "sr_stalls_" + config.shardSchema;
            if (state->flagWorlds) {
                state->eventChannel = "sr_events_" + config.shardSchema;
                state->arenaChannel = "sr_arena_" + config.shardSchema;
                if (!state->busListener->Listen(state->eventChannel) || !state->busListener->Listen(state->arenaChannel)) {
                    error = "database: the shard's bus: LISTEN refused";
                    return false;
                }
            }
            if (!state->busListener->Listen(state->partyChannel) || !state->busListener->Listen(state->chatChannel) ||
                !state->busListener->Listen(state->communityChannel) || !state->busListener->Listen(state->academyChannel) || !state->busListener->Listen(state->moveChannel) ||
                (!state->stallChannel.empty() && !state->busListener->Listen(state->stallChannel)) ||
                (!state->openMarketChannel.empty() && !state->busListener->Listen(state->openMarketChannel)) ||
                (!state->instanceChannel.empty() && !state->busListener->Listen(state->instanceChannel)) ||
                !state->busListener->Listen(state->adminChannel) || !state->busListener->Listen(state->uniqueChannel) ||
                !state->busListener->Listen(state->rocChannel) || !state->busListener->Listen(state->templeChannel) ||
                !state->busListener->Listen(state->clockChannel) ||
                !state->busListener->Listen(state->qinChannel)) {
                error = "database: the shard's bus: LISTEN refused";
                return false;
            }
            if (state->parties) state->parties->Hello();
            if (state->community) state->community->Hello();
            if (state->academies) state->academies->Hello();
            // The website's commands of the last minute not done yet (a server starting takes those, never older ones).
            state->database.Submit([s = state.get()](Database& db) {
                bool ok = false;
                std::vector<AdminCommand> recent = RecentAdminCommands(db, s->config.shardSchema, s->config.id, 60, ok);
                for (AdminCommand& c : recent) {
                    auto block = State::BlockOf(db, c);
                    s->completions->Post([s, c = std::move(c), block = std::move(block)] { s->RunAdmin(c, block); });
                }
            });
            if (state->stalls) state->stalls->Hello();
            // The accounts' silk (SilkStore.h): every shard's game servers hear it.
            if (state->magicPop) {
                state->popChannel = "sr_mall_" + config.shardSchema;
                if (!state->busListener->Listen(state->popChannel)) {
                    error = "database: the shard's bus: LISTEN refused";
                    return false;
                }
            }
            if (state->mall) {
                state->silkChannel = SilkChannel();
                if (!state->busListener->Listen(state->silkChannel)) {
                    error = "database: the shard's bus: LISTEN refused";
                    return false;
                }
            }
            if (!state->config.fortresses.empty()) {
                state->fortressChannel = "sr_fortress_" + config.shardSchema;
                if (!state->busListener->Listen(state->fortressChannel)) {
                    error = "database: the shard's bus: LISTEN refused";
                    return false;
                }
            }
        }
    }
    if (!state->config.fortresses.empty() && state->world) {
        // The fortresses (Fortress.h): their state from the database, their structures in the world.
        auto db = Database::Connect(state->config.databaseUrl, error);
        FortressState data;
        if (!db || !LoadFortressState(*db, data, error)) {
            error = "database: fortresses: " + error;
            return false;
        }
        // The periods are kept by the server that runs the fortress of the lowest id; the others follow it.
        FortressConfig fortressConfig = config.fortress;
        const auto lowest = std::min_element(state->config.fortresses.begin(), state->config.fortresses.end(),
                                             [](const FortressSpec& a, const FortressSpec& b) { return a.id < b.id; });
        fortressConfig.periodKeeper = lowest->local;
        state->fortresses = std::make_unique<Fortresses>(*state, state->config.fortresses, std::move(fortressConfig));
        state->fortresses->Load(std::move(data));
        state->fortresses->Hello();
        core::LogInfo("{}: {} fortresses", config.name, state->fortresses->Count());
    }
    state->nextSave = Clock::now() + config.saveInterval;
    state->nextStats = Clock::now() + config.statsInterval;
    state->nextSample = Clock::now() + config.sampleInterval;
    core::LogInfo("{}: server {} '{}' listening on {}:{}", config.name, config.id, config.serverName, config.listen.address,
                  state->listener->Port());
    state_ = std::move(state);
    return true;
}

void GameServer::Poll(std::chrono::milliseconds wait) {
    if (!state_) return;
    SR_ZONE("GameServer::Poll");
    State& s = *state_;
    if (s.world) {
        // Woken for the next tick at the latest.
        const auto untilTick = std::chrono::duration_cast<std::chrono::milliseconds>(s.world->NextTick() - WorldClock::now());
        wait = std::clamp(untilTick, std::chrono::milliseconds(0), wait);
    }
    s.net->WaitForActivity(wait);
    s.completions->Run();
    if (!s.reporting && Clock::now() >= s.nextReport) s.Report();
    s.peers->Pump(*s.listener, [&](Peer& peer, const net::Message& m) { s.Handle(peer, m); });
    if (!s.loggingOut.empty()) s.FinishLogouts();
    if (s.busListener && s.guilds) {
        for (const Database::Notification& n : s.busListener->TakeNotifications()) {
            if (n.channel == s.busChannel) s.guilds->BusMessage(n.payload);
            if (n.channel == s.partyChannel && s.parties) s.parties->BusMessage(n.payload, Clock::now());
            if (n.channel == s.chatChannel) s.ChatBusMessage(n.payload);
            if (n.channel == s.communityChannel && s.community) s.community->BusMessage(n.payload, Clock::now());
            if (n.channel == s.academyChannel && s.academies) s.academies->BusMessage(n.payload);
            if (n.channel == s.moveChannel) s.movedAway.erase(std::atoll(n.payload.c_str())); // it has arrived
            if (n.channel == s.stallChannel && s.stalls) s.stalls->BusMessage(n.payload, Clock::now());
            if (n.channel == s.openMarketChannel && s.openMarket) s.openMarket->BusMessage(n.payload);
            if (n.channel == s.instanceChannel && s.instances) s.instances->BusMessage(n.payload);
            if (n.channel == s.eventChannel && s.flagWorlds) s.flagWorlds->BusMessage(n.payload);
            if (n.channel == s.arenaChannel && s.arenas) s.arenas->BusMessage(n.payload);
            if (n.channel == s.adminChannel) s.AdminNotice(n.payload);
            if (n.channel == s.uniqueChannel) s.UniqueBusMessage(n.payload);
            if (n.channel == s.rocChannel) s.RocBusMessage(n.payload);
            if (n.channel == s.clockChannel) s.SetGameHour(std::atoi(n.payload.c_str()));
            if (n.channel == s.qinChannel) s.QinBusMessage(n.payload);
            if (n.channel == s.templeChannel) s.TempleBusMessage(n.payload);
            if (n.channel == s.fortressChannel && s.fortresses) s.fortresses->BusMessage(n.payload);
            if (n.channel == s.silkChannel && s.mall) s.mall->BusMessage(n.payload);
            if (n.channel == s.popChannel) {
                if (const auto a = MagicPop::DecodeAnnounce(n.payload)) s.PopAnnounceHere(*a);
            }
        }
    }
    if (!s.whispers.empty()) s.FinishWhispers(Clock::now());
    if (!s.moves.empty() || !s.movedAway.empty()) s.FinishMoves(Clock::now());
    // The game's own turn (the features' timers, the monsters, the fights, the players' passes): once a world tick,
    // not on every wake. A wake comes with every message a player sends, hundreds a second with many players, and each
    // pass walks every player; the messages themselves are still taken as they come.
    const bool turn = !s.world || WorldClock::now() >= s.world->NextTick();
    if (turn) {
        if (s.guilds) s.guilds->Tick(Clock::now());
        if (s.community) s.community->Tick(Clock::now());
        if (s.academies) s.academies->Tick(Clock::now());
        if (s.exchanges) s.exchanges->Tick(Clock::now());
        if (s.alchemy) s.alchemy->Tick(Clock::now());
        if (s.coses) s.coses->Tick(Clock::now());
        if (s.stalls) s.stalls->Tick(Clock::now());
        if (s.openMarket) s.openMarket->Tick(Clock::now());
        if (s.quests && Clock::now() >= s.nextQuestTick) {
            s.nextQuestTick = Clock::now() + std::chrono::seconds(1);
            s.quests->Tick(Clock::now());
            s.FollowQuestCoses();
        }
        if (s.summons) s.summons->Tick(Clock::now());
        if (s.instances) s.instances->Tick(Clock::now());
        if (s.flagWorlds) s.flagWorlds->Tick(Clock::now());
        if (s.arenas) s.arenas->Tick(Clock::now());
        if (s.roc) s.roc->Tick(Clock::now());
        if (s.qinTomb) s.qinTomb->Tick(Clock::now());
        if (s.temples) s.temples->Tick(Clock::now());
        s.TempleTick(Clock::now());
        if (s.fortresses) {
            s.fortresses->Tick(Clock::now(), std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        }
    }
    // The new items' ids kept in stock whatever takes them (a give, a quest's reward, a pickup, a purchase): not only
    // after an item's move (a bag that only fills ran it dry: every give refused, every pickup left on the ground).
    s.TopUpItemIds();
    if (s.world) {
        s.FinishReturns();
        s.FinishPickups();
    }
    if (s.world && turn) {
        s.ExpireGround();
        // The monsters' turn, then the players' fight (their hits, buffs and casts land on it, monsters or not).
        const auto now = Clock::now();
        if (s.monsters) s.MonsterTurn(now);
        s.Berserks(now);
        s.FightPlayers(now);
        s.StepPotions(Clock::now());
        s.TickPlayerStates(Clock::now());
        s.TickServices(Clock::now());
        s.TickSpirits(Clock::now());
        s.AutoPotions(Clock::now());
        s.Recover(Clock::now());
        s.Motions(Clock::now());
        s.Traces(Clock::now());
        s.FreePvps(Clock::now());
        s.TickSuitChanges(Clock::now());
        if (s.trades) s.trades->Tick(Clock::now());
        if (s.bandits) s.bandits->Tick(Clock::now());
        s.TickTax(Clock::now());
        s.TickBargain(Clock::now());
        s.TickWeather(Clock::now());
        s.TickClock(Clock::now());
        s.TickPvp(Clock::now());
        s.parties->Tick(Clock::now());
    }
    if (s.world) {
        s.world->Update(WorldClock::now(), s.outbox);
        for (const World::ZoneEntered& z : s.world->TakeZonesEntered()) s.EnterZone(z);
        const auto deliver = Clock::now();
        s.Deliver();
        s.deliverMsMax = std::max(s.deliverMsMax, std::chrono::duration<double, std::milli>(Clock::now() - deliver).count());
        if (!s.saving && Clock::now() >= s.nextSave) s.SaveAll();
        if (s.config.statsInterval.count() > 0 && Clock::now() >= s.nextStats) s.LogStats();
        if (s.config.sampleInterval.count() > 0 && Clock::now() >= s.nextSample) s.SampleLoad();
        s.FlushLog();
    }
}

void GameServer::Run(const std::atomic<bool>& stop) {
    while (!stop) Poll(std::chrono::milliseconds(100));
}

void GameServer::Stop() {
    if (!state_) return;
    State& s = *state_;
    if (s.world) s.SaveAll();  // every place, before the sessions go
    s.peers->CloseAll(); // queues each session's release (and each player's place)
    s.listener->Close();
    s.FlushLog(true); // the logs queued so far
    // The other servers hear of it at once, not when its silence has gone on long enough.
    const std::string goodbye = s.parties ? s.parties->GoodbyeText() : std::string();
    const std::string communityGoodbye = s.community ? s.community->GoodbyeText() : std::string();
    const std::string academyGoodbye = s.academies ? s.academies->GoodbyeText() : std::string();
    const std::string stallGoodbye = s.stalls ? s.stalls->GoodbyeText() : std::string();
    // After every job before it (the pool runs them on several connections): a report still on its way must not list it again.
    s.database.SubmitBarrier([id = s.config.id, channel = s.partyChannel, goodbye, communityChannel = s.communityChannel, communityGoodbye,
                              stallChannel = s.stallChannel, stallGoodbye, academyChannel = s.academyChannel, academyGoodbye](Database& db) {
        if (!academyGoodbye.empty()) Notify(db, academyChannel, academyGoodbye);
        if (!goodbye.empty()) Notify(db, channel, goodbye);
        if (!communityGoodbye.empty()) Notify(db, communityChannel, communityGoodbye);
        if (!stallGoodbye.empty()) Notify(db, stallChannel, stallGoodbye);
        StopGameServer(db, id);
        ReleaseServerSessions(db, id);
        ForgetServerUniques(db, id, std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
    });
    s.database.Stop(); // runs what is queued, then joins
    state_.reset();
}

std::uint16_t GameServer::Port() const { return state_ ? state_->listener->Port() : 0; }
std::size_t GameServer::Online() const { return state_ ? state_->online : 0; }
const World* GameServer::GetWorld() const { return state_ ? state_->world.get() : nullptr; }
const Guilds* GameServer::GetGuilds() const { return state_ ? state_->guilds.get() : nullptr; }
const Fortresses* GameServer::GetFortresses() const { return state_ ? state_->fortresses.get() : nullptr; }
Mall* GameServer::GetMall() { return state_ ? state_->mall.get() : nullptr; }

} // namespace sr::server
