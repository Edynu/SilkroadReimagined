#pragma once

// The world's messages, our own form of what the original's world
// carries: after the select the client asks to enter; the server answers
// first with the player's own character, then with the world around it (entities appearing,
// disappearing and moving). A move is asked as a point; the server checks the walk on its own
// navigation and says where each walker goes from where, at which speed: the clients move them
// there themselves until the next word.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "sr/game/GuildProtocol.h"
#include "sr/game/LobbyProtocol.h"
#include "sr/game/Protocol.h"

namespace sr::game::proto {

// A point in the world: a region (assets::RegionId: y << 8 | x) and a place in it (0..1920 on x
// and z; y the height). A dungeon's region has bit 15 set (0x8000 | its dungeoninfo.txt id):
// x and z are then in the dungeon's own space (tens of thousands either side of 0), not a region's.
struct WorldPosition {
    std::uint16_t region = 0;
    float x = 0, y = 0, z = 0;
    // The server's own: the instance (a layer of the world) the point is in, 0
    // the world everyone shares. Never sent: a client sees only its own layer, and decodes 0.
    std::uint32_t layer = 0;
    template <typename V>
    void Visit(V& v) {
        v("region", region);
        v("x", x);
        v("y", y);
        v("z", z);
    }
};

// World-wide coordinates (x east, z north) of a point, and back (the region a point falls in). The
// outdoor world is 256 x 256 regions (0..491,520); each dungeon gets a square of its own beyond it
// (kDungeonFlatBase on, kDungeonFlatSpan apart: the largest dungeon reaches 22,466 from its middle),
// so nothing in one dungeon is ever near anything in another or outdoors.
// An instance's layer lies kLayerFlatSpan north of the one before it (beyond the dungeons), so nothing in one
// layer is near anything in another.
constexpr double kDungeonFlatBase = 1'000'000.0, kDungeonFlatSpan = 100'000.0, kLayerFlatSpan = 10'000'000.0;
struct FlatPosition {
    double x = 0, z = 0;
};
inline FlatPosition ToFlat(const WorldPosition& p) {
    const double layer = double(p.layer) * kLayerFlatSpan;
    if ((p.region & 0x8000) != 0) return {kDungeonFlatBase + double(p.region & 0x7FFF) * kDungeonFlatSpan + p.x, kDungeonFlatBase + p.z + layer};
    return {double(p.region & 0xFF) * 1920.0 + p.x, double(p.region >> 8) * 1920.0 + p.z + layer};
}
inline WorldPosition FromFlat(FlatPosition f, float y) {
    const double layerIndex = std::floor(f.z / kLayerFlatSpan + 0.5);
    const std::uint32_t layer = layerIndex <= 0 ? 0u : std::uint32_t(layerIndex);
    f.z -= double(layer) * kLayerFlatSpan;
    if (f.x > kDungeonFlatBase / 2 || f.z > kDungeonFlatBase / 2) {
        const double index = std::floor((f.x - kDungeonFlatBase) / kDungeonFlatSpan + 0.5);
        const int dungeon = index < 0 ? 0 : index > 0x7FFF ? 0x7FFF : int(index);
        return {std::uint16_t(0x8000 | dungeon), float(f.x - kDungeonFlatBase - dungeon * kDungeonFlatSpan), y, float(f.z - kDungeonFlatBase), layer};
    }
    int rx = int(std::floor(f.x / 1920.0)), rz = int(std::floor(f.z / 1920.0));
    rx = rx < 0 ? 0 : rx > 255 ? 255 : rx;
    rz = rz < 0 ? 0 : rz > 255 ? 255 : rz;
    return {std::uint16_t((rz << 8) | rx), float(f.x - rx * 1920.0), y, float(f.z - rz * 1920.0), layer};
}

// --- Entering and leaving (0x1000) ------------------------------------------------------------

// The selected character into the world.
struct WorldEnter {
    static constexpr MessageId kId = 0x1000;
    template <typename V>
    void Visit(V&) {}
};

enum class WorldStatus : std::uint8_t {
    Ok = 0,
    NotSelected = 1, // no character was selected in the lobby
    Busy = 2,        // the server could not (database)
    Full = 3,        // the world is at its capacity
    ServerDown = 4,  // the game server of the character's part of the world is not running
};

// The player's own character, before anything of the world around it.
struct WorldEntered {
    static constexpr MessageId kId = 0x1001;
    WorldStatus status = WorldStatus::Ok;
    std::uint32_t entity = 0; // its id in the world
    CharacterSummary character;
    WorldPosition position;
    float yaw = 0;                       // facing, radians (0: +z)
    float walkSpeed = 0, runSpeed = 0;   // world units a second
    std::uint16_t tickRate = 0;          // the world's ticks a second (for the client's information)
    std::uint32_t maxHp = 0, maxMp = 0;  // its most health and mana (`character` has what it has; C10b)
    template <typename V>
    void Visit(V& v) {
        v("status", status);
        v("entity", entity);
        v("character", character);
        v("position", position);
        v("yaw", yaw);
        v("walkSpeed", walkSpeed);
        v("runSpeed", runSpeed);
        v("tickRate", tickRate);
        v("maxHp", maxHp);
        v("maxMp", maxMp);
    }
};

// Back to the lobby (positions are saved).
struct WorldLeave {
    static constexpr MessageId kId = 0x1002;
    template <typename V>
    void Visit(V&) {}
};

struct WorldLeft {
    static constexpr MessageId kId = 0x1003;
    template <typename V>
    void Visit(V&) {}
};

// The server is about to close the connection, and why (ours: the original's server just closes it): the account
// blocked (the website's kick: SroWeb's sr_admin_commands; the login refuses it from now on), or a game master's
// /kick. The client says so where it says a lost connection.
// Replaced: the same character entered the world from another connection (ours: the original's server is one process).
enum class DisconnectReason : std::uint8_t { Blocked = 1, Kicked = 2, Replaced = 3 };
struct Disconnected {
    static constexpr MessageId kId = 0x1004;
    DisconnectReason reason = DisconnectReason::Kicked;
    std::string text;       // the reason as given, if any (a block's: _Punishment's description)
    std::int64_t until = 0; // a block's end (Unix seconds; 0: none, or no end)
    template <typename V>
    void Visit(V& v) {
        v("reason", reason);
        v("text", text);
        v("until", until);
    }
};

// --- Moving (0x1010) --------------------------------------------------------------------------

// Where the player clicked.
struct MoveTo {
    static constexpr MessageId kId = 0x1010;
    WorldPosition target;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
    }
};

// A click on the sky (or land beyond the pick's reach): that way, and on the same way until something stops the
// walker (the original's; `target` a point along it). The server walks it leg by leg (World::MoveToward).
struct MoveToward {
    static constexpr MessageId kId = 0x1015;
    WorldPosition target;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
    }
};

namespace detail {

// A leg's values packed (EntityMoved): x and z (0 to 2048) in 1/32ths, y (-2048 to 2048) in 1/16ths, the speed (0 to
// 655) in 1/100ths; false when the value is outside its range (it then goes as its float).
inline bool PackXz(float v, std::uint16_t& q) {
    const float s = std::round(v * 32.0f);
    if (!(s >= 0.0f && s <= 65535.0f)) return false;
    q = std::uint16_t(s);
    return true;
}
inline bool PackY(float v, std::int16_t& q) {
    const float s = std::round(v * 16.0f);
    if (!(s >= -32768.0f && s <= 32767.0f)) return false;
    q = std::int16_t(s);
    return true;
}
inline bool PackSpeed(float v, std::uint16_t& q) {
    const float s = std::round(v * 100.0f);
    if (!(s >= 0.0f && s <= 65535.0f)) return false;
    q = std::uint16_t(s);
    return true;
}

// One place of a leg, written or read: packed, or (`exact`) as its floats.
template <typename V>
void LegPlace(V& v, bool exact, WorldPosition& p) {
    v("region", p.region);
    if (exact) {
        v("x", p.x);
        v("y", p.y);
        v("z", p.z);
        return;
    }
    std::uint16_t x = 0, z = 0;
    std::int16_t y = 0;
    if constexpr (std::is_same_v<V, Writer>) {
        PackXz(p.x, x);
        PackY(p.y, y);
        PackXz(p.z, z);
    }
    v("x", x);
    v("y", y);
    v("z", z);
    if constexpr (std::is_same_v<V, Reader>) {
        p.x = float(x) / 32.0f;
        p.y = float(y) / 16.0f;
        p.z = float(z) / 32.0f;
    }
}

} // namespace detail

// A walker's leg: from where it is now to where it stops (equal: it stands), at a speed. Sent on
// the latest lane, keyed by the entity (only the newest leg matters); a tick's legs for a player
// go packed into one frame (net/Framing.h). Most of the world's traffic, so its places are packed
// too (detail::PackXz and the others: 23 bytes where the floats take 36), each place or the
// speed whose values fall outside their range as its floats; `packed` (on the wire only) has a
// bit for each of them that does (1 from, 2 to, 4 the speed). The logs show the plain values.
struct EntityMoved {
    static constexpr MessageId kId = 0x1011;
    std::uint32_t entity = 0;
    WorldPosition from, to;
    float speed = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        if constexpr (std::is_same_v<V, detail::Writer> || std::is_same_v<V, detail::Reader>) {
            std::uint8_t exact = 0;
            if constexpr (std::is_same_v<V, detail::Writer>) {
                std::uint16_t qx = 0, qs = 0;
                std::int16_t qy = 0;
                const auto fits = [&](const WorldPosition& p) { return detail::PackXz(p.x, qx) && detail::PackY(p.y, qy) && detail::PackXz(p.z, qx); };
                exact = std::uint8_t((fits(from) ? 0 : 1) | (fits(to) ? 0 : 2) | (detail::PackSpeed(speed, qs) ? 0 : 4));
            }
            v("packed", exact);
            detail::LegPlace(v, (exact & 1) != 0, from);
            detail::LegPlace(v, (exact & 2) != 0, to);
            if (exact & 4) {
                v("speed", speed);
            } else {
                std::uint16_t qs = 0;
                if constexpr (std::is_same_v<V, detail::Writer>) detail::PackSpeed(speed, qs);
                v("speed", qs);
                if constexpr (std::is_same_v<V, detail::Reader>) speed = float(qs) / 100.0f;
            }
        } else {
            v("from", from);
            v("to", to);
            v("speed", speed);
        }
    }
};

// --- Who is around (0x1020) -------------------------------------------------------------------

// An entity comes into view, with its leg if it is walking.
// What an entity is: a character (players, NPCs: `character` says which), an item lying on the
// ground (C7i: `character.model` is its itemdata RefObjID, `amount` a stack's count or the gold,
// `plus` its plus) or a monster (`character.model` its characterdata RefObjID, `character.hp` its
// health now, `rarity` its kind: 0 general, 1 champion, 3 unique, 4 giant, 5 titan, 6 elite, as the
// target window shows them).
// A skill object (Harmony's circle, the poison field, the fire trap: a skill with an area of kind 3): its
// `character.model` is the skill's id, its `amount` its owner's entity.
// `fresh`: a ground item that has just fallen, told so to the players who see it fall (the server's
// first pass after it was put down); one already lying there when a player comes into sight of it is
// not. The 1.188 client plays a fresh item's drop sound and, for gold, its coin shower first.
// A COS (a player's ride, transport or pet): its `character.model` its characterdata RefObjID,
// `character.hp` its health, its `amount` its owner's entity.
// A thief or hunter monster (the jobs' bandits): its `amount` the byte its look is built from
// (the original's extra spawn byte after the rarity).
enum class EntityKind : std::uint8_t { Character = 0, GroundItem = 1, Monster = 2, SkillObject = 3, Cos = 4 };

// A monster's most health: its reference's (characterdata col 59) times its kind's, as the client
// works it out for the target window (champion x 2, giant x 20, titan
// x 100, elite x 4).
inline std::uint32_t MonsterMaxHp(std::uint32_t referenceHp, std::uint8_t rarity) {
    switch (rarity) {
    case 1: return referenceHp * 2;
    case 4: return referenceHp * 20;
    case 5: return referenceHp * 100;
    case 6: return referenceHp * 4;
    default: return referenceHp;
    }
}

struct EntityAppeared {
    static constexpr MessageId kId = 0x1020;
    std::uint32_t entity = 0;
    CharacterSummary character; // model, name, look (players, NPCs; a ground item: its model only)
    WorldPosition position;
    float yaw = 0;
    bool moving = false;
    WorldPosition to;
    float speed = 0;
    EntityKind kind = EntityKind::Character;
    std::uint32_t amount = 0;
    std::uint8_t plus = 0;
    std::uint8_t rarity = 0;
    bool fresh = false;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("character", character);
        v("position", position);
        v("yaw", yaw);
        v("moving", moving);
        v("to", to);
        v("speed", speed);
        v("kind", kind);
        v("amount", amount);
        v("plus", plus);
        v("rarity", rarity);
        v("fresh", fresh);
    }
};

