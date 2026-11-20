#pragma once

// The PvP events over the network: our own messages (0x1360-0x13BF). Capture the Flag and Event So-Ok (0x1360-0x137F)
// carry what the original's 0x34B1 / 0x74B2 and So-Ok's state byte carry;
// 0x1380-0x13BF are the Battle Arena's.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

// --- Capture the Flag and So-Ok (0x1360-0x137F) ----------------------------------------------------------------

// So-Ok's registration row pressed (the original's 0x74B2, no body): it toggles, registered -> cancelled, else checked
// and registered. Answered by FlagWorldNotice {personal, code}.
struct FlagWorldRegister {
    static constexpr MessageId kId = 0x1360;
    template <typename V>
    void Visit(V&) {}
};

// A row of So-Ok's coded quests (the prize swap): `choice` 0xFF opens it (talk A with its rows),
// 0 its first row (the swap: talk C, D or E), 1 the other (the end of the talk).
struct EventTalk {
    static constexpr MessageId kId = 0x1361;
    std::uint32_t npc = 0;
    std::uint16_t quest = 0;
    std::uint8_t choice = 0xFF;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("quest", quest);
        v("choice", choice);
    }
};

// The developer's switches (/FLAGWORLD; ours): 1 open the registration now, 2 start the play now, 3 end it now (the
// results), 4 cancel everything (everyone out, no rewards). A development server's only.
struct FlagWorldDev {
    static constexpr MessageId kId = 0x1362;
    std::uint8_t action = 0;
    template <typename V>
    void Visit(V& v) {
        v("action", action);
    }
};

// The original's 0x34B1 to the client: `personal` 0 a state broadcast (game::FlagState: 2, 3, 4, 5, 8 {value: the
// world}, 9, 0x0A {value: the actor's entity}, 0x0D, 0x0E), 1 a code to this player (game::FlagCode, its FF form:
// 0x0B {value: the door's zone id, team, name}, 0x0E {value: elapsed ms}, 0x09 / 0x10 {team, name}, ...).
struct FlagWorldNotice {
    static constexpr MessageId kId = 0x1370;
    std::uint8_t personal = 0;
    std::uint8_t code = 0;
    std::uint32_t value = 0;
    std::uint8_t team = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("personal", personal);
        v("code", code);
        v("value", value);
        v("team", team);
        v("name", name, 64);
    }
};

// After an Event So-Ok's NpcMenu (her select answer's tail): the flag
// world's state byte (0 none, 1 not registered: "Capture the Flag Match Registration", 2 registered: its cancel), and
// her coded quests' rows (the swap).
struct EventDesk {
    static constexpr MessageId kId = 0x1371;
    std::uint32_t npc = 0;
    std::uint8_t flagState = 0;
    struct Row {
        std::uint16_t quest = 0;
        std::string title; // a text key
        template <typename V>
        void Visit(V& v) {
            v("quest", quest);
            v("title", title, 128);
        }
    };
    std::vector<Row> quests;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("flagState", flagState);
        v("quests", quests);
    }
};

// What So-Ok says in a coded quest's talk (the original's dialogue: a text key and its rows' keys, numbered by the
// NPC window): no rows ends the talk's choices.
struct EventSays {
    static constexpr MessageId kId = 0x1372;
    std::uint32_t npc = 0;
    std::uint16_t quest = 0;
    std::string text;
    std::vector<std::string> rows;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("quest", quest);
        v("text", text, 128);
        v("rows", rows);
    }
};

// A player's flag team (0 red, 1 blue, 0xFF none), to those who see him and kept for
// newcomers: in a match who is an enemy (another team) and the team's look.
struct EntityFlagTeam {
    static constexpr MessageId kId = 0x1373;
    std::uint32_t entity = 0;
    std::uint8_t team = 0xFF;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("team", team);
    }
};

// --- The Battle Arena (0x1380-0x13BF) ------------------------------------------------

// The arena's request (the original's 0x74D3): action 1 register {mode, kind (1 score, 2 flag)}, the guild mode with the
// chosen members' characters; action 2 cancel. Answered by ArenaNotice {personal, code}.
struct ArenaRegister {
    static constexpr MessageId kId = 0x1380;
    std::uint8_t action = 1;
    std::uint8_t mode = 0;
    std::uint8_t kind = 1;
    std::vector<std::uint32_t> members;
    template <typename V>
    void Visit(V& v) {
        v("action", action);
        v("mode", mode);
        v("kind", kind);
        v("members", members);
    }
};

// The developer's switches (ours): 1 open `mode`'s registration now, 2 start its play now, 3 end it now (the results),
// 4 cancel everything (everyone home, no results).
struct ArenaDev {
    static constexpr MessageId kId = 0x1381;
    std::uint8_t action = 0;
    std::uint8_t mode = 0;
    template <typename V>
    void Visit(V& v) {
        v("action", action);
        v("mode", mode);
    }
};

