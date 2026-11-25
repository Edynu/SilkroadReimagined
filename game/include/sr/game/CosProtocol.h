#pragma once

// COS over the network: our own messages (0x1300-0x131F) for a player's rides, transports and
// pets. A ride or a transport is summoned by using its scroll from the bag (UseItem); the COS is an entity of the
// world (EntityKind::Cos, its `amount` its owner's entity) that its owner alone commands: Ride boards or leaves it,
// Dismiss sends it away. Who rides what is told to everyone who sees the rider (CosRiding, kept for newcomers after
// the rider's appearance): their clients seat the rider on its mount, which then goes where the rider goes.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/Protocol.h"
#include "sr/game/WorldProtocol.h"

namespace sr::game::proto {

inline bool IsCosMessage(MessageId id) { return id >= 0x1300 && id < 0x1320; }

// A grab pet's settings (_CharCOS.PetOption; the 1.188 setup page, ifcossetup.txt): grabbing on, and anyone's items
// too ("Grab all items"; off: only those dropped for its owner), then what it grabs: gold, equipment, the rest (the
// 1.188 page's three boxes, ids 25..27).
constexpr std::uint32_t kGrabOn = 0x80, kGrabAnyone = 0x40, kGrabGold = 0x01, kGrabEquipment = 0x02, kGrabOther = 0x04;
constexpr std::uint32_t kGrabMask = kGrabOn | kGrabAnyone | kGrabGold | kGrabEquipment | kGrabOther;
// A pet's bag page (the 1.188 window's lattice: 7 x 4).
constexpr int kCosBagPage = 28;

// --- From the server -----------------------------------------------------------------------------------------

// The owner's own COS came out (its status icon and commands; also after each WorldEntered while it is out).
struct CosSummoned {
    static constexpr MessageId kId = 0x1300;
    std::uint32_t entity = 0;
    std::uint32_t model = 0;   // characterdata RefObjID
    std::uint8_t kind = 0;     // game::CosKind
    std::uint32_t hp = 0, maxHp = 0;
    std::uint32_t item = 0;    // the itemdata RefObjID it was summoned with
    // A pet's (_CharCOS): its name (empty: none given yet), its level, its hunger points (an attack pet's; 0..10000).
    std::string name;
    std::uint8_t level = 0;
    std::uint16_t hgp = 0;
    std::uint32_t option = 0; // a grab pet's settings (kGrab*)
    // An attack pet's experience in its level (_CharCOS.ExpOffset); a grab pet's rental left, in seconds (0: none).
    std::uint64_t experience = 0;
    std::uint32_t rentSeconds = 0;
    bool aggressive = true; // an attack pet's stance (CosStance)
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("model", model);
        v("kind", kind);
        v("hp", hp);
        v("maxHp", maxHp);
        v("item", item);
        v("name", name, 32);
        v("level", level);
        v("hgp", hgp);
        v("option", option);
        v("experience", experience);
        v("rentSeconds", rentSeconds);
        v("aggressive", aggressive);
    }
};

// An attack pet's experience as it is now in its level, and what it just gained (the original's 0x30C9 type 3; the
// 1.188 client's UIIT_MSG_COSPET_GAIN_EXP). A level reached comes as a CosSummoned of the new level's row.
struct CosExperience {
    static constexpr MessageId kId = 0x1308;
    std::uint32_t entity = 0;
    std::uint64_t experience = 0, gained = 0;
    std::uint64_t lost = 0; // taken by its death (the original's negative change: "You have lost [%d]amount of pet experience.")
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("experience", experience);
        v("gained", gained);
        v("lost", lost);
    }
};

// An attack pet's HGP now (the original's 0x30C9 type 4: at each tenth it eats, and when fed).
struct CosHgp {
    static constexpr MessageId kId = 0x130A;
    std::uint32_t entity = 0;
    std::uint16_t hgp = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("hgp", hgp);
    }
};

// An attack pet's stance now (the command bar's cos_cmd_aggressive / _defensive): aggressive, it
// takes on its owner's targets too; either way it fights back and defends its owner.
struct CosStance {
    static constexpr MessageId kId = 0x1309;
    std::uint32_t entity = 0;
    bool aggressive = true;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("aggressive", aggressive);
    }
};