// Something a walker does where it stands, for everyone who sees it (the original's action answer):
// for now picking an item up (the PICK clip).
enum class EntityActionKind : std::uint8_t { PickUp = 1 };

struct EntityAction {
    static constexpr MessageId kId = 0x1012;
    std::uint32_t entity = 0;
    EntityActionKind action = EntityActionKind::PickUp;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("action", action);
    }
};

// A player's own view (the client's Online options): at most this many
// players and monsters, within this radius in the minimap's units (0: the server's). A player can
// only lower its view: the server keeps the smaller of this and its own, and answers with what it
// applies (ViewLimitsAnswer, also sent once on entering the world, with its own most).
struct ViewLimits {
    static constexpr MessageId kId = 0x1013;
    std::uint16_t players = 0, monsters = 0;
    std::uint16_t radius = 0;
    template <typename V>
    void Visit(V& v) {
        v("players", players);
        v("monsters", monsters);
        v("radius", radius);
    }
};
struct ViewLimitsAnswer {
    static constexpr MessageId kId = 0x1014;
    std::uint16_t players = 0, monsters = 0;       // what this player's view holds at most now
    std::uint16_t maxPlayers = 0, maxMonsters = 0; // the server's shares (what a slider may reach)
    std::uint16_t radius = 0;                      // how far a view reaches, in the minimap's units
    template <typename V>
    void Visit(V& v) {
        v("players", players);
        v("monsters", monsters);
        v("maxPlayers", maxPlayers);
        v("maxMonsters", maxMonsters);
        v("radius", radius);
    }
};

struct EntityGone {
    static constexpr MessageId kId = 0x1021;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

// --- Teleports (0x1030) -----------------------------------------------------------------------

// At a gate, the point it links to: `gate` is the gate's reference id
// (teleportbuilding), `destination` a teleportdata id. On Ok the traveller is taken there and
// enters again (WorldEntered: the picture starts afresh); otherwise GateRefused.
struct UseGate {
    static constexpr MessageId kId = 0x1030;
    std::uint32_t gate = 0;
    std::uint32_t destination = 0;
    template <typename V>
    void Visit(V& v) {
        v("gate", gate);
        v("destination", destination);
    }
};

enum class GateStatus : std::uint8_t {
    Ok = 0,
    NoSuchGate = 1,    // not a gate, or no point of its own
    TooFar = 2,        // the traveller is not at it
    NoSuchLink = 3,    // it does not lead there
    Nowhere = 4,       // the destination has no arrival place
    NotEnoughGold = 5, // the link's fee is more than the traveller has
    NoRecall = 6,      // its point may not be designated as the return point
    ServerDown = 7,    // the destination's game server is not running
    // The link's restrictions (the original's codes and the 1.188 client's texts):
    WrongLevel = 8,    // 0x1C15 UIIT_MSG_INTERACTION_FAIL_OUT_OF_REQUIRED_LEVEL_FOR_TELEPORT
    Transport = 9,     // 0x1C10 UIIT_MSG_INTERACTION_FAIL_CANT_USE_TELEPORT_WITH_TRADECART
    Condition = 10,    // 0x1C23 / 0x1C24 UIIT_MSG_QUEST_ROC_CANNOT_ENTRANCE (an item, the job's state)
    TransportFar = 11, // 0x1C13 UIIT_MSG_INTERACTION_FAIL_TOO_FAR_FROM_TRADECART: its transport over 1000 away
    // A murderer: a gate structure in the main world (0x1C16,
    // UIIT_MSG_INTERACTION_FAIL_MURDERER_CAN_USE_PORTAL_STRUCTURE); a link's restriction kind 3 on the PvP state
    // (0x1C19; no link of the 1.188 data has one).
    Murderer = 12, PvpState = 13,
    // The Pharaoh's tomb's (the original's world answers 0x11, 0x12, 0x13, 2, 0xE, 7 as teleport
    // errors 0x1C27, 0x1C28, 0x1C2A, 3, 0x1C25, 0x1C29):
    TombEntries = 14,   // UIIT_MSG_EGYPT_TOMB_ENTRANCE_CONDITION_ERROR_01: the day's entries used
    TombMaster = 15,    // _ERROR_02: the party's master has not gone in, or chose another difficulty
    TombParty = 16,     // _ERROR_05: a party of two at least
    InstanceFull = 17,  // UIIT_MSG_INTERACTION_FAIL_OUT_OF_REQUIRED_NUMBER_FOR_TELEPORT
    InstanceNone = 18,  // UIIT_MSG_INSTANCEDUNGEON_ERROR_MSG_01: none can be made now
    InInstance = 19,    // UIIT_MSG_INSTANCEDUNGEON_ERROR_MSG_02: already in one
    Riding = 20,        // 0x1C30 UIIT_MSG_INTERACTION_FAIL_CANT_USE_TELEPORT_WITH_RIDE
    JobSuit = 21,       // UIIT_MSG_FLAGWAR_GAME_JOB_NOT_ENTER (0x1C2B, the world's job-suit refusal)
    // The job temples' (answers 0x14, 0x10 / 0x19, 0x17, 0x15 as 0x1C2C, 0x1C2F, 0x1C2E, 0x1C2D):
    ShrineLevel = 22,   // UIIT_MSG_EGYPT_SHRINE_ENTRANCE_CONDITION_ERROR_01: the level
    ShrineNoRight = 23, // _ERROR_02: "Has no right to move on." (the reputation's alliance, a shut temple)
    ShrineJobSuit = 24, // _ERROR_03: no job suit on
    ShrineRatio = 25,   // _ERROR_04: the alliances' share
    // Into a fortress's world (0x1C1A..0x1C1E; game::FortressError::Entry*):
    FortressJobSuit = 26,        // UIIT_MSG_FORT_ENTRANCE_FORTRESS_FAIL_01
    FortressFull = 27,           // UIIT_MSG_FORT_PORTALSTONE_FAIL_04
    FortressNotParticipant = 28, // UIIT_MSG_FORT_ENTRANCE_FORTRESS_FAIL_02
    FortressNotMember = 29,      // UIIT_MSG_FORT_NOT_POSSESS_FORTRESS_MEMBER
    FortressClosed = 30,         // UIIT_MSG_FORT_ETC_ERR_CANT_ENTER_5MINBEFORE
    Returning = 31,  // 0x1C14 UIIT_MSG_INTERACTION_FAIL_TELESCROLL_ALREADY_WORKING: a return scroll counting down
    ThiefDen = 32,   // 0x1C18 UIIT_INTERACTION_FAIL_ONLY_ROBBER_CAN_TELEPORT_TO_THIEFDEN: a trader's or hunter's league
    Busy = 33,       // 0x1C0B UIIT_MSG_INTERACTION_FAIL_IM_BUSY: an exchange, a stall, an alchemy run (an NPC's talk)
};

// The world goes on on another game server of the shard: connect there and show
// the ticket; the character enters at once (WorldEntered, as after any teleport).
struct ServerMove {
    static constexpr MessageId kId = 0x1038;
    std::string host;
    std::uint16_t port = 0;
    std::vector<std::uint8_t> ticket;
    template <typename V>
    void Visit(V& v) {
        v("host", host, 255);
        v("port", port);
        v("ticket", ticket, 64);
    }
};

struct GateRefused {
    static constexpr MessageId kId = 0x1031;
    GateStatus status = GateStatus::Ok;
    template <typename V>
    void Visit(V& v) {
        v("status", status);
    }
};

// Talking to a gate (C7i): the traveller at it asks what it offers;
// the server answers with the services the NPC window lists (the original's NPC select answer
// carries them as a mask: kGateRecallPoint "Designate as return/recall point", kGateTeleport "Select
// teleport area"), or GateRefused.
constexpr std::uint32_t kGateRecallPoint = 0x40, kGateTeleport = 0x80;

struct GateTalk {
    static constexpr MessageId kId = 0x1032;
    std::uint32_t gate = 0;
    template <typename V>
    void Visit(V& v) {
        v("gate", gate);
    }
};

// The fortress tax at an NPC or a gate (the original's select answer 0xB045 ends an NPC
// of a fortress's with its u16 rate): the fortress taxing it (0: none), its rate, and whether the rate is waived
// for the player (a positive one for the holder's guild and its union; the 1.188 client decides it by the guilds'
// names, ours by the server). Prices show with game::FortressTax added (a purchase) or taken off (a sale).
struct NpcTax {
    std::uint32_t fortress = 0;
    std::int16_t rate = 0;
    bool exempt = false;
    template <typename V>
    void Visit(V& v) {
        v("fortress", fortress);
        v("rate", rate);
        v("exempt", exempt);
    }
};

struct GateMenu {
    static constexpr MessageId kId = 0x1033;
    std::uint32_t gate = 0;
    std::uint32_t services = 0;
    NpcTax tax;
    template <typename V>
    void Visit(V& v) {
        v("gate", gate);
        v("services", services);
        v("tax", tax);
    }
};

// Talking to an NPC (C7i; the original's select of an NPC, 0x7045): the player near it asks what
// it offers; the server answers with its services (the NPC window's menu, the original's mask:
// kNpcStorage "Deposit into storage."...), or GateRefused (TooFar, NoSuchGate when it is no NPC).
constexpr std::uint32_t kNpcShop = 0x1, kNpcTalk = 0x2, kNpcStorage = 0x4;
// It keeps guilds (the original's NPC function option 15): create, leave,
// disband, promote.
constexpr std::uint32_t kNpcGuild = 0x8;

struct NpcTalk {
    static constexpr MessageId kId = 0x1036;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

// A trader league member at a specialty NPC (the select answer's tail): the shard's
// TRADE_SAFETY_OPTION (0 counted, 1 never, 2 no limit; 0xFF none: not a trader there) and, counted, the day's limit and
// count. The talk's top line "Safe Trade Attempts [count]/[limit]" when the option is 0.
struct SafeTradeTalk {
    std::uint8_t option = 0xFF, limit = 0, count = 0;
    template <typename V>
    void Visit(V& v) {
        v("option", option);
        v("limit", limit);
        v("count", count);
    }
};

struct NpcMenu {
    static constexpr MessageId kId = 0x1037;
    std::uint32_t entity = 0;
    std::uint32_t services = 0;
    NpcTax tax;
    SafeTradeTalk safe;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("services", services);
        v("tax", tax);
        v("safe", safe);
    }
};

// The gate's point becomes the character's return point (kept by the server: return scrolls and
// resurrection take it there); ReturnPointSet, or GateRefused.
struct DesignateReturn {
    static constexpr MessageId kId = 0x1034;
    std::uint32_t gate = 0;
    template <typename V>
    void Visit(V& v) {
        v("gate", gate);
    }
};

struct ReturnPointSet {
    static constexpr MessageId kId = 0x1035;
    std::uint32_t point = 0; // a teleportdata id
    template <typename V>
    void Visit(V& v) {
        v("point", point);
    }
};

// --- Logging out (0x1040) -------------------------------------------------------------------

// The system menu's Restart and Exit: the client asks, the server
// counts down and then takes the character out of the world (WorldLeft; on Exit the client then
// quits). The client shows the seconds left itself, one a second. A move or a teleport during the
// countdown cancels it.
enum class LogoutKind : std::uint8_t {
    Exit = 1,    // to Windows
    Restart = 2, // to the character select
};

struct LogoutRequest {
    static constexpr MessageId kId = 0x1040;
    LogoutKind kind = LogoutKind::Restart;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
    }
};

// The original's 0xB005 / 0xB006: the count 5 s, 60 s with trade goods in a COS;
// refused in combat (0x801) for those the original keeps (a job suit, free PvP, the fortress world at war, a guild at
// war) and while teleporting (0x802). Only the player's own cancel (LogoutCancel) stops a count.
enum class LogoutStatus : std::uint8_t {
    Counting = 0,   // `seconds` until the character leaves
    Cancelled = 1,  // the countdown stopped (the player's cancel)
    InCombat = 2,   // refused: not during a fight (0x801)
    Teleporting = 3, // refused: not on the way through a gate (0x802)
};

// The count stopped by the player (the original's 0x7006: its client sends it on a click in the world while counting).
struct LogoutCancel {
    static constexpr MessageId kId = 0x1042;
    template <typename V>
    void Visit(V&) {}
};

