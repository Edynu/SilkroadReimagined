#pragma once

// The Academy over the network: our own messages, requests 0x1420-0x143F and answers 0x1440-0x145F,
// for what the original's carry (requests 0x7470..0x7483, answers 0xB470.., the members 0x3C80 / 0x3C81). The refusals keep
// the original's codes (game/Academy.h academy::code; the client words them as 1.188 does).

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game::proto {

// --- Requests -----------------------------------------------------------------------------------------------------

// Found an academy (0x7470: level 60, 100,000 gold; the storage keeper's row).
struct AcademyFound {
    static constexpr MessageId kId = 0x1420;
    std::uint32_t npc = 0; // the storage keeper whose row asked it (the original's 0x7470 carries it)
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};
// The guardian disbands it (0x7471).
struct AcademyDisband {
    static constexpr MessageId kId = 0x1421;
    template <typename V>
    void Visit(V&) {}
};
// The guardian invites a player he sees (0x7472, the target's entity).
struct AcademyInvite {
    static constexpr MessageId kId = 0x1422;
    std::uint32_t target = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
    }
};
// The invited player's answer.
struct AcademyAnswer {
    static constexpr MessageId kId = 0x1423;
    std::uint8_t accept = 0;
    template <typename V>
    void Visit(V& v) {
        v("accept", accept);
    }
};
// The guardian bans a member by name (0x7473).
struct AcademyBan {
    static constexpr MessageId kId = 0x1424;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 32);
    }
};
// A member leaves (0x7474).
struct AcademyLeave {
    static constexpr MessageId kId = 0x1425;
    template <typename V>
    void Visit(V&) {}
};
// A member graduates (0x7475): an apprentice's rating of its guardian, the judge window's choice 0..4 (academy::kRatingPoints);
// an assistant's 0.
struct AcademyGraduate {
    static constexpr MessageId kId = 0x1426;
    std::uint8_t rating = 0;
    template <typename V>
    void Visit(V& v) {
        v("rating", rating);
    }
};
// The guardian graduates a member by name (0x7476: an apprentice past 40, an assistant past 60; no reward).
struct AcademyGraduateForce {
    static constexpr MessageId kId = 0x1427;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 32);
    }
};
// The guardian writes the academy's notice (0x7477).
struct AcademyNoticeWrite {
    static constexpr MessageId kId = 0x1428;
    std::string title, text;
    template <typename V>
    void Visit(V& v) {
        v("title", title, 128);
        v("text", text, 2048);
    }
};
// The honour ranking asked at a storage keeper (0x7478, the NPC).
struct AcademyRankingAsk {
    static constexpr MessageId kId = 0x1429;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};
// The guardian's academy put on the matching list (0x747A) or its entry changed (0x747B): who it takes
// (academy::MatchKind) and its subject.
struct AcademyMatchRegister {
    static constexpr MessageId kId = 0x142A;
    std::uint8_t kind = 1;
    std::string title;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("title", title, 64);
    }
};
struct AcademyMatchModify {
    static constexpr MessageId kId = 0x142B;
    std::uint8_t kind = 1;
    std::string title;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("title", title, 64);
    }
};
// The guardian's entry taken off the list (0x747C).
struct AcademyMatchDelete {
    static constexpr MessageId kId = 0x142C;
    template <typename V>
    void Visit(V&) {}
};
// A page of the list (0x747D): the search's honour class (0 all, else 1..5), kind (0 all) and guardian's name (empty: any).
struct AcademyMatchListAsk {
    static constexpr MessageId kId = 0x142D;
    std::uint8_t page = 0, honorClass = 0, kind = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("page", page);
        v("honorClass", honorClass);
        v("kind", kind);
        v("name", name, 32);
    }
};
// A player asks to join a listed academy (0x747E).
struct AcademyMatchJoin {
    static constexpr MessageId kId = 0x142E;
    std::uint32_t entry = 0;
    template <typename V>
    void Visit(V& v) {
        v("entry", entry);
    }
};
// The guardian's answer to a join request (0x747F).
struct AcademyMatchReply {
    static constexpr MessageId kId = 0x142F;
    std::uint8_t accept = 0;
    template <typename V>
    void Visit(V& v) {
        v("accept", accept);
    }
};

