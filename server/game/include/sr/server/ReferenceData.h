#pragma once

// The server's own reference data from the original's tables in our database: the drops and the nests, read from the
// shard schema's _Ref* and Tab_Ref* tables (imported by SroSetup). Rows the client lacks (later content) are left out:
// every item and monster is checked against the client's tables.

#include <string>

#include "sr/assets/GameData.h"
#include "sr/server/Database.h"
#include "sr/game/Berserk.h"
#include "sr/server/Monsters.h"

namespace sr::server {

struct ReferenceCounts {
    int records = 0;
    int missingItems = 0, missingMonsters = 0; // codenames the client lacks, left out
};

// Whether the connection's shard schema holds the original's drop and spawn tables with rows.
bool HasReferenceTables(Database& db);

// The text of monster_drops.tsv (record kinds: Drops.h / the generator's docstring).
bool DropRecords(Database& db, const assets::ItemTable& items, const assets::CharacterTable& characters, std::string& out,
                 ReferenceCounts& counts, std::string& error);

// The text of monster_nests.tsv: each monster nest's policy (codename, region, x, z rounded, radius,
// spawn radius, champion %, refill delay min and max in seconds, most alive, aggressive, sight).
bool NestRecords(Database& db, std::string& out, ReferenceCounts& counts, std::string& error);

// Every monster's 32 Resist columns (_RefObjChar: ResistFrozen .. Resist32) by its RefObjID: what the
// states roll against (the client's characterdata does not carry them).
bool MonsterResistanceRecords(Database& db, MonsterResistances& out, std::string& error);

// The hwan levels' berserk modes (_RefHWANLevel: ParamFourcc / ParamValue pairs, SPDU the speed's
// percent, DABR the cut of damage taken).
bool HwanLevelRecords(Database& db, std::vector<game::HwanLevel>& out, std::string& error);

// The magic option groups (_RefMagicOptGroup in service): the socket stones' skills by level, MagicType 3.
bool MagicOptGroupRecords(Database& db, std::vector<assets::MagicOptGroupRow>& out, std::string& error);

} // namespace sr::server