struct LogoutAnswer {
    static constexpr MessageId kId = 0x1041;
    LogoutStatus status = LogoutStatus::Counting;
    LogoutKind kind = LogoutKind::Restart;
    std::uint8_t seconds = 0;
    template <typename V>
    void Visit(V& v) {
        v("status", status);
        v("kind", kind);
        v("seconds", seconds);
    }
};

// --- Quick slots (0x1050) ------------------------------------------------------------------

// The underbar's shortcuts: the server keeps each character's 51
// (0 the right button's, 1..40 the pages', 41..50 the extra bar's) and sends them when it enters;
// the client tells every change, as the original's 0x7541. Kinds as game::QuickSlot::Kind (0 empty).
struct QuickSlotRecord {
    std::uint8_t slot = 0;
    std::uint8_t kind = 0;
    std::uint32_t value = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
        v("kind", kind);
        v("value", value);
    }
};

struct QuickSlotSet {
    static constexpr MessageId kId = 0x1050;
    QuickSlotRecord record;
    template <typename V>
    void Visit(V& v) {
        v("record", record);
    }
};

// The filled ones, when the character enters the world (not again after a teleport).
struct QuickSlotList {
    static constexpr MessageId kId = 0x1051;
    std::vector<QuickSlotRecord> slots;
    template <typename V>
    void Visit(V& v) {
        v("slots", slots, 51);
    }
};

// --- Chat (0x1060) ----------------------------------------------------------------------------

// The chat. The kinds keep the original's chat type numbers.
enum class ChatKind : std::uint8_t {
    All = 1,      // the people around the speaker (those who see it)
    Whisper = 2,  // one player, by name
    Gm = 3,       // a game master to the people around
    Party = 4,
    Guild = 5,
    Global = 6,   // every player on the server (the Global chatting item)
    Notice = 7,   // the server's notice: no speaker
    Stall = 9,    // the people at a stall
    Union = 11,
    Academy = 16,
};

// A line is at most 100 characters (the original's input takes 100 bytes; ours counts characters).
constexpr std::size_t kChatCharacters = 100;

// A line the player typed: `target` only for a whisper. `index` (0..250, the client's counter)
// comes back in the answer: the client prints its own line once the server took it.
struct ChatSend {
    static constexpr MessageId kId = 0x1060;
    ChatKind kind = ChatKind::All;
    std::uint8_t index = 0;
    std::string target;
    std::string text;
    std::uint8_t slot = 0; // a global line: the bag place of the Global chatting item that pays for it
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("index", index);
        v("target", target, 32);
        v("text", text, kChatCharacters * 4);
        v("slot", slot);
    }
};

// Why a line was not taken (the original's chat error codes; the gaps are codes it never shows).
enum class ChatResult : std::uint8_t {
    Ok = 0,
    TargetNotFound = 3, // nobody of that name is in the world
    WhispersOff = 6,    // the target takes no whispers
    InvalidCommand = 8, // a kind this player may not send, or a bad text
    NotInParty = 10,
    NotAlly = 11,
    NotInUnion = 12,    // union chat from a guild in no union
    Restricted = 13,    // too many lines too fast: `seconds` until the player may chat again
    NoUnionRight = 14,
};

struct ChatAnswer {
    static constexpr MessageId kId = 0x1061;
    ChatResult result = ChatResult::Ok;
    ChatKind kind = ChatKind::All;
    std::uint8_t index = 0;
    std::uint16_t seconds = 0;
    template <typename V>
    void Visit(V& v) {
        v("result", result);
        v("kind", kind);
        v("index", index);
        v("seconds", seconds);
    }
};

// Someone's line: `sender` is the speaker's entity when the listener sees it (All, Gm; 0
// otherwise), `name` its name (empty for a notice). The speaker's own lines come back as the
// answer, except a global line, which reaches everyone the same way.
struct ChatLine {
    static constexpr MessageId kId = 0x1062;
    ChatKind kind = ChatKind::All;
    std::uint32_t sender = 0;
    std::string name;
    std::string text;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("sender", sender);
        v("name", name, 32);
        v("text", text, kChatCharacters * 4);
    }
};

// --- Items (0x1070) ---------------------------------------------------------------------------

// The inventory: places 0..12 are what the character wears (head, chest,
// shoulders, hands, legs, feet, weapon, shield or ammunition, job suit, earring, necklace, two
// rings: the original's numbers), 13.. the bag. One item where it lies; fields that do not apply
// to its kind are 0 (the original sends them by kind).
// A made socket of an item (the original's binding block of type 1): its index, the stone's
// skill at its level and the stone (0, 0, 0: empty).
struct SocketRecord {
    std::uint8_t slot = 0;
    std::uint32_t skill = 0;
    std::uint8_t level = 0;
    std::uint32_t stone = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
        v("skill", skill);
        v("level", level);
        v("stone", stone);
    }
};
struct ItemRecord {
    std::uint8_t slot = 0;
    std::uint32_t item = 0; // itemdata RefObjID
    std::uint8_t plus = 0;
    std::uint64_t variance = 0;
    std::uint32_t durability = 0;
    std::uint16_t quantity = 1;
    std::vector<std::uint64_t> magic; // option id | value << 32, at most 12
    std::uint8_t advanced = 0;        // an advanced elixir's plus (0: none)
    std::vector<SocketRecord> sockets; // its made sockets (at most 3)
    std::string creator;               // who made it (a specialty good's buyer's alias; empty: none)
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
        v("item", item);
        v("plus", plus);
        v("variance", variance);
        v("durability", durability);
        v("quantity", quantity);
        v("magic", magic, 12);
        v("advanced", advanced);
        v("sockets", sockets, 3);
        v("creator", creator, 32);
    }
};

// The inventory when the character enters the world (after WorldEntered; not again after a
// teleport, like QuickSlotList).
struct InventoryList {
    static constexpr MessageId kId = 0x1070;
    std::uint8_t size = 45; // the places, the 13 worn ones included (at most 109: Inventory::kMaxSize)
    std::uint64_t gold = 0;
    std::vector<ItemRecord> items;
    template <typename V>
    void Visit(V& v) {
        v("size", size);
        v("gold", gold);
        v("items", items, 109);
    }
};

// The bag's places grew (a quest's reward): its new size, the worn places included.
struct InventoryGrown {
    static constexpr MessageId kId = 0x107B;
    std::uint8_t size = 45;
    template <typename V>
    void Visit(V& v) {
        v("size", size);
    }
};

// Move an item (the original's 0x706D kind 0): within the bag, from the bag onto the body and back,
// ring to ring. `quantity` 0: all of it; less than the stack onto an empty place: a split.
struct ItemMove {
    static constexpr MessageId kId = 0x1071;
    std::uint8_t from = 0, to = 0;
    std::uint16_t quantity = 0;
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("to", to);
        v("quantity", quantity);
    }
};

enum class ItemStatus : std::uint8_t {
    Ok = 0,
    NoItem = 1,      // nothing in `from`
    BadSlot = 2,     // out of range, or beyond the inventory's size
    WrongSlot = 3,   // that item is not worn there (or not worn at all)
    Level = 4,       // below the item's level
    Race = 5,        // another country's item
    Sex = 6,         // the other sex's item
    Broken = 7,      // durability 0
    BagFull = 8,     // no room for what the move pushes into the bag (a shield off for a two-handed weapon)
    Failed = 9,      // the server could not
    MixedArmour = 10, // clothes / robes with armour (light and heavy armour mix)
    NotEnoughGold = 11, // a purchase or a buy-back costs more than the character has
    NotForSale = 12,    // the shop has no such good, or the item cannot be sold
    TooFar = 13,        // a ground item out of reach
    NotYours = 14,      // a ground item still kept for whoever it fell for
    GuildBuyDenied = 15,   // a guild's good: its points are for the guild's master to spend (0x18C4)
    GuildPointsShort = 16, // a guild's good: the guild has too few points (0x18C5)
    FortressPosition = 17, // a fortress weapon needs a fortress position that fits it (0x18E0)
    Exchanging = 18,       // the bag and the gold are held by an open exchange (0x1835)
    PlaceTaken = 19,       // an avatar of that kind is worn already (0x180E)
    NeedsDress = 20,       // an attachment with no dress worn (1.188 UIIT_STT_AVATAR_MAIL_ONLY_WEAR)
    JobSuit = 21,          // the job suit's rules refused it: `code` the original's (0x18xx)
    JobChange = 22,        // the job suit goes on or comes off in 10 s (0x189B): the move's ItemMoved comes then
    Mastery = 23,          // a European armour's mastery requirement unmet (0x186C)
    HonorShort = 24,       // a shop good's honour points more than the character has (0x18D4)
    TokensShort = 25,      // a shop good's Egypt tokens not in the bag (0x184A)
    CoinsShort = 26,       // a shop good's Arena Coins not in the bag (0x184D)
    Bound = 27,            // ours: an item bound to its character (the beta quests' weapons) is never dropped nor put in a guild's storage
};

// The answer to the mover: every place the move touched, as it is now (unlike the original, which
// sends the move back and lets the client redo the merge, split or swap).
struct ItemMoved {
    static constexpr MessageId kId = 0x1072;
    ItemStatus status = ItemStatus::Ok;
    std::uint8_t from = 0, to = 0;
    std::vector<ItemRecord> changed;   // the touched places that hold something now
    std::vector<std::uint8_t> emptied; // the touched places that are empty now
    std::uint16_t code = 0;            // JobSuit, JobChange: the original's code
    template <typename V>
    void Visit(V& v) {
        v("status", status);
        v("from", from);
        v("to", to);
        v("changed", changed, 4);
        v("emptied", emptied, 4);
        v("code", code);
    }
};

// --- Avatars ---------------------------------------------------------------

// The avatars the character wears, after InventoryList: each where it is (`slot`: its avatar place, 0 the hat, 1 the
// dress, 2 the attachment, 3 the flag, 4 the Devil's Spirit; game::avatar).
struct AvatarList {
    static constexpr MessageId kId = 0x107C;
    std::vector<ItemRecord> items;
    template <typename V>
    void Visit(V& v) {
        v("items", items, 5);
    }
};

// An avatar put on or taken off (the original's 0x7034 kinds 0x24 and 0x23): `wear`: the one in the bag's place
// `place` into the avatar place of its kind; else the one in the avatar place `place` into the bag's first free place
// (the original's server picks it whatever the client asks), a dress with its attachment into the next.
struct AvatarMove {
    static constexpr MessageId kId = 0x107D;
    bool wear = true;
    std::uint8_t place = 0;
    template <typename V>
    void Visit(V& v) {
        v("wear", wear);
        v("place", place);
    }
};

// The answer to the mover: the bag's places and the avatar places the move touched, as they are now.
struct AvatarMoved {
    static constexpr MessageId kId = 0x107E;
    ItemStatus status = ItemStatus::Ok;
    std::vector<ItemRecord> bag, avatars;                  // what holds something now
    std::vector<std::uint8_t> bagEmptied, avatarsEmptied; // what is empty now
    template <typename V>
    void Visit(V& v) {
        v("status", status);
        v("bag", bag, 2);
        v("avatars", avatars, 2);
        v("bagEmptied", bagEmptied, 2);
        v("avatarsEmptied", avatarsEmptied, 2);
    }
};

// What a character wears changed (the original's 0x3314 / 0x377C in one): to everyone who sees it,
// the owner too. `item` 0: the place is empty now; `previous`: what was there (0: nothing), which
// the look loses.
struct EquipmentChanged {
    static constexpr MessageId kId = 0x1073;
    std::uint32_t entity = 0;
    std::uint8_t slot = 0;
    std::uint32_t item = 0;
    std::uint8_t plus = 0;
    std::uint32_t previous = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("slot", slot);
        v("item", item);
        v("plus", plus);
        v("previous", previous);
    }
};

// Use the item in an inventory place (the original's 0x704C): the server takes one (ItemMoved with
// the place as it is now) and does what it does: a potion's HP and MP, a pill's cure (with its kind's
// wait: Cooldown, and ItemUsed to those who see the user), the speed scrolls, the monster mask; the
// return scroll waits its time (Param1, ms) and takes the character to its return point
// (WorldEntered). Anything else is refused without an answer.
// A bag place's item used. The reverse return scroll (the original's 0x704C option byte)
// carries where to: `option` 2 the last recall point, 3 the place of death, 7 the map's point `point`
// (_RefOptionalTeleport's ID); 0 for every other item. `target`: the user's selection (an entity, 0 none), which the coded
// quests' items take as their monster (the original's server reads its player's selected target: the traps, the
// yeoha's branch).
struct UseItem {
    static constexpr MessageId kId = 0x1075;
    std::uint8_t slot = 0;
    std::uint8_t option = 0;
    std::uint32_t point = 0;
    std::uint32_t target = 0;
    template <typename V>
    void Visit(V& v) {
        v("slot", slot);
        v("option", option);
        v("point", point);
        v("target", target);
    }
};
// The reverse return scroll's choices (UseItem::option; the original's).
constexpr std::uint8_t kReverseToRecall = 2, kReverseToDeath = 3, kReverseToPoint = 7;

