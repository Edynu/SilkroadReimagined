#include "sr/game/Party.h"

#include <algorithm>
#include <cmath>
#include <variant>

namespace sr::game {

PartyExperience PartyExperienceShares(const std::vector<PartyKiller>& eligible, int monsterLevel, bool partyMonster) {
    PartyExperience out;
    if (eligible.empty()) return out;
    int europeans = 0, chinese = 0;
    for (const PartyKiller& k : eligible) {
        (k.european ? europeans : chinese) += 1;
        out.levelSum += k.level;
        out.maxLevel = std::max(out.maxLevel, k.level);
    }
    const int count = int(eligible.size());
    // The members' bonus: kept as a float, as the original keeps it.
    float bonus = 1.0f;
    if (europeans > 0) {
        bonus = float(1.0 + double(europeans - 1) * 0.2 + double(float(double(chinese) * 0.1)));
    } else if (chinese > 0 && chinese == count) {
        bonus = float(1.0 + double(chinese - 1) * 0.1);
    }
    out.bonus = bonus;
    out.averageLevel = out.levelSum > 0 ? out.levelSum / count : 0;
    // A party monster below the members' average level gives half.
    const float scale = partyMonster && monsterLevel < out.averageLevel ? 0.5f : 1.0f;
    for (const PartyKiller& k : eligible) {
        const float share = out.levelSum > 0 ? float(k.level) / float(out.levelSum) : 0.0f;
        float f = bonus;
        // A member taking over 0.7 of three or more, the highest of them 21 or more: half the bonus.
        if (share > 0.7f && count >= 3 && out.maxLevel >= 21) f = float(1.0 + (double(bonus) - 1.0) * 0.5);
        out.factors.push_back(double(float(share * f * scale)));
    }
    return out;
}

double SoloExperienceFactor(int partyMembers, int playerLevel, int monsterLevel, bool partyMonster) {
    float factor = 1.0f;
    if (partyMembers > 0) factor = float(double(partyMembers - 1) * 0.03 + 1.0);
    if (partyMonster && monsterLevel < playerLevel) factor *= 0.5f;
    return double(factor);
}

std::vector<std::uint64_t> SplitPartyGold(std::uint64_t amount, std::size_t receivers) {
    if (receivers == 0) return {};
    std::vector<std::uint64_t> out(receivers, amount / receivers);
    out[0] += amount % receivers; // the picker's
    return out;
}

std::uint8_t PartyTenths(std::uint32_t value, std::uint32_t most, bool atLeastOne) {
    if (most == 0) return 0;
    const std::uint64_t tenths = std::min<std::uint64_t>(10, std::uint64_t(value) * 10 / most);
    if (atLeastOne && value > 0 && tenths == 0) return 1;
    return std::uint8_t(tenths);
}

const char* PartyStatusText(proto::PartyStatus status) {
    using S = proto::PartyStatus;
    switch (status) {
    case S::Ok: return "";
    case S::Unknown: return "UIIT_MSG_PARTYERR_UNKNOWN_ERROR";
    case S::InvalidTarget: return "UIIT_MSG_PARTYERR_INVALID_TARGET";
    case S::TooFar: return "UIIT_MSG_INVIET_PARTY_FAR";
    case S::MatchBusy: return "UIIT_MSG_PARTYMATCH_ERROR_BUSY";
    case S::OutOfLevel: return "UIIT_MSG_PARTYERR_OUT_OF_LEVEL";
    case S::AlreadyFull: return "UIIT_MSG_PARTYERR_ALREADY_FULL";
    case S::Busy: return "UIIT_MSG_PARTYERR_TARGET_BUSY";
    case S::LowLevel: return "UIIT_MSG_PARTYERR_TOO_LOW_CREATOR_LEVEL";
    case S::Refused: return "UIIT_MSG_PARTYERR_CREATE_PARTY_REFUSED";
    case S::NoParty: return "UIIT_MSG_PARTYERR_CANT_FIND_PARTY";
    case S::NoInviter: return "UIIT_MSG_PARTYERR_CANT_FIND_CREATER";
    case S::InvalidUser: return "UIIT_MSG_PARTYERR_INVALID_USER";
    case S::Timeout: return "UIIT_MSG_PARTYERR_TIMEOUT";
    case S::LeaderLeft: return "UIIT_MSG_PARTYERR_CREATER_LEFT_PARTY";
    case S::ExistingMember: return "UIIT_MSG_PARTYMATCH_JOIN_ERROR_PARTY";
    case S::FullShared: return "UIIT_MSG_PARTYERR_MEMBER_FULL_EXP_SHARED_PARTY";
    case S::FullFree: return "UIIT_MSG_PARTYERR_MEMBER_FULL_NONEEXP_SHARED_PARTY";
    case S::JoinRefused: return "UIIT_MSG_PARTYERR_JOIN_PARTY_REFUSED";
    case S::OtherParty: return "UIIT_MSG_PARTYERR_MEMBER_OF_ANOTHER_PARTY";
    case S::MatchAgain: return "UIIT_MSG_PARTYMATCH_RECORD_ERROR_AGAIN";
    case S::MatchNone: return "UIIT_MSG_PARTYERR_CANT_FIND_PARTY";
    case S::MatchLeader: return "UIIT_MSG_PARTYMATCH_RECORD_ERROR_PARTYLEADER";
    case S::MatchLevel: return "UIIT_MSG_PARTYMATCH_JOIN_ERROR_LEVEL";
    case S::MatchJoining: return "UIIT_MSG_PARTYMATCH_JOIN_ERROR_DUPLE";
    case S::MatchListed: return "UIIT_MSG_PARTYMATCH_RECORD_ERROR_INVITE";
    case S::JobMismatch: return "UIIT_MSG_PARTY_USER_MISSMATCH"; //, the Party line only
    case S::JobChanging: return "UIIT_MSG_JOBITEM_WEAR_ERR_PARTYMATCH";
    case S::ArenaOpponent: return "UIIT_MGS_ARENA_ERR_CANT_PARTY_WITH_OPPONENT_IN_BATTLE_ARENA";
    case S::ArenaMatching: return "UIIT_MGS_ARENA_ERR_FREGIST_PARTY_IN_BATTLE_ARENA";
    case S::NoAlias: return "UIIT_MSG_PARTYERR_UNKNOWN_OPERATION"; // code 5: "Wrong calculation." (1.188 group 2)
    case S::NotLeader: return "UIIT_MSG_PARTYMATCH_RECORD_ERROR_PARTYLEADER";
    }
    return "UIIT_MSG_PARTYERR_UNKNOWN_ERROR";
}

std::uint32_t PartyView::SelfId() const {
    const proto::PartyMemberRecord* self = MemberByEntity(selfEntity_);
    return self != nullptr ? self->id : 0;
}

const proto::PartyMemberRecord* PartyView::Member(std::uint32_t id) const {
    const auto it = std::find_if(members.begin(), members.end(), [&](const proto::PartyMemberRecord& m) { return m.id == id; });
    return it != members.end() ? &*it : nullptr;
}

const proto::PartyMemberRecord* PartyView::MemberByEntity(std::uint32_t entity) const {
    if (entity == 0) return nullptr;
    const auto it = std::find_if(members.begin(), members.end(), [&](const proto::PartyMemberRecord& m) { return m.entity == entity; });
    return it != members.end() ? &*it : nullptr;
}

std::vector<const proto::PartyMemberRecord*> PartyView::Others() const {
    std::vector<const proto::PartyMemberRecord*> out;
    for (const proto::PartyMemberRecord& m : members) {
        if (m.entity != selfEntity_ || selfEntity_ == 0) out.push_back(&m);
    }
    return out;
}

std::string PartyView::NameOf(std::uint32_t id) const {
    const proto::PartyMemberRecord* m = Member(id);
    return m != nullptr ? m->name : std::string();
}

void PartyView::Clear() {
    party = 0;
    settings = 0;
    leader = 0;
    members.clear();
}

std::pair<std::uint32_t, std::uint32_t> DominantMasteries(const std::map<std::uint32_t, int>& masteries) {
    std::vector<std::pair<int, std::uint32_t>> trained;
    for (const auto& [id, level] : masteries) {
        if (level > 0) trained.push_back({level, id});
    }
    std::stable_sort(trained.begin(), trained.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    return {trained.size() > 0 ? trained[0].second : 0u, trained.size() > 1 ? trained[1].second : 0u};
}

bool PartyView::Apply(const TitleEvent& event, double now) {
    if (const auto* invited = std::get_if<proto::PartyInvited>(&event)) {
        invitation = *invited;
        invitedAt = now;
    } else if (const auto* result = std::get_if<proto::PartyResult>(&event)) {
        if (result->status != proto::PartyStatus::Ok) notices.push_back({PartyStatusText(result->status), {result->name}});
        // A request the player had, answered for it (timed out): its box goes.
        if (result->action == proto::PartyAction::Answer) invitation.reset();
    } else if (const auto* info = std::get_if<proto::PartyInfo>(&event)) {
        party = info->party;
        settings = info->settings;
        leader = info->leader;
        members = info->members;
        invitation.reset();
    } else if (const auto* joined = std::get_if<proto::PartyMemberJoined>(&event)) {
        if (Member(joined->member.id) == nullptr) members.push_back(joined->member);
        notices.push_back({"UIIT_MSG_PARTY_JOIN", {joined->member.name}});
    } else if (const auto* left = std::get_if<proto::PartyMemberLeft>(&event)) {
        // The reason's line with the member's name, then, when it is the player, the
        // party is gone for it.
        const std::string name = NameOf(left->member);
        switch (left->reason) {
        case proto::PartyLeaveReason::Logout: notices.push_back({"UIIT_MSG_PARTY_LOGOUT", {name}}); break;
        case proto::PartyLeaveReason::Left: notices.push_back({"UIIT_MSG_PARTY_SECEDE", {name}}); break;
        case proto::PartyLeaveReason::Kicked: notices.push_back({"UIIT_MSG_PARTY_BOOTED", {name}}); break;
        case proto::PartyLeaveReason::Migration: break;
        }
        if (left->member == SelfId()) {
            Clear();
        } else {
            members.erase(std::remove_if(members.begin(), members.end(), [&](const proto::PartyMemberRecord& m) { return m.id == left->member; }),
                          members.end());
        }
    } else if (std::holds_alternative<proto::PartyDismissed>(event)) {
        Clear();
        notices.push_back({"UIIT_MSG_PARTY_BROKEN", {}});
    } else if (const auto* update = std::get_if<proto::PartyMemberUpdate>(&event)) {
        auto it = std::find_if(members.begin(), members.end(), [&](const proto::PartyMemberRecord& m) { return m.id == update->member; });
        if (it == members.end()) return true;
        if ((update->mask & proto::kPartyLevel) != 0) it->level = update->level;
        if ((update->mask & proto::kPartyVitals) != 0) {
            it->hpTenths = update->hpTenths;
            it->mpTenths = update->mpTenths;
        }
        if ((update->mask & proto::kPartyPosition) != 0) it->position = update->position;
        if ((update->mask & proto::kPartyEntity) != 0) it->entity = update->entity;
        if ((update->mask & proto::kPartyMasteries) != 0) {
            it->mastery1 = update->mastery1;
            it->mastery2 = update->mastery2;
        }
    } else if (const auto* changed = std::get_if<proto::PartyLeaderChanged>(&event)) {
        leader = changed->member;
        notices.push_back({"UIIT_MSG_PARTY_HOST_ROLE_DELEGATED", {NameOf(changed->member)}});
    } else if (const auto* set = std::get_if<proto::PartySettingsChanged>(&event)) {
        settings = set->settings;
        notices.push_back({(settings & kPartyExpShare) != 0 ? "UIIT_STT_PARTY_EXP_SHARE" : "UIIT_STT_PARTY_EXP_SELF", {}});
        notices.push_back({(settings & kPartyItemShare) != 0 ? "UIIT_STT_PARTY_ITEM_SHARE" : "UIIT_STT_PARTY_ITEM_SELF", {}});
    } else if (const auto* loot = std::get_if<proto::PartyLoot>(&event)) {
        // "Item [%s %d pieces] is distributed to [%s]." / "[%d] gold is distributed to [%s].";
        // the item's name is the client's: its refId as "#item:<id>".
        if (loot->refId != 0) {
            notices.push_back({"UIIT_MSG_PARTYGET_ITEM_EXPENDABLE", {"#item:" + std::to_string(loot->refId), std::to_string(loot->count), NameOf(loot->member)}});
        } else {
            notices.push_back({"UIIT_MSG_PARTYGET_GOLD", {std::to_string(loot->gold), NameOf(loot->member)}});
        }
    } else if (const auto* match = std::get_if<proto::PartyMatchAnswer>(&event)) {
        using A = proto::PartyMatchAction;
        const bool ok = match->status == proto::PartyStatus::Ok;
        switch (match->action) {
        case A::Register:
        case A::Modify:
            if (ok) {
                ownEntry = match->entry;
                notices.push_back({match->action == A::Register ? "UIIT_MSG_PARTYMATCH_RECORD_COMPLETE" : "UIIT_MSG_PARTYMATCH_MODIFY_COMPLETE", {}});
            }
            break;
        case A::Delete:
            if (ok) {
                ownEntry = 0;
                notices.push_back({"UIIT_MSG_PARTYMATCH_DELETE_COMPLETE", {}});
            }
            break;
        case A::Removed:
            ownEntry = 0;
            notices.push_back({match->status == proto::PartyStatus::AlreadyFull ? "UIIT_MSG_PARTYMATCH_DELETE_COMPLETE2" : "UIIT_MSG_PARTYMATCH_DELETE_COMPLETE", {}});
            break;
        case A::Join:
            // Sent: waiting for the leader (the joining window); refused before it was sent.
            if (ok) {
                joining = match->entry;
                joiningAt = now;
            } else {
                joining = 0;
            }
            break;
        case A::Answer:
            if (joining != 0) {
                // The leader's answer to the player's request.
                joining = 0;
                if (ok) notices.push_back({"UIIT_MSG_PARTYMATCH_JOIN_COMPLETE_MEMBER", {}});
                else if (match->status == proto::PartyStatus::Timeout) notices.push_back({"UIIT_MSG_PARTYMATCH_JOIN_NOREPLY", {}});
                else notices.push_back({"UIIT_MSG_PARTYERR_JOIN_PARTY_REFUSED", {}});
                ++version;
                return true;
            }
            // The leader's request window: answered, or timed out.
            joinRequest.reset();
            if (match->status == proto::PartyStatus::Timeout) notices.push_back({"UIIT_MSG_PARTYMATCH_JOIN_NOREPLY", {}});
            ++version;
            return true;
        }
        if (!ok && match->action != A::Removed) notices.push_back({PartyStatusText(match->status), {}});
    } else if (const auto* listing = std::get_if<proto::PartyMatchListing>(&event)) {
        this->listing = *listing;
        ownEntry = listing->own;
    } else if (const auto* request = std::get_if<proto::PartyMatchJoinRequest>(&event)) {
        joinRequest = *request;
        joinRequestAt = now;
    } else if (const auto* entered = std::get_if<proto::WorldEntered>(&event)) {
        if (entered->status == proto::WorldStatus::Ok) selfEntity_ = entered->entity;
        return false;
    } else if (std::holds_alternative<proto::WorldLeft>(event)) {
        // Out of the world: out of the party (the server takes the player out).
        Clear();
        invitation.reset();
        joinRequest.reset();
        joining = 0;
        ownEntry = 0;
        ++version;
        return false;
    } else {
        return false;
    }
    ++version;
    return true;
}

} // namespace sr::game
