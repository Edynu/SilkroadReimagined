#pragma once

// The quests over the network: our own messages (0x1260-0x128F) for what the original's
// carry (its NPC talk 0x30D4 / 0x70D8, the quest news 0x30D5, the abandon 0x70D9, the reward's choice 0x7515).
// A quest's definition goes to the client as its text (game::ParseQuest), once a session,
// before any message names it: the client reads the original's quests and a server's own the same way, the
// texts from its text tables (@KEY) or the words themselves.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

inline bool IsQuestMessage(MessageId id) { return id >= 0x1260 && id < 0x1290; }

constexpr std::size_t kQuestGoals = 8;        // ours: the goals a quest may have (the original's quests have up to 4)
constexpr std::size_t kQuestsAtOnce = 20;     // ours: quests a character holds at once (the original's limit not traced)
constexpr float kQuestReach = 100.0f;         // ours: from how far an NPC is talked to (the gates' and shops' reach)
constexpr std::uint32_t kOurQuestIds = 20000; // a server's own quests are numbered from here

// A quest's state, the original's numbers (QUEST_STATUS_*; _CharQuest.Status).
enum class QuestStatus : std::uint8_t {
    Achieving = 1, // under way
    Done = 4,      // not under way (finished when its count is above 0)
    Abandoned = 6, // given up (may be taken again)
    Achieved = 8,  // its goals met: back to its NPC
};

struct QuestDefinition {
    static constexpr MessageId kId = 0x1260;
    std::string text; // the definition (game::WriteQuest)
    static constexpr std::size_t kMaxText = 32768;
    template <typename V>
    void Visit(V& v) {
        v("text", text, kMaxText);
    }
};

// A quest under way: its state, each goal's progress (kills, items held, 1 when a delivery or talk is done) and
// the seconds left on its timer (0: none).
struct QuestState {
    std::uint32_t quest = 0;
    QuestStatus status = QuestStatus::Achieving;
    std::vector<std::uint16_t> progress;
    std::uint32_t secondsLeft = 0;
    template <typename V>
    void Visit(V& v) {
        v("quest", quest);
        v("status", status);
        v("progress", progress);
        v("secondsLeft", secondsLeft);
    }
};

// On entering the world: the quests under way, and those finished (with how often).
struct QuestList {
    static constexpr MessageId kId = 0x1261;
    std::vector<QuestState> active;
    struct Finished {
        std::uint32_t quest = 0;
        std::uint16_t times = 0;
        template <typename V>
        void Visit(V& v) {
            v("quest", quest);
            v("times", times);
        }
    };
    std::vector<Finished> finished;
    template <typename V>
    void Visit(V& v) {
        v("active", active);
        v("finished", finished);
    }
};

// A quest taken, its progress or its state changed.
struct QuestChanged {
    static constexpr MessageId kId = 0x1262;
    QuestState state;
    template <typename V>
    void Visit(V& v) {
        v("state", state);
    }
};

enum class QuestEnd : std::uint8_t { Finished = 1, Abandoned = 2, TimeOut = 3 };
struct QuestGone {
    static constexpr MessageId kId = 0x1263;
    std::uint32_t quest = 0;
    QuestEnd why = QuestEnd::Finished;
    std::uint16_t times = 0; // how often it has been finished now
    template <typename V>
    void Visit(V& v) {
        v("quest", quest);
        v("why", why);
        v("times", times);
    }
};

// What an NPC has to do with the player's quests (sent after its NpcMenu): a row each in its window.
enum class NpcQuestKind : std::uint8_t { Offer = 0, UnderWay = 1, HandIn = 2 };
struct NpcQuests {
    static constexpr MessageId kId = 0x1264;
    std::uint32_t npc = 0;
    struct Row {
        std::uint32_t quest = 0;
        NpcQuestKind kind = NpcQuestKind::Offer;
        template <typename V>
        void Visit(V& v) {
            v("quest", quest);
            v("kind", kind);
        }
    };
    std::vector<Row> rows;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("rows", rows);
    }
};

// The player picks a quest's row at an NPC: the NPC answers what it says now (a moment of the quest's talks, or
// a goal's talk) and what the player may answer.
struct QuestTalk {
    static constexpr MessageId kId = 0x1270;
    std::uint32_t npc = 0, quest = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("quest", quest);
    }
};

enum class QuestChoice : std::uint8_t {
    None = 0,   // the talk only
    Accept = 1, // accept or decline the offer
    HandIn = 2, // hand over the goal's items, end the talk goal, or finish the quest (and choose the reward)
    Rows = 3,   // the server's rows (QuestSays::rows): the one picked sent back as a hand-in with its index (a talk game's)
};
struct QuestSays {
    static constexpr MessageId kId = 0x1265;
    std::uint32_t npc = 0, quest = 0;
    std::string moment; // a QuestTalk moment (offer, not-done, done, ...); empty: `goal`'s own talk
    std::uint8_t goal = 0;
    QuestChoice choice = QuestChoice::None;
    // A talk game's (QuestDef::talkGame, ours): what is said, a text key, in place of the moment's lines; and its rows
    // (text keys) with QuestChoice::Rows.
    std::string text;
    std::vector<std::string> rows;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("quest", quest);
        v("moment", moment);
        v("goal", goal);
        v("choice", choice);
        v("text", text, 128);
        v("rows", rows, 16);
    }
};

struct QuestAccept {
    static constexpr MessageId kId = 0x1271;
    std::uint32_t npc = 0, quest = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("quest", quest);
    }
};

