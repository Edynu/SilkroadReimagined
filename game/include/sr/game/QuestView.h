#pragma once

// The player's quests as the client keeps them: the definitions the server sent (read from
// their text, game::ParseQuest), the quests under way with their goals' progress, those finished, the rows of the
// NPC talked to, and what the NPC said last. The HUD draws from it; Apply returns lines for the system messages.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sr/game/Quest.h"
#include "sr/game/QuestProtocol.h"

namespace sr::game {

class QuestView {
public:
    struct Line {
        enum class Kind : std::uint8_t {
            Taken,    // a quest taken
            Progress, // a goal advanced (`goal`, `value` of its need)
            Achieved, // every goal met: back to its NPC
            Finished, // finished ("Quest completed.")
            Abandoned,
            TimeOut,
            Refused,  // a request refused (`code`)
            Rewarded, // its reward (`reward`)
        };
        Kind kind = Kind::Taken;
        std::uint32_t quest = 0;
        std::uint8_t goal = 0;
        std::uint32_t value = 0, need = 0;
        proto::QuestCode code = proto::QuestCode::Ok;
        proto::QuestRewarded reward;
    };

    std::vector<Line> Apply(const proto::QuestEvent& event);
    // The player left the world.
    void Reset();

    const QuestDef* Find(std::uint32_t quest) const;
    const std::map<std::uint32_t, QuestDef>& Defs() const { return defs_; }
    const std::map<std::uint32_t, proto::QuestState>& Active() const { return active_; }
    // Quest -> how often it has been finished.
    const std::map<std::uint32_t, std::uint16_t>& Finished() const { return finished_; }
    // The rows of the NPC last talked to (its entity in `npc`); empty when it has none.
    const proto::NpcQuests& NpcRows() const { return npcRows_; }
    // What an NPC said last, until it is taken (the talk window shows it).
    std::optional<proto::QuestSays> TakeSays();
    // What a goal needs (its count; one for a delivery or a talk).
    static std::uint32_t Need(const QuestGoal& g);
    // The quest's talk at an NPC for a moment (none: empty).
    static QuestText TalkOf(const QuestDef& q, const std::string& npc, const std::string& moment);
    // Every line of that moment in their order: the pages the NPC's window shows one after another (the original's
    // multi-page talks, LuaQuestShowMenuCommon's pages: QNO_KT_POTION_2's offer, QNO_WC_FERRY4_1's story).
    static std::vector<QuestText> TalksOf(const QuestDef& q, const std::string& npc, const std::string& moment);
    // The mark over an NPC (its codename), none when it has nothing for the player.
    std::optional<proto::QuestMark> MarkOf(const std::string& npc) const {
        const auto it = marks_.find(npc);
        return it != marks_.end() ? std::optional(it->second) : std::nullopt;
    }
    const std::map<std::string, proto::QuestMark>& Marks() const { return marks_; }
    // Bumped by every change (windows redraw when it moves).
    std::uint32_t Revision() const { return revision_; }

private:
    std::map<std::uint32_t, QuestDef> defs_;
    std::map<std::uint32_t, proto::QuestState> active_;
    std::map<std::uint32_t, std::uint16_t> finished_;
    proto::NpcQuests npcRows_;
    std::optional<proto::QuestSays> says_;
    std::map<std::string, proto::QuestMark> marks_;
    std::uint32_t revision_ = 0;
};

} // namespace sr::game
