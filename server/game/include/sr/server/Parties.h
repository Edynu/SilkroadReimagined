#pragma once

// Parties and party matching on the game server: in memory, as the original keeps them (its matching listings in the
// shard manager, which ours does not have: the game server keeps them too). The rules are the original's:
//
// - A player of level 5 or more asks another to party with the party's settings (exp shared: up to
//   8 members; else 4; items shared; anyone may invite); the invited player has 30 s to answer; the
//   inviter becomes the master. The master (or anyone, with the setting) invites more, up to the
//   party's size. A player is invited from 600 away at most.
// - A member leaves or is sent away by the master; the master leaving dismisses the party; a party
//   left with one member is dismissed. A master leaving the world hands the party to the next member
//   (ours where the original's shard manager decided it: the client's "entrusted to" line).
// - The members are told of each other's level, HP and MP (in tenths, when a tenth changes) and
//   where they are (ours: every 2 s when moved).
// - A kill's experience: an exp-shared party's damage counts as one, its members within 1000 of the
//   kill's owner share it by level with the members' bonus; the others as before
//   (game::PartyExperienceShares, game::SoloExperienceFactor). The kill's drops are the owner's: the
//   one who did the most harm in the group that did the most.
// - Items shared: an item lying for a member may be picked up by any other; what a member picks up
//   goes round the members within 1000 of it in turn (one who is not near is passed over); gold is
//   split among them, what cannot be split the picker's.
// - Matching: a party's listing (title, purpose, levels; ours: the exp and item settings chosen with
//   it), pages of 12, a join request to the leader with 30 s to answer; the listing goes when the
//   party is full, dismissed, or its leader leaves.
//
// Across a shard's game servers: a party lives on one of them, its
// home (where it was formed); the others keep a mirror of it for their own members. What changes it is
// decided at home: a member elsewhere has its request sent there, and home tells the mirrors the new
// state. The listings are kept by every server, so a player on one can join a party on another. A
// player can only be invited from where it is seen, so members come together across servers
// through matching. Near-by things (shared experience, loot, gold) only reach members on the same
// server: the others are in another copy of the world.
//
// The game server is its host: its players as they are now, and a way to tell them.

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "sr/game/Party.h"
#include "sr/game/WorldProtocol.h"

namespace sr::server {

using PartyMessage = std::variant<game::proto::PartyInvited, game::proto::PartyResult, game::proto::PartyInfo, game::proto::PartyMemberJoined,
                                  game::proto::PartyMemberLeft, game::proto::PartyDismissed, game::proto::PartyMemberUpdate,
                                  game::proto::PartyLeaderChanged, game::proto::PartySettingsChanged, game::proto::PartyLoot,
                                  game::proto::PartyMatchAnswer, game::proto::PartyMatchListing, game::proto::PartyMatchJoinRequest,
                                  game::proto::ChatLine>;
using PartyRequest = std::variant<game::proto::PartyInvite, game::proto::PartyInviteAnswer, game::proto::PartyLeave, game::proto::PartyKick,
                                  game::proto::PartySetSettings, game::proto::PartyMatchRegister, game::proto::PartyMatchDelete,
                                  game::proto::PartyMatchList, game::proto::PartyMatchJoin, game::proto::PartyMatchJoinAnswer>;

class PartyHost {
public:
    // A player in the world as the parties see it now.
    struct Player {
        std::uint32_t entity = 0;
        std::uint32_t character = 0;
        std::string name;
        int level = 1;
        std::uint8_t race = 0; // 0 China, 1 Europe
        bool alive = true;
        std::uint32_t hp = 0, maxHp = 0, mp = 0, maxMp = 0;
        std::optional<game::proto::WorldPosition> position;
        std::uint32_t model = 0;
        std::uint32_t mastery1 = 0, mastery2 = 0; // its two highest (game::DominantMasteries)
        std::uint16_t server = 0; // the game server it is on (`entity` is that server's)
        // Its job: the state from the suit worn (game::kNoJob none), an alias, a suit's change
        // under way.
        std::uint8_t jobState = 4;
        bool alias = false;
        bool jobChanging = false;
        // In a Battle Arena match: its team (0: in none).
        std::uint8_t arenaTeam = 0;
    };
    virtual ~PartyHost() = default;
    virtual std::optional<Player> PlayerAt(std::uint32_t entity) const = 0;
    virtual void Tell(std::uint32_t entity, const PartyMessage& message) = 0;
    // The shard's other game servers: this one's id, and a bus message for all of them, in order.
    virtual std::uint16_t PartyServer() const { return 0; }
    virtual bool PartyOnBus() const { return false; }
    virtual void PartyPublish(std::string text) { (void)text; }
};

class Parties {
public:
    using Clock = std::chrono::steady_clock;
    explicit Parties(PartyHost& host) : host_(host) {}