// What is due at this NPC: its deliveries and talks, and the quest finished when its goals are met there (the
// reward's items chosen by their index, when it lets the player choose).
struct QuestHandIn {
    static constexpr MessageId kId = 0x1272;
    std::uint32_t npc = 0, quest = 0;
    std::vector<std::uint8_t> choices;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("quest", quest);
        v("choices", choices);
    }
};

struct QuestAbandon {
    static constexpr MessageId kId = 0x1273;
    std::uint32_t quest = 0;
    template <typename V>
    void Visit(V& v) {
        v("quest", quest);
    }
};

enum class QuestCode : std::uint8_t {
    Ok = 0,
    TooFar = 1,      // the NPC is not near
    NotOffered = 2,  // not offered to this player (here, now)
    TooMany = 3,     // kQuestsAtOnce under way
    BagFull = 4,     // no room for its items or its reward
    NotDone = 5,     // its goals are not met
    NoItems = 6,     // the items to hand over are not in the bag
    BadChoice = 7,   // the reward's choice does not fit
    NotUnderWay = 8,
    Busy = 9,        // dead, or in a dealing
};
// A request's answer (the moment the NPC says on success is in QuestSays).
struct QuestAnswer {
    static constexpr MessageId kId = 0x1266;
    std::uint32_t quest = 0;
    QuestCode code = QuestCode::Ok;
    template <typename V>
    void Visit(V& v) {
        v("quest", quest);
        v("code", code);
    }
};

// A quest's reward given, as given (after the server's rates): for the system lines.
struct QuestRewarded {
    static constexpr MessageId kId = 0x1267;
    std::uint32_t quest = 0;
    std::uint64_t exp = 0, skillExp = 0, gold = 0;
    std::uint32_t sp = 0, inventory = 0;
    std::vector<std::uint32_t> items; // their refIds
    template <typename V>
    void Visit(V& v) {
        v("quest", quest);
        v("exp", exp);
        v("skillExp", skillExp);
        v("gold", gold);
        v("sp", sp);
        v("inventory", inventory);
        v("items", items);
    }
};

// The NPCs with something about the player's quests, the kind of mark over each (the 1.188 client's kinds,
// 1 SYSTEM_EXCLAMATION_START, a quest to take; 2 _GOING, one under way; 3 _END, one to hand in).
// The whole list each time it changes.
enum class QuestMark : std::uint8_t { Start = 1, Going = 2, End = 3 };
struct QuestMarks {
    static constexpr MessageId kId = 0x1268;
    struct Mark {
        std::string npc; // its codename
        QuestMark kind = QuestMark::Start;
        template <typename V>
        void Visit(V& v) {
            v("npc", npc, 128);
            v("kind", kind);
        }
    };
    std::vector<Mark> marks;
    template <typename V>
    void Visit(V& v) {
        v("marks", marks);
    }
};

// The level-up's quest guide (1.188's side icon CIFQuestInfoGuide, ginterface 46, and its window CIFQuestInfo 45): at a
// level-up the quests of the new level for the player (the quest's level that level; up to level 40
// the player's race or both; none it has a record of), each with its name and its guide's two lines (_RefQuest's NPC and
// condition texts, SN_NN_*, SN_NC_*: a key, or the words of ours). Empty: the icon put away.
struct QuestGuide {
    static constexpr MessageId kId = 0x1269;
    struct Row {
        std::uint32_t quest = 0;
        std::string name, npc, condition;
        template <typename V>
        void Visit(V& v) {
            v("quest", quest);
            v("name", name, 128);
            v("npc", npc, 512);
            v("condition", condition, 512);
        }
    };
    std::vector<Row> rows;
    template <typename V>
    void Visit(V& v) {
        v("rows", rows);
    }
};

// What the server says about quests, as the client's session hands it on.
using QuestEvent = std::variant<QuestDefinition, QuestList, QuestChanged, QuestGone, NpcQuests, QuestSays, QuestAnswer, QuestRewarded, QuestMarks, QuestGuide>;

// A server's quest message as the session's event; false for another id or a malformed payload.
inline bool DecodeQuestEvent(MessageId id, std::span<const std::byte> payload, QuestEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case QuestDefinition::kId: return as(QuestDefinition{});
    case QuestList::kId: return as(QuestList{});
    case QuestChanged::kId: return as(QuestChanged{});
    case QuestGone::kId: return as(QuestGone{});
    case NpcQuests::kId: return as(NpcQuests{});
    case QuestSays::kId: return as(QuestSays{});
    case QuestAnswer::kId: return as(QuestAnswer{});
    case QuestRewarded::kId: return as(QuestRewarded{});
    case QuestMarks::kId: return as(QuestMarks{});
    case QuestGuide::kId: return as(QuestGuide{});
    default: return false;
    }
}

inline const char* QuestMessageName(MessageId id) {
    switch (id) {
    case QuestDefinition::kId: return "QuestDefinition";
    case QuestList::kId: return "QuestList";
    case QuestChanged::kId: return "QuestChanged";
    case QuestGone::kId: return "QuestGone";
    case NpcQuests::kId: return "NpcQuests";
    case QuestSays::kId: return "QuestSays";
    case QuestAnswer::kId: return "QuestAnswer";
    case QuestRewarded::kId: return "QuestRewarded";
    case QuestMarks::kId: return "QuestMarks";
    case QuestGuide::kId: return "QuestGuide";
    case QuestTalk::kId: return "QuestTalk";
    case QuestAccept::kId: return "QuestAccept";
    case QuestHandIn::kId: return "QuestHandIn";
    case QuestAbandon::kId: return "QuestAbandon";
    default: return "Quest?";
    }
}

} // namespace sr::game::proto
