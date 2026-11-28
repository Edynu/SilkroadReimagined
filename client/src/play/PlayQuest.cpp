// The quests on the game screen: the server's quest messages into the player's QuestView,
// what an NPC says into its talk window, and the lines of what happened: the quest banner across the top for a
// goal's progress, a quest's goals met and a quest completed; the system messages for the reward and refusals.

#include <algorithm>
#include <cmath>
#include <format>

#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

// A quest's text in the client's language: a key of its text tables, or the words themselves.
std::string TextOf(const UiData& data, const game::QuestText& t) {
    if (!t.key.empty()) return data.HasText(t.key) ? data.Text(t.key) : t.key;
    return t.literal;
}

} // namespace

// A timed quest's last minute inside an instance: "After %d seconds, the quest will complete and you will be returned ..."
// at every tenth second and each of the last ten (the 1.188 quest window's timer, CNIFQuest's).
void PlayScreen::StepQuestTimers(ScreenContext& context) {
    if (instance_.world == 0 || questEnds_.empty()) return;
    for (const auto& [quest, ends] : questEnds_) {
        const int left = int(std::ceil(ends - context.time));
        if (left <= 0 || left >= 60 || !(left % 10 == 0 || left <= 10)) continue;
        if (int& said = questTimerSaid_[quest]; said != left) {
            said = left;
            std::string line = gui_.data().Text("UIIT_MSG_QUEST_INS_TIMEOUT");
            if (const std::size_t at = line.find("%d"); at != std::string::npos) line.replace(at, 2, std::to_string(left));
            player_.Post(game::GameMessage::Kind::General, line, context.time);
        }
    }
}

// The event guide's rows for the player's country: "xxx" anyone, 중국 / 유럽 a country, an NPC row's "CH-..." / "EU-..." its
// country's NPC; any other words (a state's, a window's) anyone.
static bool EventGuideFor(std::string_view who, bool europe) {
    while (!who.empty() && who.front() == ' ') who.remove_prefix(1);
    if (who == "\xEC\xA4\x91\xEA\xB5\xAD" || who.starts_with("CH-")) return !europe;
    if (who == "\xEC\x9C\xA0\xEB\x9F\xBD" || who.starts_with("EU-")) return europe;
    return true;
}

void PlayScreen::GuideEvent(int kind, std::string_view value) {
    if (!eventGuidesKnown_ || replay_ || !shared_ || !shared_->session) return;
    const assets::GameGuideData* data = hudContext_.gameGuide ? hudContext_.gameGuide() : nullptr;
    if (data == nullptr) return;
    const bool europe = player_.self.race == 1;
    std::uint64_t bits = 0;
    for (const assets::EventGuideRow& e : data->events) {
        if (!e.inService || e.kind != kind || e.guide < proto::kEventGuideFirst || e.guide >= proto::kEventGuideFirst + 64) continue;
        const std::uint64_t bit = std::uint64_t(1) << (e.guide - proto::kEventGuideFirst);
        if (((hudContext_.eventGuides | eventGuidesAsked_ | bits) & bit) != 0 || !EventGuideFor(e.who, europe)) continue;
        // A level row: reached (its value the level); the others: that value met.
        const bool met = kind == 1 ? int(player_.self.level) >= std::atoi(e.value.c_str()) : e.value == value;
        if (met) bits |= bit;
    }
    if (bits == 0) return;
    eventGuidesAsked_ |= bits;
    shared_->session->SendGame(proto::EventGuideMet::kId, proto::Encode(proto::EventGuideMet{bits}));
}

void PlayScreen::StepEventGuides(ScreenContext&) {
    if (!eventGuidesKnown_ || replay_ || !hudContext_.gameGuide || hudContext_.gameGuide() == nullptr) return;
    const game::CharacterStatus& s = player_.self;
    GuideWatch& w = guideWatch_;
    if (!w.entered) {
        w.entered = true;
        GuideEvent(9, "1"); // the first entry into the world
    }
    if (int(s.level) != w.level) {
        w.level = s.level;
        GuideEvent(1, {});
    }
    if (s.berserk >= game::CharacterStatus::kBerserkBars) GuideEvent(6, "1");
    if (s.maxHp > 0 && s.hp > 0 && s.hp * 2 <= s.maxHp) GuideEvent(6, "2");
    if (std::any_of(player_.effects.begin(), player_.effects.end(), [](const game::ActiveEffect& e) { return e.kind == game::ActiveEffect::Kind::State; })) {
        GuideEvent(6, "3");
    }
    if (w.skillPointsKnown && s.skillPoints > w.skillPoints) GuideEvent(6, "4");
    w.skillPoints = s.skillPoints;
    w.skillPointsKnown = true;
    if (player_.job.type != game::JobType::None) GuideEvent(5, std::to_string(int(player_.job.type)));
    if (hud_.Shown(hud::kMainPopup) && (hud_.Page() == hud::kPageParty || hud_.Page() == hud::kPageAcademy)) GuideEvent(8, std::to_string(hud_.Page()));
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr) return;
    const proto::WorldPosition& at = self->position;
    if (!w.first) w.first = at;
    if (at.region != w.first->region || std::hypot(at.x - w.first->x, at.z - w.first->z) > 1.0f) GuideEvent(9, "2");
    // The field: out of every town (regioninfo.txt's areas); a death there, then standing in a town again.
    const bool inTown = towns_.TownAt(at.region, at.x, at.z) >= 0;
    if (!inTown) GuideEvent(9, "3");
    if (s.hp == 0 && !inTown) w.diedInField = true;
    if (w.diedInField && s.hp > 0 && inTown) {
        w.diedInField = false;
        GuideEvent(9, "4");
    }
}

