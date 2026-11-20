#pragma once

// The game server's guilds and unions: creating a guild at a guild NPC,
// inviting and the answer, leaving, putting out, disbanding, ranks and authority, the master's place
// handed over, the notice, titles, guild levels and points, the guild's and the union's chat,
// unions of up to eight guilds, the guilds' and unions' emblems, and the master's election. The rules, numbers and
// refusals are the original game server's (game/Guild.h); the state is the original's tables (GuildStore.h).
//
// Ours: the original's game server checked a request and handed the work to its shard manager,
// which wrote the database and told every game server; here the game server keeps every guild in
// memory (loaded at the start), changes it at once and writes the change behind, in order (one write
// at a time). A shard's game servers share them through the database:
// each change's notification on the shard's channel (PostgreSQL NOTIFY, after its writes) has the
// others load those guilds and unions again and tell their members; the members' messages (lines,
// chat) reach those playing on another server the same way, and who is online where; an election
// is held by the server that called it, the others pass its requests on.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sr/game/Guild.h"
#include "sr/game/GuildProtocol.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/GuildStore.h"

namespace sr::server {

class Database;

// A player in the world as the guilds see it (the game server fills it in).
struct GuildActor {
    std::uint64_t peer = 0;
    std::uint32_t entity = 0;
    std::int64_t character = 0;
    std::string name;
    int level = 1;
    std::uint32_t model = 0;
    bool dead = false;
    std::uint64_t gold = 0;
    std::uint32_t skillPoints = 0;
    bool jobSuit = false; // a job suit worn (its job state not 4)
};

// What the guilds need of the game server.
class GuildHost {
public:
    virtual ~GuildHost() = default;
    virtual void GuildSend(std::uint64_t peer, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // A player in the world by its entity.
    virtual std::optional<GuildActor> GuildPlayerAt(std::uint32_t entity) const = 0;
    // How far apart two entities stand (negative: one is not in the world).
    virtual double GuildDistance(std::uint32_t a, std::uint32_t b) const = 0;
    virtual bool IsNpc(std::uint32_t entity) const = 0;
    // An NPC that keeps guilds (the original's NPC function option 15).
    virtual bool IsGuildNpc(std::uint32_t entity) const = 0;
    // A name the name rules take (letters, the abuse filter).
    virtual bool GuildNameAllowed(std::string_view name) const = 0;
    virtual bool TakeGold(std::uint64_t peer, std::uint64_t amount) = 0;
    virtual bool GiveGold(std::uint64_t peer, std::uint64_t amount) {
        (void)peer;
        (void)amount;
        return false;
    }
    virtual bool TakeSkillPoints(std::uint64_t peer, std::uint32_t points) = 0;
    // A guild owns a fortress; a fortress war is on and the guild is in it.
    virtual bool GuildHoldsFortress(std::uint32_t guild) const {
        (void)guild;
        return false;
    }
    virtual bool GuildInFortressWar(std::uint32_t guild) const {
        (void)guild;
        return false;
    }
    // One of the item in an inventory place used up, when it is that item (itemdata codename).
    virtual bool TakeGuildItem(std::uint64_t peer, std::uint8_t slot, std::string_view codename) = 0;
    // To those who see the entity, itself too.
    virtual void GuildToObservers(std::uint32_t entity, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // What newcomers seeing the entity are told after its appearance (empty: nothing).
    virtual void GuildAppearance(std::uint32_t entity, game::proto::MessageId id, std::vector<std::byte> payload) = 0;
    // `task` on a database thread, then `done` on the server's.
    virtual void GuildSubmit(std::function<void(Database&)> task, std::function<void()> done) = 0;
    // The shard's bus: this game server's id and the channel its game servers share (empty: alone).
    virtual std::uint16_t GuildServer() const { return 0; }
    virtual std::string GuildChannel() const { return {}; }
    // The guild recall: whether a member may be asked now (the original leaves out one
    // with a teleport under way), and a member's yes: its checks (0x18C1 when dead, travelling, a murderer,
    // with a transport out or busy) and, passed, its way to `to` (0).
    virtual bool GuildRecallAskable(std::uint32_t entity) const {
        (void)entity;
        return true;
    }
    virtual std::uint16_t GuildRecallGo(std::uint32_t entity, const game::proto::WorldPosition& to) {
        (void)entity;
        (void)to;
        return 0x18C1;
    }
};

class Guilds {
public:
    using Clock = std::chrono::steady_clock;
    explicit Guilds(GuildHost& host);
    ~Guilds();
    Guilds(const Guilds&) = delete;
    Guilds& operator=(const Guilds&) = delete;

    void Load(GuildData data);
    // A player entered the world (it is told its guild and union; its members see it online) or left it.
    void Entered(const GuildActor& actor);
    void Left(std::int64_t character);
    // Gone to another game server of the shard: still online, nobody is told (Left if it never arrives).
    void MovedAway(std::int64_t character);
    void LevelChanged(std::int64_t character, int level);
    // Skill points a member earned by playing, as guild points (the game
    // server gathers them by ten, and the rest when the member leaves): the guild's and the member's.
    void PointsEarned(std::int64_t character, std::uint32_t points);
    // A guild request (GuildProtocol.h ids); false when it does not decode.
    bool Handle(const GuildActor& actor, game::proto::MessageId id, std::span<const std::byte> payload);
    // A developer's guild of `actor` (game::proto::DevGuild): made as a creation is, none of its checks but the name.
    void DevCreate(const GuildActor& actor, const std::string& name, int level = 1);
    // A guild or union chat line from `actor` to the members it reaches; the result for its answer.
    game::proto::ChatResult Chat(const GuildActor& actor, const game::proto::ChatLine& line);
    // Proposals not answered in time; elections whose time is up; recalls not answered in time.
    void Tick(Clock::time_point now);
    // The guild recall (the use's checks are the game server's): every member of
    // `master`'s guild online, here and on the shard's other servers (the bus), but the master and those travelling,
    // asked to come to `to` (GuildRecallAsk) for `seconds`; a yes (GuildRecallAnswer) within them goes there
    // (GuildHost::GuildRecallGo). False when the master is in no guild.
    bool Recall(const GuildActor& master, const game::proto::WorldPosition& to, std::uint32_t seconds);
    // A recall asked of the character and not answered yet (tests).
    bool RecallAsked(std::int64_t character) const;
    // A notification of the shard's channel (another game server's; this server's own are passed over),
    // and this server's greeting at the start (the others tell who of theirs is online).
    void BusMessage(const std::string& payload);
    void Hello();
    void SetElectionSeconds(int seconds);
    void SetWarEndSeconds(int seconds); // a surrender's wait (60; tests shorten it)
    // The guild a character belongs to (0: none).
    std::uint32_t GuildOf(std::int64_t character) const;
    // Guild wars: whether two characters' guilds are at war, whether a
    // character's guild is at any; a player killed by a player (its level the killer's guild's points).
    bool AtWar(std::int64_t a, std::int64_t b) const;
    bool AtAnyWar(std::int64_t character) const;
    // Its place in the fortress war (`_GuildMember.SiegeAuthority`; 0: none or no guild).
    std::uint8_t PositionOf(std::int64_t character) const;
    // Whether a member of the guild holds that fortress position (the smith's and the trainer's 0.85).
    bool AnyHolds(std::uint32_t guild, std::uint8_t position) const;
    // Guild points spent by the guild itself (a fortress's upgrade); false when it has too few.
    bool TakeGuildPoints(std::uint32_t guild, std::uint32_t points);
    void PlayerKilled(std::int64_t killer, std::int64_t victim, int killerLevel, int victimLevel);
    // The guild storage's checks for a member: its guild and the guild's level,
    // and the original's refusal: 0x4C0D in no guild, 0x4C4A below level 2, 0x4C1E neither the master
    // nor holding bit 0x08 (0: it may).
    struct StorageAccess {
        std::uint32_t guild = 0;
        int level = 0;
        std::uint16_t refusal = 0;
    };
    StorageAccess StorageAccessOf(std::int64_t character) const;
    // A shop's price in guild points: the buyer must be the
    // master of a guild that holds them. Then SpendPoints takes them, tells the members and saves.
    enum class PointsPayment : std::uint8_t { Ok, NotMaster, Short };
    PointsPayment CanPayPoints(std::int64_t character, std::uint32_t points) const;
    void SpendPoints(std::int64_t character, std::uint32_t points, std::uint8_t reason);
    // A guild as another feature needs it (the fortress war): its name,
    // level, members and union, and (by a member) whether the member is its master; none: no guild.
    struct Facts {
        std::uint32_t id = 0;
        std::string name;
        int level = 0;
        std::size_t members = 0;
        std::uint32_t alliance = 0;
        bool master = false;
        std::string masterName; // its master's name (the fortress list's second string)
        std::uint32_t crest = 0; // its emblem's revision (0: none)
    };
    std::optional<Facts> FactsOf(std::int64_t character) const;
    std::optional<Facts> FactsOfGuild(std::uint32_t guild) const;
    std::size_t Count() const;
    // Writes still to go (tests wait for them).
    bool Writing() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
