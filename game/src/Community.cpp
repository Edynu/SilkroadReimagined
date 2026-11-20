#include "sr/game/Community.h"

#include <algorithm>
#include <variant>

namespace sr::game {

namespace {

template <typename... F>
struct Overloaded : F... {
    using F::operator()...;
};

} // namespace

const proto::FriendRecord* CommunityView::Friend(std::uint32_t character) const {
    const auto it = std::find_if(friends_.begin(), friends_.end(), [&](const proto::FriendRecord& f) { return f.character == character; });
    return it != friends_.end() ? &*it : nullptr;
}

bool CommunityView::IsBlocked(const std::string& name) const { return std::find(blocked_.begin(), blocked_.end(), name) != blocked_.end(); }

std::vector<CommunityView::Line> CommunityView::Apply(const proto::CommunityEvent& event) {
    std::vector<Line> lines;
    ++revision_;
    std::visit(Overloaded{
                   [&](const proto::CommunityLists& m) {
                       friends_ = m.friends;
                       blocked_ = m.blocked;
                       unread_ = m.unread;
                       letters_.clear();
                       body_.reset();
                       listed_ = false;
                   },
                   [&](const proto::FriendProposal& m) { proposal_ = m; },
                   [&](const proto::FriendAdded& m) {
                       if (Friend(m.record.character) == nullptr) friends_.push_back(m.record);
                       Line l;
                       l.kind = Line::Kind::FriendAdded;
                       l.name = m.record.name;
                       lines.push_back(std::move(l));
                   },
                   [&](const proto::FriendRemoved& m) {
                       std::erase_if(friends_, [&](const proto::FriendRecord& f) { return f.character == m.character; });
                   },
                   [&](const proto::FriendPresence& m) {
                       const auto it = std::find_if(friends_.begin(), friends_.end(), [&](const proto::FriendRecord& f) { return f.character == m.character; });
                       if (it == friends_.end()) return;
                       const bool changed = (it->online != 0) != (m.online != 0);
                       it->online = m.online;
                       if (m.announce == 0 || !changed) return;
                       Line l;
                       l.kind = m.online != 0 ? Line::Kind::FriendOnline : Line::Kind::FriendOffline;
                       l.name = it->name;
                       lines.push_back(std::move(l));
                   },
                   [&](const proto::CommunityAnswer& m) {
                       Line l;
                       l.kind = Line::Kind::Answer;
                       l.topic = m.topic;
                       l.code = m.code;
                       l.name = m.name;
                       lines.push_back(std::move(l));
                   },
                   [&](const proto::LetterListing& m) {
                       letters_ = m.rows;
                       listed_ = true;
                       unread_ = int(std::count_if(letters_.begin(), letters_.end(), [](const proto::LetterRecord& r) { return r.read == 0; }));
                   },
                   [&](const proto::LetterBody& m) {
                       body_ = m;
                       for (proto::LetterRecord& r : letters_) {
                           if (r.id != m.id || r.read != 0) continue;
                           r.read = 1;
                           if (unread_ > 0) --unread_;
                       }
                   },
                   [&](const proto::LetterArrived& m) {
                       unread_ = m.unread;
                       Line l;
                       l.kind = Line::Kind::LetterArrived;
                       lines.push_back(std::move(l));
                   },
                   [&](const proto::WhisperBlockChanged& m) {
                       if (m.result == 0) {
                           if (m.op == 1 && !IsBlocked(m.name)) blocked_.push_back(m.name);
                           if (m.op == 2) std::erase(blocked_, m.name);
                       }
                       Line l;
                       l.kind = Line::Kind::Block;
                       l.op = m.op;
                       l.result = m.result;
                       l.name = m.name;
                       lines.push_back(std::move(l));
                   },
               },
               event);
    return lines;
}

} // namespace sr::game
