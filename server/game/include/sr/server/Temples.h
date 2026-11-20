#pragma once

// The job temples of Egypt on the game server: run by the server holding the temples' area
// (dungeon region 0x8010, INS_TEMPLE_DEFAULT).
//
// - The area: its two gates by alliance (173 the traders' and hunters', 174 the thieves'; levels 105..999 by their links)
//   ask a job suit on and, once 30 are inside, the alliance's share under 70 % (AreaRefusal).
// - The schedule (`_Schedule` 9 Selkis and Neith, 10 Anubis and Isis, 11 Haroeris; or --temple-cycle): a pair of temples
//   opens 30 minutes into its two-hour period, warned 30, 10 and 5 minutes before, and stays open to its end. An opening
//   makes one instance of each temple (Instances::OpenWorld), puts up its door in the area (INS_TEMPLE_TELEPORT, used as
//   a gap) and its way out inside; the closing sends everyone inside back to the area.
// - Anubis's and Haroeris's temples admit by the alliances' reputation (`_AssociationReputation`): at the opening the
//   alliance ahead enters alone; equal, everyone (Anubis) or nobody (Haroeris); then that temple's reputations go back to
//   0. The temple quests' points raise them (AddReputation).
// - Haroeris's temple holds MOB_SD_HAROERIS; killed, Seth's temple opens, its door inside Haroeris's.
// The original's numbers and places are the database's rows and Temples.cpp's tables, with their addresses.

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "sr/game/Fortress.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/Monsters.h"

namespace sr::server {

class Instances;
class World;
struct GameWorldCatalog;

struct TempleConfig {
    bool enabled = true;
    std::vector<game::ScheduleRow> schedule; // `_Schedule`'s rows of 9, 10 and 11
    // Tries (ours): every pair's period `cyclePeriod` seconds from the Unix epoch, the opening a quarter in (0: the schedule).
    int cyclePeriod = 0;
};

class TempleHost {
public:
    virtual ~TempleHost() = default;
    struct Facts {
        int level = 0;
        std::uint8_t jobState = 4; // game::JobState: 1 trader, 2 thief, 3 hunter, 4 none
        bool freePvp = false, riding = false, dead = false;
        std::uint8_t league = 0; // the league joined (game::JobType: 1 trader, 2 thief, 3 hunter; 0 none)
    };
    virtual std::optional<Facts> TempleFacts(std::uint32_t entity) = 0;
    // The players in the temples' area and the temples on this server, by alliance: the traders and hunters, the thieves.
    virtual std::pair<int, int> TempleAllianceCounts() = 0;
    // A message to every player in the temples' area and the temples.
    virtual void TempleTell(game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // The alliances' reputations written (Anubis's traders and hunters, Anubis's thieves, Haroeris's two).
    virtual void TempleSaveReputation(const std::array<int, 4>& values) = 0;
};

class Temples {
public:
    using Clock = std::chrono::steady_clock;
    // The area's gates (`_RefTeleport`): the traders' and hunters' and the thieves'.
    static constexpr std::uint32_t kAreaGateTraderHunter = 173, kAreaGateThief = 174;

    Temples(TempleHost& host, Instances& instances, Monsters& monsters, World& world, std::shared_ptr<const GameWorldCatalog> catalog,
            TempleConfig config, std::array<int, 4> reputation);
    ~Temples();
    Temples(const Temples&) = delete;
    Temples& operator=(const Temples&) = delete;

    // The schedule and the opened temples.
    void Tick(Clock::time_point now);
    // The area's gate (`gate` 173 or 174, where it leads): Ok, or why not. First the suit of that gate's alliance in the job
    // slot (a trader's or a hunter's to 173, a thief's to 174; else 0x1C2F "no right"), then the
    // world's admission (level, suit, ride, free battle, the alliances' share).
    game::proto::GateStatus AreaRefusal(std::uint32_t entity, std::uint32_t gate);
    // A player come into the world inside the temples' area who may not be there (AreaRefusal): where it is sent, by its
    // league (FAILED_ENTER_WORLD_TO_SPECIAL_TARGET_*, the thieves' place, the traders' and hunters'); none:
    // stays (it may, or no league).
    std::optional<game::proto::WorldPosition> RefusedPlace(std::uint32_t entity);
    // A temple's door (its entity, in the area or Haroeris's temple): whether it is one, and its use.
    bool IsDoor(std::uint32_t entity) const;
    game::proto::GateStatus UseDoor(std::uint32_t entity, std::uint32_t door);
    // A door selected (proto::GapInfo, `temple`): Seth's with its temple's players and the most it takes.
    std::optional<game::proto::GapInfo> DoorInfo(std::uint32_t door) const;
    void Killed(const Monsters::Damaged& killed, const game::proto::WorldPosition& at);
    // A temple quest's points (temple 0 Anubis, 1 Haroeris; alliance 1 traders and hunters, 2 thieves).
    void AddReputation(int temple, int alliance, int points);
    // A developer's switches: a pair's schedule (9, 10, 11) opened or closed now.
    bool Open(int define);
    void Close(int define);

    // The reputation board (proto::TempleReputation's two rows: Anubis's pair, Haroeris's): the last decision, the next
    // opening, since when the reputation is gathered, the two alliances' reputation.
    std::vector<game::proto::TempleReputationRow> Board() const;
    // The two temples' reputations alone (proto::TemplePoints, the original's 0xB4E0).
    static game::proto::TemplePoints PointsOf(const std::vector<game::proto::TempleReputationRow>& board);
    // The NPCs that offer the board (the client's row 0x3C).
    static bool OffersBoard(const std::string& codename);

    // For the tests and the log.
    std::array<int, 4> Reputation() const;
    std::uint32_t LayerOf(std::uint32_t world) const;  // a temple world's open instance (0: shut)
    std::uint32_t DoorOf(std::uint32_t world) const;   // its door's entity (0: none)
    std::uint8_t AllowedOf(std::uint32_t world) const; // TempleNotice::kNobody .. kEveryone

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
