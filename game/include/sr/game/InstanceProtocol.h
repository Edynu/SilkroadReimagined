#pragma once

// The Forgotten World over the network: our own messages (0x1290-0x12AF) for the
// Dimension Hole's gap, entering and leaving an instance, the instance's notices and the talisman collection book.
// The hole and a talisman are used from the bag as any item is (UseItem); a talisman's use is asked first by the
// client (UIIT_MSG_COLLECTION_WINDOW_RECORD_CONFIRM) and sent as RegisterTalisman.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

inline bool IsInstanceMessage(MessageId id) { return id >= 0x1290 && id < 0x12B0; }

constexpr float kGapReach = 100.0f; // ours: from how far the gap is entered (an NPC's reach)
// A party recall pillar's service in NpcMenu (1.188's NPC function 0x27: its menu row "Summon Party member",
// UIIT_PAG_PARTY_SUMMON, kind 0x3D), told by the server to a player by its instance's pillar.
constexpr std::uint32_t kNpcPartySummon = 0x400;

// --- From the server -----------------------------------------------------------------------------------------

// The gap of a hole (the selected gap's window, UIIT_STT_GOD_*): what it opens and for whom.
struct GapInfo {
    static constexpr MessageId kId = 0x1290;
    std::uint32_t entity = 0;  // the gap
    std::uint32_t world = 0;   // _RefGame_World id
    std::string theme;         // its theme's name key (UIIT_STT_GOD_TOGUI ...)
    std::uint8_t grade = 1;    // the stars (UIIT_STT_GOD_CLASS_<n>)
    std::uint16_t minLevel = 0, maxLevel = 0;
    std::string master;        // the party master who opened it
    std::uint32_t secondsLeft = 0; // until the gap disappears
    bool own = false;          // the asker's party's (its name board shows the master)
    // A job temple's door: no hole's lines. Seth's (INS_TEMPLE_SETH_TELEPORT) also says the players
    // inside its temple and its WorldMaxUserCount (the original's two bytes after its selection, shown in its
    // tooltip "[number of people inside] %d / %d"); 0 otherwise.
    bool temple = false;
    std::uint8_t inside = 0, capacity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("world", world);
        v("theme", theme, 128);
        v("grade", grade);
        v("minLevel", minLevel);
        v("maxLevel", maxLevel);
        v("master", master, 64);
        v("secondsLeft", secondsLeft);
        v("own", own);
        v("temple", temple);
        v("inside", inside);
        v("capacity", capacity);
    }
};

// Why a hole's use or an entry was refused: the original's text key (UIIT_MGS_ERR_GOD_*, ...), shown in the chat.
struct InstanceRefused {
    static constexpr MessageId kId = 0x1291;
    std::string text;
    template <typename V>
    void Visit(V& v) {
        v("text", text, 128);
    }
};

// A trigger's message (ACTION_MESSAGE_NOTIFY_WINDOW / _SYSTEM_WINDOW): its text key, on screen and / or in the chat.
struct InstanceNotice {
    static constexpr MessageId kId = 0x1292;
    std::string text;
    bool notify = false; // on screen (the notice window)
    bool system = false; // in the chat
    // On the quest notice (window 42): a quest's or an item use's words, the original's 0x30EC (shows its
    // key there; SN_TALK_COMMON_END with snd_quest_end): "You have captured a Sloth Antinoc. ...".
    bool quest = false;
    // On the warning banner (the original's 0x300C notices: 0x0C16 a text key, the Roc's,
    // puts them in the chat, and on the warning banner).
    bool warning = false;
    std::string arg; // a text key (or words) for the text's %s (the tomb's difficulty)
    template <typename V>
    void Visit(V& v) {
        v("text", text, 128);
        v("notify", notify);
        v("system", system);
        v("quest", quest);
        v("warning", warning);
        v("arg", arg, 128);
    }
};

// The character is in an instance (sent after its WorldEntered there), or out of it again (world 0); and the re-entry
// limit's timed job on its buff board (reads GROUP_CONFIG_INS_GOD's TIMEDJOB_*_FORCLIENT: the icon, the
// subject, the explanation, 300 s), its seconds left (0: none). Sent at entering, at leaving and at login.
struct InstanceState {
    static constexpr MessageId kId = 0x1293;
    std::uint32_t world = 0;        // _RefGame_World id; 0: the shared world
    std::uint32_t secondsLeft = 0;  // until the instance ends
    std::uint32_t jobSeconds = 0;   // the re-entry limit left
    std::string jobIcon, jobSubject, jobText;
    template <typename V>
    void Visit(V& v) {
        v("world", world);
        v("secondsLeft", secondsLeft);
        v("jobSeconds", jobSeconds);
        v("jobIcon", jobIcon, 128);
        v("jobSubject", jobSubject, 128);
        v("jobText", jobText, 128);
    }
};

