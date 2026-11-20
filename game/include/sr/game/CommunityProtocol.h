#pragma once

// Friends, the whisper block list and letters over the network: our own
// messages (0x11E0-0x11FF) for what the original's carry (its requests 0x7302-0x730D). The limits and
// the error numbers are the original's; the client turns the numbers into its texts.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

constexpr std::size_t kMaxFriends = 20;       // _Friend_Add's limit,
constexpr std::size_t kMaxWhisperBlocks = 20; // _BlockedWhisperer_Add's
constexpr std::size_t kMaxLetters = 20;       // a letter's number is below 20
constexpr std::size_t kLetterChars = 300;     // _Memo_Add's @Message
constexpr std::size_t kCommunityNameChars = 12;
// No time to answer a friend proposal (0: none): the original's shard manager keeps no proposal, it relays it and acts on
// the answer whenever it comes (the game server only forwards the answer); a proposal ends when answered or when a side
// leaves.
constexpr double kFriendAnswerSeconds = 0.0;

// The original's numbers (friends, letters; the ones marked ours have no number there).
enum class CommunityCode : std::uint16_t {
    Ok = 0,
    FriendListFull = 0x6408,
    AlreadyFriend = 0x6409,
    NoAnswer = 0x640A,
    Refused = 0x640B,
    UnknownUser = 0x640D,
    InProgress = 0x640E,
    TargetListFull = 0x640F,
    LetterFailed = 0x6410,
    LetterBoxFull = 0x6414,
    TargetBoxFull = 0x6413,  // ours: UIIT_MSG_LETTERERR_OBJECT_FULLLETTERBOX
    LetterUnknownUser = 0x6412, // ours: UIIT_MSG_LETTERERR_UNKNOWNUSER
    GuildTooLow = 0x6415,
    NoGuild = 0x6416,
};

// What an answer is about.
enum class CommunityTopic : std::uint8_t { Friend = 0, Letter = 1, LetterMany = 2 };

// --- Requests (0x11E0) ------------------------------------------------------------------------

// A proposal to the character of that name, wherever it is (the original's 0x7302).
struct FriendAsk {
    static constexpr MessageId kId = 0x11E0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 64);
    }
};

// The answer to a FriendProposal (0x7303).
struct FriendReply {
    static constexpr MessageId kId = 0x11E1;
    std::uint32_t request = 0;
    std::uint8_t accept = 0;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("accept", accept);
    }
};

// Both sides lose each other (0x7304).
struct FriendDelete {
    static constexpr MessageId kId = 0x11E2;
    std::uint32_t character = 0;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
    }
};

// The list of letters (0x730B).
struct LettersAsk {
    static constexpr MessageId kId = 0x11E3;
    template <typename V>
    void Visit(V&) {}
};

// A letter's text; it counts as read from now on (0x7308).
struct LetterOpen {
    static constexpr MessageId kId = 0x11E4;
    std::uint64_t id = 0;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
    }
};

// A letter to a character, online or not (0x7309).
struct LetterWrite {
    static constexpr MessageId kId = 0x11E5;
    std::string to, text;
    template <typename V>
    void Visit(V& v) {
        v("to", to, 64);
        v("text", text, 1000);
    }
};

// (0x730A)
struct LetterDelete {
    static constexpr MessageId kId = 0x11E6;
    std::uint64_t id = 0;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
    }
};

// One letter to several (0x730C): 1 the guild's members (guild level 3 and up), 2 the union's guild
// masters (level 4 and up).
struct LetterWriteMany {
    static constexpr MessageId kId = 0x11E7;
    std::uint8_t kind = 1;
    std::vector<std::string> names;
    std::string text;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("names", names, 64);
        v("text", text, 1000);
    }
};

// The block list kept on the server (0x730D): 1 add, 2 remove.
struct WhisperBlock {
    static constexpr MessageId kId = 0x11E8;
    std::uint8_t op = 1;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("op", op);
        v("name", name, 64);
    }
};

// --- The server's messages (0x11F0) -----------------------------------------------------------

struct FriendRecord {
    std::uint32_t character = 0;
    std::string name;
    std::uint32_t model = 0; // the race's mark
    std::uint8_t online = 0;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("name", name, 64);
        v("model", model);
        v("online", online);
    }
};

// Everything on entering the world.
struct CommunityLists {
    static constexpr MessageId kId = 0x11F0;
    std::vector<FriendRecord> friends;
    std::vector<std::string> blocked;
    std::uint16_t unread = 0;
    template <typename V>
    void Visit(V& v) {
        v("friends", friends, kMaxFriends);
        v("blocked", blocked, kMaxWhisperBlocks);
        v("unread", unread);
    }
};

