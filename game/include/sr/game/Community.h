#pragma once

// The client's picture of the player's friends, whisper block list and letters: what the Community pages draw.
// Headless, like the guild's view.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sr/game/CommunityProtocol.h"

namespace sr::game {

class CommunityView {
public:
    // A line for the system messages; the HUD words it in the client's texts.
    struct Line {
        enum class Kind : std::uint8_t { FriendOnline, FriendOffline, FriendAdded, Answer, Block, LetterArrived };
        Kind kind = Kind::Answer;
        proto::CommunityTopic topic = proto::CommunityTopic::Friend;
        proto::CommunityCode code = proto::CommunityCode::Ok;
        std::uint8_t op = 0, result = 0;
        std::string name;
    };

    std::vector<Line> Apply(const proto::CommunityEvent& event);

    const std::vector<proto::FriendRecord>& Friends() const { return friends_; }
    const proto::FriendRecord* Friend(std::uint32_t character) const;
    const std::vector<std::string>& Blocked() const { return blocked_; }
    bool IsBlocked(const std::string& name) const;
    const std::vector<proto::LetterRecord>& Letters() const { return letters_; }
    int Unread() const { return unread_; }
    // The letter read last (none before the first).
    const std::optional<proto::LetterBody>& Body() const { return body_; }
    // Anything the pages show changed.
    std::uint64_t Revision() const { return revision_; }
    // A proposal waiting for the player's yes or no, once.
    std::optional<proto::FriendProposal> TakeProposal() { return std::exchange(proposal_, std::nullopt); }
    // The letter box was opened and listed once this session.
    bool LettersListed() const { return listed_; }

private:
    std::vector<proto::FriendRecord> friends_;
    std::vector<std::string> blocked_;
    std::vector<proto::LetterRecord> letters_;
    std::optional<proto::LetterBody> body_;
    std::optional<proto::FriendProposal> proposal_;
    int unread_ = 0;
    bool listed_ = false;
    std::uint64_t revision_ = 1;
};

} // namespace sr::game