// The character's collection book (_CharCollectionBook): every talisman registered, by theme and slot. Sent on
// entering the world.
struct CollectionBook {
    static constexpr MessageId kId = 0x1294;
    struct Entry {
        std::uint32_t theme = 0; // _RefCollectionBook_Theme id
        std::uint8_t slot = 0;   // 1..8
        template <typename V>
        void Visit(V& v) {
            v("theme", theme);
            v("slot", slot);
        }
    };
    std::vector<Entry> entries;
    template <typename V>
    void Visit(V& v) {
        v("entries", entries);
    }
};

// A talisman registered (the bag's item taken by ItemMoved) or refused (its text key).
struct TalismanRegistered {
    static constexpr MessageId kId = 0x1295;
    bool ok = false;
    std::uint32_t theme = 0;
    std::uint8_t slot = 0;
    std::string text; // UIIT_MSG_COLLECTION_WINDOW_RECORD_FINISH, _RECORD_ERR
    template <typename V>
    void Visit(V& v) {
        v("ok", ok);
        v("theme", theme);
        v("slot", slot);
        v("text", text, 128);
    }
};

// The party recall pillar's window (NPC_FWORLD_RECALL_PARTY; 1.188 CNIFSummonPartyUserWnd, res_ui\nifsummonpartyuserwnd.2dt):
// the master's party members, each one's state (1 inside already "Complete", 2 "Possible", 3
// "Impossible" with its reason, UIIT_STT_PARTY_SUMMON_IMPOSSILBE_<n>), its level and the minutes left to enter.
struct SummonList {
    static constexpr MessageId kId = 0x1296;
    struct Row {
        std::uint32_t character = 0;
        std::string name;
        std::uint8_t level = 0;
        std::uint8_t state = 2;  // 1 inside, 2 possible, 3 impossible
        std::uint8_t reason = 0; // impossible: 1 not in a town, 2 full, 3 level, 4 job outfit, 5 stall or trade, 6 transport,
                                 // 7 guild war, 8 dead, 9 free PvP
        std::uint16_t minutes = 0;
        template <typename V>
        void Visit(V& v) {
            v("character", character);
            v("name", name, 64);
            v("level", level);
            v("state", state);
            v("reason", reason);
            v("minutes", minutes);
        }
    };
    std::vector<Row> rows;
    std::uint8_t maxUsers = 4;
    template <typename V>
    void Visit(V& v) {
        v("rows", rows);
        v("maxUsers", maxUsers);
    }
};

// The party's master summons the character into its instance (UIIT_STT_GNGWC_PARTY_MEMBER_CALL, 20 s
// to answer: SummonReply).
struct SummonAsked {
    static constexpr MessageId kId = 0x1297;
    std::string master;
    std::uint32_t seconds = 20;
    template <typename V>
    void Visit(V& v) {
        v("master", master, 64);
        v("seconds", seconds);
    }
};

// What became of a summon, to the master: a text key (UIIT_STT_GNGWC_PARTY_MEMBER_CALL_REFUSAL "%s canceled the
// summon.", _CANNOT "Unable to summon. %s is not ready...", UIIT_STT_PARTY_SUMMON_WAIT_ERR, UIIT_MGS_ERR_PARTY_SUMMON_NOT,
// UIIT_STT_PARTY_SUMMON_IMPOSSILBE_<n>) and the member's name for its %s.
struct SummonResult {
    static constexpr MessageId kId = 0x1298;
    std::string text;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("text", text, 128);
        v("name", name, 64);
    }
};

// The Roc's countdowns on the Peak (the original's 0x300C codes 0x0C14 / 0x0C15 with the minutes):
// kind 0 the berserk's (minutes 0: UIIT_MSG_QUEST_ROC_BERSERKER_START, 0xFF: _BERSERKER_AGAIN, else
// UIIT_MSG_QUEST_ROC_BERSERKER "After [%d] Mins later, Roc will start to its berserker mode."), kind 1 the escape's
// (UIIT_MSG_QUEST_ROC_BYEBYE "After [%d] Mins later, Roc will fly away from Mt. Peak.").
struct RocNotice {
    static constexpr MessageId kId = 0x1299;
    static constexpr std::uint8_t kBerserk = 0, kEscape = 1;
    static constexpr std::uint8_t kStarted = 0, kAdvanced = 0xFF;
    std::uint8_t kind = kBerserk;
    std::uint8_t minutes = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("minutes", minutes);
    }
};

