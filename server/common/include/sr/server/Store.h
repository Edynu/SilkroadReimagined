#pragma once

// What the servers keep in PostgreSQL, in the original server's tables: accounts (sro_vt_account.tb_user and our
// sr_account_security), the shard list
// (_shard and our sr_shard_reports), tickets and sessions (ours), notices (_notice), blocks
// (_blockeduser, _punishment); a shard's characters (_char, _user, ...), items (_items, _inventory,
// _itempool), storage (_chest, _chestinfo, _accountjid), skills (_charskill, _charskillmastery),
// quick slots and the auto potion (_clientconfig, our sr_auto_potion). The shard's tables are found through the connection's search path
// (its shard schema, then the account's). Each call is one short statement or transaction on the
// given connection (run them on a DatabasePool thread).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sr/game/Items.h"
#include "sr/game/Jobs.h"
#include "sr/game/Potions.h"
#include "sr/game/Weather.h"
#include "sr/server/Database.h"
#include "sr/server/InstanceStore.h"

namespace sr::server {

// --- Accounts ---------------------------------------------------------------------------------

// 3-25 ASCII letters, digits or '_' (tb_user.struserid is 25 long; unique ignoring case).
bool ValidAccountName(std::string_view name);
// 6-64 bytes.
bool ValidPassword(std::string_view password);

enum class CreateAccountResult : std::uint8_t { Created, NameTaken, InvalidName, InvalidPassword, KeyRefused, Failed };
// What goes in tb_user.password beside our hash: the original's MD5, so the
// owner's web tools keep working (weak), or nothing.
enum class LegacyPasswords : std::uint8_t { Keep, Clear };
// What the website's registration adds (SroWeb): the e-mail (tb_user.email, may be empty), the address it
// came from (tb_user.reg_ip), and a beta key used up in the same transaction (KeyRefused when it is not
// one, or was used already).
struct NewAccountDetails {
    std::string email;
    std::string address;
    std::optional<std::vector<std::uint8_t>> betaKeyHash; // BetaKeyHash()
};
// Hashes the password (Argon2id) on the calling thread.
CreateAccountResult CreateAccount(Database& db, std::string_view name, std::string_view password, std::string& error,
                                  LegacyPasswords legacy = LegacyPasswords::Keep, const NewAccountDetails& details = {});
// An e-mail address the registration takes: 6-50 characters, one '@', a dot after it, no spaces.
bool ValidEmail(std::string_view email);

// Beta keys (sr_beta_keys): `count` new ones, each four groups of four letters and digits (XXXX-XXXX-XXXX-XXXX);
// only their hashes are stored, so they are shown once. Empty on failure.
std::vector<std::string> MakeBetaKeys(Database& db, int count, std::string_view note, std::string& error);
// The stored form of a key as a player types it (case, spaces and dashes do not matter).
std::vector<std::uint8_t> BetaKeyHash(std::string_view key);

struct AccountRow {
    std::int64_t id = 0;
    std::string name;
    std::string passwordHash; // ours (Argon2id, PHC); empty: the account has only the original's
    std::string legacyHash;   // tb_user.password (the original's MD5), maybe empty
    int failedLogins = 0;
    bool blocked = false;
    std::int64_t blockedUntil = 0; // Unix seconds; 0 with `blocked`: no end
    std::string blockReason;
};

// Nothing when there is no such account; `ok` is false when the query failed.
std::optional<AccountRow> FindAccount(Database& db, std::string_view name, bool& ok);

// A wrong password: counts it; at `limit` in a row the account is blocked for `blockSeconds`
// (and the count starts over). Returns the count after this one (`limit` when it blocked).
int RecordWrongPassword(Database& db, std::int64_t account, int limit, int blockSeconds, const std::string& reason);
void RecordLogin(Database& db, std::int64_t account);
// Our hash for an account that logged in with the original's (and tb_user.password kept or cleared).
bool StorePasswordHash(Database& db, std::int64_t account, const std::string& phc, LegacyPasswords legacy);
// The account's login block now: until when (Unix seconds, 0: no end) and why; none when not blocked.
std::optional<std::pair<std::int64_t, std::string>> AccountBlock(Database& db, std::int64_t account);
// Blocks an account (`seconds` 0: no end); an empty reason unblocks it.
bool SetBlock(Database& db, std::string_view name, std::string_view reason, std::int64_t seconds);

// --- The website's rankings (SroWeb) --------------------------------------------------------
// The top `count` of a shard (the connection's search path): characters by level then experience, guilds
// by level then gathered SP, the three jobs by job level then job experience. Game masters (accounts with
// sec_primary below 3, characters named "[GM]..."), the bot benchmark's accounts (bot<N>) and deleted
// characters are left out.
struct RankingRow {
    std::string name;
    int value = 0;      // level, guild level, job level
    std::string detail; // a character's guild (else its race), a guild's master, the job's name
};
struct Rankings {
    std::vector<RankingRow> players, guilds, jobs;
    std::vector<RankingRow> uniqueKills; // the most uniques killed (sr_unique_events); detail: the last one
};
Rankings LoadRankings(Database& db, int count, bool& ok);
// The shard's numbers for the website: accounts, characters, guilds, uniques killed (in all and the last day).
struct ShardStats {
    std::int64_t accounts = 0, characters = 0, guilds = 0, uniquesKilled = 0, uniquesKilledToday = 0;
};
ShardStats LoadShardStats(Database& db, bool& ok);

// --- The unique monsters (sr_unique_events, the shard's) -----------------------------------------
// What a game server says of the uniques of its world (Monsters::Events::uniques, a unique's kill): its coming, its
// death with its killer, its going with the game server. Times are the game server's (Unix seconds), so the order
// holds whichever database thread writes first.
struct UniqueEvent {
    enum class Kind : std::uint8_t { Appeared = 1, Killed = 2, Gone = 3 };
    Kind kind = Kind::Appeared;
    std::uint32_t refObjId = 0;
    std::string codename, name; // name: the client's text (TextData_Object), else the codename
    std::uint16_t server = 0;
    std::uint16_t region = 0;
    float x = 0, z = 0;
    std::int64_t killerId = 0; // a kill: the character of the killing hit
    std::string killerName;
    double at = 0;
};
bool RecordUniqueEvent(Database& db, const UniqueEvent& event);
// A game server starting or stopping: the uniques it last said were there went with it (a Gone each).
bool ForgetServerUniques(Database& db, std::uint16_t server, double at);
// The record kept short (a game server starting): events older than `days` go, but never a unique's latest nor
// its last kill (what the website shows). The rows removed, or -1.
int PruneUniqueEvents(Database& db, int days);
// Each unique's latest: alive or not since when, and its last killer. Alive only while its game server reports (one
// gone without saying so leaves its last word "appeared" until it starts again).
struct UniqueState {
    std::string name;
    bool alive = false;
    std::int64_t since = 0; // Unix seconds of its latest event
    std::string killer;     // its last killer (empty: never killed)
    std::int64_t killedAt = 0;
};
std::vector<UniqueState> LoadUniques(Database& db, bool& ok);

// --- Game servers -----------------------------------------------------------------------------

struct GameServerInfo {
    std::uint16_t id = 0;
    std::string name;
    std::uint8_t group = 0;
    std::string groupName;
    std::string host;
    std::uint16_t port = 0;
    std::uint32_t capacity = 0;      // the most players this game server takes
    std::uint32_t shardCapacity = 0; // its shard's, all its game servers together (0: their capacities added up)
    std::uint32_t online = 0;
    std::string schema = "sro_vt_shard"; // its shard's schema (_shard.szdbconfig)
    bool reporting = false; // reported recently and not stopped (filled by ListGameServers)
    std::int64_t startedAt = 0; // Unix seconds of this run's first report (ListGameServers; 0: never)
};

// A game server's report of itself (every few seconds while it runs).
bool ReportGameServer(Database& db, const GameServerInfo& server, std::string& error);
// A game server's load over its last sample's span (sr_server_samples, the site's server page): its ticks, their average
// and slowest time against the budget, the players online and the monsters alive; samples older than 7 days go.
struct ServerSample {
    std::uint16_t server = 0;
    std::uint32_t ticks = 0, overBudget = 0, players = 0, monsters = 0;
    double tickAvgMs = 0, tickMaxMs = 0, budgetMs = 0;
};
bool SaveServerSample(Database& db, const ServerSample& sample);
// The game server is going away (it is listed as offline at once).
bool StopGameServer(Database& db, std::uint16_t id);
// Every game server, by id; a report older than `staleSeconds` counts as offline.
std::vector<GameServerInfo> ListGameServers(Database& db, int staleSeconds, bool& ok);
// One entry for each shard (the game servers sharing a schema): what the server list shows. Its id and
// name are its lowest-numbered server's; its players and places the sum; up while any server reports.
std::vector<GameServerInfo> ShardsOf(const std::vector<GameServerInfo>& servers);
// The game server to take a player's character list: of the shard that `row` (the list's id) belongs to,
// `lobby` if it names one that is up, else the one with the most room. None when none is up.
std::optional<GameServerInfo> ChooseLobbyServer(const std::vector<GameServerInfo>& servers, std::uint16_t row, std::uint16_t lobby);
// The game's area name of every region (_RefRegion; empty on a database without it).
std::vector<std::pair<std::int16_t, std::string>> LoadRegionAreas(Database& db, bool& ok);
// The original's climates (_RefClimate) and each region's (_RefRegion.Climate): the weather's (game::ClimateWeather).
// Empty on a database without them.
struct ClimateData {
    std::vector<game::ClimateRow> climates;
    std::unordered_map<std::uint16_t, std::int32_t> regionClimate; // region id -> climate id
};
ClimateData LoadClimates(Database& db, bool& ok);

// --- Login tickets ----------------------------------------------------------------------------

// A new one-time ticket admitting `account` to game server `server` for `seconds`. Only its
// SHA-256 is stored. Empty on failure.
std::vector<std::uint8_t> IssueTicket(Database& db, std::int64_t account, std::uint16_t server, int seconds, std::string& error);
// The ticket's account, if it is valid for `server` and not expired; it is used up either way.
// `character` (if asked for): the character a moving player's ticket carries, 0 for a login's.
std::optional<std::int64_t> ConsumeTicket(Database& db, const std::vector<std::uint8_t>& ticket, std::uint16_t server, std::int64_t* character = nullptr);
// A player moving to another game server of the shard: a ticket carrying its character, and its one
// session handed to the server it goes to.
std::vector<std::uint8_t> IssueMoveTicket(Database& db, std::int64_t account, std::int64_t character, std::uint16_t to, int seconds, std::string& error);
// Sessions this server holds for nobody connected, older than `seconds` (a player who never arrived).
void ReleaseAbandonedSessions(Database& db, std::uint16_t server, const std::vector<std::int64_t>& connected, int seconds);
// Drops expired tickets; returns how many.
int PurgeTickets(Database& db);

// --- Online sessions --------------------------------------------------------------------------

// Takes the account's one session for game server `server`, held by the connection `token` (a number of its own,
// never 0). False if the account is on a game server that still reports (within `staleSeconds`); a session left on
// a dead server is taken over. `ok` is false when the query failed.
bool ClaimSession(Database& db, std::int64_t account, std::uint16_t server, int staleSeconds, std::int64_t token, bool& ok);
// A moving player's session taken up on its new server by the connection `token` (the move left it at 0).
void AdoptSession(Database& db, std::int64_t account, std::uint16_t server, std::int64_t token);
// Frees the session only while the connection `token` holds it (0: a move's that nobody took up): a connection that
// closes late never frees the session another one holds now.
void ReleaseSession(Database& db, std::int64_t account, std::uint16_t server, std::int64_t token);
// Every session of a game server (when it starts or stops).
void ReleaseServerSessions(Database& db, std::uint16_t server);
// Whether the account is on a game server that still reports.
bool HasLiveSession(Database& db, std::int64_t account, int staleSeconds, bool& ok);

// --- Characters -------------------------------------------------------------------------------

// The places a character wears items in (the original's numbers); 13.. the bag.
enum class EquipSlot : std::uint8_t {
    Head = 0, Chest = 1, Shoulders = 2, Hands = 3, Legs = 4, Feet = 5, Weapon = 6, Shield = 7, JobSuit = 8, Earring = 9, Necklace = 10,
    RingLeft = 11, RingRight = 12
};

// An item a character has and where it lies (_items, placed by _inventory / _chest).
struct StoredItem {
    std::int64_t id = 0; // _items.id64 (0: not stored yet)
    std::uint8_t slot = 0;
    std::uint32_t refId = 0;
    std::uint8_t plus = 0;
    std::uint64_t variance = 0;
    std::uint32_t durability = 0;
    std::uint16_t quantity = 1;
    std::vector<std::uint64_t> magic; // option id | value << 32 (_items.magparam1..12)
    // _items.data holds one number: the count (anything but equipment) or the durability. Writing,
    // this says which; reading, both fields get it and the game server keeps the one its row means.
    bool counted = false;
    // A rental's end (_RentItemInfo.PeriodEndTime, Unix seconds; INT64_MAX: no end; 0: not rented), read
    // with the items (ItemSelect); the row stays with the item's id wherever it goes. Last: the saves'
    // brace lists end at `counted`.
    std::int64_t rentUntil = 0;
    // An advanced elixir's binding (`_BindingOptionWithItem` bOptType 2: nOptValue, nOptID; 0: none), read with the
    // items and written by WriteItem. After the brace lists' fields too.
    std::uint8_t advanced = 0;
    std::uint32_t advancedRecipe = 0;
    // Bound to its character (ours, `sr_bound_items`: the beta quests' rewards): read with the items, written
    // by WriteItem; refused by the exchange, stalls, the Open Market, the guild storage and dropping.
    bool bound = false;
    // Its sockets (`_BindingOptionWithItem` bOptType 1, a row per made socket by nSlot: nOptID the skill, nOptLvl its
    // level, nParam1 the stone; an empty one all 0), read with the items and written by WriteItem.
    game::ItemSockets sockets{};
};

// A trader's safe trade (`_CharTrijobSafeTrade`): the count of the day, active, and the day
// of its last reset (yyyymmdd of LastSafeTrade).
struct SafeTradeRow {
    int count = 0;
    bool active = false;
    std::int32_t day = 0;
};

struct CharacterRow {
    std::int64_t id = 0;
    std::string name;
    std::uint32_t model = 0;
    std::uint8_t height = 0, level = 1;
    std::uint8_t maxLevel = 1; // LoadCharacter only: the highest level it reached (_char.maxlevel)
    // LoadCharacter only: the reverse return scroll's two places: where it last read
    // a return scroll (_char.TelRegion, TelPos*) and where it last died (DiedRegion, DiedPos*); region 0: none.
    struct Spot {
        std::uint16_t region = 0;
        float x = 0, y = 0, z = 0;
    };
    Spot recallSpot, deathSpot;
    std::uint64_t experience = 0;
    std::uint16_t strength = 0, intelligence = 0, statPoints = 0;
    std::uint32_t hp = 0, mp = 0;
    std::uint32_t skillPoints = 0, skillExperience = 0; // LoadCharacter only
    // LoadCharacter only: the berserk gauge (_char.remainhwancount) and the hwan level.
    std::uint8_t berserkPoints = 0;
    int hwanLevel = 0;
    // LoadCharacter only: the marks over its head (_char.autoinvestexp; proto::kMarkHelper).
    std::uint8_t marks = 0;
    // LoadCharacter only: its stall's decoration, the item mall's booth item used (_StaticAvatar.Param1, inferred;
    // 0: the plain booth).
    std::uint32_t stallSkin = 0;
    std::vector<std::pair<std::uint32_t, int>> masteries; // LoadCharacter only: mastery id, level
    std::vector<std::uint32_t> skills; // LoadCharacter only: the skill learned in each group (its id: the level's row)
    bool deleting = false;
    std::int64_t deleteSeconds = 0; // left before it is gone
    struct Item {
        EquipSlot slot;
        std::uint32_t item;
        std::uint8_t plus;
        std::uint32_t durability = 0; // a new character's: the item's new value
        std::uint16_t quantity = 1;   // ammunition
    };
    std::vector<Item> equipment; // the worn ones (places 0..12), by slot
    std::vector<StoredItem> items; // everything it has (LoadCharacter only), by slot
    std::vector<StoredItem> avatars; // the avatars it wears (_InventoryForAvatar; LoadCharacter only), by avatar place
    std::uint64_t gold = 0;
    std::uint8_t inventorySize = 45;
    std::uint32_t returnGate = 0; // _char.appointedteleport: the gate (its object id) designated (0: none yet)
    // Where it is in the world; none until it first entered.
    struct Place {
        std::uint16_t region = 0;
        float x = 0, y = 0, z = 0, yaw = 0;
    };
    std::optional<Place> place;
    // The filled quick slots (slot, kind, value), by slot.
    struct QuickSlot {
        std::uint8_t slot = 0, kind = 0;
        std::uint32_t value = 0;
    };
    std::vector<QuickSlot> quickSlots;
    // Its auto potion: the original's words in _clientconfig 11..14, the
    // items dropped in its rows in our sr_auto_potion; the defaults when it has none.
    game::AutoPotion autoPotion;
    // LoadCharacter only: its job's league (_chartrijob) and its alias (_char.nickname16).
    game::JobInfo job;
    std::string alias;
    std::int64_t jobLeftAt = 0; // when it last left a league (Unix seconds; 0 past its 7 days)
    std::optional<SafeTradeRow> safeTrade; // LoadCharacter only: its row (a trader league member's)
    std::int64_t bargainUntil = 0;         // LoadCharacter only: the bargain goods' lock-out's end (Unix seconds; timed job 2/7)
    // LoadCharacter only: the PK counts (_char.DailyPK, TotalPK, PKPenaltyPoint) and the PK keeper (`_TimedJob` 2/0: the
    // seconds left of its period at its stamp, Data1 as Unix seconds; none: no row).
    std::uint8_t dailyPk = 0;
    std::uint16_t totalPk = 0;
    std::int32_t pkPoints = 0;
    // LoadCharacter only: its academy's honour points (_TrainingCampMember.HonorPoint; 0 outside one), a shop's price.
    std::int64_t honorPoints = 0;
    struct PkKeeper {
        std::int64_t secondsLeft = 0, stamp = 0;
    };
    std::optional<PkKeeper> pkKeeper;
    // The buffs it left running (a skill and the time it had left).
    struct Buff {
        std::uint32_t skill = 0;
        std::uint64_t remainingMs = 0;
    };
    std::vector<Buff> buffs;
    // The skills' reuses of five minutes or more it left running (LoadCharacter only).
    struct LongCooldown {
        std::uint32_t skill = 0;
        std::int64_t started = 0; // Unix seconds
    };
    std::vector<LongCooldown> longCooldowns;
    // Its quests as _CharQuest keeps them (LoadCharacter only): the original's status, times
    // finished, the start and end (Unix seconds), the goals' counts (QuestData1 / QuestData2).
    struct Quest {
        std::uint32_t quest = 0;
        std::uint8_t status = 0;
        std::uint16_t times = 0;
        std::int64_t started = 0, ended = 0, data1 = 0, data2 = 0;
    };
    std::vector<Quest> quests;
    // Its counted deeds (ours, sr_char_records: the beta quests' `start record`; LoadCharacter only), by name.
    std::map<std::string, int> deeds;
    // Its Forgotten World records (LoadCharacter only): the collection book, the
    // instances entered (the latest first).
    std::vector<BookEntry> book;
    std::vector<InstanceEntry> instanceEntries;
    std::vector<ItemWait> itemWaits; // a Dimension Hole's 5 hours
};

// One character of the account on `server` (with its equipment and place), if it is there and
// not waiting to be deleted.
std::optional<CharacterRow> LoadCharacter(Database& db, std::int64_t account, std::uint16_t server, std::int64_t character, bool& ok);

// Where characters are now (the world's batches; each one row).
struct CharacterPlace {
    std::int64_t character = 0;
    CharacterRow::Place place;
};
bool SavePlaces(Database& db, const std::vector<CharacterPlace>& places);

// A character's items after moves, in one transaction: each of `present` is where it says now
// (written by its id, which the game server took from ReserveItemIds; a new one is inserted); the
// places in `emptied` hold nothing any more (what was there and is not among `present` goes), and
// the items `gone` (merged into others) are deleted.
// `avatars`: the avatar places' side (_InventoryForAvatar), in the same transaction: each of its
// `present` in its avatar place (out of the bag), its `emptied` places hold nothing (what went into the bag is kept).
struct AvatarPlaces {
    std::vector<StoredItem> present;
    std::vector<std::uint8_t> emptied;
};
bool SaveItems(Database& db, std::int64_t character, const std::vector<StoredItem>& present, const std::vector<std::uint8_t>& emptied,
               const std::vector<std::int64_t>& gone, const AvatarPlaces& avatars = {});
// An exchange between two characters, in one transaction: each side's places
// as SaveItems writes them (an item may leave one side's place for the other's), then both sides' gold.
// Items in a third character's inventory or a storage are never touched from here.
struct ExchangeSaveSide {
    std::int64_t character = 0;
    std::uint64_t gold = 0;
    std::vector<StoredItem> present;
    std::vector<std::uint8_t> emptied;
    std::vector<std::int64_t> gone;
};
bool SaveExchange(Database& db, const ExchangeSaveSide& a, const ExchangeSaveSide& b);
// The same inside a transaction already open (a purchase in the item mall writes one side, `b` = `a`, with its silk);
// `gold` false: the characters' gold is left as it is.
// --- The item mall's services ---
// The items in magic cubes (`_inventoryforlinkedstorage`: the cube's item id, the place, the item), by cube.
std::unordered_map<std::int64_t, std::vector<StoredItem>> LoadCubeItems(Database& db, const std::vector<std::int64_t>& cubes, bool& ok);
// A cube's change and the bag's side in one transaction: `item` into the cube's place `slot` (`into`), or the place
// emptied (its item now in the bag's side).
bool SaveCubeChange(Database& db, const ExchangeSaveSide& side, std::int64_t cube, std::uint8_t slot, const StoredItem* item, bool into);
// The skin change: the character's model and scale (`_char.refobjid`, `scale`).
bool SaveCharacterModel(Database& db, std::int64_t character, std::uint32_t model, std::uint8_t scale);
// A skill withdrawn: its group's row `previous` gone, `now` (0: none) learned instead.
bool SaveSkillWithdrawal(Database& db, std::int64_t character, std::uint32_t previous, std::uint32_t now);
bool SaveSidesIn(Database& tx, const ExchangeSaveSide& a, const ExchangeSaveSide& b, std::string& error, bool gold = true);
// A stall's sale from afar, in one transaction and only while the database's clock is
// before `deadlineMs` (Unix ms): the item `serial` leaves the owner's `ownerPlace` for the buyer's
// `buyerPlace` (which must hold nothing), the owner's gold becomes `ownerGold`. The buyer's _char row is
// locked first, so FarSaleLanded waits for it. False when it was not made.
bool SaveFarSale(Database& db, std::int64_t owner, std::uint8_t ownerPlace, std::uint64_t ownerGold, std::int64_t buyer, std::uint8_t buyerPlace,
                 std::int64_t serial, std::int64_t deadlineMs);
// Whether a sale from afar landed: the item `serial` in the buyer's `place`. None while the database's clock
// is not yet past `deadlineMs` (a sale may still be made) or when the read failed.
std::optional<bool> FarSaleLanded(Database& db, std::int64_t buyer, std::uint8_t place, std::int64_t serial, std::int64_t deadlineMs);
// Gold added to what the database holds for a character (one who left the world meanwhile).
bool AddGold(Database& db, std::int64_t character, std::uint64_t gold);

// --- The Open Market: the original's `_OpenMarket`, a listing per account and number ---------

struct OpenMarketRecord {
    std::int64_t account = 0;
    std::uint32_t listing = 0;
    std::string seller;
    std::uint8_t status = 0; // game::openmarket::Status
    std::uint32_t refId = 0, group = 0, count = 0;
    std::uint8_t degree = 0;
    std::uint64_t price = 0, deposit = 0, saleFee = 0;
    std::int64_t secondsLeft = 0; // to its EndDate
    bool held = false;            // its item still held (on sale or expired)
    StoredItem item;              // when held
};
// A listing made (_OpenMarket_Insert): the item at the bag place `place` (its id `item`) leaves the bag into the row; the
// character's gold written (`gold`: what it has after the deposit). The listing's number, or nothing.
struct OpenMarketPut {
    std::int64_t account = 0, character = 0;
    std::string seller;
    std::uint8_t place = 0;
    std::int64_t item = 0;
    std::uint32_t refId = 0, group = 0, count = 0;
    std::uint8_t degree = 0;
    std::uint64_t price = 0, deposit = 0, saleFee = 0;
    std::int64_t seconds = 0;
    std::uint32_t ticket = 0; // UseCash: the ticket's RefObjID (0: none)
    std::uint64_t gold = 0;
};
std::optional<std::uint32_t> PutOpenMarket(Database& db, const OpenMarketPut& put);
// An account's listings (all but those gone), oldest first.
std::vector<OpenMarketRecord> LoadOpenMarket(Database& db, std::int64_t account, bool& ok);
// A page of a search: on sale and not ended, the group (and the degree, 0 any; the seller, empty any), newest first, of
// the first `most`; `total` the matches.
std::vector<OpenMarketRecord> SearchOpenMarket(Database& db, std::uint32_t group, std::uint8_t degree, const std::string& seller, int page, int rows,
                                               int most, std::uint32_t& total);
// What taking a listing's item gives.
enum class OpenMarketTake : std::uint8_t { Ok, NotOnSale, Expired, NotListed, BagTaken, Failed };
// A purchase (_OpenMarket_Purchase): the listing on sale and not ended at `price`; its item into the buyer's bag place; the
// buyer's gold written; the row sold, kept 28 days (`keepSeconds`). `out` the listing (its item, its seller's character).
OpenMarketTake BuyOpenMarket(Database& db, std::int64_t account, std::uint32_t listing, std::uint64_t price, std::int64_t buyer, std::uint8_t place,
                             std::uint64_t buyerGold, std::int64_t keepSeconds, OpenMarketRecord& out, std::int64_t& sellerCharacter);
// Taken back (_OpenMarket_Cancle): on sale or expired; its item into the owner's bag place; the row gone, its deposit kept.
OpenMarketTake TakeBackOpenMarket(Database& db, std::int64_t account, std::uint32_t listing, std::int64_t character, std::uint8_t place,
                                  OpenMarketRecord& out);
// Sold listings collected (_OpenMarket_Receipt): each sold; the rows gone; the character's gold `gold` (what it has after).
// False when one is not sold or their payouts are not `payout`.
bool SettleOpenMarket(Database& db, std::int64_t account, const std::vector<std::uint32_t>& listings, std::uint64_t payout, std::int64_t character,
                      std::uint64_t gold);
// The account's listings' times (the original's each minute, for an owner online): on sale past its end -> expired, kept
// `keepSeconds` more; sold or expired past that -> gone (an expired one's item deleted). What changed: (listing, its
// RefObjID, its new status).
struct OpenMarketChange {
    std::uint32_t listing = 0, refId = 0;
    std::uint8_t status = 0;
};
std::vector<OpenMarketChange> AgeOpenMarket(Database& db, std::int64_t account, std::int64_t keepSeconds);
// Items and gold for a character that is not in the game (an operator's or a developer's
// command): each item into the first free place of the bag (its `slot` is ignored), the gold
// added; all or nothing.
enum class GiveResult : std::uint8_t { Done, NoCharacter, Online, BagFull, Failed };
GiveResult GiveItems(Database& db, std::uint16_t server, std::string_view name, const std::vector<StoredItem>& items, std::uint64_t gold,
                     std::string& error);

// `count` ids for new items (the items table's sequence); empty when the database could not.
std::vector<std::int64_t> ReserveItemIds(Database& db, int count);

// An account's storage on a game server (none yet: empty, 150 places).
struct StorageRow {
    std::uint8_t size = 150;
    std::uint64_t gold = 0;
    std::vector<StoredItem> items; // slot: the storage's place (id unused)
};
std::optional<StorageRow> LoadStorage(Database& db, std::int64_t account, std::uint16_t server);
// The places that changed: each of `present` as it is now, those in `emptied` empty; the gold.
bool SaveStorage(Database& db, std::int64_t account, std::uint16_t server, const std::vector<StoredItem>& present,
                 const std::vector<std::uint8_t>& emptied, std::uint64_t gold);

// A guild's storage (_GuildChest by guild, its gold in _Guild.Gold); `size` is not kept (the level's).
std::optional<StorageRow> LoadGuildChest(Database& db, std::uint32_t guild);
bool SaveGuildChest(Database& db, std::uint32_t guild, const std::vector<StoredItem>& present, const std::vector<std::uint8_t>& emptied,
                    std::uint64_t gold);

// A character's running buffs as it leaves (all replaced).
bool SaveBuffs(Database& db, std::int64_t character, const std::vector<CharacterRow::Buff>& buffs);
// A skill's reuse of five minutes or more, as the original keeps one: _timedjob category 6, job 200, the skill
// and the second it began. Written when it begins; removed once it is over.
bool SaveLongCooldown(Database& db, std::int64_t character, std::uint32_t skill, std::int64_t started);
bool DeleteLongCooldowns(Database& db, std::int64_t character, const std::vector<std::uint32_t>& skills);

// A character's gold, and its return point (the gate's object id, as _char.appointedteleport keeps it).
bool SaveGold(Database& db, std::int64_t character, std::uint64_t gold);
bool SaveReturnGate(Database& db, std::int64_t character, std::uint32_t gate);
// The marks over its head (the original keeps them in _char.autoinvestexp).
bool SaveMarks(Database& db, std::int64_t character, std::uint8_t marks);
// A quest of the character (_CharQuest's row, made or replaced).
bool SaveCharQuest(Database& db, std::int64_t character, const CharacterRow::Quest& quest);
// The bag's places (_char.inventorysize) and the hwan level reached (_char.hwanlevel).
bool SaveInventorySize(Database& db, std::int64_t character, std::uint8_t size);
bool SaveHwanLevel(Database& db, std::int64_t character, int level);
// The reverse return scroll's places (CharacterRow::recallSpot, deathSpot): kept in _char's Tel* and Died* columns.
bool SaveRecallSpot(Database& db, std::int64_t character, const CharacterRow::Spot& spot);
bool SaveDeathSpot(Database& db, std::int64_t character, const CharacterRow::Spot& spot);
// _RefOptionalTeleport's rows in service: the reverse return scroll's map points.
struct OptionalTeleportRow {
    std::uint32_t id = 0;
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0;
    int levelMin = 0, levelMax = 0;
};
bool LoadOptionalTeleports(Database& db, std::vector<OptionalTeleportRow>& out, std::string& error);
// The stall's decoration (_StaticAvatar.Param1; its row made when missing).
bool SaveStallSkin(Database& db, std::int64_t character, std::uint32_t refId);
// A pet's timed job (the original's _TimedJobForPet, CharID its _CharCOS row): the
// category (game::kPetJobSkill a skill on it, game::kPetJobItem a composite item's), the job (the skill, the item),
// its end (TimeToKeep: Unix seconds), Data1..8 and the item's serial, as the original writes them
// (_ADDTIMEDJOBFORPET).
struct PetJobRow {
    std::uint8_t category = 0;
    std::uint32_t job = 0;
    std::int64_t until = 0;
    std::int32_t data[8]{};
    std::int64_t serial = 0;
};
// A pet's job written (one of the same category and job before it replaced), or taken away.
bool AddPetJob(Database& db, std::int64_t cos, const PetJobRow& job);
bool EndPetJob(Database& db, std::int64_t cos, std::uint8_t category, std::uint32_t job);
// A pet (_CharCOS): its row, made at its first summon and linked from its summoning item's
// _Items.Data in the same transaction (as the original's _AddNewCOS; its bag, _InvCOS, comes with the grab pets').
struct CosRow {
    std::int64_t id = 0;
    std::int64_t owner = 0;    // OwnerCharID
    std::uint32_t refId = 0;   // RefCharID: its characterdata row (an attack pet's is its level's)
    std::uint32_t hp = 0;
    std::uint8_t state = 0;    // game::CosState
    std::string name;          // CharName (empty: none given)
    int level = 1;
    std::int64_t experience = 0; // ExpOffset
    int hgp = 0;
    std::uint32_t option = 0;  // PetOption (a grab pet's settings)
    std::int64_t rentUntil = 0; // RentEndTime (Unix seconds; 0: none)
    // Read with the row (LoadCos): its bag's size (its characterdata row's _RefObjChar.InventorySize; 0: none, or the
    // reference tables are not in this database) and its bag (_InvCOS: slot the bag's place).
    int bagSize = 0;
    std::vector<StoredItem> bag;
    // Its timed jobs (_TimedJobForPet), read with it as the original reads them with its data and bag; those over are the game server's to end.
    std::vector<PetJobRow> jobs;
};
std::optional<CosRow> LoadCos(Database& db, std::int64_t id);
// A pet's bag and its owner's inventory after moves between them (or a pick-up into the bag), in one transaction:
// `inventory` as SaveExchange's sides are (its places now, those emptied, items merged away; its gold not written),
// then the bag's places (`present` by place, `emptied`). An item moved from one to the other keeps its id.
bool SaveCosItems(Database& db, const ExchangeSaveSide& inventory, std::int64_t cos, const std::vector<StoredItem>& present,
                  const std::vector<std::uint8_t>& emptied);
// The new row's id (0: failed); `item` the summoning item's _Items.ID64 (0: none to link).
std::int64_t AddCos(Database& db, const CosRow& row, std::int64_t item, std::string& error);
bool SaveCos(Database& db, const CosRow& row);
// A pet named, once (the original's _COS_Naming: a name no other pet has, case aside): Named, Taken (another pet's),
// HasOne (it is named already), Failed (the database).
enum class CosNaming : std::uint8_t { Named, Taken, HasOne, Failed };
CosNaming NameCos(Database& db, std::int64_t id, std::string_view name);
// The teleport zones (`_RefEventZone`, service 1, event TELEPORT): the zone's name in
// lower case -> the teleport point (strParam1).
bool LoadTeleportZones(Database& db, std::unordered_map<std::string, std::string>& out, std::string& error);
// Its health and mana as it left the world (C10b).
bool SaveVitals(Database& db, std::int64_t character, std::uint32_t hp, std::uint32_t mp);
// _Char.LastLogout: now (the character left the world; the guild election's rules read it).
bool SaveLogout(Database& db, std::int64_t character);
// Its berserk gauge (_char.remainhwancount: 0..5).
bool SaveBerserkPoints(Database& db, std::int64_t character, std::uint8_t points);
// Its PK counts (_char.DailyPK, TotalPK, PKPenaltyPoint).
bool SavePkCounts(Database& db, std::int64_t character, std::uint8_t daily, std::uint16_t total, std::int32_t points);
// Its PK keeper (`_TimedJob` category 2, job 0: TimeToKeep the seconds left at `stamp`, Data1 the stamp as Unix seconds;
// ours for Data1, the original's packed local time); none: the row deleted.
bool SavePkKeeper(Database& db, std::int64_t character, std::optional<CharacterRow::PkKeeper> keeper);
// Its level, experience, skill points and stats (C10c).
struct CharacterProgress {
    std::int64_t character = 0;
    std::uint8_t level = 1;
    std::uint64_t experience = 0;
    std::uint32_t skillPoints = 0, skillExperience = 0;
    std::uint16_t strength = 0, intelligence = 0, statPoints = 0;
};
bool SaveProgress(Database& db, const std::vector<CharacterProgress>& progress);
// A mastery's level now (C7f); a skill learned: its id in place of the group's last one (`previous`,
// 0: none), as the original keeps one row per group (_charskill).
bool SaveMastery(Database& db, std::int64_t character, std::uint32_t mastery, int level);
bool SaveSkill(Database& db, std::int64_t character, std::uint32_t previous, std::uint32_t skill);

// One quick slot of a character (kind 0: emptied).
bool SaveQuickSlot(Database& db, std::int64_t character, const CharacterRow::QuickSlot& slot);
// A character's auto potion, all of it in one transaction: the original's words (_clientconfig
// types 11, 12, 13 the rows, 14 the delay; slot 0, type 0, as its game server writes them) and the
// rows' items (sr_auto_potion; a row without one has none).
bool SaveAutoPotion(Database& db, std::int64_t character, const game::AutoPotion& settings);

// The account's characters on `server` (the shard of the connection), oldest first; characters
// asked to be deleted more than `deleteSeconds` ago (_deletedchar) are deleted first.
std::vector<CharacterRow> ListCharacters(Database& db, std::int64_t account, std::uint16_t server, int deleteSeconds, bool& ok);

struct NewCharacter {
    std::string name;
    std::uint32_t model = 0;
    std::uint8_t height = 0;
    std::uint16_t strength = 0, intelligence = 0;
    std::uint32_t hp = 0, mp = 0;
    std::uint8_t level = 1;
    std::vector<CharacterRow::Item> equipment;
};

enum class CreateCharacterResult : std::uint8_t { Created, NameTaken, TooMany, Failed };
// `maxCharacters` per account on this server, counting those waiting to be deleted.
CreateCharacterResult CreateCharacter(Database& db, std::int64_t account, std::uint16_t server, const NewCharacter& character,
                                      int maxCharacters, std::string& error);
// Whether a name is free on `server` (ignoring case).
bool CharacterNameFree(Database& db, std::uint16_t server, std::string_view name, bool& ok);
// Ours (a tester's twin for tries with two clients, the user's call 2026-11-17; SroDb --copy-character): the character
// `from` copied whole under the name `to` onto `account`: its row (level, stats, gold, place, jobs' levels), masteries,
// skills, quests, timed jobs, quick slots and options, the job league's row, the collection book, the auto potion, and
// every item it carries or wears as a new item (its plus, blues, sockets and advanced elixir, a magic cube's contents, a
// pet's COS with its own timed jobs). Not its guild, alias, friends, letters, the account's storage nor its stall. Both
// should be out of the game. False with `error`.
bool CopyCharacter(Database& db, std::int64_t account, std::string_view from, std::string_view to, std::string& error);

enum class CharacterChange : std::uint8_t { Done, NotFound, Deleting, NotDeleting, Failed };
// Starts the wait before a character is gone (`delaySeconds`); restorable until then.
CharacterChange DeleteCharacter(Database& db, std::int64_t account, std::uint16_t server, std::string_view name, int delaySeconds);
CharacterChange RestoreCharacter(Database& db, std::int64_t account, std::uint16_t server, std::string_view name, int delaySeconds);
// The character to enter the world with (`id`): it must not be waiting to be deleted.
CharacterChange SelectCharacter(Database& db, std::int64_t account, std::uint16_t server, std::string_view name, std::int64_t& id);

// --- Notices ----------------------------------------------------------------------------------

struct NoticeRow {
    std::string title, body;
    std::int64_t posted = 0; // Unix seconds
};
std::vector<NoticeRow> LatestNotices(Database& db, int count, bool& ok);
bool PostNotice(Database& db, std::string_view title, std::string_view body);

} // namespace sr::server