// A pet's bag as it is now (after its summon and after every change): its places and what is in them (ItemRecord's
// slot: the bag's place, 0..size-1).
struct CosBag {
    static constexpr MessageId kId = 0x1304;
    std::uint32_t entity = 0;
    std::uint8_t size = 0;
    std::vector<ItemRecord> items;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("size", size);
        v("items", items, 255);
    }
};

// A grab pet's settings as the server keeps them now (after a change, or grabbing turned off by a full bag).
struct CosOption {
    static constexpr MessageId kId = 0x1305;
    std::uint32_t entity = 0;
    std::uint32_t option = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("option", option);
    }
};

// The pets of the owner's summoning items as their rows are now (at its entry into the world for every pet item in
// its bag, then for one whenever it changes: summoned, unsummoned, dead, named): what the item's tooltip and slot show
// (the 1.188 client keeps them with the item). `id` is the _CharCOS row the item's durability names.
struct CosPetItem {
    std::int64_t id = 0;
    std::uint32_t model = 0;    // its characterdata row
    std::uint8_t state = 0;     // game::CosState
    std::string name;
    std::uint8_t level = 0;
    std::uint32_t rentSeconds = 0; // a grab pet's rental left (0: none, or over)
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("model", model);
        v("state", state);
        v("name", name, 32);
        v("level", level);
        v("rentSeconds", rentSeconds);
    }
};
struct CosPetItems {
    static constexpr MessageId kId = 0x1306;
    std::vector<CosPetItem> pets;
    template <typename V>
    void Visit(V& v) {
        v("pets", pets, 128);
    }
};

// A pet was named (its owner's CosRename, once for good).
struct CosRenamed {
    static constexpr MessageId kId = 0x1307;
    std::uint32_t entity = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("name", name, 32);
    }
};

// The owner's COS is gone (dismissed, killed, or its owner went where it cannot follow).
struct CosDismissed {
    static constexpr MessageId kId = 0x1301;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

// A rider got on or off its mount (the original's 0xB0CB), for everyone who sees the rider.
struct CosRiding {
    static constexpr MessageId kId = 0x1302;
    std::uint32_t rider = 0;
    std::uint32_t mount = 0;
    bool riding = false;
    template <typename V>
    void Visit(V& v) {
        v("rider", rider);
        v("mount", mount);
        v("riding", riding);
    }
};

// Why a summon or a command was refused: the original's text key (UIIT_MSG_COS_*), shown in the chat.
struct CosRefused {
    static constexpr MessageId kId = 0x1303;
    std::string text;
    std::string arg; // the text's %d / %s, when it has one
    template <typename V>
    void Visit(V& v) {
        v("text", text, 128);
        v("arg", arg, 64);
    }
};

// --- From the client -----------------------------------------------------------------------------------------

// Board (`ride`) or leave one's COS (the command bar's Ride / Dismount; the original's 0x70CB).
struct CosRide {
    static constexpr MessageId kId = 0x1310;
    std::uint32_t entity = 0;
    bool ride = false;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("ride", ride);
    }
};

// Send one's COS away (the command bar's Terminate / Unsummon; the original's 0x70C6 / 0x7116): a scroll's COS is
// gone, a pet goes back into its item (its _CharCOS row kept, inactive).
struct CosDismiss {
    static constexpr MessageId kId = 0x1311;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

// A pet's command (the command bar's, the 1.188 client's numbers; the original's 0x70C5): 2 Attack (an
// attack pet on `target`, a monster: the original's subcommand 2, event 0x19), 3 Follow (back
// to its owner, its attack given up), 6 the stance (aggressive / defensive in turn: CosStance).
enum class CosOrder : std::uint8_t { Attack = 2, Follow = 3, Stance = 6 };
struct CosCommand {
    static constexpr MessageId kId = 0x1312;
    std::uint32_t entity = 0;
    CosOrder order = CosOrder::Follow;
    std::uint32_t target = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("order", order);
        v("target", target);
    }
};

// A grab pet's settings changed (the setup page's Confirm): kGrab* bits.
struct CosSetOption {
    static constexpr MessageId kId = 0x1313;
    std::uint32_t entity = 0;
    std::uint32_t option = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("option", option);
    }
};

// An item between a pet's bag and its owner's inventory, or within the bag (the COS window's drags; the original's
// _STRG_MOVEITEM_PET_PC / _PC_PET): `from` and `to` a bag place when its side is the pet's (`fromPet`, `toPet`), else an
// inventory place of the bag (13..). A whole stack moves (merged into a stack of the same item, else onto an empty place).
struct CosBagMove {
    static constexpr MessageId kId = 0x1314;
    std::uint32_t entity = 0;
    std::uint8_t from = 0, to = 0;
    bool fromPet = false, toPet = false;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("from", from);
        v("to", to);
        v("fromPet", fromPet);
        v("toPet", toPet);
    }
};