// An item's use refused with the original's code (its 0xB04C answer with result 2; the client prints its text):
// for the reverse return scroll 0x18F5 in an instance, 0x187A at the moment, 0x185F, 0x185D already travelling,
// 0x1875 a murderer, 0x185E a transport out, 0x1885 no recall point, 0x1886 no place of death, 2 no such point,
// 0x18FD the point's level.
struct ItemRefused {
    static constexpr MessageId kId = 0x107F;
    std::uint32_t item = 0; // itemdata RefObjID
    std::uint16_t code = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item);
        v("code", code);
    }
};

// The return scroll is being read (the casting gauge): the scroll and its
// time. It ends with the teleport (WorldEntered) or a cancel (ReturnCancelled).
struct ReturnStarted {
    static constexpr MessageId kId = 0x1076;
    std::uint32_t item = 0; // itemdata RefObjID
    std::uint32_t ms = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item);
        v("ms", ms);
    }
};

// The gauge's cancel (the original's 0x72DD): the scroll is not given back.
struct CancelReturn {
    static constexpr MessageId kId = 0x1077;
    template <typename V>
    void Visit(V&) {}
};

struct ReturnCancelled {
    static constexpr MessageId kId = 0x1078;
    template <typename V>
    void Visit(V&) {}
};

// A buff on the character (the buff board's icons): a skill's effect for `ms` (a speed scroll's,
// for now). Sent when it starts, again with what is left after each WorldEntered (which clears
// the client's list), and replaced when the same skill starts again (the same token).
struct BuffAdded {
    static constexpr MessageId kId = 0x1079;
    std::uint32_t token = 0; // the server's instance of it
    std::uint32_t skill = 0; // skilldata id
    std::uint32_t ms = 0;    // left
    template <typename V>
    void Visit(V& v) {
        v("token", token);
        v("skill", skill);
        v("ms", ms);
    }
};

struct BuffRemoved {
    static constexpr MessageId kId = 0x107A;
    std::uint32_t token = 0;
    template <typename V>
    void Visit(V& v) {
        v("token", token);
    }
};

// --- Items on the ground (0x10A0; C7i) ------------------------------------------------------------

// An item of the bag dropped at the character's feet (the original's 0x706D kind 7), gold dropped
// (kind 0x0A); a ground item picked up (its entity): the server walks the character to it when it is
// farther than its reach and picks it up on arrival, as the original's does (no "too far"). ItemMoved answers for the bag, GoldChanged for
// the gold; the item comes and goes for everyone near as an entity (EntityAppeared / EntityGone).
struct DropItem {
    static constexpr MessageId kId = 0x10A0;
    std::uint8_t place = 0;
    template <typename V>
    void Visit(V& v) {
        v("place", place);
    }
};

struct DropGold {
    static constexpr MessageId kId = 0x10A1;
    std::uint64_t amount = 0;
    template <typename V>
    void Visit(V& v) {
        v("amount", amount);
    }
};

struct PickUp {
    static constexpr MessageId kId = 0x10A2;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

// --- Fighting (0x10B0; C10b) -----------------------------------------------------------------------

// The player's normal attack on `target` (a monster): the server walks the character into reach
// and swings until the target dies or another command comes; 0 stops.
struct AttackTarget {
    static constexpr MessageId kId = 0x10B0;
    std::uint32_t target = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
    }
};

// A swing, for everyone who sees the attacker: `attacker` uses `skill` (skilldata id; the basic
// attacks are skills too) on `target`; its animation starts now. Each of the skill's hits lands
// later as an EntityDamaged.
// A cast its caster's server gave up before its release (a failed re-check on a skill tick, a
// stun, a knock, a freeze or a sleep on the caster): nothing paid, nothing struck. Those
// who see the caster end its presentation.
struct EntityCastCancelled {
    static constexpr MessageId kId = 0x10BE;
    std::uint32_t entity = 0, skill = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("skill", skill);
    }
};

struct EntitySwing {
    static constexpr MessageId kId = 0x10B1;
    std::uint32_t attacker = 0, target = 0, skill = 0;
    template <typename V>
    void Visit(V& v) {
        v("attacker", attacker);
        v("target", target);
        v("skill", skill);
    }
};

// A teleport skill's move (ours; the original's skill result flags 8 and 2 with their point), to everyone who sees the
// caster, the caster too, just before its swing: a
// dash (Ghost Walk, 'tele') runs the caster to `to` at kBlinkDashSpeed facing it, its WAIT clip and effect on
// the way; a put (the Wizard's Teleport, 'tel2') sets it there at once. The server has it there already: its
// stand there (EntityMoved) follows and changes nothing.
enum class BlinkKind : std::uint8_t { Dash = 1, Put = 2 };
inline constexpr float kBlinkDashSpeed = 500.0f; // units a second
struct EntityBlink {
    static constexpr MessageId kId = 0x10BD;
    std::uint32_t entity = 0, skill = 0;
    BlinkKind kind = BlinkKind::Dash;
    WorldPosition to;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("skill", skill);
        v("kind", kind);
        v("to", to);
    }
};

// A hit landed, for everyone who sees the target: it lost `damage` to `attacker` and has `hp` left.
// kDamageState: a state's tick (burn, poison, bleed), which the target does
// not react to. kDamageCompanion: the strike of the attacker's companion (the Bow's hawk, its 'summ'
// buff; the original's 0x30D1, B 5.6), `imbue` then the companion's
// skill: the client flies it to the target and shows the hit as it attacks. kDamageReturn: a skill's
// strike that is no swing's (an aura's pulse, a skill object's; the original's B0BC forms 2 and 3),
// `imbue` then its skill: shown at once, its S_RETURN stages on the target. kDamageBurst: a time bomb
// going off on its holder (the original's B0BC form 4), `imbue` its
// skill: its S_RETURN stages that play at the source play on the holder (the 1.188 client's case 4).
enum DamageFlags : std::uint8_t {
    kDamageCritical = 1, kDamageKilled = 2, kDamageBlocked = 4, kDamageState = 8, kDamageCompanion = 16, kDamageReturn = 32,
    kDamageBurst = 64
};
struct EntityDamaged {
    static constexpr MessageId kId = 0x10B2;
    std::uint32_t target = 0, attacker = 0, damage = 0, hp = 0;
    std::uint8_t flags = 0;
    std::uint32_t imbue = 0; // the attacker's imbue whose force the hit carried (its skill; 0: none): its hit effect shows
    // The hit's place in its swing (0 the first; a chain's rows count on): the client shows it at the
    // swing's damage moment of that number, as the 1.188 client shows its results.
    std::uint8_t strike = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
        v("attacker", attacker);
        v("damage", damage);
        v("hp", hp);
        v("flags", flags);
        v("imbue", imbue);
        v("strike", strike);
    }
};

// A state an entity is in: its kind (game::StateKind), its level and the time
// it has left.
struct StateRecord {
    std::uint8_t kind = 0;
    std::uint8_t level = 0;
    std::uint32_t ms = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("level", level);
        v("ms", ms);
    }
};
// The states an entity is in, for everyone who sees it: whenever one starts, ends or is replaced
// (all of them), and to a newcomer after its EntityAppeared. The icons, effects and motions.
// The buffs an entity holds (their skills, imbues and toggles too), as everyone who sees it is told
// them on every change and newcomers after its appearance: their effects show on it (skilleffect.txt's
// ACT_S as one comes, ACT_L while it lasts, DEACT as it goes). The
// holder's own board comes with BuffAdded / BuffRemoved.
struct EntityBuffs {
    static constexpr MessageId kId = 0x10BB;
    std::uint32_t entity = 0;
    std::vector<std::uint32_t> skills;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("skills", skills);
    }
};

// A skill object struck (the original's B0BC: form 2 a field's pulse, form 3 a trap going off
// 31-skill-presentation.md B 5.4), for everyone who sees it: its hits follow as EntityDamaged from its
// owner; a trap is gone after it.
struct SkillObjectStruck {
    static constexpr MessageId kId = 0x10BC;
    std::uint32_t object = 0, skill = 0;
    bool trap = false;
    template <typename V>
    void Visit(V& v) {
        v("object", object);
        v("skill", skill);
        v("trap", trap);
    }
};

struct EntityStates {
    static constexpr MessageId kId = 0x10B9;
    std::uint32_t entity = 0;
    std::vector<StateRecord> states;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("states", states);
    }
};

// A hit knocked its target down or back, for everyone who sees it: it is
// pushed to `to` and cannot act for `ms` (down: it lies, then rises).
enum class KnockKind : std::uint8_t { Down = 1, Back = 2 };
struct EntityKnocked {
    static constexpr MessageId kId = 0x10BA;
    std::uint32_t entity = 0, attacker = 0;
    KnockKind kind = KnockKind::Down;
    WorldPosition to;
    std::uint32_t ms = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("attacker", attacker);
        v("kind", kind);
        v("to", to);
        v("ms", ms);
    }
};

// The player's own health and mana and their most (on entering, and whenever they change).
struct Vitals {
    static constexpr MessageId kId = 0x10B3;
    std::uint32_t hp = 0, maxHp = 0, mp = 0, maxMp = 0;
    template <typename V>
    void Visit(V& v) {
        v("hp", hp);
        v("maxHp", maxHp);
        v("mp", mp);
        v("maxMp", maxMp);
    }
};

// Dead: back to life (the death window's choice; 1: at the return point).
struct Resurrect {
    static constexpr MessageId kId = 0x10B4;
    std::uint8_t kind = 1;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
    }
};

// --- Experience (0x10B5; C10c) ---------------------------------------------------------------------

// The player's level, experience, skill points and stats with the numbers they make (on entering
// and whenever any of them changes).
struct Progress {
    static constexpr MessageId kId = 0x10B5;
    std::uint8_t level = 1;
    std::uint64_t experience = 0;      // in this level
    std::uint32_t skillExperience = 0; // toward the next skill point (400)
    std::uint32_t skillPoints = 0;
    std::uint16_t strength = 0, intelligence = 0, statPoints = 0;
    std::uint32_t phyAttackMin = 0, phyAttackMax = 0, magAttackMin = 0, magAttackMax = 0;
    std::uint16_t phyDefence = 0, magDefence = 0, hitRate = 0, parryRate = 0;
    template <typename V>
    void Visit(V& v) {
        v("level", level);
        v("experience", experience);
        v("skillExperience", skillExperience);
        v("skillPoints", skillPoints);
        v("strength", strength);
        v("intelligence", intelligence);
        v("statPoints", statPoints);
        v("phyAttackMin", phyAttackMin);
        v("phyAttackMax", phyAttackMax);
        v("magAttackMin", magAttackMin);
        v("magAttackMax", magAttackMax);
        v("phyDefence", phyDefence);
        v("magDefence", magDefence);
        v("hitRate", hitRate);
        v("parryRate", parryRate);
    }
};

// What a kill gave the player (its Progress follows): experience and skill experience, and from
// which monster.
struct ExperienceGained {
    static constexpr MessageId kId = 0x10B6;
    std::uint32_t from = 0;
    std::uint64_t experience = 0;
    std::uint32_t skillExperience = 0;
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("experience", experience);
        v("skillExperience", skillExperience);
    }
};

// Someone reached a new level, for everyone who sees it.
struct EntityLevelUp {
    static constexpr MessageId kId = 0x10B7;
    std::uint32_t entity = 0;
    std::uint8_t level = 1;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("level", level);
    }
};

// A stat point spent (the character page's + buttons): 0 strength, 1 intelligence.
struct RaiseStat {
    static constexpr MessageId kId = 0x10B8;
    std::uint8_t stat = 0;
    template <typename V>
    void Visit(V& v) {
        v("stat", stat);
    }
};

// --- Skills (0x10C0; C7f) ---------------------------------------------------------------------------

struct MasteryRecord {
    std::uint32_t mastery = 0;
    std::uint8_t level = 0;
    template <typename V>
    void Visit(V& v) {
        v("mastery", mastery);
        v("level", level);
    }
};

// The character's masteries (every one its race may train, in the table's order, at its level: 0
// not trained; the client's tabs come from this list) and the skills it knows (each group's learned
// level's skilldata id): on entering and after each change.
struct SkillBookList {
    static constexpr MessageId kId = 0x10C0;
    std::vector<MasteryRecord> masteries;
    std::vector<std::uint32_t> skills;
    template <typename V>
    void Visit(V& v) {
        v("masteries", masteries, 64);
        v("skills", skills, 1024);
    }
};

