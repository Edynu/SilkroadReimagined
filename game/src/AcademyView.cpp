#include "sr/game/AcademyView.h"

#include <algorithm>

namespace sr::game {

std::vector<AcademyView::Line> AcademyView::Apply(const proto::AcademyEvent& event, const std::string& self, double time) {
    std::vector<Line> lines;
    ++version;
    const auto ends = [time](std::uint32_t minutes) { return time + double(minutes) * 60.0; };
    if (const auto* info = std::get_if<proto::AcademyInfo>(&event)) {
        info_ = *info;
        if (info_.id == 0) {
            accumulation_ = {};
            reserves_.clear();
            places_.clear();
        }
    } else if (const auto* place = std::get_if<proto::AcademyPlace>(&event)) {
        places_[place->character] = place->position;
    } else if (const auto* acc = std::get_if<proto::AcademyAccumulation>(&event)) {
        // The original's 0x3C82 and 0x3C86.
        switch (acc->kind) {
        case 0:
        case 1: accumulation_ = {true, false, ends(acc->minutes), acc->points}; break;
        case 7:
            accumulation_ = {true, true, time, acc->points};
            lines.push_back({"UIIT_CTL_TC_SAVE_BUFF_TIME_OVER", {}});
            break;
        case 10:
            if (accumulation_.shown && !accumulation_.over) accumulation_.ends = ends(acc->minutes);
            break;
        default: accumulation_ = {}; break;
        }
    } else if (const auto* reserve = std::get_if<proto::AcademyReserve>(&event)) {
        // The original's 0xB483 and 0x3C87.
        const auto find = [this](std::uint32_t donor) {
            return std::find_if(reserves_.begin(), reserves_.end(), [donor](const Reserve& r) { return r.donor == donor; });
        };
        switch (reserve->kind) {
        case 0:
        case 1:
            if (reserve->kind == 1) reserves_.clear();
            for (const proto::AcademyReserveEntry& e : reserve->entries) {
                const auto it = find(e.donor);
                if (it != reserves_.end()) *it = {e.donor, ends(e.minutes), e.points};
                else reserves_.push_back({e.donor, ends(e.minutes), e.points});
                // "You received a buff from %s." (the donor named when it is a member).
                if (reserve->kind == 0) {
                    if (const std::string name = NameOf(e.donor); !name.empty()) lines.push_back({"UIIT_MSG_TC_RECIEVE_BUFF", {name}});
                }
            }
            break;
        case 2: lines.push_back({"UIIT_MSG_TC_PASS_ERROR", {}}); break;
        case 10:
            for (const proto::AcademyReserveEntry& e : reserve->entries) {
                if (const auto it = find(e.donor); it != reserves_.end()) it->ends = ends(e.minutes);
            }
            break;
        default:
            for (const proto::AcademyReserveEntry& e : reserve->entries) {
                if (const auto it = find(e.donor); it != reserves_.end()) reserves_.erase(it);
            }
            break;
        }
    } else if (const auto* ask = std::get_if<proto::AcademyAsk>(&event)) {
        ask_ = *ask;
    } else if (const auto* ranking = std::get_if<proto::AcademyRanking>(&event)) {
        ranking_ = *ranking;
        rankingArrived_ = true;
    } else if (const auto* list = std::get_if<proto::AcademyMatchList>(&event)) {
        matches_ = *list;
        // A search that found none (1.188's matching list says so in the system messages).
        if (list->rows.empty()) lines.push_back({"UIIT_STT_TC_MACHING_REFERENCE_ERROR", {}});
    } else if (const auto* progress = std::get_if<proto::AcademyMatchProgress>(&event)) {
        joining = progress->waiting != 0;
    } else if (const auto* refusal = std::get_if<proto::AcademyRefusal>(&event)) {
        if (const char* key = academy::RefusalKey(refusal->code); key != nullptr && *key != '\0') lines.push_back({key, {}});
    } else if (const auto* news = std::get_if<proto::AcademyNews>(&event)) {
        // The words of 1.188's handlers: oneself or another member named.
        using K = proto::AcademyNewsKind;
        const bool own = news->self != 0 || news->name == self;
        switch (news->kind) {
        case K::Founded: lines.push_back({"UIIT_MSG_TC_OPEN_SUCCESS", {}}); break;
        case K::Joined:
            lines.push_back(own ? Line{"UIIT_STT_TC_JOIN_ANNOUNCE", {}} : Line{"UIIT_MSG_TC_MACHING_JOINING_MEMBER", {news->name}});
            break;
        case K::Left:
            lines.push_back(own ? Line{"UIIT_STT_TC_OUT_TRAININGCAMP_ANNOUNCE", {}} : Line{"UIIT_STT_TC_OUT_TRAININGCAMP_OTHER_USER", {news->name}});
            break;
        case K::Banned:
            lines.push_back(own ? Line{"UIIT_MSG_TC_EXPULSION_RESULT_ANNOUNCE", {}} : Line{"UIIT_MSG_TC_EXPULSION_RESULT", {news->name}});
            break;
        case K::Online: lines.push_back({"UIIT_MSG_TC_CONNECT2", {std::string("@") + AcademyClassKey(news->memberClass), news->name}}); break;
        case K::Offline: lines.push_back({"UIIT_MSG_TC_DISCONNECT2", {std::string("@") + AcademyClassKey(news->memberClass), news->name}}); break;
        case K::Disbanded: lines.push_back({"UIIT_STT_TC_DISSOLUTION_SUCCESS", {}}); break;
        case K::Pending: lines.push_back({"UIIT_STT_TC_PROPOSAL_PENDING", {news->name}}); break;
        case K::NoReply: lines.push_back({"UIIT_MSG_TC_JOIN_NOREPLY", {}}); break;
        case K::Graduated:
            lines.push_back(own ? Line{"UIIT_STT_TC_GRADUATION_CONGRATULATIONS", {}} : Line{"UIIT_STT_TC_GRADUATION_ANNOUNCE", {news->name}});
            break;
        case K::NoticeChanged: lines.push_back({"UIIT_MSG_TC_COMMON_KNOW_REMIND_UPDATE", {}}); break;
        case K::MatchEntered: lines.push_back({"UIIT_STT_TC_MACHING_ENTRY", {}}); break;
        case K::MatchChanged: lines.push_back({"UIIT_STT_TC_MACHING_CORRECT", {}}); break;
        case K::MatchDeleted: lines.push_back({"UIIT_STT_TC_MACHING_DELETE", {}}); break;
        case K::MatchPulled: lines.push_back({"UIIT_STT_TC_MACHING_PULL_DELETE", {}}); break;
        case K::JoinCancelled: lines.push_back({"UIIT_MSG_TC_JOIN_CANCEL", {}}); break;
        case K::JoinComplete: lines.push_back({"UIIT_STT_TC_MACHING_COMPLETE", {}}); break;
        }
    }
    return lines;
}

const proto::AcademyMember* AcademyView::Guardian() const {
    for (const proto::AcademyMember& m : info_.members) {
        if (m.memberClass == std::uint8_t(academy::Class::Guardian)) return &m;
    }
    return nullptr;
}

std::string AcademyView::NameOf(std::uint32_t character) const {
    for (const proto::AcademyMember& m : info_.members) {
        if (m.character == character) return m.name;
    }
    return {};
}

const proto::AcademyMember* AcademyView::Find(const std::string& name) const {
    for (const proto::AcademyMember& m : info_.members) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

std::vector<const proto::AcademyMember*> AcademyView::OfClass(academy::Class c) const {
    std::vector<const proto::AcademyMember*> out;
    for (const proto::AcademyMember& m : info_.members) {
        if (m.memberClass == std::uint8_t(c)) out.push_back(&m);
    }
    return out;
}

bool AcademyView::IsGuardian(const std::string& self) const {
    const proto::AcademyMember* g = Guardian();
    return g != nullptr && g->name == self;
}

std::optional<proto::AcademyAsk> AcademyView::TakeAsk() {
    auto ask = std::move(ask_);
    ask_.reset();
    return ask;
}

const char* AcademyGradeKey(std::uint8_t grade) {
    switch (grade) {
    case 1: return "UIIT_STT_TC_FIRST_STUDENT";
    case 2: return "UIIT_STT_TC_STUDENT";
    case 3: return "UIIT_STT_TC_SECOND_STUDENT";
    case 4: return "UIIT_STT_TC_THIRD_STUDENT";
    case 5: return "UIIT_STT_TC_FIRST_GUARDIAN";
    case 6: return "UIIT_STT_TC_SECOND_GUARDIAN";
    case 7: return "UIIT_STT_TC_GUARDIAN";
    default: return "";
    }
}

const char* AcademyClassKey(std::uint8_t memberClass) {
    switch (academy::Class(memberClass)) {
    case academy::Class::Guardian: return "UIIT_STT_TC_GUARDIAN";
    case academy::Class::Assistant: return "UIIT_STT_TC_ASSISTANT_GUARDIAN";
    default: return "UIIT_STT_TC_STUDENT";
    }
}

} // namespace sr::game
