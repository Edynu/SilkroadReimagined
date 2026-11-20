#pragma once

// Parties and party matching: the rules the client and our server share, from the original game server, and the
// client's picture of its party and of the matching listings, fed by the session's party events. Headless.

#include <cstdint>
#include <map>
#include <utility>
#include <optional>
#include <string>
#include <vector>

#include "sr/game/TitleSession.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game {

// A party's settings, the original's party type byte: exp shared
// among the members near (the party of 8), items and gold shared, members other than the master
// may invite.
enum PartySettingBits : std::uint8_t { kPartyExpShare = 0x01, kPartyItemShare = 0x02, kPartyAnyoneInvites = 0x04 };
constexpr std::uint8_t kPartySettingsMask = kPartyExpShare | kPartyItemShare | kPartyAnyoneInvites;

// How many a party holds: 8 with the exp shared, else 4 (full at 8 / 4, errors 0x2C13 /
// 0x2C14).
constexpr int PartyCapacity(std::uint8_t settings) { return (settings & kPartyExpShare) != 0 ? 8 : 4; }
// A member's two highest masteries for the party's records (id -> level; 0: none): those trained (level
// above 0), highest first by level (ours: the lower id first on a tie).
std::pair<std::uint32_t, std::uint32_t> DominantMasteries(const std::map<std::uint32_t, int>& masteries);

constexpr int kPartyMinLevel = 5;            // a party is formed from this level (0x2C0A)
constexpr double kPartyShareRange = 1000.0;  // exp, items and gold reach members this near
constexpr double kPartyInviteRange = 600.0;  // a player is invited from this near
constexpr int kPartyProposalSeconds = 30;    // a request waits this long for its answer

// --- Experience

// A member of an exp-shared party near the kill: its level and race.
struct PartyKiller {
    int level = 1;
    bool european = false;
};
// What each eligible member's kill experience is multiplied by (in their order), and how it was
// worked out: the members' levels summed, the highest, the average (whole), the members' bonus.
struct PartyExperience {
    std::vector<double> factors;
    int levelSum = 0, maxLevel = 0, averageLevel = 0;
    double bonus = 1.0;
};
// The share of a kill's experience (the experience the whole party's damage earns) each eligible
// member gets: its level over the levels' sum, times the members' bonus (with a European among
// them 1 + 0.2 a European after the first + 0.1 a Chinese; all Chinese 1 + 0.1 a member after the
// first), the bonus halved for a member whose share is over 0.7 when 3 or more share and the highest
// level is 21 or more, and halved again for a party monster below the members' average level.
PartyExperience PartyExperienceShares(const std::vector<PartyKiller>& eligible, int monsterLevel, bool partyMonster);
// The same multiplier for a player outside an exp-shared party: 1 + 0.03 a member after the first in
// a party whose exp is not shared (`partyMembers`, 0 none), halved for a party monster below its level.
double SoloExperienceFactor(int partyMembers, int playerLevel, int monsterLevel, bool partyMonster);

// --- Items and gold

// Gold picked up in an item-shared party, among `receivers` (the picker first): equal shares, what
// cannot be split the picker's.
std::vector<std::uint64_t> SplitPartyGold(std::uint64_t amount, std::size_t receivers);

// --- What members are told of each other

// HP or MP in tenths (0..10), as the members' bars show them: told when the tenth changes. Ours: a
// living member's HP shows at least one tenth (the original counts it as 1 + whole tenths).
std::uint8_t PartyTenths(std::uint32_t value, std::uint32_t most, bool atLeastOne);

// --- Party matching

// A listing's purpose (the register window's radio buttons; their default titles).
enum class PartyPurpose : std::uint8_t { Hunting = 0, Quest = 1, Trade = 2, Thief = 3 };
constexpr int kPartyPurposes = 4;
constexpr int kPartyMatchPageSize = 12; // the matching window's rows (ifpartymatch.txt SlotList)

// The client's party, as the party windows show it, and the matching it sees.
class PartyView {
public:
    // The player's entity (its own member record is the one with it; the client does not know its
    // character's id).
    void SetSelf(std::uint32_t entity) { selfEntity_ = entity; }
    // A party or matching event; others are ignored. True when it was one.
    bool Apply(const TitleEvent& event, double now);

    bool InParty() const { return party != 0; }
    bool IsLeader() const { return party != 0 && leader == SelfId(); }
    std::uint32_t SelfId() const;
    const proto::PartyMemberRecord* Member(std::uint32_t id) const;
    const proto::PartyMemberRecord* MemberByEntity(std::uint32_t entity) const;
    // The members other than the player, in their order (the quick party window, Ctrl + 2..8).
    std::vector<const proto::PartyMemberRecord*> Others() const;
    int Capacity() const { return PartyCapacity(settings); }

    std::uint32_t party = 0;
    std::uint8_t settings = 0;
    std::uint32_t leader = 0;
    std::vector<proto::PartyMemberRecord> members; // the player among them, in the order they joined

    // A request to party waiting for the player's answer (the confirm box), and when it came.
    std::optional<proto::PartyInvited> invitation;
    double invitedAt = 0;
    // The settings the player forms its next party with (the Set window when in none; the original
    // keeps them in the client).
    std::uint8_t preferred = kPartyExpShare | kPartyItemShare;

    // Matching: the page last listed, the player's own listing (0: none) and the form it was listed
    // with, a join request waiting for the leader's answer, the listing the player asked to join.
    proto::PartyMatchListing listing;
    std::uint32_t ownEntry = 0;
    proto::PartyMatchForm ownForm;
    std::optional<proto::PartyMatchJoinRequest> joinRequest;
    double joinRequestAt = 0;
    std::uint32_t joining = 0;
    double joiningAt = 0;

    // What the system messages say (a text key and its arguments; the client puts the names in).
    struct Notice {
        std::string key;
        std::vector<std::string> args;
    };
    std::vector<Notice> notices;
    // Counts every change (the windows refresh).
    std::uint64_t version = 0;

private:
    void Clear();
    std::string NameOf(std::uint32_t id) const;
    std::uint32_t selfEntity_ = 0;
};

// The text key the original's client shows for a party status, empty for none.
const char* PartyStatusText(proto::PartyStatus status);

} // namespace sr::game