void PlayScreen::HandleQuest(ScreenContext& context, const proto::QuestEvent& event) {
    const UiData& data = gui_.data();
    const auto post = [&](const std::string& line) { player_.Post(game::GameMessage::Kind::General, line, context.time); };
    const auto banner = [&](const std::string& line) { player_.Notify(game::Banner::Kind::Quest, line, context.time); };
    lastTime_ = context.time;
    // The timed quests' ends (for the instance's last-minute lines, StepQuestTimers).
    const auto timer = [&](const proto::QuestState& s) {
        if (s.secondsLeft > 0) questEnds_[s.quest] = context.time + double(s.secondsLeft);
        else questEnds_.erase(s.quest);
    };
    if (const auto* list = std::get_if<proto::QuestList>(&event)) {
        questEnds_.clear();
        for (const proto::QuestState& s : list->active) timer(s);
    } else if (const auto* changed = std::get_if<proto::QuestChanged>(&event)) {
        timer(changed->state);
    } else if (const auto* gone = std::get_if<proto::QuestGone>(&event)) {
        questEnds_.erase(gone->quest);
    }
    // The party's master opened the quest's instance: "For this quest, you will teleport. Continue?" (the original's
    // invitation kind 7, a Confirm box), the answer sent; --guild-accept / --party-accept answer it yes.
    if (const auto* ask = std::get_if<proto::QuestEnterAsk>(&event)) {
        const std::uint32_t quest = ask->quest;
        const auto answer = [this, quest](bool yes) {
            questEnterBox_ = nullptr;
            // Yes while dead: 1.188's box (on_252, a quest instance's kind 4) says "A teleport cannot be carried out when
            // you are dead." on the warning banner and sends nothing (the server's wait ends it as a no).
            if (yes && player_.self.hp == 0) {
                player_.Notify(game::Banner::Kind::Warning, gui_.data().Text("UIIT_MSG_QUEST_INS_DEATH"), lastTime_);
                return;
            }
            core::LogInfo("game: quest {}: its instance's entry answered {}", quest, yes ? "yes" : "no");
            if (!replay_ && shared_ && shared_->session) {
                shared_->session->SendGame(proto::QuestEnterReply::kId, proto::Encode(proto::QuestEnterReply{quest, std::uint8_t(yes ? 1 : 0)}));
            }
        };
        if (context.config.GetBool("guild-accept", false) || context.config.GetBool("party-accept", false)) {
            answer(true);
        } else if (ConfirmBox* box = hud_.Confirm("UIIT_STT_CONFIRM_BOX", {gui_.data().Text("UIIT_MSG_QUEST_ENTER_INSTANCE")}, answer)) {
            questEnterBox_ = box;
            questEnterUntil_ = context.time + proto::kQuestEnterAskSeconds;
        }
        return;
    }
    if (const auto* guides = std::get_if<proto::EventGuideState>(&event)) {
        hudContext_.eventGuides = guides->mask;
        eventGuidesKnown_ = true;
        eventGuidesAsked_ &= ~guides->mask;
        // A page newly met: its side icon (the highest new bit's page, as the original reads its answer).
        if (guides->added != 0) {
            int bit = 63;
            while (((guides->added >> bit) & 1u) == 0) --bit;
            ShowEventGuide(hudContext_, proto::kEventGuideFirst + std::uint32_t(bit));
            core::LogInfo("game: event guide {} met", proto::kEventGuideFirst + std::uint32_t(bit));
        }
        return;
    }
    if (const auto* guide = std::get_if<proto::QuestGuide>(&event)) {
        ShowQuestGuide(hudContext_, guide->rows); // the level-up's quest guide
        return;
    }
    for (const game::QuestView::Line& line : quests_.Apply(event)) {
        using Kind = game::QuestView::Line::Kind;
        const game::QuestDef* q = quests_.Find(line.quest);
        const std::string name = q != nullptr ? TextOf(data, q->name) : std::to_string(line.quest);
        switch (line.kind) {
        case Kind::Taken:
            post(std::format("{}: {}", data.Text("UIIT_STT_QUEST"), name));
            // A quest given by itself (the Beginner's Assistant) tells where to go on the quest notice: its `accepted`
            // words ("...You will receive a gift if you go to a potion merchant.").
            if (q != nullptr && q->given) {
                const auto said = std::find_if(q->talks.begin(), q->talks.end(), [](const game::QuestTalk& t) { return t.moment == "accepted"; });
                if (said != q->talks.end()) banner(TextOf(data, said->text));
            }
            core::LogInfo("game: quest {} taken", q != nullptr ? q->codename : std::to_string(line.quest));
            if (q != nullptr) GuideEvent(2, q->codename);
            break;
        case Kind::Progress:
            // The goal's line with its count put in (its text's "%d": "Collect 10 strong straws (3)"), on the quest
            // notice (formats the record's text with its values, shows it in window 42,
            // GDR_UPDATE_QUEST_INFO). A goal without a text of its own (ours): the quest's name and the count.
            if (q != nullptr && line.goal < q->goals.size()) {
                const game::QuestText& text = q->goals[line.goal].text;
                banner(text.Empty() ? std::format("{} ({}/{})", name, line.value, line.need) : FormatText(TextOf(data, text), {std::to_string(line.value)}));
                core::LogInfo("game: quest {}: goal {} at {} of {}", q->codename, line.goal, line.value, line.need);
            }
            break;
        case Kind::Achieved: {
            // The finisher's words for the goals met (BASIC_MENUSTRING_ACHIEVED_NOW: "The hunting of Water Ghosts is
            // completed. Go and report to Yangyun."), else the quest's name and "(Done)".
            const game::QuestText now = q != nullptr ? game::QuestView::TalkOf(*q, q->finisher, "done-now") : game::QuestText{};
            banner(now.Empty() ? std::format("{} ({})", name, data.Text("UIIT_STT_QUEST_END")) : TextOf(data, now));
            core::LogInfo("game: quest {}: its goals met", q != nullptr ? q->codename : std::to_string(line.quest));
            break;
        }
        case Kind::Finished:
            banner(std::format("{} {}", name, data.Text("SN_TALK_COMMON_END")));
            core::LogInfo("game: quest {} finished", q != nullptr ? q->codename : std::to_string(line.quest));
            break;
        case Kind::Abandoned:
        case Kind::TimeOut:
            post(std::format("{}: {}", name, data.Text(line.kind == Kind::TimeOut ? "SN_TALK_COMMON_FAIL" : "UIIT_STT_QUEST_GIVEUP")));
            core::LogInfo("game: quest {} {}", q != nullptr ? q->codename : std::to_string(line.quest), line.kind == Kind::TimeOut ? "timed out" : "given up");
            break;
        case Kind::Refused:
            if (line.code == proto::QuestCode::TooMany) post(data.Text("SN_TALK_COMMON_MAXQUEST"));
            else if (line.code == proto::QuestCode::BagFull) post(data.Text("UIIT_MSG_STRGERR_INVENTORY_FULL"));
            // The reward refused by the server (the reward window checks the choice first, MSG01 / MSG04): the
            // original's answer to the choice, "The reward was not given due to an unknown error."
            else if (line.code == proto::QuestCode::BadChoice) post(data.Text("UIIT_STT_QUESTREWARD_ERROR"));
            core::LogInfo("game: quest {}: refused ({})", line.quest, int(line.code));
            break;
        case Kind::Rewarded: {
            // The reward window's words (UIIT_STT_QUESTREWARD_*): "EXP 200", "SP EXP 1200", "SP 9", "Gold 50".
            const proto::QuestRewarded& r = line.reward;
            if (r.exp != 0) post(std::format("{} {}", data.Text("UIIT_STT_QUESTREWARD_EXP"), r.exp));
            if (r.skillExp != 0) post(std::format("{} {}", data.Text("UIIT_STT_QUESTREWARD_SPEXP"), r.skillExp));
            if (r.sp != 0) post(std::format("{} {}", data.Text("UIIT_STT_QUESTREWARD_SP"), r.sp));
            if (r.gold != 0) post(std::format("{} {}", data.Text("UIIT_STT_QUESTREWARD_GOLD"), r.gold));
            break;
        }
        }
    }
    // What the NPC says: into its talk window (when it is the one open).
    if (std::holds_alternative<proto::QuestSays>(event)) {
        if (auto says = quests_.TakeSays()) {
            if (auto* npc = dynamic_cast<NpcWindow*>(hud_.interface() ? hud_.interface()->Window(hud::kNpc) : nullptr); npc != nullptr && npc->npc() == says->npc) {
                npc->ShowQuestSays(*says);
            }
        }
    }
}

} // namespace sr::client