    // A message of the party block (0x10D0..0x10EF) from a player in the world: done. Malformed: the
    // caller closes the connection.
    enum class Handled : std::uint8_t { No, Yes, Malformed };
    Handled Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now);
    // Requests that ran out of time, the members' news, listings of full parties.
    void Tick(Clock::time_point now);
    // A player leaves the world (back to the lobby, or gone): out of its party, its requests and its
    // listing dropped. Call while the host still has it.
    void LeaveWorld(std::uint32_t entity);
    // A member going to another game server of the shard stays a member: gone
    // from here, then Arrived there. MoveFailed if it never arrives.
    void MovedAway(std::uint32_t entity);
    void Arrived(std::uint32_t entity);
    void MoveFailed(std::uint32_t character);

    // --- The shard's other game servers -------------------------------------------------------------
    // This server has started (the others tell it what it missed) / a message from another.
    void Hello();
    void BusMessage(const std::string& text, Clock::time_point now);
    // This server stopping, as a message to send (empty: nobody to tell).
    std::string GoodbyeText() const;
    // Another game server of the shard is up.
    bool OthersOnline() const { return !peers_.empty(); }
    // A party chat line: to the speaker's party, on every server (false: it has none).
    bool Chat(std::uint32_t entity, const game::proto::ChatLine& line);

    // --- What the game server asks ------------------------------------------------------------------
    // The party of a player (0: none), its settings, its members' entities (itself among them; empty:
    // none; the skills' party areas).
    std::uint32_t PartyOf(std::uint32_t entity) const;
    // Whether it is its party's master.
    bool Leads(std::uint32_t entity) const;
    // Whether an invitation it made or was made to it waits for its answer (the original's party state 1 / 3).
    bool Proposing(std::uint32_t entity) const;
    // Its party's members as the parties know them, on every server (none: no party).
    struct MemberView {
        std::uint32_t character = 0;
        std::string name;
        int level = 0;
        std::uint16_t server = 0;  // the game server it is on
        std::uint32_t entity = 0;  // on that server
    };
    std::vector<MemberView> MembersOf(std::uint32_t entity) const;
    std::uint8_t SettingsOf(std::uint32_t entity) const;
    std::vector<std::uint32_t> MemberEntities(std::uint32_t entity) const;
    // Two characters in the same party with items shared: one may pick up what lies for the other.
    bool SharesItems(std::uint32_t characterA, std::uint32_t characterB) const;

    // A kill's experience and drops: who hurt the monster and by how much (entities), its level,
    // whether it is a party monster, where it fell. `owner` the drops' (0: none); each share is the
    // `share` the game server's reward takes (the player's part of the kill times its party factor).
    struct KillShare {
        std::uint32_t entity = 0;
        double share = 0;
    };
    struct Kill {
        std::uint32_t owner = 0;
        std::vector<KillShare> shares;
        int partyMembers = 0; // the most members of one exp-shared party sharing it (alive, within reach)
    };
    Kill ShareKill(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& damageBy, int monsterLevel, bool partyMonster,
                   const std::optional<game::proto::WorldPosition>& at) const;