// --- Answers ------------------------------------------------------------------------------------------------------

// The player's academy as it is now (none: id 0), at the entry and after every change of it.
struct AcademyMember {
    std::uint32_t character = 0;
    std::string name;
    std::uint32_t model = 0;
    std::uint8_t memberClass = 0; // academy::Class
    std::uint8_t grade = 0;       // the title (academy::GradeOf)
    std::uint8_t level = 0, maxLevel = 0;
    std::uint32_t honor = 0;
    std::uint8_t online = 0;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("name", name, 32);
        v("model", model);
        v("memberClass", memberClass);
        v("grade", grade);
        v("level", level);
        v("maxLevel", maxLevel);
        v("honor", honor);
        v("online", online);
    }
};
struct AcademyInfo {
    static constexpr MessageId kId = 0x1440;
    std::uint32_t id = 0;
    std::uint8_t rank = 5; // the honour rank: 1..4, 5 none
    std::uint32_t graduates = 0, evaluation = 0;
    std::int64_t founded = 0; // Unix seconds
    std::string noticeTitle, notice;
    std::vector<AcademyMember> members;
    std::uint32_t matchEntry = 0; // its entry on the matching list (0: none)
    std::uint8_t matchKind = 0;
    std::string matchTitle;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("rank", rank);
        v("graduates", graduates);
        v("evaluation", evaluation);
        v("founded", founded);
        v("noticeTitle", noticeTitle, 128);
        v("notice", notice, 2048);
        v("members", members);
        v("matchEntry", matchEntry);
        v("matchKind", matchKind);
        v("matchTitle", matchTitle, 64);
    }
};
// An invitation to the player (UIIT_STT_TC_JOIN_REQUEST "[%s] has requested for Academy registration. Approve?"); and the same
// question to a guardian whose listed academy a player asks to join (`join` 1, its level).
struct AcademyAsk {
    static constexpr MessageId kId = 0x1441;
    std::string from;
    std::uint8_t join = 0, level = 0;
    template <typename V>
    void Visit(V& v) {
        v("from", from, 32);
        v("join", join);
        v("level", level);
    }
};
// A request refused: the request's id and the original's code (academy::code), the seconds a wait has left.
struct AcademyRefusal {
    static constexpr MessageId kId = 0x1442;
    MessageId request = 0;
    std::uint16_t code = 0;
    std::uint32_t seconds = 0;
    template <typename V>
    void Visit(V& v) {
        v("request", request);
        v("code", code);
        v("seconds", seconds);
    }
};
// What happened in the player's academy, worded by the client (its UIIT_*_TC_* lines); `name` the member it names.
enum class AcademyNewsKind : std::uint8_t {
    Founded = 1,        // UIIT_MSG_TC_OPEN_SUCCESS
    Joined = 2,         // UIIT_STT_TC_JOIN_ANNOUNCE (oneself) / UIIT_STT_TC_JOIN_OTHER_USER
    Left = 3,           // UIIT_STT_TC_OUT_TRAININGCAMP_ANNOUNCE / _OTHER_USER
    Banned = 4,         // UIIT_MSG_TC_EXPULSION_RESULT_ANNOUNCE / UIIT_MSG_TC_EXPULSION_RESULT
    Online = 5,         // UIIT_MSG_TC_CONNECT2 (its class, its name)
    Offline = 6,        // UIIT_MSG_TC_DISCONNECT2
    Disbanded = 7,      // UIIT_STT_TC_DISSOLUTION_SUCCESS
    Pending = 8,        // UIIT_STT_TC_PROPOSAL_PENDING (the inviter's)
    NoReply = 9,        // UIIT_MSG_TC_JOIN_NOREPLY
    Graduated = 10,     // UIIT_STT_TC_GRADUATION_CONGRATULATIONS (oneself) / UIIT_STT_TC_GRADUATION_ANNOUNCE
    NoticeChanged = 11, // UIIT_MSG_TC_COMMON_KNOW_REMIND_UPDATE
    MatchEntered = 12,  // UIIT_STT_TC_MACHING_ENTRY
    MatchChanged = 13,  // UIIT_STT_TC_MACHING_CORRECT
    MatchDeleted = 14,  // UIIT_STT_TC_MACHING_DELETE
    MatchPulled = 15,   // UIIT_STT_TC_MACHING_PULL_DELETE (full: taken off the list)
    JoinCancelled = 16, // UIIT_MSG_TC_JOIN_CANCEL (the guardian said no)
    JoinComplete = 17,  // UIIT_STT_TC_MACHING_COMPLETE
};
struct AcademyNews {
    static constexpr MessageId kId = 0x1443;
    AcademyNewsKind kind = AcademyNewsKind::Joined;
    std::string name;
    std::uint8_t memberClass = 0;
    std::uint8_t self = 0; // it names the player himself
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("name", name, 32);
        v("memberClass", memberClass);
        v("self", self);
    }
};
// The honour ranking (0xB478): the 50 places' academies, their guardians.
struct AcademyRankRow {
    std::uint8_t place = 0, rank = 5;
    std::string guardian;
    std::uint8_t level = 0;
    std::uint32_t graduates = 0;
    std::string guild;
    template <typename V>
    void Visit(V& v) {
        v("place", place);
        v("rank", rank);
        v("guardian", guardian, 32);
        v("level", level);
        v("graduates", graduates);
        v("guild", guild, 32);
    }
};
struct AcademyRanking {
    static constexpr MessageId kId = 0x1444;
    std::vector<AcademyRankRow> rows;
    template <typename V>
    void Visit(V& v) {
        v("rows", rows);
    }
};
// A page of the matching list (0xB47D).
struct AcademyMatchRow {
    std::uint32_t entry = 0;
    std::uint8_t level = 0, race = 0; // the guardian's; race 0 China, 1 Europe
    std::string guardian, title;
    std::uint32_t graduates = 0;
    std::uint8_t members = 0, rank = 5, kind = 1;
    template <typename V>
    void Visit(V& v) {
        v("entry", entry);
        v("level", level);
        v("race", race);
        v("guardian", guardian, 32);
        v("title", title, 64);
        v("graduates", graduates);
        v("members", members);
        v("rank", rank);
        v("kind", kind);
    }
};
struct AcademyMatchList {
    static constexpr MessageId kId = 0x1445;
    std::uint8_t page = 0, pages = 0;
    std::vector<AcademyMatchRow> rows;
    template <typename V>
    void Visit(V& v) {
        v("page", page);
        v("pages", pages);
        v("rows", rows);
    }
};
// A join request's progress for the one who asked (0xB47E): 1 waiting for the guardian (the progress box), 0 over (the
// news says how).
struct AcademyMatchProgress {
    static constexpr MessageId kId = 0x1446;
    std::uint8_t waiting = 0;
    template <typename V>
    void Visit(V& v) {
        v("waiting", waiting);
    }
};