// Someone proposes to make friends.
struct FriendProposal {
    static constexpr MessageId kId = 0x11F1;
    std::uint32_t request = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("name", name, 64);
    }
};

struct FriendAdded {
    static constexpr MessageId kId = 0x11F2;
    FriendRecord record;
    template <typename V>
    void Visit(V& v) {
        v("record", record);
    }
};

struct FriendRemoved {
    static constexpr MessageId kId = 0x11F3;
    std::uint32_t character = 0;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
    }
};

// A friend came or went (`announce`: said in the system lines; 0 only corrects the lamp).
struct FriendPresence {
    static constexpr MessageId kId = 0x11F4;
    std::uint32_t character = 0;
    std::uint8_t online = 0;
    std::uint8_t announce = 1;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("online", online);
        v("announce", announce);
    }
};

// How a request ended (Ok for a letter sent; failures for the rest). `name`: whom it is about.
struct CommunityAnswer {
    static constexpr MessageId kId = 0x11F5;
    CommunityTopic topic = CommunityTopic::Friend;
    CommunityCode code = CommunityCode::Ok;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("topic", topic);
        v("code", code);
        v("name", name, 64);
    }
};

struct LetterRecord {
    std::uint64_t id = 0;
    std::string from;
    std::uint8_t read = 0;
    std::uint16_t year = 0;
    std::uint8_t month = 0, day = 0, hour = 0, minute = 0;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("from", from, 64);
        v("read", read);
        v("year", year);
        v("month", month);
        v("day", day);
        v("hour", hour);
        v("minute", minute);
    }
};

struct LetterListing {
    static constexpr MessageId kId = 0x11F6;
    std::vector<LetterRecord> rows;
    template <typename V>
    void Visit(V& v) {
        v("rows", rows, kMaxLetters + 64); // a box filled before our limit is shown whole
    }
};

struct LetterBody {
    static constexpr MessageId kId = 0x11F7;
    std::uint64_t id = 0;
    std::string from, text;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("from", from, 64);
        v("text", text, 1000);
    }
};

// A new letter is in the box.
struct LetterArrived {
    static constexpr MessageId kId = 0x11F8;
    std::uint16_t unread = 0;
    template <typename V>
    void Visit(V& v) {
        v("unread", unread);
    }
};

// A block list request's end (0xB66F's numbers): 0 done, 1 already there, 2 no such character, 3 full.
struct WhisperBlockChanged {
    static constexpr MessageId kId = 0x11F9;
    std::uint8_t op = 1;
    std::uint8_t result = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("op", op);
        v("result", result);
        v("name", name, 64);
    }
};

using CommunityEvent = std::variant<CommunityLists, FriendProposal, FriendAdded, FriendRemoved, FriendPresence, CommunityAnswer,
                                    LetterListing, LetterBody, LetterArrived, WhisperBlockChanged>;

inline bool IsCommunityMessage(MessageId id) { return id >= 0x11E0 && id < 0x1200; }

// A server's community message as the session's event; false for another id or a malformed payload.
inline bool DecodeCommunityEvent(MessageId id, std::span<const std::byte> payload, CommunityEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case CommunityLists::kId: return as(CommunityLists{});
    case FriendProposal::kId: return as(FriendProposal{});
    case FriendAdded::kId: return as(FriendAdded{});
    case FriendRemoved::kId: return as(FriendRemoved{});
    case FriendPresence::kId: return as(FriendPresence{});
    case CommunityAnswer::kId: return as(CommunityAnswer{});
    case LetterListing::kId: return as(LetterListing{});
    case LetterBody::kId: return as(LetterBody{});
    case LetterArrived::kId: return as(LetterArrived{});
    case WhisperBlockChanged::kId: return as(WhisperBlockChanged{});
    default: return false;
    }
}

inline const char* CommunityMessageName(MessageId id) {
    switch (id) {
    case CommunityLists::kId: return "CommunityLists";
    case FriendProposal::kId: return "FriendProposal";
    case FriendAdded::kId: return "FriendAdded";
    case FriendRemoved::kId: return "FriendRemoved";
    case FriendPresence::kId: return "FriendPresence";
    case CommunityAnswer::kId: return "CommunityAnswer";
    case LetterListing::kId: return "LetterListing";
    case LetterBody::kId: return "LetterBody";
    case LetterArrived::kId: return "LetterArrived";
    case WhisperBlockChanged::kId: return "WhisperBlockChanged";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