    // The member an item picked up goes to: the next in the party's turn within 1000 of the picker
    // (the picker when its items are not shared, or nobody near). Each call takes a turn.
    std::uint32_t LootRecipient(std::uint32_t picker);
    // Gold picked up: the members it is split among and their shares (the picker first; only the
    // picker when its items are not shared).
    std::vector<std::pair<std::uint32_t, std::uint64_t>> SplitGold(std::uint32_t picker, std::uint64_t amount) const;
    // What the shared loot gave a member, to its party (nothing outside a party).
    void TellLoot(std::uint32_t recipient, std::uint32_t refId, std::uint16_t count, std::uint64_t gold);

    std::size_t Count() const { return parties_.size(); }
    std::size_t Listings() const { return entries_.size(); }

private:
    struct Member {
        std::uint32_t character = 0;
        std::uint32_t entity = 0;  // on its own server
        std::uint16_t server = 0;  // that server
        std::string name;
        std::uint8_t race = 0;
        std::uint32_t model = 0;
        // What the others were last told of it.
        std::uint8_t level = 0, hp = 0, mp = 0;
        std::uint32_t mastery1 = 0, mastery2 = 0;
        game::proto::WorldPosition position;
        Clock::time_point positionAt{};
        std::uint8_t jobState = 4; // as it joined (a suit does not change in a party: 0x18A0)
    };
    struct Party {
        std::uint32_t id = 0;
        std::uint16_t home = 0;             // the server deciding it (this one, or it is a mirror)
        bool shared = false;                // it has had members elsewhere: the others hear of it
        std::uint8_t settings = 0;
        std::uint32_t leader = 0;           // a member's character
        std::vector<Member> members;        // in the order they joined
        std::deque<std::uint32_t> lootTurn; // characters, the next to get an item first
    };
    struct Proposal {
        std::uint32_t inviter = 0;          // its entity
        std::uint32_t inviterCharacter = 0;
        std::uint8_t settings = 0;
        bool join = false;
        Clock::time_point until{};
    };
    struct Entry {
        std::uint32_t id = 0;
        std::uint16_t home = 0; // the server keeping it
        PartyHost::Player leader;
        game::proto::PartyMatchForm form;
        std::uint8_t members = 1, capacity = 4; // as the list shows them, kept by its home
    };
    struct JoinRequest {
        PartyHost::Player applicant, leader;
        std::uint32_t entry = 0;
        Clock::time_point until{};
    };

    // Where a request is decided, and doing so (`me` may be on another server).
    std::uint16_t Self() const { return host_.PartyServer(); }
    std::uint16_t HomeFor(const PartyHost::Player& me, const PartyRequest& request) const;
    void Apply(const PartyHost::Player& me, const PartyRequest& request, Clock::time_point now);
    // The player here by its entity, marked as this server's; a member when it is on this server.
    std::optional<PartyHost::Player> Here(std::uint32_t entity) const;
    std::optional<PartyHost::Player> HereOf(const Member& member) const;
    Member MakeMember(const PartyHost::Player& player) const;

    void Publish(std::uint8_t kind, std::uint16_t to, std::uint32_t party, std::uint32_t character, std::uint32_t entity, std::uint32_t except,
                 std::uint16_t message, std::span<const std::byte> payload);
    void PublishState(Party& party, std::uint32_t moved = 0);
    void DropProposals(std::uint32_t entity, const std::optional<PartyHost::Player>& me);
    void MemberArrived(const PartyHost::Player& me);
    void PublishEntry(const Entry& entry, std::uint16_t to = 0);
    void ApplyAdd(const PartyHost::Player& member, const PartyHost::Player& inviter, std::uint32_t partyId);
    // Someone left the world here or elsewhere: what is waiting on it goes.
    void CharacterGone(const PartyHost::Player& me);
    // A server stopped: its listings, the parties it decided, its members in ours.
    void ServerGone(std::uint16_t server);
    void SyncEntry(std::uint32_t leaderCharacter);

