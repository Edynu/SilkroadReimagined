#pragma once

// The Roc on the game server: the Mountain Peak (`INS_ROC_TOP`) and its ruler.
//
// - The gate (`GATE_RC_MOUNTAIN_GATE` -> `GATE_RC_ROC_GATE`, its link's checks: the Key of the Bloodline used up, levels
//   80..102, no job) opens on the schedule (`SCHEDULE_DEF_MEET_ROC`, Mondays 15:30 for 10 minutes; or a developer's
//   switch, or the tries' cycle): a Peak instance (a layer, Instances::OpenWorld) is made, the shard told
//   (UIIT_MSG_QUEST_ROC_APPEAR); it shuts at the period's end or at the Roc's entrance (UIIT_MSG_QUEST_ROC_DISAPPEAR).
//   Only the game server holding the Peak's regions runs it; the others hear its notices on the bus.
// - On the Peak: the 8 seal stones; a stone fought calls its follower (its 'ssou': Devil Shaitan, a wing tribe knight);
//   the 8 followers dead, the Roc comes 10 minutes later after its entrance scene (QuestScene roc_entrance, 42 s). Still
//   standing an hour after the opening, everyone is sent out.
// - The Roc: the berserk countdown, its berserk an hour after it came (MSKILL_RM_ROC_ATTACK10), its spirits topped up to
//   three kinds every 5 minutes, its escape 3.5 hours after it came (warned at 30, 15, 10, 5, 3, 1 minutes); killed, an
//   exit stands where it fell and everyone is sent out 30 minutes later.
// The original's numbers and places are in Roc.cpp with their addresses.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sr/game/Fortress.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/Monsters.h"

namespace sr::assets {
class CharacterTable;
class SkillTable;
} // namespace sr::assets

namespace sr::server {

class Instances;

struct RocConfig {
    bool enabled = true;
    std::vector<game::ScheduleRow> schedule; // `_Schedule`'s rows of SCHEDULE_DEF_MEET_ROC (`_RefScheduleDefine` 1)
    // Tries (ours): the gate opens every `cyclePeriod` seconds for `cycleOpen` seconds in place of the schedule (0: the
    // schedule), and the Peak's times are divided by `timeScale`.
    int cycleOpen = 0, cyclePeriod = 0;
    double timeScale = 1.0;
    // The entrance scene's 42 s kept at their real length when the times are sped up (the clients play it whole; the Roc
    // comes after it); the tests speed it up too.
    bool realSceneTime = true;
};

class RocHost {
public:
    virtual ~RocHost() = default;
    // A text key's notice to every player of the shard (on screen and in the chat).
    virtual void RocShardNotice(const std::string& key) = 0;
    // A counted deed of the player (ours, sr_char_records: the beta quests): the Roc's defeat witnessed in the Peak (`roc`).
    virtual void RocDeed(std::uint32_t /*entity*/, std::string_view /*name*/) {}
    // A message to one player.
    virtual void RocSend(std::uint32_t entity, game::proto::MessageId id, const std::vector<std::byte>& payload) = 0;
    // A teleport point's arrival (`_RefTeleport`; none: unknown).
    virtual std::optional<game::proto::WorldPosition> RocPoint(std::uint32_t point) = 0;
};

class Roc {
public:
    using Clock = std::chrono::steady_clock;
    // The teleport points (`_RefTeleport`): the mountain's gate (where the Peak's players are sent), the Peak's arrival.
    static constexpr std::uint32_t kMountainGate = 160, kPeakGate = 162;

    Roc(RocHost& host, Instances& instances, Monsters& monsters, const assets::CharacterTable& characters, const assets::SkillTable* skills,
        RocConfig config, std::uint32_t seed);
    ~Roc();
    Roc(const Roc&) = delete;
    Roc& operator=(const Roc&) = delete;

    // The schedule (the gate opened and shut) and the Peak's timers.
    void Tick(Clock::time_point now);
    // A developer's switches: the gate opened (a Peak made), shut, the Peak ended (everyone out), the seal stones'
    // followers counted dead (the Roc's entrance 10 minutes later).
    bool Open(Clock::time_point now);
    void Shut();
    void End();
    void Break(Clock::time_point now);
    // A monster killed (the followers, the spirits, the Roc).
    void Killed(const Monsters::Damaged& killed, const game::proto::WorldPosition& at, Clock::time_point now);
    // A player through the gate into the Peak (its link already checked): false when the gate is shut (nothing done).
    bool Admit(std::uint32_t entity);

    // Its state, for the tests and the log.
    bool GateOpen() const;
    std::uint32_t Layer() const; // the Peak's (0: none)
    int Step() const;            // 0 the stones, 1 the entrance, 2 the Roc to come, 3 the Roc, 5 the end's wait, 6 over
    int FollowersLeft() const;
    std::uint32_t RocEntity() const; // the Roc's (0: not standing)
    int SpiritKinds() const;         // the spirits' kinds standing

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace sr::server
