#pragma once

// The client's view of the player's academy: the server's AcademyEvents applied, the members as
// the Academy page shows them, the lines the client writes for its news and refusals (their 1.188 text keys and
// arguments), an invitation waiting for the player's answer.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sr/game/Academy.h"
#include "sr/game/AcademyProtocol.h"

namespace sr::game {

class AcademyView {
public:
    // A line for the system messages: a text key and what fills its %s in order (an argument starting with @ is itself
    // a text key).
    struct Line {
        std::string key;
        std::vector<std::string> args;
    };

    // `self`: the player's name (its own row, its own news); `time` the HUD's clock (the buffs' ends).
    std::vector<Line> Apply(const proto::AcademyEvent& event, const std::string& self, double time = 0.0);

    // The experience buffs on the board: an apprentice's accumulation (its icon
    // tc_amends_buff.ddj) and the buffs received (tc_reserve_buff.ddj), each ending a day after it began.
    struct Accumulation {
        bool shown = false;
        bool over = false;     // its time over: the points wait to be passed
        double ends = 0.0;     // the HUD's clock
        std::uint32_t points = 0;
    };
    struct Reserve {
        std::uint32_t donor = 0;
        double ends = 0.0;
        std::uint32_t points = 0;
    };
    const Accumulation& Accumulated() const { return accumulation_; }
    const std::vector<Reserve>& Reserves() const { return reserves_; }
    // A member's name by its character (empty: not a member now).
    std::string NameOf(std::uint32_t character) const;
    // Where the other members were last told to be (the world map's marks; AcademyPlace).
    const std::map<std::uint32_t, proto::WorldPosition>& Places() const { return places_; }

    bool InAcademy() const { return info_.id != 0; }
    const proto::AcademyInfo& Info() const { return info_; }
    const proto::AcademyMember* Guardian() const;
    const proto::AcademyMember* Find(const std::string& name) const;
    // The assistants then the apprentices, in the order the server lists them (assistants in the first two
    // slots, apprentices in the five after).
    std::vector<const proto::AcademyMember*> OfClass(academy::Class c) const;
    // The player is the guardian.
    bool IsGuardian(const std::string& self) const;

    // An invitation, or (`join`) a join request to the guardian, not yet put to the player.
    std::optional<proto::AcademyAsk> TakeAsk();

    // The honour ranking's last answer and the matching list's (`version` changes with each).
    const proto::AcademyRanking& Ranking() const { return ranking_; }
    const proto::AcademyMatchList& Matches() const { return matches_; }
    std::uint64_t version = 0;
    // A ranking arrived for the window to open (taken once).
    bool TakeRankingArrived() {
        const bool r = rankingArrived_;
        rankingArrived_ = false;
        return r;
    }
    // A join request of the player's waiting for the guardian (the progress box), since `joiningAt` (the HUD's clock).
    bool joining = false;
    double joiningAt = 0.0;
    std::string joiningGuardian;

private:
    proto::AcademyInfo info_;
    std::optional<proto::AcademyAsk> ask_;
    proto::AcademyRanking ranking_;
    proto::AcademyMatchList matches_;
    bool rankingArrived_ = false;
    Accumulation accumulation_;
    std::vector<Reserve> reserves_;
    std::map<std::uint32_t, proto::WorldPosition> places_;
};

// A member's title key (by its grade 1..7), empty when none.
const char* AcademyGradeKey(std::uint8_t grade);
// A class's name in the online and offline lines (UIIT_STT_TC_GUARDIAN, _ASSISTANT_GUARDIAN, _STUDENT).
const char* AcademyClassKey(std::uint8_t memberClass);

} // namespace sr::game
