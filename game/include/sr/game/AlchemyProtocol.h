#pragma once

// Alchemy over the network: our own messages (0x12B0-0x12CF) for what the original's carry
// (its 0x7150 / 0x7151 strengthening, 0x7155 making and disjoining, 0x7157 dismantling, 0x716A the sockets; its answers
// 0xB150..0xB16A).
// The rules, the timing and the error numbers are the original's (game/Alchemy.h); the client
// turns the numbers into its texts. The bag's places that change go to the player as ItemMoved
// before the result, so the window reads the item as it is now.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game::proto {

inline bool IsAlchemyMessage(MessageId id) { return id >= 0x12B0 && id < 0x12D0; }

// An NPC's service (NpcMenu.services): the town smiths' "Grant magic option on avatar item" (the original's NPC function
// 0x20).
constexpr std::uint32_t kNpcAvatarGrant = 0x800000;

// Strengthening (the Strengthen tab): the equipment in the bag's `target`, an elixir, a magic stone or an attribute
// stone in `material`, the lucky powder in `powder` (0: none; with an elixir only). The result after the original's
// four ticks (about 3..4 s), unless cancelled.
struct AlchemyStrengthen {
    static constexpr MessageId kId = 0x12B0;
    std::uint8_t target = 0, material = 0, powder = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
        v("material", material);
        v("powder", powder);
    }
};

// Stops what runs (a strengthening before its result, a disjoining's next stacks).
struct AlchemyCancel {
    static constexpr MessageId kId = 0x12B1;
    template <typename V>
    void Visit(V&) {}
};

// Making a stone (the Manufacture tab): the tablet and the places of its elements; at once.
struct AlchemyManufacture {
    static constexpr MessageId kId = 0x12B2;
    std::uint8_t tablet = 0;
    std::vector<std::uint8_t> elements;
    template <typename V>
    void Visit(V& v) {
        v("tablet", tablet);
        v("elements", elements, 4);
    }
};

// Disjoining monster materials with the Void Rondo (the bag's own; the Disjoint tab): the first stack at once, each
// next three ticks later.
struct AlchemyDisjoint {
    static constexpr MessageId kId = 0x12B3;
    std::vector<std::uint8_t> materials;
    template <typename V>
    void Visit(V& v) {
        v("materials", materials, 8);
    }
};

// Dismantling a piece of equipment with the Destroyer Rondo (the bag's own; the Dismantle tab); at once.
struct AlchemyDismantle {
    static constexpr MessageId kId = 0x12B4;
    std::uint8_t target = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
    }
};

// Sockets (the original's 0x716A actions 3, 2, 4): the equipment in the bag's `target`, the
// socket's `index` (0..2).
// Making a socket with the bag's first socket maker (ITEM_ETC_ARCHEMY_ETC_02); at once.
struct SocketMake {
    static constexpr MessageId kId = 0x12B5;
    std::uint8_t target = 0, index = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
        v("index", index);
    }
};

// Putting the socket stone at the bag's `stone` into an empty socket: on the 4th one-second tick as a strengthening
// (AlchemyCancel or a death before it stops it, nothing used); checked when asked and again then.
struct SocketInsert {
    static constexpr MessageId kId = 0x12B6;
    std::uint8_t target = 0, stone = 0, index = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
        v("stone", stone);
        v("index", index);
    }
};

// Erasing a socket's stone with the bag's first Destruction Rondo (ITEM_ETC_ARCHEMY_RONDO_03); at once, the socket
// stays, empty.
struct SocketErase {
    static constexpr MessageId kId = 0x12B7;
    std::uint8_t target = 0, index = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
        v("index", index);
    }
};

// The smith's blessing of an avatar (the original's 0x34A9 {u8 place, string option}):
// the avatar in the bag's `place`, one option of its list by name (magicoptionassign.txt's), at the smith `npc` in reach
// (ours: the original's handler looks at no NPC). Free; at once; the result an AlchemyResult (AvatarGrant: the option's
// id and value, or the code, game/Alchemy.h code::kGrant*) after the avatar's ItemMoved.
struct AvatarGrant {
    static constexpr MessageId kId = 0x12B9;
    std::uint32_t npc = 0;
    std::uint8_t place = 0;
    std::string option;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("place", place);
        v("option", option);
    }
};

enum class AlchemyOperation : std::uint8_t {
    Strengthen = 1, Manufacture = 2, Disjoint = 3, Dismantle = 4, SocketMake = 5, SocketInsert = 6, SocketErase = 7, AvatarGrant = 8
};
enum class AlchemyOutcome : std::uint8_t {
    Success = 1,
    Failure = 2,   // the roll lost: what was used is gone, the item as the failure left it
    Destroyed = 3, // a failed reinforcement broke the item
    Cancelled = 4, // a cancel or a death before the result: nothing used
    Refused = 5,   // `code` says why: nothing used
};

// The answer to any of them (the bag's changed places came just before as ItemMoved).
struct AlchemyResult {
    static constexpr MessageId kId = 0x12B8;
    AlchemyOperation operation = AlchemyOperation::Strengthen;
    AlchemyOutcome outcome = AlchemyOutcome::Success;
    std::uint16_t code = 0;          // the original's number (game/Alchemy.h code::)
    std::uint8_t place = 0;          // the equipment, the tablet, the material stack or the piece it concerns
    // Strengthening: the plus before and after; a failure's durability penalty; Astral's +4.
    std::uint8_t plusBefore = 0, plusAfter = 0;
    bool durabilityLost = false;
    bool astralKept = false;
    // A stone: its option (a magic option's id, or the white field's index for an attribute stone) and its value;
    // what assimilation changed (-1: nothing; an APE charge stopped it).
    bool attribute = false;
    std::uint32_t option = 0;
    std::uint32_t value = 0;
    std::int8_t assimilatedField = -1, assimilatedOption = -1;
    bool apeUsed = false;
    std::uint8_t remaining = 0; // disjoining: the stacks still to do
    // Sockets: the socket's index; an insertion's skill in `option`, its level in `value`, the stone in `stone`.
    std::uint8_t socket = 0;
    std::uint32_t stone = 0;
    template <typename V>
    void Visit(V& v) {
        v("operation", operation);
        v("outcome", outcome);
        v("code", code);
        v("place", place);
        v("plusBefore", plusBefore);
        v("plusAfter", plusAfter);
        v("durabilityLost", durabilityLost);
        v("astralKept", astralKept);
        v("attribute", attribute);
        v("option", option);
        v("value", value);
        v("assimilatedField", assimilatedField);
        v("assimilatedOption", assimilatedOption);
        v("apeUsed", apeUsed);
        v("remaining", remaining);
        v("socket", socket);
        v("stone", stone);
    }
};

} // namespace sr::game::proto