// A mastery's next level, a skill's next level (its skilldata id), with skill points.
struct RaiseMastery {
    static constexpr MessageId kId = 0x10C1;
    std::uint32_t mastery = 0;
    template <typename V>
    void Visit(V& v) {
        v("mastery", mastery);
    }
};
struct LearnSkill {
    static constexpr MessageId kId = 0x10C2;
    std::uint32_t skill = 0;
    template <typename V>
    void Visit(V& v) {
        v("skill", skill);
    }
};
// The answer to either: game::SkillCheck's value (0: done; the book and the progress follow).
struct SkillBookAnswer {
    static constexpr MessageId kId = 0x10C3;
    std::uint8_t status = 0;
    std::uint32_t mastery = 0, skill = 0;
    template <typename V>
    void Visit(V& v) {
        v("status", status);
        v("mastery", mastery);
        v("skill", skill);
    }
};

// A skill used on a target (0: none): an attack skill walks the character into its reach and goes
// off as the next swing; the normal attack carries on after it.
// `point`: where on the ground a skill of a place goes (a teleport: the Chinese Ghost Walk, the
// Wizard's Teleport; `hasPoint` false: none given).
struct UseSkill {
    static constexpr MessageId kId = 0x10C4;
    std::uint32_t skill = 0, target = 0;
    bool hasPoint = false;
    WorldPosition point;
    template <typename V>
    void Visit(V& v) {
        v("skill", skill);
        v("target", target);
        v("hasPoint", hasPoint);
        v("point", point);
    }
};
// Untouchable: just risen, it can neither attack nor be attacked for 5 s. Overlap: a buff it may not
// stand beside runs (one of its overlap groups, or a higher rank of its group).
// InBattle: a stealth or an invisibility while in battle (attacking or attacked within 20 s; 0x3028). Links: its owner
// holds as many links of the skill's kind as it may (0x3029).
enum class SkillUseStatus : std::uint8_t {
    Ok = 0, Unknown = 1, Weapon = 2, Mana = 3, Cooling = 4, Target = 5, NotYet = 6, Dead = 7, Ammunition = 8, Untouchable = 9, Overlap = 10,
    ReviveLevel = 11, // a resurrection on a dead player above its level (K9)
    InBattle = 12, Links = 13,
    Transformed = 14, // a skill while in a monster's shape (0x3030)
    Berserk = 15,     // a stealth or a transform in berserk (0x3031)
    // The jobs' PvP: not both on a battlefield (0x3018), a member of one's party
    // (0x3022), within 10 s of the suit going on (0x3019), the caster riding (0x3021), a job monster one's job may not
    // attack (0x3006).
    NotBattlefield = 16, PartyMember = 17, JobLock = 18, Riding = 19, JobMonster = 20,
    // A thief on a one-star trader in safe trade, or on his transport (0x3024).
    SafeTrade = 21,
    // The PK rules: DailyPK at 15 (0x3014), the points at 200,000 (0x3015), the
    // striker below level 20 (0x3016), the target below 20 (0x3017), the server's PK switch (0x3025), the duplicate skill
    // on or by one under job, free battle or murderer state (0x3039).
    DailyPkLimit = 22, TotalPkLimit = 23, YourLevelTooLow = 24, TargetLevelTooLow = 25, PkProhibited = 26, Duplicate = 27,
    // The target out of the caster's line of sight: the walk on the floor from one to the other is blocked (0x3010).
    Obstacle = 28,
    // A fortress weapon's skill without a fitting fortress position (0x3047, UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06).
    FortressPosition = 29
};
// The action list's count (the original's B074 second byte): the commands the server
// holds for the player, 0, 1 (the one running) or 2 (and one waiting). Two held: the client sends no attack and no
// queued skill, a ground click no stop.
// The weather where the player stands (its region's climate, game::ClimateWeather): sent on
// entering the world, on reaching a region of another weather and when its region's weather changes.
struct WeatherNow {
    static constexpr MessageId kId = 0x1005;
    std::uint8_t kind = 1; // game::WeatherKind: 1 clear, 2 rain, 3 snow
    std::uint8_t amount = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("amount", amount);
    }
};

struct ActionHeld {
    static constexpr MessageId kId = 0x10CE;
    std::uint8_t count = 0;
    template <typename V>
    void Visit(V& v) {
        v("count", count);
    }
};
// The player's stop (the original's 0x7074 phase 2): a ground click with one command held sends it before its walk.
// One held: cancelled, unless a skill whose cast went off or a chain under way; two: the waiting one dropped.
struct ActionStop {
    static constexpr MessageId kId = 0x10CF;
    std::uint8_t reserved = 0;
    template <typename V>
    void Visit(V& v) {
        v("reserved", reserved);
    }
};
struct SkillUseAnswer {
    static constexpr MessageId kId = 0x10C5;
    std::uint32_t skill = 0;
    SkillUseStatus status = SkillUseStatus::Ok;
    template <typename V>
    void Visit(V& v) {
        v("skill", skill);
        v("status", status);
    }
};
// A wait started: a skill's (its group's and its cool-down group's) or an item's (its refId; potions
// share theirs by type), for `ms` from now. The quick slots show it.
enum class CooldownKind : std::uint8_t { Skill = 1, Item = 2 };
struct Cooldown {
    static constexpr MessageId kId = 0x10C6;
    CooldownKind kind = CooldownKind::Skill;
    std::uint32_t id = 0;
    std::uint32_t ms = 0;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("id", id);
        v("ms", ms);
    }
};

// The player ends one of its buffs (a right click on the board): the server ends it unless its skill
// may not be ended ('nbuf' without 'lnks'); BuffRemoved answers.
struct CancelBuff {
    static constexpr MessageId kId = 0x10C7;
    std::uint32_t token = 0;
    template <typename V>
    void Visit(V& v) {
        v("token", token);
    }
};

// A resurrection offered to a dead player (K9, the original's peer
// proposal of kind 4): `caster` cast `skill` on it; it answers with RevivalAnswer. A newer one takes
// its place; rising another way drops it.
struct RevivalOffered {
    static constexpr MessageId kId = 0x10C8;
    std::uint32_t caster = 0, skill = 0;
    template <typename V>
    void Visit(V& v) {
        v("caster", caster);
        v("skill", skill);
    }
};
// The dead player's answer: up where it lies with the skill's HP, MP and experience back, or not.
struct RevivalAnswer {
    static constexpr MessageId kId = 0x10C9;
    std::uint8_t accept = 0;
    template <typename V>
    void Visit(V& v) {
        v("accept", accept);
    }
};

// A skill took hold on a player, for everyone who sees it (itself too): a heal or a cure landing, a
// resurrection's rise once accepted. Its skill's ACT_S shows on it (skilleffect.txt: the heal's
// effect on the healed, the water's resurrection, the Cleric's rebirth); the caster's own swing showed
// the cast. A buff's comes with the buff (EntityBuffs, BuffAdded).
struct SkillTook {
    static constexpr MessageId kId = 0x10CA;
    std::uint32_t entity = 0, caster = 0, skill = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("caster", caster);
        v("skill", skill);
    }
};

// A character's whole look changed, for everyone who sees it (itself too): a transform begun or over
// (the original's 0x3207 / 0x3208): a monster's model (no equipment),
// another player's look (the Rogue's duplicate), or its own again.
struct EntityLook {
    static constexpr MessageId kId = 0x10CB;
    std::uint32_t entity = 0;
    std::uint32_t model = 0;
    std::uint8_t height = 0;
    std::vector<EquippedItem> equipment, avatars;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("model", model);
        v("height", height);
        v("equipment", equipment, 16);
        v("avatars", avatars, 8);
    }
};

// A link the player made (the original's B0BE, to its owner only):
// its skill, its own token (BuffRemoved ends it), whom it is on and its time left. The client shows it
// on its own buffs board when the skill's 'lnks' fourth word is 1, and a mark ('hntp')
// on its minimap.
struct LinkAdded {
    static constexpr MessageId kId = 0x10CC;
    std::uint32_t token = 0;
    std::uint32_t skill = 0;
    std::uint32_t target = 0;
    std::uint32_t ms = 0;
    template <typename V>
    void Visit(V& v) {
        v("token", token);
        v("skill", skill);
        v("target", target);
        v("ms", ms);
    }
};
// Where a mark's target is ('hntp', Tag Point; the original's 0x3028 to its owner; ours:
// once a second), also beyond sight.
struct LinkPosition {
    static constexpr MessageId kId = 0x10CD;
    std::uint32_t token = 0;
    WorldPosition position;
    float yaw = 0;
    template <typename V>
    void Visit(V& v) {
        v("token", token);
        v("position", position);
        v("yaw", yaw);
    }
};

// --- Berserk (0x10E8) --------------------------------------------------------

// The player asks to go berserk (the original's 0x70A7 with 1: the Tab key, the bar's button).
struct BerserkRequest {
    static constexpr MessageId kId = 0x10E8;
    template <typename V>
    void Visit(V&) {}
};
// The refusal (the original's B0A7 codes): 1 the gauge not full, 3 in berserk already, dead
// or the like, 4 the player's body in a state that takes none, 5 transformed, hidden or the like.
enum class BerserkStatus : std::uint8_t { Ok = 0, NotFull = 1, Busy = 3, State = 4, Transformed = 5 };
struct BerserkAnswer {
    static constexpr MessageId kId = 0x10E9;
    BerserkStatus status = BerserkStatus::Ok;
    template <typename V>
    void Visit(V& v) {
        v("status", status);
    }
};
// The player's gauge (0..5; the original's point update kind 4).
struct BerserkPoints {
    static constexpr MessageId kId = 0x10EA;
    std::uint8_t points = 0;
    template <typename V>
    void Visit(V& v) {
        v("points", points);
    }
};
// An entity goes berserk or out of it (the original's body state 1): those who see it, the player too,
// and newcomers after its appearance; `ms` the time left (0 when it ends). `variant`: 0 the first mode
// (SYSTEM_CH_HWANMODE, red), 1 the second (SYSTEM_CH_HWAN2MODE, blue: game::BerserkVariant, from hwan level 5), 2 the
// party link's (SYSTEM_CH_HWAN2LINKMODE; the client shows it, the server does not send it yet).
struct EntityBerserk {
    static constexpr MessageId kId = 0x10EB;
    std::uint32_t entity = 0;
    std::uint8_t on = 0;
    std::uint8_t variant = 0;
    std::uint32_t ms = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("on", on);
        v("variant", variant);
        v("ms", ms);
    }
};

// --- The character's own actions (0x1200; the Action page) -----------------

// The motions a player asks for (the original's 0x704F): walk, run, or sit /
// stand (a toggle). Sitting and standing take kMotionChangeSeconds through kMotionChanging.
constexpr std::uint8_t kMotionStand = 0, kMotionWalk = 2, kMotionRun = 3, kMotionSit = 4, kMotionChanging = 0x12;
constexpr double kMotionChangeSeconds = 1.5;
struct SetMotion {
    static constexpr MessageId kId = 0x1200;
    std::uint8_t motion = 0; // kMotionWalk, kMotionRun or kMotionSit
    template <typename V>
    void Visit(V& v) {
        v("motion", motion);
    }
};
// An entity's motion now (the original's 0x30BF channel 1): those who see it, itself too, and newcomers
// after its appearance. `motion` kMotionStand, kMotionSit or kMotionChanging (towards `to`, sitting down
// or standing up); `walking`: its moves at its walk speed.
struct EntityMotion {
    static constexpr MessageId kId = 0x1201;
    std::uint32_t entity = 0;
    std::uint8_t motion = 0;
    std::uint8_t to = 0;
    std::uint8_t walking = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("motion", motion);
        v("to", to);
        v("walking", walking);
    }
};
// The marks over a player's head (the original's 0x7402 / 0xB402; `_Char.AutoInvestExp`): kMarkBeginner
// (up to level 19), kMarkHelper (SYSTEM_HELPERMARK), kMarkGuildWar (SYSTEM_GUILD_WAR_MARK; the server's).
constexpr std::uint8_t kMarkBeginner = 0x1, kMarkHelper = 0x2, kMarkGuildWar = 0x4;
struct SetMarks {
    static constexpr MessageId kId = 0x1202;
    std::uint8_t marks = 0;
    template <typename V>
    void Visit(V& v) {
        v("marks", marks);
    }
};
// A player's marks: those who see it, itself too, and newcomers after its appearance.
struct EntityMarks {
    static constexpr MessageId kId = 0x1203;
    std::uint32_t entity = 0;
    std::uint8_t marks = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("marks", marks);
    }
};
// Trace (the original's 0x7074 action 3): the player follows `target` (0: stops),
// coming nearer when farther than kTraceFar beyond both bodies, stopping within kTraceNear; a walk, an
// attack or a cast of its own ends it, as does the target going out of sight.
constexpr float kTraceNear = 50.0f, kTraceFar = 80.0f;
struct Trace {
    static constexpr MessageId kId = 0x1204;
    std::uint32_t target = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
    }
};
// The Action page's emotes (the original's 0x7097): a
// gesture, told as it is to everyone who sees the player (the original's 0x3091), the player too. The
// 1.188 client's numbers: 0 greeting, 1 pokun, 2 rush, 3 joy, 4 no, 5 yes, 6 laugh; each plays
// the animation type 50 + the gesture (ANI_EMOTION01..; the models' "default" set) once.
constexpr std::uint32_t kEmoteFirstState = 50;
struct Emote {
    static constexpr MessageId kId = 0x1205;
    std::uint8_t gesture = 0;
    template <typename V>
    void Visit(V& v) {
        v("gesture", gesture);
    }
};
struct EntityEmote {
    static constexpr MessageId kId = 0x1206;
    std::uint32_t entity = 0;
    std::uint8_t gesture = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("gesture", gesture);
    }
};

