#pragma once

// The game's logs, in the original's log tables (its SRO_VT_LOG; ours the shard's log schema, LogSchemaOf): what
// happened to characters (_LogEventChar) and to items and gold (_LogEventItem; a mall purchase in _LogCashItem too),
// with the original's event and operation numbers (the _AddLogChar / _AddLogItem rows its game server writes).
//
// Ours, how they are written: the game server's thread queues the rows in memory (it never waits on them); about once
// a second it hands what is queued to one job of its database pool, which writes the batch in one transaction (COPY).
// The event tables are partitioned by month (a month's partition made before its first row lands; indexes by
// character, item and time). A batch the database cannot take goes into a spill file, written first by the next batch
// that can.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sr::server {

class Database;

namespace logs {
// _LogEventChar.EventID (the original's LOG_CHAREVENT_*: its report procedures' numbers and its game server's;
// docs/re/77-logs.md).
enum class CharEvent : std::uint8_t {
    Created = 1,      // _ReportDailyLog's character creation
    Login = 4,        // entered the world
    Logout = 6,       // left it
    Died = 20,        // killed (by a monster or a player: the description says by whom)
    LevelChanged = 22 // Data1 the old level, Data2 the new (_Report_SpendTimePerLevel)
};
// _LogEventItem.Operation (the original's LOG_ITEMEVENT_*: its item operations' log numbers; docs/re/77-logs.md).
enum class ItemOp : std::uint8_t {
    ToStorage = 20,      // into the storage
    FromStorage = 21,    // out of it
    GoldToStorage = 22,  // gold into the storage
    GoldFromStorage = 23,
    PickUp = 30,         // off the ground
    Drop = 31,           // onto it
    Buy = 32,            // from an NPC's shop
    Sell = 33,           // to one
    DropGold = 34,
    BuyCash = 35,        // from the item mall (a _LogCashItem row too)
};
// _LogEventItem.TargetStorage: where the item went.
enum class Storage : std::uint8_t { Inventory = 0, Storage = 3 };
constexpr std::uint8_t kNoSlot = 0xFF; // a slot that is no place (the ground, an NPC)
} // namespace logs

struct CharLogRow {
    std::int64_t character = 0;
    std::uint8_t event = 0;
    std::int32_t data1 = 0, data2 = 0;
    std::string pos, desc; // EventPos (ours: region(x,y,z)), strDesc
    std::int64_t at = 0;   // Unix seconds (0: when queued)
};
struct ItemLogRow {
    std::int64_t character = 0;
    std::uint32_t refId = 0;
    std::int32_t data = 0; // dwData: the count, or the durability
    std::uint8_t storage = 0, operation = 0, from = logs::kNoSlot, to = logs::kNoSlot;
    std::string pos, desc;
    std::int64_t serial = 0, gold = 0;
    std::int64_t at = 0;
};

// The log schema of a shard (the original's SRO_VT_LOG): sro_vt_log for the first, <shard>_log for the others.
std::string LogSchemaOf(const std::string& shardSchema);

class GameLog {
public:
    GameLog(std::string schema, std::filesystem::path spill);

    // Queued (the server's thread).
    void Char(CharLogRow row);
    void Item(ItemLogRow row);
    struct Batch {
        std::vector<CharLogRow> chars;
        std::vector<ItemLogRow> items;
        bool Empty() const { return chars.empty() && items.empty(); }
    };
    // What is queued since the last take.
    Batch Take();
    // A batch written on `db` (a database job), the spill's rows first: false when the database refused it (it is in
    // the spill then). A spill left with no batch is written by an empty one.
    bool Write(Database& db, const Batch& batch);
    bool Spilled() const;
    const std::string& Schema() const { return schema_; }

private:
    std::string schema_;
    std::filesystem::path spill_;
    Batch queued_;
    mutable std::mutex writing_;                     // two jobs never write at once (the spill, the partitions)
    std::set<std::pair<std::string, int>> months_;   // (table, yyyymm) partitions made
    std::set<std::string> partitioned_, plain_;      // the tables found partitioned or not
    bool EnsureMonth(Database& tx, const std::string& table, std::int64_t at);
    bool WriteRows(Database& db, const Batch& batch, std::string& error);
    void ToSpill(const Batch& batch);
    Batch FromSpill();
};

} // namespace sr::server
