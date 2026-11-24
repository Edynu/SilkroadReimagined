#pragma once

// The Qin-Shi tomb's upper floors on the game server: the snake gate, floors 5 and 6.
//
// - The schedule (`_Schedule`'s define 2: every day at 10:59:12 and 13:59:12 for 10 minutes; or a developer's switch, or
//   the tries' cycle) opens the snake gate (`STORE_TQ_SNAKEGATE` on the 4th floor, its point GATE_JINSI_04x01-bldg, which
//   has no link of its own): a copy of each floor is made (`INS_QINSHI_FLOOR_5`, `INS_QINSHI_FLOOR_6`: layers,
//   Instances::OpenWorld), the gate leads into floor 5's copy at the point GATE_JINSI_05x01, and the shard is told
//   (UIIT_TQ_F4_TELEPORT_OPEN). At the period's end the gate shuts (UIIT_TQ_F4_TELEPORT_CLOSE) and no one else comes in;
//   the copies stay an hour more (their GAMEWORLD_DISAPPEARANCE_TIME rows, told as the warnings at 60, 30, 10 and 5
//   minutes left), then everyone inside goes back where it came in. Only the game server holding the 4th floor runs it;
//   the others hear its notices on the bus.
// - Floor 5: its four guardians stand from the start; the four dead, the Flame Master comes; it dead, three portals to
//   floor 6 stand at the floor's three places in one of six orders, each into one of floor 6's sides.
// - Floor 6: the Black Snake (east), the snake generals (south), a crowd of the tomb's monsters (west), the White Snake
//   (north) from the start; a side cleared, its portal up to the summit stands, and the first raises the summit's portal
//   down to the north. The summit's gates (STORE_TQ_6FGATE1..4) lead down by their own links.
// A portal is a runtime teleport object (`INS_QUEST_TELEPORT`, used as a gap: Instances::AddPortal). The original's
// spawners, places and numbers are in QinTomb.cpp.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sr/game/Fortress.h"
#include "sr/game/WorldProtocol.h"

namespace sr::assets {
class CharacterTable;
} // namespace sr::assets

namespace sr::server {

class Instances;
class Monsters;

struct QinTombConfig {
    bool enabled = true;
    std::vector<game::ScheduleRow> schedule; // `_Schedule`'s rows of define 2
    // Tries (ours): the gate opens every `cyclePeriod` seconds for `cycleOpen` seconds in place of the schedule (0: the
    // schedule), and the copies' hour after the gate is divided by `timeScale`.
    int cycleOpen = 0, cyclePeriod = 0;
    double timeScale = 1.0;
};

class QinTombHost {
public:
    virtual ~QinTombHost() = default;
    // A text key's notice to every player of the shard (the warning banner and the chat).
    virtual void QinShardNotice(const std::string& key) = 0;
    // A teleport point's arrival (`_RefTeleport`; none: unknown).
    virtual std::optional<game::proto::WorldPosition> QinPoint(std::uint32_t point) = 0;
};

class QinTomb {
public:
    using Clock = std::chrono::steady_clock;
    // The teleport points (`_RefTeleport`): the snake gate's (GATE_JINSI_04x01-bldg), floor 5's arrival (GATE_JINSI_05x01).
    static constexpr std::uint32_t kSnakeGate = 131, kFloor5Gate = 136;
    // The floors' regions: the 4th (the snake gate), the 5th, the 6th.
    static constexpr std::uint16_t kFloor4 = 0x8004, kFloor5 = 0x8003, kFloor6 = 0x8002;

    QinTomb(QinTombHost& host, Instances& instances, Monsters& monsters, const assets::CharacterTable& characters, QinTombConfig config,
            std::uint32_t seed);
    ~QinTomb();
    QinTomb(const QinTomb&) = delete;
    QinTomb& operator=(const QinTomb&) = delete;

    // The schedule (the gate opened and shut), the spawners' chains, the copies' end.
    void Tick(Clock::time_point now);
    // A developer's switches: the gate opened (new copies; an older pair ended first), shut (the copies' last hour), the
    // copies ended at once (everyone back).
    bool Open(Clock::time_point now);
    void Shut(Clock::time_point now);
    void End();
    // A player through the snake gate into floor 5's copy (the gate's menu offered it): Ok, or why not (the gate shut:
    // Nowhere; the copy full: InstanceFull; already in an instance).
    game::proto::GateStatus Admit(std::uint32_t entity);

    // Its state, for the tests and the log.
    bool GateOpen() const;
    std::uint32_t Floor5() const; // the copies' layers (0: none)
    std::uint32_t Floor6() const;
    // A spawner of the original's (Chins_Floor5_East, ...): started (its monsters raised, or its portals put up), cleared.
    bool Started(const std::string& spawner) const;
    bool Cleared(const std::string& spawner) const;
    // The portals standing in a copy's layer, and the order drawn for floor 5's (0..5).
    std::vector<std::uint32_t> Portals(std::uint32_t layer) const;
    int Floor5Order() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