// Free PvP (the Action page's 1017): a team, 1 Red Hawk, 2 Black Turtle, 3 Blue Dragon,
// 4 White Tiger (hostile to the other teams), 5 Giraffe (hostile to everyone, its own colour too); 0 none. A change
// takes kFreePvpSeconds standing; asking 0 during it cancels it.
constexpr std::uint8_t kFreePvpTeams = 5;
constexpr double kFreePvpSeconds = 10.0;
// The original's refusals (0xB516's codes; the client's texts UIIT_MGS_ERR_FRPVP_*).
enum class FreePvpCode : std::uint16_t {
    Ok = 0,
    CannotEnter = 0x8006,   // "Cannot enter Free PvP mode right now."
    JobState = 0x8007,      // "Cannot enter Free PvP mode while in Job mode."
    CannotLeave = 0x8008,   // "Cannot exit Free PvP mode right now."
    CannotChange = 0x800A,  // "Cannot change teams right now."
    Cancelled = 0x800B,     // ours: a change under way given up (UIIT_MGS_ERR_FRPVP_ON_CANCEL / _CANCEL_CHANGE_TEAM)
};

// The team asked for (the original's 0x7516 {u8}).
struct FreePvp {
    static constexpr MessageId kId = 0x1207;
    std::uint8_t team = 0;
    template <typename V>
    void Visit(V& v) {
        v("team", team);
    }
};

// A request refused or a change given up (0xB516 {2, u16}).
struct FreePvpAnswer {
    static constexpr MessageId kId = 0x1208;
    FreePvpCode code = FreePvpCode::Ok;
    template <typename V>
    void Visit(V& v) {
        v("code", code);
    }
};

// A player's team as those around see it (0xB516 {1, u32 entity, u8 team}; the cape SYSTEM_FREEPVP_RED.. on the
// character); `changing` 1 while a change runs (the original's 0x3041 to those around), with the
// team it goes to in `to`. Also sent with the player to new viewers.
struct EntityFreePvp {
    static constexpr MessageId kId = 0x1209;
    std::uint32_t entity = 0;
    std::uint8_t team = 0;
    std::uint8_t changing = 0;
    std::uint8_t to = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("team", team);
        v("changing", changing);
        v("to", to);
    }
};

// --- NPC shops (0x1090; C7i) ----------------------------------------------------------------------

// What the character sold at shops this session, newest last (the original's repurchase list: the
// last 5): the item as it was and what buying it back costs.
struct BuybackRecord {
    ItemRecord item; // slot: its index in the list
    std::uint64_t price = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item);
        v("price", price);
    }
};

// At a merchant (its entity): its shop is open (the goods are in the client's tables; the server
// checks every deal against them); ShopAnswer with the buy-back list, or GateRefused.
// The shop put away (its window closed; the original's 0x704B in its shop's interaction): at a specialty NPC, a trader
// league member's safe trade is decided.
struct ShopClose {
    static constexpr MessageId kId = 0x1097;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

struct ShopOpen {
    static constexpr MessageId kId = 0x1090;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

// A good bought: the tab (as the NPC's tabs count, assets::ShopTab::index) and its slot, how many
// (stackables; equipment one).
struct ShopBuy {
    static constexpr MessageId kId = 0x1091;
    std::uint32_t entity = 0;
    std::uint8_t tab = 0, slot = 0;
    std::uint16_t count = 1;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("tab", tab);
        v("slot", slot);
        v("count", count);
    }
};

// An item of the bag sold (count 0: all of it).
struct ShopSell {
    static constexpr MessageId kId = 0x1092;
    std::uint32_t entity = 0;
    std::uint8_t place = 0;
    std::uint16_t count = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("place", place);
        v("count", count);
    }
};

// One of the buy-back list bought back.
struct ShopBuyback {
    static constexpr MessageId kId = 0x1093;
    std::uint32_t entity = 0;
    std::uint8_t index = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("index", index);
    }
};

// Every deal's answer (and the open's): the bag's places it changed, the buy-back list as it is now.
// The gold comes in GoldChanged.
struct ShopAnswer {
    static constexpr MessageId kId = 0x1094;
    ItemStatus status = ItemStatus::Ok;
    std::uint32_t entity = 0;
    bool opened = false; // the answer to ShopOpen
    std::vector<ItemRecord> changed;
    std::vector<std::uint8_t> emptied;
    std::vector<BuybackRecord> buyback;
    template <typename V>
    void Visit(V& v) {
        v("status", status);
        v("entity", entity);
        v("opened", opened);
        v("changed", changed, 4);
        v("emptied", emptied, 4);
        v("buyback", buyback, 5);
    }
};

// An NPC's repair (the original's 0x703E {u32 NPC, u8 mode[, u8 slot]}): mode 2 every worn and carried piece (the
// fortress smith's "Item repairing"
// row, the 1.188 client's box kind 0xC "Repair all"), 1 the piece at `place`. Each piece costs
// game::PriceRepair; with too little gold a piece is mended as far as the gold goes and the rest stay as they are.
struct NpcRepair {
    static constexpr MessageId kId = 0x1095;
    std::uint32_t npc = 0;
    std::uint8_t mode = 2;
    std::uint8_t place = 0;
    template <typename V>
    void Visit(V& v) {
        v("npc", npc);
        v("mode", mode);
        v("place", place);
    }
};
// Its answer: done, or the original's code (5 the NPC does not repair, 0x1C07 no gold, 0x1C11 not repairable, 0x1C12 a
// broken piece that cannot be revived); the pieces mended as they are now and the gold it took (the gold in GoldChanged).
struct NpcRepaired {
    static constexpr MessageId kId = 0x1096;
    bool ok = false;
    std::uint16_t code = 0;
    std::uint64_t gold = 0;
    std::vector<ItemRecord> changed;
    template <typename V>
    void Visit(V& v) {
        v("ok", ok);
        v("code", code);
        v("gold", gold);
        v("changed", changed, 120);
    }
};

// --- Storage (0x1080; C7i) ---------------------------------------------------------------------

// At a storage keeper (its entity): the account's storage (StorageList), or GateRefused. Each storage
// message says which storage: the account's (`guild` 0) or its guild's (1: at an NPC that keeps guilds,
// the original's 0x7250..0x7252), refused with StorageRefused.
struct StorageOpen {
    static constexpr MessageId kId = 0x1080;
    std::uint32_t entity = 0;
    std::uint8_t guild = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("guild", guild);
    }
};

struct StorageList {
    static constexpr MessageId kId = 0x1081;
    std::uint8_t size = 150;
    std::uint64_t gold = 0;
    std::vector<ItemRecord> items; // slot: the storage's place
    std::uint8_t guild = 0;
    template <typename V>
    void Visit(V& v) {
        v("size", size);
        v("gold", gold);
        v("items", items, 180);
        v("guild", guild);
    }
};

// An item between the storage and the bag, or within the storage (the original's 0x706D kinds 1,
// 2 and 3); StorageMoved answers with every place it touched as it is now.
struct StorageMove {
    static constexpr MessageId kId = 0x1082;
    bool fromStorage = false, toStorage = false;
    std::uint8_t from = 0, to = 0;
    std::uint16_t quantity = 0; // 0: all
    std::uint8_t guild = 0;
    template <typename V>
    void Visit(V& v) {
        v("fromStorage", fromStorage);
        v("toStorage", toStorage);
        v("from", from);
        v("to", to);
        v("quantity", quantity);
        v("guild", guild);
    }
};

struct StorageMoved {
    static constexpr MessageId kId = 0x1083;
    ItemStatus status = ItemStatus::Ok;
    std::vector<ItemRecord> inventoryChanged, storageChanged;
    std::vector<std::uint8_t> inventoryEmptied, storageEmptied;
    std::uint8_t guild = 0;
    template <typename V>
    void Visit(V& v) {
        v("status", status);
        v("inventoryChanged", inventoryChanged, 4);
        v("storageChanged", storageChanged, 4);
        v("inventoryEmptied", inventoryEmptied, 4);
        v("storageEmptied", storageEmptied, 4);
        v("guild", guild);
    }
};

// Gold into the storage or out of it (the original's 0x706D kinds 0x0B, 0x0C); StorageGoldChanged
// and GoldChanged answer, or nothing when it cannot be (not enough, the storage not open).
struct StorageGold {
    static constexpr MessageId kId = 0x1084;
    bool deposit = true;
    std::uint64_t amount = 0;
    std::uint8_t guild = 0;
    template <typename V>
    void Visit(V& v) {
        v("deposit", deposit);
        v("amount", amount);
        v("guild", guild);
    }
};

struct StorageGoldChanged {
    static constexpr MessageId kId = 0x1085;
    std::uint64_t gold = 0;
    std::uint8_t guild = 0;
    template <typename V>
    void Visit(V& v) {
        v("gold", gold);
        v("guild", guild);
    }
};

// The guild's storage put away (its window closed): another member may open it (the original's 0x7251).
struct StorageClose {
    static constexpr MessageId kId = 0x1086;
    std::uint8_t guild = 1;
    template <typename V>
    void Visit(V& v) {
        v("guild", guild);
    }
};
// The guild's storage refused (the original's codes): 0x4C0D in no guild,
// 0x4C4A the guild below level 2, 0x4C1E no authority; ours 1: another member (`name`) uses it.
struct StorageRefused {
    static constexpr MessageId kId = 0x1087;
    std::uint16_t code = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("code", code);
        v("name", name, 32);
    }
};

// The character's gold now (a fee paid; before the teleport's WorldEntered).
struct GoldChanged {
    static constexpr MessageId kId = 0x1074;
    std::uint64_t gold = 0;
    template <typename V>
    void Visit(V& v) {
        v("gold", gold);
    }
};

// --- Parties and party matching (0x10D0..0x10EF) ----------------------------
//
// A party is kept by the game server in memory, as the original's: its
// settings (game::PartySettingBits: exp shared, items shared, anyone may invite), its master and its
// members in the order they joined. Members are named by their character's id (stable through
// teleports); `entity` is where each is in the world now.

// A member as the party windows show it: its name, level and race, HP and MP in tenths (0..10: the
// original tells them only when a tenth changes), where it is, its portrait's model.
struct PartyMemberRecord {
    std::uint32_t id = 0;     // its character
    std::uint32_t entity = 0; // in the world (0: not there)
    std::string name;
    std::uint8_t level = 1;
    std::uint8_t race = 0; // 0 China, 1 Europe
    std::uint8_t hpTenths = 10, mpTenths = 10;
    WorldPosition position;
    std::uint32_t model = 0; // characterdata RefObjID
    std::string guild;
    // Its two highest masteries (skillmasterydata ids; 0 none): the quick party bar's icons (the
    // original's member news, mask 8).
    std::uint32_t mastery1 = 0, mastery2 = 0;
    template <typename V>
    void Visit(V& v) {
        v("id", id);
        v("entity", entity);
        v("name", name, 32);
        v("level", level);
        v("race", race);
        v("hpTenths", hpTenths);
        v("mpTenths", mpTenths);
        v("position", position);
        v("model", model);
        v("guild", guild, 32);
        v("mastery1", mastery1);
        v("mastery2", mastery2);
    }
};

