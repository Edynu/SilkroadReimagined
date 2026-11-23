#pragma once

// The game server's character lobby (C5d), after Hello / Welcome: the
// client shows its login ticket, then lists, creates, deletes, restores and selects characters.
// The original's lobby carries the same.

#include <cstdint>
#include <string>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/game/TitleProtocol.h"

namespace sr::game::proto {

// --- Entering --------------------------------------------------------------------------------

struct LobbyEnter {
    static constexpr MessageId kId = 0x0300;
    std::vector<std::uint8_t> ticket; // from the login server
    template <typename V>
    void Visit(V& v) {
        v("ticket", ticket, kTicketBytes);
    }
};

enum class LobbyEnterStatus : std::uint8_t {
    Accepted = 0,
    InvalidTicket = 1,   // unknown, used, expired, or for another server: log in again
    AlreadyLoggedIn = 2, // the account is on a game server already
    ServerFull = 3,
    Busy = 4,
};

// Never Describe into a log: LobbyEnter carries the ticket.
struct LobbyEntered {
    static constexpr MessageId kId = 0x0301;
    LobbyEnterStatus status = LobbyEnterStatus::Accepted;
    std::uint8_t maxCharacters = 0;
    template <typename V>
    void Visit(V& v) {
        v("status", status);
        v("maxCharacters", maxCharacters);
    }
};

// --- The list --------------------------------------------------------------------------------

struct EquippedItem {
    std::uint32_t item = 0; // itemdata RefObjID
    std::uint8_t plus = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item);
        v("plus", plus);
    }
};

// What the lobby shows of a character (the original's list record).
struct CharacterSummary {
    std::uint32_t model = 0; // characterdata RefObjID
    std::string name;
    std::uint8_t height = 0;
    std::uint8_t level = 1;
    std::uint64_t experience = 0;
    std::uint16_t strength = 0;
    std::uint16_t intelligence = 0;
    std::uint16_t statPoints = 0;
    std::uint32_t hp = 0;
    std::uint32_t mp = 0;
    bool deleting = false;
    std::uint32_t deleteMinutes = 0; // left before it is gone (deleting only)
    std::uint8_t guildRank = 0;
    std::string guildName;
    std::vector<EquippedItem> equipment;
    std::vector<EquippedItem> avatars;
    template <typename V>
    void Visit(V& v) {
        v("model", model);
        v("name", name, 32);
        v("height", height);
        v("level", level);
        v("experience", experience);
        v("strength", strength);
        v("intelligence", intelligence);
        v("statPoints", statPoints);
        v("hp", hp);
        v("mp", mp);
        v("deleting", deleting);
        v("deleteMinutes", deleteMinutes);
        v("guildRank", guildRank);
        v("guildName", guildName, 32);
        v("equipment", equipment, 16);
        v("avatars", avatars, 8);
    }
};

struct CharacterListRequest {
    static constexpr MessageId kId = 0x0302;
    template <typename V>
    void Visit(V&) {}
};

struct CharacterList {
    static constexpr MessageId kId = 0x0303;
    std::vector<CharacterSummary> characters;
    std::uint8_t maxCharacters = 0;
    template <typename V>
    void Visit(V& v) {
        v("characters", characters, 16);
        v("maxCharacters", maxCharacters);
    }
};

// --- Actions ---------------------------------------------------------------------------------

struct CharacterCreate {
    static constexpr MessageId kId = 0x0304;
    std::string name;
    std::uint32_t model = 0;
    std::uint8_t height = 0;
    // The starter items chosen: chest, legs and shoes of one armour type, and a weapon.
    std::uint32_t chest = 0, legs = 0, shoes = 0, weapon = 0;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 32);
        v("model", model);
        v("height", height);
        v("chest", chest);
        v("legs", legs);
        v("shoes", shoes);
        v("weapon", weapon);
    }
};

struct CharacterNameCheck {
    static constexpr MessageId kId = 0x0305;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 32);
    }
};

struct CharacterDelete {
    static constexpr MessageId kId = 0x0306;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 32);
    }
};

struct CharacterRestore {
    static constexpr MessageId kId = 0x0307;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 32);
    }
};

struct CharacterSelect {
    static constexpr MessageId kId = 0x0308;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 32);
    }
};

enum class LobbyAction : std::uint8_t { Create = 1, NameCheck = 2, Delete = 3, Restore = 4, Select = 5, List = 6 };

// The original's lobby failures (its codes in brackets), and ours.
enum class LobbyStatus : std::uint8_t {
    Ok = 0,
    InvalidCreation = 1,   // a model or item creation does not offer [3]
    NoWeapon = 2,          // [4]
    TooManyCharacters = 3, // [5]
    Failed = 4,            // [6]
    BadName = 5,           // the length or a character [12]
    NameNotAllowed = 6,    // a forbidden word [13]
    NameTaken = 7,         // [16]
    NotFound = 8,          // no such character of this account
    Deleting = 9,          // it waits to be deleted: restore it first
    NotDeleting = 10,      // restore of a character that is not being deleted
    Busy = 11,             // the server cannot answer now
    GuildMaster = 12,      // a guild's master is not deleted while it leads its guild
};

struct LobbyResult {
    static constexpr MessageId kId = 0x0309;
    LobbyAction action = LobbyAction::Create;
    LobbyStatus status = LobbyStatus::Ok;
    template <typename V>
    void Visit(V& v) {
        v("action", action);
        v("status", status);
    }
};

} // namespace sr::game::proto
