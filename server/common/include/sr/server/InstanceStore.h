#pragma once

// A character's Forgotten World records in the original's tables: its collection
// book (_CharCollectionBook: a row per talisman registered) and its instance entries (_CharInstanceWorldData: a row
// per world, the instance it last entered, where it came from, when). The connection's search path names the
// shard's schema.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sr/server/Database.h"

namespace sr::server {

struct BookEntry {
    std::uint32_t theme = 0; // _RefCollectionBook_Theme id
    std::uint8_t slot = 0;   // 1..8
};

struct InstanceEntry {
    std::uint32_t key = 0;   // DungeonKeyID: the instance
    std::uint32_t world = 0; // WorldID
    std::uint32_t layer = 0; // LayerID
    std::int64_t opened = 0; // OpenedTime, Unix seconds
    std::uint16_t region = 0;
    std::int32_t x = 0, y = 0, z = 0; // where the character entered from
    bool active = false;     // IsActivated: inside it now
    std::uint32_t count = 0; // EnterCount
    std::int64_t last = 0;   // LastEnterTime, Unix seconds
};

// An item group's long wait (a Dimension Hole's COOLTIME group 0x71, 5 hours): kept in _timedjob as the skills' long
// waits are (category 6; job 201 ours, the skills' 200), its group in Data1, when it began (Unix seconds) in Data2.
struct ItemWait {
    std::uint32_t group = 0;
    std::int64_t started = 0;
};
bool LoadItemWaits(Database& db, std::int64_t character, std::vector<ItemWait>& out, std::string& error);
bool SaveItemWait(Database& db, std::int64_t character, const ItemWait& wait, std::string& error);

bool LoadCollectionBook(Database& db, std::int64_t character, std::vector<BookEntry>& out, std::string& error);
bool AddCollectionBookEntry(Database& db, std::int64_t character, const BookEntry& entry, std::string& error);
// Its entries, the latest first.
bool LoadInstanceEntries(Database& db, std::int64_t character, std::vector<InstanceEntry>& out, std::string& error);
// Made or replaced (a row per character and world).
bool SaveInstanceEntry(Database& db, std::int64_t character, const InstanceEntry& entry, std::string& error);

} // namespace sr::server