// The player asks `target` (a player it sees) to party: a new party with `settings` when the player
// is in none (the original's 0x7060), into its party otherwise (0x7062; `settings` unused).
struct PartyInvite {
    static constexpr MessageId kId = 0x10D0;
    std::uint32_t target = 0;
    std::uint8_t settings = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
        v("settings", settings);
    }
};

// The invited player's answer to the request it was sent (PartyInvited).
struct PartyInviteAnswer {
    static constexpr MessageId kId = 0x10D1;
    std::uint8_t accept = 0;
    template <typename V>
    void Visit(V& v) {
        v("accept", accept);
    }
};

// Out of the party (0x7061); the master leaving dismisses it (its row's button says "Dismiss").
struct PartyLeave {
    static constexpr MessageId kId = 0x10D2;
    template <typename V>
    void Visit(V&) {}
};

// The master sends a member away (0x7063): its character's id.
struct PartyKick {
    static constexpr MessageId kId = 0x10D3;
    std::uint32_t member = 0;
    template <typename V>
    void Visit(V& v) {
        v("member", member);
    }
};

// The master changes the party's settings (ours: the original's server takes no such request; its
// party keeps the type it was formed with).
struct PartySetSettings {
    static constexpr MessageId kId = 0x10D4;
    std::uint8_t settings = 0;
    template <typename V>
    void Visit(V& v) {
        v("settings", settings);
    }
};

// What a party matching listing says (the original's 0x7069 record): the leader's party type, its
// purpose (game::PartyPurpose), the levels it takes and its title.
struct PartyMatchForm {
    std::uint8_t settings = 0;
    std::uint8_t purpose = 0;
    std::uint8_t levelMin = 1, levelMax = 1;
    std::string title;
    template <typename V>
    void Visit(V& v) {
        v("settings", settings);
        v("purpose", purpose);
        v("levelMin", levelMin);
        v("levelMax", levelMax);
        v("title", title, 128);
    }
};

// The player's party listed (0x7069), or its listing changed (0x706A) when it has one already. Ours:
// the settings chosen here are the party's (the original only showed the party's type).
struct PartyMatchRegister {
    static constexpr MessageId kId = 0x10D5;
    PartyMatchForm form;
    template <typename V>
    void Visit(V& v) {
        v("form", form);
    }
};

// The player's listing taken off (0x706B).
struct PartyMatchDelete {
    static constexpr MessageId kId = 0x10D6;
    template <typename V>
    void Visit(V&) {}
};

// A page of the listings (0x706C), those that fit the search (the matching window's): a purpose (0xFF
// all), levels the party's range must meet (0: any), a leader's name it must contain (empty: any).
struct PartyMatchList {
    static constexpr MessageId kId = 0x10D7;
    std::uint8_t page = 0;
    std::uint8_t purpose = 0xFF;
    std::uint8_t levelMin = 0, levelMax = 0;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("page", page);
        v("purpose", purpose);
        v("levelMin", levelMin);
        v("levelMax", levelMax);
        v("name", name, 32);
    }
};

// The player asks to join a listed party (0x706D); its leader is asked.
struct PartyMatchJoin {
    static constexpr MessageId kId = 0x10D8;
    std::uint32_t entry = 0;
    template <typename V>
    void Visit(V& v) {
        v("entry", entry);
    }
};

// The leader's answer to the join request it was sent (0x306E's answer).
struct PartyMatchJoinAnswer {
    static constexpr MessageId kId = 0x10D9;
    std::uint8_t accept = 0;
    template <typename V>
    void Visit(V& v) {
        v("accept", accept);
    }
};

// A request to party, to the invited player (the original's proposal 0x3080 of kinds 2 and 3): who
// asks and the party's settings; it answers with PartyInviteAnswer within kPartyProposalSeconds.
struct PartyInvited {
    static constexpr MessageId kId = 0x10DA;
    std::uint32_t from = 0; // the inviter's entity
    std::string name;
    std::uint8_t settings = 0;
    bool join = false; // into an existing party (else a new one)
    template <typename V>
    void Visit(V& v) {
        v("from", from);
        v("name", name, 32);
        v("settings", settings);
        v("join", join);
    }
};

// How a party request went: the original's error codes (0x2C02..0x2C29; the 1.188 client has a text for each), 0 for
// done. `name` the player it concerns when there is one.
enum class PartyStatus : std::uint16_t {
    Ok = 0,
    Unknown = 0x2C02,
    InvalidTarget = 0x2C03,
    TooFar = 0x2C04,
    MatchBusy = 0x2C06,
    OutOfLevel = 0x2C07,
    AlreadyFull = 0x2C08,
    Busy = 0x2C09,         // dead or otherwise unable (no text of its own)
    LowLevel = 0x2C0A,     // below kPartyMinLevel: cannot create a party
    Refused = 0x2C0C,      // the invited player said no to a new party
    NoParty = 0x2C0D,
    NoInviter = 0x2C0E,
    InvalidUser = 0x2C0F,
    Timeout = 0x2C10,      // no answer within kPartyProposalSeconds
    LeaderLeft = 0x2C11,
    ExistingMember = 0x2C12,
    FullShared = 0x2C13,   // the party (an exp-shared one, of 8) is at its most
    FullFree = 0x2C14,     // the party (of 4) is at its most
    JoinRefused = 0x2C17,  // the invited player said no to joining
    OtherParty = 0x2C18,   // the target is in another party
    MatchAgain = 0x2C1B,   // the player has a listing already
    MatchNone = 0x2C1C,    // no such listing
    MatchLeader = 0x2C1D,  // only the master lists the party
    MatchLevel = 0x2C1E,   // the player's level is not in the listing's range
    MatchJoining = 0x2C1F, // a request of the player's is waiting already
    MatchListed = 0x2C25,  // the target has a listing of its own
    JobMismatch = 0x2C23,  // the job states may not party (game::JobsMayParty), or not this purpose (JobMayListPurpose)
    JobChanging = 0x2C26,  // a job suit's change is under way
    ArenaOpponent = 0x2C28, // in a Battle Arena match: a player of the other team
    ArenaMatching = 0x2C29, // in a Battle Arena match: no party matching listing
    NoAlias = 5,           // a listing in a job suit without an alias (its client line "Wrong calculation.")
    NotLeader = 0x2C30,    // ours: only the master may do it
};
enum class PartyAction : std::uint8_t { Invite = 1, Answer = 2, Leave = 3, Kick = 4, Settings = 5 };
struct PartyResult {
    static constexpr MessageId kId = 0x10DB;
    PartyAction action = PartyAction::Invite;
    PartyStatus status = PartyStatus::Ok;
    std::string name;
    template <typename V>
    void Visit(V& v) {
        v("action", action);
        v("status", status);
        v("name", name, 32);
    }
};

// The whole party, to a member: when it joins (a new party's two members too) and whenever it enters
// the world again (a teleport). Its own record is among the members.
struct PartyInfo {
    static constexpr MessageId kId = 0x10DC;
    std::uint32_t party = 0;
    std::uint8_t settings = 0;
    std::uint32_t leader = 0; // a member's id
    std::vector<PartyMemberRecord> members;
    template <typename V>
    void Visit(V& v) {
        v("party", party);
        v("settings", settings);
        v("leader", leader);
        v("members", members, 8);
    }
};

// A member joined (to the members already in; the original's 0x3864 kind 2).
struct PartyMemberJoined {
    static constexpr MessageId kId = 0x10DD;
    PartyMemberRecord member;
    template <typename V>
    void Visit(V& v) {
        v("member", member);
    }
};

// A member is gone (0x3864 kind 3), and why: the client's reasons.
enum class PartyLeaveReason : std::uint8_t { Logout = 1, Left = 2, Migration = 3, Kicked = 4 };
struct PartyMemberLeft {
    static constexpr MessageId kId = 0x10DE;
    std::uint32_t member = 0;
    PartyLeaveReason reason = PartyLeaveReason::Left;
    template <typename V>
    void Visit(V& v) {
        v("member", member);
        v("reason", reason);
    }
};

// The party is no more (0x3864 kind 1): its master left, or too few were left in it.
struct PartyDismissed {
    static constexpr MessageId kId = 0x10DF;
    std::uint8_t reason = 0; // 1 the master left, 2 too few members
    template <typename V>
    void Visit(V& v) {
        v("reason", reason);
    }
};

// What changed of a member (0x3864 kind 6): `mask` says which parts carry news.
enum PartyMemberChange : std::uint8_t {
    kPartyLevel = 0x02,    // its level
    kPartyVitals = 0x04,   // its HP and MP tenths
    kPartyMasteries = 0x08, // its two highest masteries
    kPartyPosition = 0x20, // where it is
    kPartyEntity = 0x40,   // its entity (in the world again)
};
struct PartyMemberUpdate {
    static constexpr MessageId kId = 0x10E0;
    std::uint32_t member = 0;
    std::uint8_t mask = 0;
    std::uint8_t level = 1, hpTenths = 10, mpTenths = 10;
    WorldPosition position;
    std::uint32_t entity = 0;
    std::uint32_t mastery1 = 0, mastery2 = 0;
    template <typename V>
    void Visit(V& v) {
        v("member", member);
        v("mask", mask);
        v("level", level);
        v("hpTenths", hpTenths);
        v("mpTenths", mpTenths);
        v("position", position);
        v("entity", entity);
        v("mastery1", mastery1);
        v("mastery2", mastery2);
    }
};

// A new master (0x3864 kind 9: its master left the world, the next member leads).
struct PartyLeaderChanged {
    static constexpr MessageId kId = 0x10E1;
    std::uint32_t member = 0;
    template <typename V>
    void Visit(V& v) {
        v("member", member);
    }
};

// The party's settings changed (ours, the master's PartySetSettings).
struct PartySettingsChanged {
    static constexpr MessageId kId = 0x10E2;
    std::uint8_t settings = 0;
    template <typename V>
    void Visit(V& v) {
        v("settings", settings);
    }
};

// What the shared loot gave a member (the original's 0x3068 and the gold's share), to the party: an
// item (`refId`, `count`) or gold (`gold`) and who got it.
struct PartyLoot {
    static constexpr MessageId kId = 0x10E3;
    std::uint32_t member = 0;
    std::uint32_t refId = 0; // 0: gold
    std::uint16_t count = 0;
    std::uint64_t gold = 0;
    template <typename V>
    void Visit(V& v) {
        v("member", member);
        v("refId", refId);
        v("count", count);
        v("gold", gold);
    }
};

// What the player gained from the ground, by its own pick-up or its grab pet's (ours: the original's client writes
// "item [%s %d pieces] gained." / "item [%s] gained." / "[%d] gold gained.", UIIT_MSG_STATE_GET_ITEM_* and
// UIIT_MSG_STATE_GAIN_GOLD, in the system messages; the user's report 2026-11-14): an item (`refId`, `count`) or gold.
struct ItemGained {
    static constexpr MessageId kId = 0x10E7;
    std::uint32_t refId = 0; // 0: gold
    std::uint16_t count = 0;
    std::uint64_t gold = 0;
    template <typename V>
    void Visit(V& v) {
        v("refId", refId);
        v("count", count);
        v("gold", gold);
    }
};

// A listing as the matching window shows it.
struct PartyMatchEntry {
    std::uint32_t entry = 0;  // its number
    std::uint32_t leader = 0; // the leader's character
    std::string name;         // the leader's
    std::uint8_t race = 0;
    PartyMatchForm form;
    std::uint8_t members = 1, capacity = 4;
    template <typename V>
    void Visit(V& v) {
        v("entry", entry);
        v("leader", leader);
        v("name", name, 32);
        v("race", race);
        v("form", form);
        v("members", members);
        v("capacity", capacity);
    }
};

// How a matching request went (the original's 0xB069..0xB06D answers): the request, its status, the
// listing it concerns.
enum class PartyMatchAction : std::uint8_t {
    Register = 1, Modify = 2, Delete = 3, Join = 4, Answer = 5,
    Removed = 6, // the player's listing went: the party is full, dismissed, or its leader gone
};
struct PartyMatchAnswer {
    static constexpr MessageId kId = 0x10E4;
    PartyMatchAction action = PartyMatchAction::Register;
    PartyStatus status = PartyStatus::Ok;
    std::uint32_t entry = 0;
    template <typename V>
    void Visit(V& v) {
        v("action", action);
        v("status", status);
        v("entry", entry);
    }
};

// A page of the listings that fit the search, with the player's own listing's number (0: none).
struct PartyMatchListing {
    static constexpr MessageId kId = 0x10E5;
    std::uint8_t page = 0, pages = 1;
    std::uint32_t own = 0;
    std::vector<PartyMatchEntry> entries;
    template <typename V>
    void Visit(V& v) {
        v("page", page);
        v("pages", pages);
        v("own", own);
        v("entries", entries, 16);
    }
};