// An instance's end coming (the Pharaoh's tomb's GAMEWORLD_DISAPPEARANCE_TIME warnings): the
// client's UIIT_MSG_INSTANCEDUNGEON_ALERT_MSG_01 "After %02d:%02d later, user will move to saved return place." with the
// minutes and seconds left.
struct InstanceAlert {
    static constexpr MessageId kId = 0x129B;
    std::uint32_t seconds = 0;
    template <typename V>
    void Visit(V& v) {
        v("seconds", seconds);
    }
};

// The job temples' news (the original's 0x300C 0x0C17 opening, 0x0C18 closing), to the players in
// the temples' area and the temples: a temple (its dungeon region: the client names it by its zone) and, at the opening,
// who may enter it (0 nobody, 1 the traders and hunters, 2 the thieves, 3 everyone) and whether the alliances' reputations
// were equal; before the opening, the minutes left.
struct TempleNotice {
    static constexpr MessageId kId = 0x129C;
    static constexpr std::uint8_t kBefore = 0, kOpened = 1, kClosed = 2;
    static constexpr std::uint8_t kNobody = 0, kTraderHunter = 1, kThief = 2, kEveryone = 3;
    std::uint8_t kind = kOpened;
    std::uint16_t region = 0;
    std::uint8_t allowed = kEveryone;
    bool equal = false;
    std::uint8_t minutes = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("region", region);
        v("allowed", allowed);
        v("equal", equal);
        v("minutes", minutes);
    }
};

// The temples' reputation board (the original's 0xB4DF answering 0x74DF; the window
// CNIFConfirmReputation, res_ui\reputation.2dt): refused (`ok` false) with the original's code (1 an
// NPC of the other alliance: the client's 0x1C26 UIIT_MSG_EGYPT_MENU_ERROR_UNIONMEMBER "Only members are able to use this.";
// 2 no such NPC near, 3 no league, 4 under level 105: said nothing), else two rows, Anubis's and Isis's pair and Haroeris's
// and Seth's: who may enter (TempleNotice's 0..3), the next opening, since when the reputation is gathered (the last
// decision; Unix seconds, 0 none) and the two alliances' reputation (the traders and hunters, the thieves).
struct TempleReputationRow {
    std::uint8_t allowed = 3;
    std::int64_t nextOpening = 0, since = 0;
    std::uint32_t traderHunter = 0, thief = 0;
    template <typename V>
    void Visit(V& v) {
        v("allowed", allowed);
        v("nextOpening", nextOpening);
        v("since", since);
        v("traderHunter", traderHunter);
        v("thief", thief);
    }
};
struct TempleReputation {
    static constexpr MessageId kId = 0x129D;
    static constexpr std::uint8_t kOtherAlliance = 1, kNoNpc = 2, kNoLeague = 3, kLevel = 4;
    bool ok = false;
    std::uint8_t code = 0;
    std::vector<TempleReputationRow> rows;
    template <typename V>
    void Visit(V& v) {
        v("ok", ok);
        v("code", code);
        v("rows", rows);
    }
};

// The board's own button beside the HUD (the original's 0x34E1, at the world's entry, the guide
// icon 0xA5 CIFConfirmReputationGuide, "AP confirm"), shown to a player of level 105 or more in the temples' area or a temple,
// or with a job quest under way; hidden otherwise.
struct TempleIcon {
    static constexpr MessageId kId = 0x129E;
    bool show = false;
    template <typename V>
    void Visit(V& v) {
        v("show", show);
    }
};

// The board's numbers again (the original's 0xB4E0 answering 0x74E0): the two temples'
// reputations (Anubis's, Haroeris's: the traders and hunters, the thieves), asked every 10 s while the board is shown
// (its timer 1, 10000 ms).
struct TemplePoints {
    static constexpr MessageId kId = 0x129F;
    std::uint32_t anubisTraderHunter = 0, anubisThief = 0, haroerisTraderHunter = 0, haroerisThief = 0;
    template <typename V>
    void Visit(V& v) {
        v("anubisTraderHunter", anubisTraderHunter);
        v("anubisThief", anubisThief);
        v("haroerisTraderHunter", haroerisTraderHunter);
        v("haroerisThief", haroerisThief);
    }
};

// A quest scene to play (the original's 0x3CA2, script\q_script\<name>.txt; the Roc's
// entrance `roc_entrance`, 42 s).
struct QuestScene {
    static constexpr MessageId kId = 0x129A;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 64);
    }
};

// --- From the client -----------------------------------------------------------------------------------------

