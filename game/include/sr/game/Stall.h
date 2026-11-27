#pragma once

// The client's picture of a stall and of the stall network: the stall the player is
// in (its own or one it visits) as the server tells it, and the network's pages of the last search. The
// HUD's windows read it; the game screen applies the server's news to it.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sr/game/StallProtocol.h"

namespace sr::game {

class StallView {
public:
    struct Item {
        proto::ItemRecord item; // its slot: the owner's bag place (to the owner), 0 (to a visitor)
        std::uint64_t price = 0;
    };
    struct ChatLine {
        std::string name, text;
        std::uint32_t colour = 0; // a notice's (no name): its ARGB; 0 a player's line
    };
    // A line for the system messages; the HUD words it in the client's texts.
    struct Line {
        enum class Kind : std::uint8_t {
            Refused,    // a request refused (code)
            Opened,     // the stall opened for business ("The stall is open."), or back to modifying
            Registered, // its items registered in the network (code Ok), or not (NotTown)
            Sold,       // an item of the player's stall sold (to `name`; `gold` the commission paid, if any)
            Bought,     // the player bought an item (code Ok) or not (why)
            Left,       // the player is out of the stall (code: why; Ok its own leave or close)
        };
        Kind kind = Kind::Refused;
        proto::StallCode code = proto::StallCode::Ok;
        bool open = false, network = false;
        std::string name;
        std::uint64_t gold = 0;
        proto::ItemRecord item;
    };

    // `self`: the player's entity (a stall whose owner it is, is its own).
    std::vector<Line> Apply(const proto::StallEvent& event, std::uint32_t self);
    // The player left the world: no stall any more.
    void Reset();

    bool In() const { return owner_ != 0; }
    bool Mine() const { return mine_; }
    std::uint32_t Owner() const { return owner_; }
    const std::string& OwnerName() const { return ownerName_; }
    const std::string& Name() const { return name_; }
    const std::string& Greeting() const { return greeting_; }
    bool ForBusiness() const { return open_; }
    const std::array<std::optional<Item>, proto::kStallSlots>& Items() const { return items_; }
    const std::vector<proto::StallPerson>& People() const { return people_; }
    const std::vector<ChatLine>& Chat() const { return chat_; }
    // A notice in the stall's chat (no name, its own colour): 1.188's sale lines (Msg_30B7 into the stall's chat module).
    void AddNotice(std::string text, std::uint32_t colour) {
        chat_.push_back({{}, std::move(text), colour});
        if (chat_.size() > 100) chat_.erase(chat_.begin());
        ++revision_;
    }
    // The bag place of an item the player put up in its own stall (it stays in the bag, held).
    bool Up(int place) const;
    // Anything the stall window shows changed.
    std::uint64_t Revision() const { return revision_; }

    // --- The network: the last search's pages (as the original's window keeps them) ---------
    void NewSearch(std::uint32_t group, std::uint8_t degree);
    std::uint32_t SearchGroup() const { return group_; }
    std::uint8_t SearchDegree() const { return degree_; }
    // The page asked last, the pages and the matches in all; a page already received.
    std::uint16_t Page() const { return page_; }
    void SetPage(std::uint16_t page) { page_ = page, ++networkRevision_; }
    std::uint16_t Pages() const { return pages_; }
    std::uint32_t Total() const { return total_; }
    const std::vector<proto::NetworkRow>* Rows(std::uint16_t page) const;
    // A row bought (or gone) is dropped from the page it is on; the row being bought (dropped when it is).
    void DropRow(std::uint64_t listing);
    void Buying(std::uint64_t listing) { buying_ = listing; }
    std::uint64_t NetworkRevision() const { return networkRevision_; }

private:
    void Clear();
    std::uint32_t owner_ = 0;
    bool mine_ = false, open_ = false;
    std::string ownerName_, name_, greeting_;
    std::array<std::optional<Item>, proto::kStallSlots> items_;
    std::vector<proto::StallPerson> people_;
    std::vector<ChatLine> chat_;
    std::uint64_t revision_ = 0;
    std::uint32_t group_ = 0;
    std::uint8_t degree_ = 0;
    std::uint16_t page_ = 0, pages_ = 0;
    std::uint32_t total_ = 0;
    std::map<std::uint16_t, std::vector<proto::NetworkRow>> rows_;
    std::uint64_t networkRevision_ = 0;
    std::uint64_t buying_ = 0;
};

} // namespace sr::game
