#pragma once

// Magic POP on the game server: the machine (NPC_CH_GACHA_MACHINE, an NPC of
// `_RefGachaNpcMap`) and Gori (NPC_CH_GACHA_OPERATOR).
//
// - Play (PopPlay {npc, GachaID, the card's place}; the original's 0x7118): the checks in the original's order and
//   codes (0 no such NPC, 3 out of reach, 0x1C0F not the machine, 0x1806 outside the bag or not a card, 0x1809 nothing
//   there, 0x183D a rental card run out, 2 the entry not in the machine's selection set or no consolation prize); the
//   roll (game::pop::RollPrize on two 15-bit draws); the card turned in place into the WIN or LOSE coupon carrying the
//   prize as its params; saved in one transaction (the bag held meanwhile); PopPlayed. A won entry the data marks
//   Visible is announced to every player of the shard (the bus channel sr_mall_<schema>). Nothing else is checked
//   (no state, no bag room: the card changes in place).
// - The exchange (PopExchange {npc}; the original's 0x7119) at Gori: every coupon of the bag becomes its prize in
//   place (one transaction); PopExchanged with the new items (ours: the original sends only the items' changes).
//
// The game server is its host.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "sr/assets/Gacha.h"
#include "sr/assets/GameData.h"
#include "sr/game/Inventory.h"
#include "sr/game/MagicPop.h"
#include "sr/game/MallProtocol.h"

namespace sr::server {

class PopHost {
public:
    virtual ~PopHost() = default;
    struct Player {
        std::uint64_t peer = 0;
        std::int64_t character = 0;
        std::string name;
        game::Inventory* bag = nullptr;
    };
    virtual std::optional<Player> PopPlayer(std::uint32_t entity) = 0;
    // An NPC of the world by its entity (none: no such NPC) and whether it stands within the talk's reach (300).
    struct Npc {
        std::uint32_t refObjId = 0;
        std::string codename;
    };
    virtual std::optional<Npc> PopNpc(std::uint32_t npc) const = 0;
    virtual bool PopInReach(std::uint32_t entity, std::uint32_t npc) const = 0;
    virtual const assets::ItemRow* PopItem(std::uint32_t refId) const = 0;
    virtual const assets::ItemRow* PopItem(std::string_view codename) const = 0;
    // A new item of `row` as a drop makes it (an equipment's durability and white stats).
    virtual game::ItemInstance PopFresh(const assets::ItemRow& row, int count) = 0;
    virtual const std::vector<assets::MagicOptionRow>& PopOptions() const = 0;
    // The bag's `touched` places in one transaction after every write before it; `done(ok)` on the host's turn.
    virtual void PopSave(std::uint32_t entity, const std::vector<std::uint8_t>& touched, std::function<void(bool ok)> done) = 0;
    virtual void PopSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // A win told to every player of the shard (both servers).
    virtual void PopAnnounce(const game::proto::PopAnnounce& announce) = 0;
    // The original's draw: 0..32,767 (tests fix it).
    virtual std::uint32_t PopDraw() = 0;
    // A prize's random blues when its coupon names none (the drops' rule: DropTable::RollBlues); a host without the
    // drops' tables gives none.
    virtual void PopRandomBlues(const assets::ItemRow& /*row*/, game::ItemInstance& /*item*/) {}
    virtual void PopBagChanged(std::uint32_t entity) = 0;
    virtual std::int64_t PopNow() const = 0;
};

class MagicPop {
public:
    MagicPop(PopHost& host, std::shared_ptr<const assets::GachaTable> table);

    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload);
    static bool IsRequest(game::proto::MessageId id);
    // A play or an exchange at the database holds the bag.
    bool Busy(std::uint32_t entity) const { return waiting_.contains(entity); }
    void Left(std::uint32_t entity) { waiting_.erase(entity); }
    // The machine: an NPC with a `_RefGachaNpcMap` row (NPC function 0x11); Gori: NPC_CH_GACHA_OPERATOR (0x12).
    bool IsMachine(std::uint32_t refObjId) const { return table_->Machine(refObjId) != nullptr; }
    static bool IsGori(std::string_view codename) { return codename.find("NPC_CH_GACHA_OPERATOR") != std::string_view::npos; }
    // The bus's announce (another server's win, or this one's come back).
    static std::string EncodeAnnounce(const game::proto::PopAnnounce& a);
    static std::optional<game::proto::PopAnnounce> DecodeAnnounce(std::string_view payload);

private:
    void Play(std::uint32_t entity, const game::proto::PopPlay& r);
    void Exchange(std::uint32_t entity, const game::proto::PopExchange& r);
    void Played(std::uint64_t peer, std::uint16_t code);

    PopHost& host_;
    std::shared_ptr<const assets::GachaTable> table_;
    std::unordered_set<std::uint32_t> waiting_;
};

} // namespace sr::server