// The gap selected: its window's facts (GapInfo).
struct SelectGap {
    static constexpr MessageId kId = 0x12A0;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

// Into the gap's instance (refused: InstanceRefused; taken: WorldEntered there, then InstanceState).
struct EnterGap {
    static constexpr MessageId kId = 0x12A1;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

// The talisman in a bag place into the collection book (TalismanRegistered).
struct RegisterTalisman {
    static constexpr MessageId kId = 0x12A2;
    std::uint8_t slot = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
    }
};

// Out of the instance by its exit (NPC_INS_EXIT_TELEPORT, the entity): back where the character entered from.
struct LeaveInstance {
    static constexpr MessageId kId = 0x12A3;
    std::uint32_t exit = 0;
    template <typename V>
    void Visit(V& v) {
        v("exit", exit);
    }
};

// The recall pillar used (its entity): its window's list (SummonList), for the party's master in its instance.
struct OpenSummon {
    static constexpr MessageId kId = 0x12A4;
    std::uint32_t pillar = 0;
    template <typename V>
    void Visit(V& v) {
        v("pillar", pillar);
    }
};

// The window's Summon button on a member's row.
struct SummonMember {
    static constexpr MessageId kId = 0x12A5;
    std::uint32_t character = 0;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
    }
};

// The temples' reputation board asked (the original's 0x74DF): at an NPC that offers it (its row 0x3C
// "Temple area Point": the original's kind 0 and the NPC), or by the HUD's icon (TempleIcon; kind 1: `npc` 0).
struct TempleReputationAsk {
    static constexpr MessageId kId = 0x12A7;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// The board's numbers asked again (TemplePoints; the original's 0x74E0).
struct TemplePointsAsk {
    static constexpr MessageId kId = 0x12A8;
    template <typename V>
    void Visit(V&) {}
};

// The summoned one's answer.
struct SummonReply {
    static constexpr MessageId kId = 0x12A6;
    bool yes = false;
    template <typename V>
    void Visit(V& v) {
        v("yes", yes);
    }
};

// What the server says about the Forgotten World, as the client's session hands it on.
using InstanceEvent = std::variant<GapInfo, InstanceRefused, InstanceNotice, InstanceState, CollectionBook, TalismanRegistered, SummonList, SummonAsked,
                                   SummonResult, RocNotice, QuestScene, InstanceAlert, TempleNotice, TempleReputation, TempleIcon, TemplePoints>;

// A server's instance message as the session's event; false for another id or a malformed payload.
inline bool DecodeInstanceEvent(MessageId id, std::span<const std::byte> payload, InstanceEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case GapInfo::kId: return as(GapInfo{});
    case InstanceRefused::kId: return as(InstanceRefused{});
    case InstanceNotice::kId: return as(InstanceNotice{});
    case InstanceState::kId: return as(InstanceState{});
    case CollectionBook::kId: return as(CollectionBook{});
    case TalismanRegistered::kId: return as(TalismanRegistered{});
    case SummonList::kId: return as(SummonList{});
    case SummonAsked::kId: return as(SummonAsked{});
    case SummonResult::kId: return as(SummonResult{});
    case RocNotice::kId: return as(RocNotice{});
    case QuestScene::kId: return as(QuestScene{});
    case InstanceAlert::kId: return as(InstanceAlert{});
    case TempleNotice::kId: return as(TempleNotice{});
    case TempleReputation::kId: return as(TempleReputation{});
    case TempleIcon::kId: return as(TempleIcon{});
    case TemplePoints::kId: return as(TemplePoints{});
    default: return false;
    }
}

inline const char* InstanceMessageName(MessageId id) {
    switch (id) {
    case GapInfo::kId: return "GapInfo";
    case InstanceRefused::kId: return "InstanceRefused";
    case InstanceNotice::kId: return "InstanceNotice";
    case InstanceState::kId: return "InstanceState";
    case CollectionBook::kId: return "CollectionBook";
    case TalismanRegistered::kId: return "TalismanRegistered";
    case SelectGap::kId: return "SelectGap";
    case EnterGap::kId: return "EnterGap";
    case RegisterTalisman::kId: return "RegisterTalisman";
    case LeaveInstance::kId: return "LeaveInstance";
    case SummonList::kId: return "SummonList";
    case SummonAsked::kId: return "SummonAsked";
    case SummonResult::kId: return "SummonResult";
    case RocNotice::kId: return "RocNotice";
    case QuestScene::kId: return "QuestScene";
    case InstanceAlert::kId: return "InstanceAlert";
    case TempleNotice::kId: return "TempleNotice";
    case TempleReputation::kId: return "TempleReputation";
    case TempleReputationAsk::kId: return "TempleReputationAsk";
    case TempleIcon::kId: return "TempleIcon";
    case TemplePoints::kId: return "TemplePoints";
    case TemplePointsAsk::kId: return "TemplePointsAsk";
    case OpenSummon::kId: return "OpenSummon";
    case SummonMember::kId: return "SummonMember";
    case SummonReply::kId: return "SummonReply";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
