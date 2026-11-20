#pragma once

// The PvP events' records. The connection's search
// path names the shard's schema.
//
// - The shard's queue of an event (ours, sr_event_entries; the original's queue was its shard manager's): a row per
//   registered character, written by the player's game server, read and emptied by the server hosting the matches at
//   the play's start. Where the character registered from is its way home.
// - So-Ok's Subscription Card (the original's _FlagWorld_EventParticipants and _AddFlagEventParticipant): a row per
//   account, its uses counted.

#include <cstdint>
#include <string>
#include <vector>

#include "sr/server/Database.h"

namespace sr::server {

// The events of sr_event_entries.
enum class EventKind : std::int16_t { FlagWorld = 1, Arena = 2 };

struct EventEntryRow {
    EventKind event = EventKind::FlagWorld;
    std::int64_t character = 0;
    std::string name;
    int level = 1;              // the highest level reached
    std::uint16_t server = 0;   // the game server it registered on
    std::uint16_t region = 0;   // where it registered from: its way home
    float x = 0, y = 0, z = 0;
    std::int64_t registered = 0; // Unix milliseconds
    std::int32_t mode = 0, kind = 0; // the arena's mode and map kind (0 for the flag world)
    std::int64_t group = 0;          // the arena's party or guild (0: alone)
};

// Made or replaced (one row per event and character).
bool SaveEventEntry(Database& db, const EventEntryRow& row, std::string& error);
bool DeleteEventEntry(Database& db, EventKind event, std::int64_t character, std::string& error);
// Every row of the event, then none (one transaction).
bool TakeEventEntries(Database& db, EventKind event, std::vector<EventEntryRow>& out, std::string& error);
bool LoadEventEntries(Database& db, EventKind event, std::vector<EventEntryRow>& out, std::string& error);

// The card's use (_AddFlagEventParticipant @JID): the account's row counted up, made the first time.
bool AddFlagEventParticipant(Database& db, std::int64_t jid, std::string& error);
// Its count (0: no row).
int FlagEventParticipantCount(Database& db, std::int64_t jid);

} // namespace sr::server
