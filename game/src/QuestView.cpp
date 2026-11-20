#include "sr/game/QuestView.h"

namespace sr::game {

std::uint32_t QuestView::Need(const QuestGoal& g) {
    return g.kind == QuestGoalKind::Kill || g.kind == QuestGoalKind::Drop || g.kind == QuestGoalKind::KillPlayer || g.kind == QuestGoalKind::Record ? g.count : 1;
}

QuestText QuestView::TalkOf(const QuestDef& q, const std::string& npc, const std::string& moment) {
    for (const QuestTalk& t : q.talks)
        if (t.npc == npc && t.moment == moment) return t.text;
    return {};
}

std::vector<QuestText> QuestView::TalksOf(const QuestDef& q, const std::string& npc, const std::string& moment) {
    std::vector<QuestText> out;
    for (const QuestTalk& t : q.talks)
        if (t.npc == npc && t.moment == moment) out.push_back(t.text);
    return out;
}

const QuestDef* QuestView::Find(std::uint32_t quest) const {
    const auto it = defs_.find(quest);
    return it != defs_.end() ? &it->second : nullptr;
}

std::optional<proto::QuestSays> QuestView::TakeSays() {
    std::optional<proto::QuestSays> s = std::move(says_);
    says_.reset();
    return s;
}

void QuestView::Reset() {
    active_.clear();
    finished_.clear();
    npcRows_ = {};
    says_.reset();
    marks_.clear();
    ++revision_;
}

std::vector<QuestView::Line> QuestView::Apply(const proto::QuestEvent& event) {
    std::vector<Line> lines;
    ++revision_;
    if (const auto* d = std::get_if<proto::QuestDefinition>(&event)) {
        QuestDef q;
        std::string error;
        if (ParseQuest(d->text, q, error)) defs_[q.id] = std::move(q);
    } else if (const auto* list = std::get_if<proto::QuestList>(&event)) {
        active_.clear();
        finished_.clear();
        for (const proto::QuestState& s : list->active) active_[s.quest] = s;
        for (const auto& f : list->finished) finished_[f.quest] = f.times;
    } else if (const auto* changed = std::get_if<proto::QuestChanged>(&event)) {
        const proto::QuestState& s = changed->state;
        const auto old = active_.find(s.quest);
        if (old == active_.end()) {
            lines.push_back({Line::Kind::Taken, s.quest});
        } else {
            // A goal's count that rose; its goals all met.
            const QuestDef* q = Find(s.quest);
            for (std::size_t i = 0; i < s.progress.size(); ++i) {
                const std::uint16_t before = i < old->second.progress.size() ? old->second.progress[i] : 0;
                if (s.progress[i] <= before || q == nullptr || i >= q->goals.size() || q->goals[i].hidden) continue;
                Line l{Line::Kind::Progress, s.quest, std::uint8_t(i)};
                l.value = s.progress[i];
                l.need = Need(q->goals[i]);
                lines.push_back(l);
            }
            if (s.status == proto::QuestStatus::Achieved && old->second.status != proto::QuestStatus::Achieved) lines.push_back({Line::Kind::Achieved, s.quest});
        }
        active_[s.quest] = s;
    } else if (const auto* gone = std::get_if<proto::QuestGone>(&event)) {
        active_.erase(gone->quest);
        if (gone->times > 0) finished_[gone->quest] = gone->times;
        lines.push_back({gone->why == proto::QuestEnd::Finished    ? Line::Kind::Finished
                         : gone->why == proto::QuestEnd::TimeOut   ? Line::Kind::TimeOut
                                                                   : Line::Kind::Abandoned,
                         gone->quest});
    } else if (const auto* rows = std::get_if<proto::NpcQuests>(&event)) {
        npcRows_ = *rows;
    } else if (const auto* says = std::get_if<proto::QuestSays>(&event)) {
        says_ = *says;
    } else if (const auto* answer = std::get_if<proto::QuestAnswer>(&event)) {
        if (answer->code != proto::QuestCode::Ok) {
            Line l{Line::Kind::Refused, answer->quest};
            l.code = answer->code;
            lines.push_back(l);
        }
    } else if (const auto* marks = std::get_if<proto::QuestMarks>(&event)) {
        marks_.clear();
        for (const auto& m : marks->marks) marks_[m.npc] = m.kind;
    } else if (const auto* reward = std::get_if<proto::QuestRewarded>(&event)) {
        Line l{Line::Kind::Rewarded, reward->quest};
        l.reward = *reward;
        lines.push_back(l);
    }
    return lines;
}

} // namespace sr::game