// The original's 0x34D2 to the client: `personal` 0 a broadcast (game::ArenaState: 2 {mode, kinds}, 3 {mode}, 4 {mode,
// kind}, 5 {mode}, 8 {value: the length in ms}, 0x0D / 0x0E / 0x0F {mode, kinds}), 1 a code to this player
// (game::ArenaCode: 00 {value: the action, mode}, 0x40 {value: points}, 0x80 {name}, 0x84 {team, name}, 0xF0 {value: the
// length, value2: elapsed ms, mode, kind}, ...). The score boards and the result have their own messages.
struct ArenaNotice {
    static constexpr MessageId kId = 0x1390;
    std::uint8_t personal = 0;
    std::uint8_t code = 0;
    std::uint8_t mode = 0;
    std::uint8_t kind = 0;
    std::uint16_t kinds = 0;
    std::uint32_t value = 0, value2 = 0;
    std::uint8_t team = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("personal", personal);
        v("code", code);
        v("mode", mode);
        v("kind", kind);
        v("kinds", kinds);
        v("value", value);
        v("value2", value2);
        v("team", team);
        v("name", name, 64);
    }
};

// After NPC_BATTLE_ARENA_MANAGER's NpcMenu: 0x8000 registered, else the open
// mode (low nibble; 4 none) | the kinds offered (0x20 score, 0x40 flag).
struct ArenaDesk {
    static constexpr MessageId kId = 0x1391;
    std::uint32_t npc = 0;
    std::uint16_t state = 4;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("state", state);
    }
};

// The scores (FF 41 in a score map with its top five, FF 88 in a flag map without): team 0's and team 1's totals.
struct ArenaScores {
    static constexpr MessageId kId = 0x1392;
    std::uint32_t team0 = 0, team1 = 0;
    struct Row {
        std::uint8_t team = 0;
        std::string name;
        std::uint32_t points = 0;
        template <typename V>
        void Visit(V& v) {
            v("team", team);
            v("name", name, 64);
            v("points", points);
        }
    };
    std::vector<Row> top;
    template <typename V>
    void Visit(V& v) {
        v("team0", team0);
        v("team1", team1);
        v("top", top);
    }
};

// A participant's result (the broadcast 9 {mode, outcome, coins, SP}): outcome 1 won, 2 lost, 3 draw; the coins and the
// skill experience given (the window shows them); the final scores.
struct ArenaResult {
    static constexpr MessageId kId = 0x1393;
    std::uint8_t mode = 0;
    std::uint8_t outcome = 3;
    std::uint8_t coins = 0;
    std::uint32_t sp = 0;
    std::uint32_t team0 = 0, team1 = 0;
    template <typename V>
    void Visit(V& v) {
        v("mode", mode);
        v("outcome", outcome);
        v("coins", coins);
        v("sp", sp);
        v("team0", team0);
        v("team1", team1);
    }
};

using EventEvent = std::variant<FlagWorldNotice, EventDesk, EventSays, EntityFlagTeam, ArenaNotice, ArenaDesk, ArenaScores, ArenaResult>;

// The events' range (0x1360-0x13BF).
inline bool IsEventMessage(MessageId id) { return id >= 0x1360 && id < 0x13C0; }

inline bool DecodeEventEvent(MessageId id, std::span<const std::byte> payload, EventEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case FlagWorldNotice::kId: return as(FlagWorldNotice{});
    case EventDesk::kId: return as(EventDesk{});
    case EventSays::kId: return as(EventSays{});
    case EntityFlagTeam::kId: return as(EntityFlagTeam{});
    case ArenaNotice::kId: return as(ArenaNotice{});
    case ArenaDesk::kId: return as(ArenaDesk{});
    case ArenaScores::kId: return as(ArenaScores{});
    case ArenaResult::kId: return as(ArenaResult{});
    default: return false;
    }
}

inline const char* EventMessageName(MessageId id) {
    switch (id) {
    case FlagWorldRegister::kId: return "FlagWorldRegister";
    case EventTalk::kId: return "EventTalk";
    case FlagWorldDev::kId: return "FlagWorldDev";
    case FlagWorldNotice::kId: return "FlagWorldNotice";
    case EventDesk::kId: return "EventDesk";
    case EventSays::kId: return "EventSays";
    case EntityFlagTeam::kId: return "EntityFlagTeam";
    case ArenaRegister::kId: return "ArenaRegister";
    case ArenaDev::kId: return "ArenaDev";
    case ArenaNotice::kId: return "ArenaNotice";
    case ArenaDesk::kId: return "ArenaDesk";
    case ArenaScores::kId: return "ArenaScores";
    case ArenaResult::kId: return "ArenaResult";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
