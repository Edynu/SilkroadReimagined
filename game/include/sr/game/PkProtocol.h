#pragma once

// Murderers over the network: our own messages (0x1400-0x141F) for what the original's carry
// (0x30BF channel 7: a player's PvP state to those near; 0x30CD / 0x30CE / 0x30D3: the player's own penalty points,
// DailyPK and TotalPK). The refusals of an attack keep the original's codes in proto::SkillUseStatus.

#include <cstdint>
#include <span>
#include <variant>

#include "sr/game/Protocol.h"

namespace sr::game::proto {

// A player's PvP state (game::PvpState: 0 innocent, 1 assaulter, 2 murderer), to those who see him on each change
// (0x30BF {entity, 7, state}); also kept for newcomers after his appearance (the
// original's spawn carries it in the state block).
struct EntityPvpState {
    static constexpr MessageId kId = 0x1400;
    std::uint32_t entity = 0;
    std::uint8_t state = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("state", state);
    }
};

// The player's own three counters (the original's 0x30CE {u8 DailyPK}, 0x30D3 {u16 TotalPK}, 0x30CD {u32 points}; ours
// one message): at the entry and on each change. The PK icon of the player's bar shows while any is not 0.
struct PkCounters {
    static constexpr MessageId kId = 0x1401;
    std::uint8_t daily = 0;
    std::uint16_t total = 0;
    std::uint32_t points = 0;
    template <typename V>
    void Visit(V& v) {
        v("daily", daily);
        v("total", total);
        v("points", points);
    }
};

using PkEvent = std::variant<EntityPvpState, PkCounters>;

// The murderers' messages' range (0x1400-0x141F; the server's all of them so far).
inline bool IsPkMessage(MessageId id) { return id >= 0x1400 && id < 0x1420; }

inline bool DecodePkEvent(MessageId id, std::span<const std::byte> payload, PkEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case EntityPvpState::kId: return as(EntityPvpState{});
    case PkCounters::kId: return as(PkCounters{});
    default: return false;
    }
}

inline const char* PkMessageName(MessageId id) {
    switch (id) {
    case EntityPvpState::kId: return "EntityPvpState";
    case PkCounters::kId: return "PkCounters";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