// --- The experience buffs -----------------------------------------------------------------

// An apprentice's accumulation (the original's 0x3C82 and 0x3C86): `kind` 0 its 24 hours begun, 1 as it stands (the
// entry), 7 its time over (UIIT_CTL_TC_SAVE_BUFF_TIME_OVER; the points wait to be passed), 5 / 8 / 9 gone (passed, the
// apprentice at level 40, left); 10 (ours, the 0x3C86) its minutes left alone. The points as last told (the original
// tells them only with kinds 0, 1 and 7).
struct AcademyAccumulation {
    static constexpr MessageId kId = 0x1447;
    std::uint8_t kind = 0;
    std::uint32_t minutes = 0, points = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("minutes", minutes);
        v("points", points);
    }
};
// A buff received from another member's points (the original's 0xB483 and 0x3C87): `kind` 0 one received
// (UIIT_MSG_TC_RECIEVE_BUFF), 1 the list (the entry), 2 a pass refused (UIIT_MSG_TC_PASS_ERROR: the guardian is not on),
// 5 / 8 / 9 gone (spent or its day over, level 40, its donor left); 10 (ours, the 0x3C87) its minutes left alone.
struct AcademyReserveEntry {
    std::uint32_t donor = 0; // the donor's character
    std::uint32_t minutes = 0, points = 0;
    template <typename V>
    void Visit(V& v) {
        v("donor", donor);
        v("minutes", minutes);
        v("points", points);
    }
};
struct AcademyReserve {
    static constexpr MessageId kId = 0x1448;
    std::uint8_t kind = 0;
    std::vector<AcademyReserveEntry> entries;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("entries", entries);
    }
};
// Where a member is (ours every 5 s when it moved more than 10): the world map's mark (wmap_sign_apprenticeship.ddj).
struct AcademyPlace {
    static constexpr MessageId kId = 0x1449;
    std::uint32_t character = 0;
    WorldPosition position;
    template <typename V>
    void Visit(V& v) {
        v("character", character);
        v("position", position);
    }
};
// The apprentice passes its accumulated points (the original's 0x7483; a right click on its board icon an hour after
// the accumulation began, then UIIT_CTL_TC_PASS_BUFF's box).
struct AcademyPassBuff {
    static constexpr MessageId kId = 0x1430;
    template <typename V>
    void Visit(V&) {}
};