// A pet named (the COS window's Create name, then "Confirm pet name?"; the original's 0x7117): once, for good; refused
// with UIIT_MSG_COSPETERR_PETNAME_NOTPUT (not a name the rules allow), _PETNAME_SUMENESS (another pet's already),
// UIIT_MSG_COSPET_NOTREVISION_NAME (it has one).
struct CosRename {
    static constexpr MessageId kId = 0x1315;
    std::uint32_t entity = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("name", name, 32);
    }
};

// The player's ride or transport sold back at the NPC `npc` (a stable keeper's row; the original's 0x70C7 kind 3): for
// its SellPrice (1..10,000,000), its gold told by GoldChanged and the COS gone (CosDismissed). Refused with
// UIIT_MSG_COSERR_CANT_SELL_REQUESTED_COS (a COS the keeper does not buy: CanSell 0, every ride),
// _CANT_SELL_COS_HAVE_ITEMS (goods aboard), _CANT_SELL_RIDING_COS (ridden); no COS out or the NPC not within reach:
// nothing (the original's 5 and 3).
struct CosSell {
    static constexpr MessageId kId = 0x1317;
    std::uint32_t npc = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
    }
};

// An item of the bag (`place`) used on a pet's summoning item (`target`, both bag places 13..): the Clock of
// Reincarnation on an expired grab pet, the Grass of Life on a dead attack pet (game::PetTreatment). Refused with
// UIIT_MSG_COSPETERR_CANT_USE_WRONGOBJECT (not a pet of that kind), _CANT_USEITEM (its pet is not dead).
struct CosUseOnPet {
    static constexpr MessageId kId = 0x1316;
    std::uint8_t place = 0, target = 0;
    template <typename V>
    void Visit(V& v) {
        v("place", place);
        v("target", target);
    }
};

// What the server says about COS, as the client's session hands it on.
using CosEvent = std::variant<CosSummoned, CosDismissed, CosRiding, CosRefused, CosBag, CosOption, CosPetItems, CosRenamed, CosExperience, CosStance, CosHgp>;

// A server's COS message as the session's event; false for another id or a malformed payload.
inline bool DecodeCosEvent(MessageId id, std::span<const std::byte> payload, CosEvent& out) {
    const auto as = [&]<typename T>(T message) {
        if (!Decode(payload, message)) return false;
        out = std::move(message);
        return true;
    };
    switch (id) {
    case CosSummoned::kId: return as(CosSummoned{});
    case CosDismissed::kId: return as(CosDismissed{});
    case CosRiding::kId: return as(CosRiding{});
    case CosRefused::kId: return as(CosRefused{});
    case CosBag::kId: return as(CosBag{});
    case CosOption::kId: return as(CosOption{});
    case CosPetItems::kId: return as(CosPetItems{});
    case CosRenamed::kId: return as(CosRenamed{});
    case CosExperience::kId: return as(CosExperience{});
    case CosStance::kId: return as(CosStance{});
    case CosHgp::kId: return as(CosHgp{});
    default: return false;
    }
}

inline const char* CosMessageName(MessageId id) {
    switch (id) {
    case CosSummoned::kId: return "CosSummoned";
    case CosDismissed::kId: return "CosDismissed";
    case CosRiding::kId: return "CosRiding";
    case CosRefused::kId: return "CosRefused";
    case CosRide::kId: return "CosRide";
    case CosDismiss::kId: return "CosDismiss";
    case CosCommand::kId: return "CosCommand";
    case CosBag::kId: return "CosBag";
    case CosOption::kId: return "CosOption";
    case CosSetOption::kId: return "CosSetOption";
    case CosBagMove::kId: return "CosBagMove";
    case CosPetItems::kId: return "CosPetItems";
    case CosRenamed::kId: return "CosRenamed";
    case CosRename::kId: return "CosRename";
    case CosUseOnPet::kId: return "CosUseOnPet";
    case CosExperience::kId: return "CosExperience";
    case CosStance::kId: return "CosStance";
    case CosHgp::kId: return "CosHgp";
    default: return nullptr;
    }
}

} // namespace sr::game::proto
