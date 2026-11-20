#pragma once

// The item mall's services as the database keeps them: a character's time tickets
// and premium package in `_timedjob` (categories 3, 9 and 10, the original's records; game/TimedServices.h), the pet
// growth potion's time (ours: `_timedjob` category 115, the original's `_TimedJobForPet` not in our layout), and the
// storage's size in `_chestinfo`.

#include <cstdint>
#include <vector>

namespace sr::server {

class Database;

struct TimedJobRow {
    std::uint8_t category = 0;
    std::uint32_t job = 0;
    std::int32_t timeToKeep = 0;
    std::int32_t data[8]{}; // Data1..Data8
};
// The character's jobs of these categories.
bool LoadTimedJobs(Database& db, std::int64_t character, const std::vector<std::uint8_t>& categories, std::vector<TimedJobRow>& out);
// The category's one job written (the one before replaced), or gone (`row` null).
bool SaveTimedJob(Database& db, std::int64_t character, std::uint8_t category, const TimedJobRow* row);
// The pet growth potion's category (ours).
constexpr std::uint8_t kTimedPetGrowth = 115;
// The account's storage grown to `size` places (`_chestinfo.chestsize`).
bool SaveChestSize(Database& db, std::int64_t account, int size);

} // namespace sr::server