// A player asks to join the leader's listed party (the original's 0x706D to the master): who, of what
// level and race; the leader answers with PartyMatchJoinAnswer within kPartyProposalSeconds.
struct PartyMatchJoinRequest {
    static constexpr MessageId kId = 0x10E6;
    std::uint32_t entry = 0;
    std::uint32_t applicant = 0; // its character
    std::string name;
    std::uint8_t level = 1, race = 0;
    template <typename V>
    void Visit(V& v) {
        v("entry", entry);
        v("applicant", applicant);
        v("name", name, 32);
        v("level", level);
        v("race", race);
    }
};

// --- Developer commands (0x10F0) -------------------------------------------------------------

// To a teleport point's arrival at once (a teleportdata id), no gate needed. Only a game server
// started with --dev-commands takes it; others ignore it.
struct DevGoto {
    static constexpr MessageId kId = 0x10F0;
    std::uint32_t point = 0;
    template <typename V>
    void Visit(V& v) {
        v("point", point);
    }
};

// Dies at once as a monster's hit would kill it (its death penalty too; K9's tests and tries).
struct DevDie {
    static constexpr MessageId kId = 0x10F1;
    template <typename V>
    void Visit(V&) {}
};

// One hit of `damage` on a player the sender may fight (PvP: guild wars), as its attack would land
// (tests and tries of the war's kills).
struct DevStrike {
    static constexpr MessageId kId = 0x10F3;
    std::uint32_t target = 0;
    std::uint32_t damage = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
        v("damage", damage);
    }
};

// A developer's teleport to a place of the outdoor world (region, x and z in it), put down on the highest ground there; its
// ride, transport and pets along (the F1 Teleport panel's X / Y, the minimap's numbers). In the teleports' block: the
// developer's is full. A dungeon's region (0x8000 and up) needs its height `y` (a floor of its own: kNoHeight refused); within the
// dungeon the player stands in, its layer is kept (an instance's own copy).
struct DevGotoPlace {
    static constexpr MessageId kId = 0x103F;
    static constexpr float kNoHeight = 10000.0f;
    std::uint16_t region = 0;
    float x = 0, z = 0;
    float y = kNoHeight;
    template <typename V>
    void Visit(V& v) {
        v("region", region);
        v("x", x);
        v("z", z);
        v("y", y);
    }
};

// Experience and skill experience as a kill would give them (levels, skill points, the guild's share).
// A developer's teleport to a nest of that monster (its codename), the nearest of them (tests of the quests' hunts).
struct DevGotoMonster {
    static constexpr MessageId kId = 0x10F4;
    std::string monster;
    template <typename V>
    void Visit(V& v) {
        v("monster", monster, 128);
    }
};

// A developer's item put into the bag (its codename, the count); `wear`: then worn in its place, by the move's own
// rules (ItemMove; its answer ItemMoved), what was there going to the bag. `magic` (option id | value << 32, as
// ItemRecord's) and `plus` put on the new item first (the item exam's blues); `data` (not 0)
// its data word (_Items.Data: a Devil's Spirit's awakening's end).
struct DevGive {
    static constexpr MessageId kId = 0x10F5;
    std::string item;
    std::uint32_t count = 1;
    bool wear = false;
    std::vector<std::uint64_t> magic;
    std::uint8_t plus = 0;
    std::uint32_t data = 0;
    template <typename V>
    void Visit(V& v) {
        v("item", item, 128);
        v("count", count);
        v("wear", wear);
        v("magic", magic, 12);
        v("plus", plus);
        v("data", data);
    }
};

// --- The skill lab's setup (L2): a developer's, only with --dev-commands; the exam's frame and
// the client's F1 Skill lab (a build with SR_DEV_TOOLS) send them --------------------------------------------------

// A mastery raised to `level` and every group on its board learned at the highest level that mastery level allows
// (at most `skillLevel`; 0: no cap); the character raised to `level` first when below it. No skill points spent.
struct DevLearn {
    static constexpr MessageId kId = 0x10F6;
    std::uint32_t mastery = 0; // skillmasterydata's id
    std::uint8_t level = 1;
    std::uint8_t skillLevel = 0;
    template <typename V>
    void Visit(V& v) {
        v("mastery", mastery);
        v("level", level);
        v("skillLevel", skillLevel);
    }
};

// How a lab monster behaves: as any of its kind (it fights back), standing (never moves nor fights; it keeps its
// records of who hit it and the threat), or walking or running straight away from the player who asked for it.
enum class LabBehaviour : std::uint8_t { Normal = 0, Still = 1, FleeWalk = 2, FleeRun = 3 };
// A monster (characterdata codename) put `dx`, `dz` from the player, one of a nest of its own that never refills
// (general, never aggressive), with `hp` health (0: its own). DevSpawned answers with its entity, `tag` echoed.
struct DevSpawn {
    static constexpr MessageId kId = 0x10F7;
    std::string monster;
    float dx = 0, dz = 0;
    LabBehaviour behaviour = LabBehaviour::Still;
    std::uint32_t hp = 0;
    std::uint32_t tag = 0;
    template <typename V>
    void Visit(V& v) {
        v("monster", monster, 128);
        v("dx", dx);
        v("dz", dz);
        v("behaviour", behaviour);
        v("hp", hp);
        v("tag", tag);
    }
};
struct DevSpawned {
    static constexpr MessageId kId = 0x10F8;
    std::uint32_t tag = 0;
    std::uint32_t entity = 0; // 0: not made (no such monster, no ground there)
    template <typename V>
    void Visit(V& v) {
        v("tag", tag);
        v("entity", entity);
    }
};
// A state and a knock on an entity (a monster or a player), as a hit would put them, from nobody: `state` a
// game::StateKind (kNoState: none) at `level` for `ms` (`value` its number: a burn's damage, a curse's amount);
// `knock` 0 none, 1 down, 2 back (pushed away from the asking player).
struct DevAfflict {
    static constexpr std::uint8_t kNoState = 0xFF;
    static constexpr MessageId kId = 0x10F9;
    std::uint32_t target = 0;
    std::uint8_t state = kNoState;
    std::uint16_t level = 1;
    std::uint32_t ms = 5000;
    std::int32_t value = 0;
    std::uint8_t knock = 0;
    template <typename V>
    void Visit(V& v) {
        v("target", target);
        v("state", state);
        v("level", level);
        v("ms", ms);
        v("value", value);
        v("knock", knock);
    }
};
// The asking player's own: the bits of what is put back (its skills' waits over, its health and mana full, its states
// gone, its buffs ended, its bag emptied (what it wears kept), out of battle: the 20 s after an attack over).
struct DevReset {
    static constexpr MessageId kId = 0x10FA;
    static constexpr std::uint8_t kWaits = 1, kVitals = 2, kStates = 4, kBuffs = 8, kBag = 16, kBattle = 32, kAll = 63;
    // Back to `place` too, put there along the floor (its walk stopped; the exam's fleeing monsters lead it away).
    static constexpr std::uint8_t kPlace = 64;
    // Its mall services too: the time tickets, the premium, the bonus scrolls and the growth potion ended (the item exam's
    // players use one of each kind in turn; "one running" would refuse the next).
    static constexpr std::uint8_t kServices = 128;
    std::uint8_t what = kAll;
    WorldPosition place;
    template <typename V>
    void Visit(V& v) {
        v("what", what);
        v("place", place);
    }
};
// A monster the asking player's DevSpawn made, taken away (0: all of them).
struct DevRemove {
    static constexpr MessageId kId = 0x10FB;
    std::uint32_t entity = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
    }
};

// The skill lab's look at the asking player's numbers (L5: the passives): its passives as they
// stand (each learned one, whether its conditions hold, how many changes it puts on the sheet) and its fighting numbers.
struct DevSheet {
    static constexpr MessageId kId = 0x10FC;
    std::uint8_t unused = 0;
    template <typename V>
    void Visit(V& v) {
        v("unused", unused);
    }
};
struct DevPassive {
    std::uint32_t skill = 0;
    bool active = false;
    std::uint16_t changes = 0;
    template <typename V>
    void Visit(V& v) {
        v("skill", skill);
        v("active", active);
        v("changes", changes);
    }
};
struct DevNumber {
    std::string name;
    float value = 0;
    template <typename V>
    void Visit(V& v) {
        v("name", name, 32);
        v("value", value);
    }
};
// The skill lab's: the asking player's health and mana set to these percents of their most (a heal's target, a mana
// heal's; L6).
// `maxHp` above 0: its most health that, until a DevVitals of 0 (scenario 4's second player, 10 million: no hit kills it).
struct DevVitals {
    static constexpr MessageId kId = 0x10FE;
    std::uint8_t hpPercent = 100, mpPercent = 100;
    std::uint32_t maxHp = 0;
    template <typename V>
    void Visit(V& v) {
        v("hpPercent", hpPercent);
        v("mpPercent", mpPercent);
        v("maxHp", maxHp);
    }
};
struct DevSheetAnswer {
    static constexpr MessageId kId = 0x10FD;
    std::vector<DevPassive> passives;
    std::vector<DevNumber> numbers;
    template <typename V>
    void Visit(V& v) {
        v("passives", passives, 256);
        v("numbers", numbers, 64);
    }
};

// A developer's guild of the asking player, its master and so the fortress war's Commander (position 1), none of the
// creation's checks (a guild NPC, the level, the gold): the item exam's fortress weapons.
// The guild's answer as a creation's (GuildActionAnswer).
struct DevGuild {
    static constexpr MessageId kId = 0x10FF;
    std::string name;
    std::uint8_t level = 1; // the guild's level (the item exam's crests: 4 a guild's, 5 a union's leader)
    template <typename V>
    void Visit(V& v) {
        v("name", name, 64);
        v("level", level);
    }
};

struct DevExperience {
    static constexpr MessageId kId = 0x10F2;
    std::uint64_t experience = 0;
    std::uint32_t skill = 0;
    template <typename V>
    void Visit(V& v) {
        v("experience", experience);
        v("skill", skill);
    }
};

// --- The auto potion (0x1190) ------------------------------------

// One row of the auto potion: on, its percent (1..100; the abnormal row's unused) and the item
// dropped in its slot (an itemdata id, 0: none).
struct AutoPotionRow {
    bool enabled = false;
    std::uint8_t percent = 0;
    std::uint32_t item = 0;
    template <typename V>
    void Visit(V& v) {
        v("enabled", enabled);
        v("percent", percent);
        v("item", item);
    }
};
// The player's auto potion settings (game::AutoPotion): the client sends them when the window's
// Confirm keeps them; the server keeps them per character (_ClientConfig 11..14 and sr_auto_potion),
// answers with what it kept (its ranges applied, items that do not fit a row taken out) and sends
// them when the character enters the world. The server uses the rows' items itself (ours; the
// original's client used its quick slots). The vehicle's and the attack pet's rows are ours.
struct AutoPotionSettings {
    static constexpr MessageId kId = 0x1190;
    AutoPotionRow hp, mp, abnormal, vehicle, attackPet;
    bool delayEnabled = true;
    std::uint16_t delayMs = 1000;
    template <typename V>
    void Visit(V& v) {
        v("hp", hp);
        v("mp", mp);
        v("abnormal", abnormal);
        v("vehicle", vehicle);
        v("attackPet", attackPet);
        v("delayEnabled", delayEnabled);
        v("delayMs", delayMs);
    }
};

// An item used (the original's 0x305C, to everyone who sees the user,
// itself too): who used it and which (an itemdata id). Its effect plays on the user (the 1.188
// client's SYSTEM_HPPOTION, SYSTEM_MPPOTION, ...).
struct ItemUsed {
    static constexpr MessageId kId = 0x1191;
    std::uint32_t entity = 0;
    std::uint32_t item = 0;
    template <typename V>
    void Visit(V& v) {
        v("entity", entity);
        v("item", item);
    }
};

// A unique's news, to every player of the shard (the original's 0x300C codes 0x0C05 came / 0x0C06 gone, the 1.188
// client's): the monster (characterdata id) and, gone, its killer's name (empty: it left, or no player killed
// it). The client words it (UIIT_MSG_APPEAR_* / _DEAD_* / _ANYONE_DEAD_*) as a system line and the warning banner.
struct UniqueNotice {
    static constexpr MessageId kId = 0x1063;
    std::uint8_t kind = 1; // 1 came, 2 gone
    std::uint32_t model = 0;
    std::string killer;
    template <typename V>
    void Visit(V& v) {
        v("kind", kind);
        v("model", model);
        v("killer", killer, 32);
    }
};

} // namespace sr::game::proto
