#pragma once

// The Battle Arena on a game server.
//
// - Every game server runs the same rounds: a mode's registration and its match from the original's `_Schedule` (defines
//   16..23), or the developer's switches (on every server through the bus); each tells its own players its lines and
//   takes their registrations at the manager into the shard's queue (sr_event_entries, event 2).
// - The server owning the arena's maps (ARENA_*; the host) forms the matches before the play (game::PlanArena, ours),
//   each a layer of its world (score 8 / 16 / 32, flag 8), the players taken in at their team's gate (A team 0, B team
//   1), those of the other server coming over. Score: the group's monsters of `_RefGameWorldNPC` (1 / 3 / 8 / 60 points,
//   their respawns), 6 a player's kill; flag: the poles (game::FlagMatch), 1000 a flag on a team's pole, 1 a kill. The
//   two teams are the original's relation 0x40 (foes from the move in, no records, no PK, KillRelation::Team); a death
//   rises at its gate 10 s later. At the end each participant still in its world gets its result, Arena Coins by mode
//   (lost in a full bag, as the original's) and skill experience (the level's GUST_Mob_Exp x the map's factor); home at
//   the stop (ours: where it registered from).

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sr/game/Arena.h"
#include "sr/game/Fortress.h"
#include "sr/server/FlagWorld.h"

namespace sr::server {

// An arena world (_RefGame_World 45..50) as the matches use it.
struct ArenaWorldSpec {
    std::uint16_t id = 0;
    std::string codename;
    std::uint8_t kind = game::arena::kScore; // 1 score, 2 flag
    std::uint8_t size = 8;                   // a side
    game::proto::WorldPosition gates[2];     // REF_TELEPORT_BATTLE_ARENA_A_GATE (team 0), _B_GATE (team 1)
    float reward[3] = {0, 0, 0};             // BATTLE_ARENA_REWARD_WIN / _LOSE / _DRAW
    struct Spawn {
        std::string codename;
        std::uint32_t refObjId = 0;
        game::proto::WorldPosition at;
        int group = 0, points = 0, count = 1, respawn = 0, rarity = 0, radius = 0; // _RefGameWorldNPC Param1..6
    };
    std::vector<Spawn> spawns; // score: its monsters by group; flag: its poles
};

// The arena worlds from the database's original tables (score 8 / 16 / 32 and flag 8; the flag maps of several poles are
// not built); `characters` names the spawns' rows.
bool LoadArenaWorlds(Database& db, const assets::CharacterTable* characters, std::vector<ArenaWorldSpec>& out, std::string& error);

class ArenaHost {
public:
    virtual ~ArenaHost() = default;
    struct Facts {
        bool partyLeader = false, guildMaster = false;
        std::uint32_t party = 0, guild = 0;
        std::uint8_t jobState = 4; // 1 trader, 2 thief, 3 hunter, 4 none (a suit worn)
        bool riding = false;
        std::vector<std::uint32_t> partyMembers; // its party's characters, every server's (the leader's registration)
    };
    virtual Facts ArenaFactsOf(std::uint32_t entity) = 0;
    // Skill experience given (the result's SP), and a level's GUST_Mob_Exp.
    virtual void ArenaSkillExperience(std::uint32_t entity, std::uint64_t sp) = 0;
    virtual std::uint64_t ArenaLevelExperience(int level) = 0;
    // The carrier's buff (SKILL_BATTLE_ARENA_FLAG_GET_01) on or off.
    virtual void ArenaBuff(std::uint32_t entity, bool on) = 0;
    // The server owning a region, and a player sent to its return point (home unknown: a party's or guild's member of
    // another server).
    virtual std::uint16_t ArenaOwner(std::uint16_t region) = 0;
    virtual void ArenaReturnPoint(std::uint32_t entity) = 0;
    virtual void ArenaPublish(std::string text) = 0;
};

struct ArenaConfig {
    bool enabled = false;                    // the shard's event-arena
    std::vector<game::ScheduleRow> schedule; // `_Schedule` rows of defines 16..23
    // Tries (--arena-cycle R,P): random mode again and again from the Unix epoch, R s of registration, P s of play.
    std::int64_t cycleRegister = 0, cyclePlay = 0;
    // The developer's round: its registration's and its play's seconds.
    std::int64_t devRegister = 60, devPlay = 300;
    // The fewest players a random match (0: the shard manager's by its group, 4 or 8; tries: arena-min-players).
    int minPlayers = 0;
    std::vector<ArenaWorldSpec> worlds;      // the catalog (tests give theirs; else LoadArenaWorlds)
};

class Arenas {
public:
    using Clock = std::chrono::steady_clock;
    Arenas(FlagWorldHost& host, ArenaHost& arena, World& world, Monsters* monsters, const assets::CharacterTable* characters, ArenaConfig config,
           std::uint32_t seed);
    ~Arenas();
    Arenas(const Arenas&) = delete;
    Arenas& operator=(const Arenas&) = delete;

    void Enter(std::uint32_t entity);
    void Leave(std::uint32_t entity);
    bool Handle(std::uint32_t entity, game::proto::MessageId id, std::span<const std::byte> payload, bool devCommands);
    // After an NpcMenu: the manager's state.
    void TellDesk(std::uint32_t entity, std::uint32_t npc, std::string_view codename);
    bool Select(std::uint32_t entity, std::uint32_t target);
    void Killed(const Monsters::Damaged& killed, const game::proto::WorldPosition& at);
    void Died(std::uint32_t victim, std::uint32_t killer);
    // A participant's own revival request: true (the gate's revival is the match's).
    bool Resurrect(std::uint32_t entity);
    void Tick(Clock::time_point now);
    void BusMessage(const std::string& text);
    void Dev(std::uint8_t action, std::uint8_t mode);

    bool Hostile(std::uint32_t a, std::uint32_t b) const;
    bool InMatch(std::uint32_t entity) const;
    std::optional<game::proto::WorldPosition> ReturnPlace(std::uint32_t entity) const;

    // Tests and tools.
    game::arena::Mode OpenMode() const;
    std::size_t Matches() const;
    std::uint32_t LayerOf(std::uint32_t entity) const;
    std::uint8_t TeamOf(std::uint32_t entity) const;
    std::pair<std::uint32_t, std::uint32_t> ScoresOf(std::uint32_t layer) const;
    std::size_t Worlds() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