using AcademyEvent = std::variant<AcademyInfo, AcademyAsk, AcademyRefusal, AcademyNews, AcademyRanking, AcademyMatchList, AcademyMatchProgress,
                                  AcademyAccumulation, AcademyReserve, AcademyPlace>;

inline bool IsAcademyMessage(MessageId id) { return id >= 0x1420 && id < 0x1460; }
// The requests (the client's), the rest the server's.
inline bool IsAcademyRequest(MessageId id) { return id >= 0x1420 && id < 0x1440; }

inline bool DecodeAcademyEvent(MessageId id, std::span<const std::byte> payload, AcademyEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case AcademyInfo::kId: return as(AcademyInfo{});
    case AcademyAsk::kId: return as(AcademyAsk{});
    case AcademyRefusal::kId: return as(AcademyRefusal{});
    case AcademyNews::kId: return as(AcademyNews{});
    case AcademyRanking::kId: return as(AcademyRanking{});
    case AcademyMatchList::kId: return as(AcademyMatchList{});
    case AcademyMatchProgress::kId: return as(AcademyMatchProgress{});
    case AcademyAccumulation::kId: return as(AcademyAccumulation{});
    case AcademyReserve::kId: return as(AcademyReserve{});
    case AcademyPlace::kId: return as(AcademyPlace{});
    default: return false;
    }
}

inline const char* AcademyMessageName(MessageId id) {
    switch (id) {
    case AcademyFound::kId: return "AcademyFound";
    case AcademyDisband::kId: return "AcademyDisband";
    case AcademyInvite::kId: return "AcademyInvite";
    case AcademyAnswer::kId: return "AcademyAnswer";
    case AcademyBan::kId: return "AcademyBan";
    case AcademyLeave::kId: return "AcademyLeave";
    case AcademyGraduate::kId: return "AcademyGraduate";
    case AcademyGraduateForce::kId: return "AcademyGraduateForce";
    case AcademyNoticeWrite::kId: return "AcademyNoticeWrite";
    case AcademyRankingAsk::kId: return "AcademyRankingAsk";
    case AcademyMatchRegister::kId: return "AcademyMatchRegister";
    case AcademyMatchModify::kId: return "AcademyMatchModify";
    case AcademyMatchDelete::kId: return "AcademyMatchDelete";
    case AcademyMatchListAsk::kId: return "AcademyMatchListAsk";
    case AcademyMatchJoin::kId: return "AcademyMatchJoin";
    case AcademyMatchReply::kId: return "AcademyMatchReply";
    case AcademyInfo::kId: return "AcademyInfo";
    case AcademyAsk::kId: return "AcademyAsk";
    case AcademyRefusal::kId: return "AcademyRefusal";
    case AcademyNews::kId: return "AcademyNews";
    case AcademyRanking::kId: return "AcademyRanking";
    case AcademyMatchList::kId: return "AcademyMatchList";
    case AcademyMatchProgress::kId: return "AcademyMatchProgress";
    case AcademyAccumulation::kId: return "AcademyAccumulation";
    case AcademyReserve::kId: return "AcademyReserve";
    case AcademyPassBuff::kId: return "AcademyPassBuff";
    case AcademyPlace::kId: return "AcademyPlace";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