    void Invite(const PartyHost::Player& me, const game::proto::PartyInvite& m, Clock::time_point now);
    void Answer(const PartyHost::Player& me, bool accept);
    void Leave(const PartyHost::Player& me, game::proto::PartyLeaveReason reason);
    void Kick(const PartyHost::Player& me, std::uint32_t member);
    void SetSettings(const PartyHost::Player& me, std::uint8_t settings, game::proto::PartyAction action);
    void Register(const PartyHost::Player& me, const game::proto::PartyMatchForm& form);
    void Delete(const PartyHost::Player& me);
    void List(const PartyHost::Player& me, const game::proto::PartyMatchList& m);
    void Join(const PartyHost::Player& me, std::uint32_t entry, Clock::time_point now);
    void AnswerJoin(const PartyHost::Player& me, bool accept);

    // A new party of two, or a member into an existing one (the members told).
    Party& Form(const PartyHost::Player& leader, const PartyHost::Player& member, std::uint8_t settings);
    bool Add(Party& party, const PartyHost::Player& member);
    // A member out (the members, it too, told why), then the party dismissed when one is left.
    void Remove(Party& party, std::uint32_t character, game::proto::PartyLeaveReason reason);
    void Dismiss(Party& party, std::uint8_t reason);
    // A leaving master's listing made its successor's (SM).
    void HandListing(std::uint32_t leaderCharacter, const Member& next);
    void RemoveListing(std::uint32_t leaderCharacter, game::proto::PartyStatus why);
    // Its listing goes once the party is full.
    void CheckFull(const Party& party);

    Party* PartyAt(std::uint32_t entity);
    const Party* PartyAt(std::uint32_t entity) const;
    Party* PartyOfCharacter(std::uint32_t character);
    const Party* PartyOfCharacter(std::uint32_t character) const;
    game::proto::PartyInfo InfoOf(const Party& party) const;
    game::proto::PartyMemberRecord RecordOf(const Member& member) const;
    // The message as this server's players should see it (entities and places are per server).
    PartyMessage Localised(const Party& party, PartyMessage message) const;
    void TellHere(const Party& party, const PartyMessage& message, std::uint32_t exceptCharacter);
    void TellParty(const Party& party, const PartyMessage& message, std::uint32_t exceptCharacter = 0);
    void TellOne(const PartyHost::Player& who, const PartyMessage& message);
    void Result(std::uint32_t entity, game::proto::PartyAction action, game::proto::PartyStatus status, std::string name = {});
    void Result(const PartyHost::Player& who, game::proto::PartyAction action, game::proto::PartyStatus status, std::string name = {});
    void MatchAnswer(const PartyHost::Player& who, game::proto::PartyMatchAction action, game::proto::PartyStatus status, std::uint32_t entry);

    PartyHost& host_;
    std::unordered_map<std::uint32_t, Party> parties_;
    std::unordered_map<std::uint32_t, std::uint32_t> partyOf_;     // character -> party id
    std::unordered_map<std::uint32_t, Proposal> proposals_;        // by the invited player's entity
    std::map<std::uint32_t, Entry> entries_;                       // by number: the listing's order
    std::unordered_map<std::uint32_t, std::uint32_t> entryOf_;     // leader character -> its listing
    std::unordered_map<std::uint32_t, JoinRequest> joinRequests_;  // by the leader's character
    std::unordered_map<std::uint32_t, std::uint32_t> applying_;    // applicant character -> the leader's character
    std::uint32_t nextParty_ = 1, nextEntry_ = 1;
    Clock::time_point nextNews_{};
    std::unordered_map<std::uint32_t, PartyHost::Player> moving_; // left for another server, not yet arrived
    std::map<std::uint16_t, Clock::time_point> peers_;             // the shard's other servers, when last heard
    Clock::time_point nextAlive_{};
};

} // namespace sr::server
