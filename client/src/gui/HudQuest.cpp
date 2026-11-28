// The quest windows of the 1.188 client, from their res_ui layouts:
// - CNIFQuest (res_ui\questlist.2dt, Q): ten rows of 576 x 32, one a quest under way: its check box (listed in the
//   quick window), its title "name (times finished/times allowed)" (the record's count and its
//   CompletionLimit), its time ("No limit", "--: --", "N min",
//   "N h N min") or its countdown "h:m:s", the map button, the content button by the level gap
//   (more than 9 under the player blue, above it red) which opens the reward window.
// - CNIFQuestReward (res_ui\quest_re.2dt): the quest's page (its SN_PAYCON_* rich text), its reward's numbers in four
//   rows (EXP, Gold, SP EXP, SP, AP, the non-zero ones in that order), its items, and a button: "Abandon
//   quest" for one under way, "Reward" at its finisher (the items chosen first: "You must select your reward.",
//   "You must select all items.", each as a line and on the warning banner; "You cannot select more."). With items
//   to choose, "Reward" opens CNIFQuestReward_ConfirmWnd (res_ui\corfim.2dt): the items picked, five to a page, and
//   Confirm (the reward taken, both windows closed) or Cancel (back to the choice).
// - CNIFMiniQuestWnd (res_ui\questminilist.2dt): the quick window, up to three quests ("Max. of three quests can be
//   listed in quick window."), each "[On] name" with its goals under it, "[On] goal (n/m)" / "[Done] ...",
//   folded and unfolded by its button (qst_list_open/close_button).

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>

#include "gui/Hud.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr const char* kListLayout = "res_ui\\questlist.2dt";
constexpr const char* kRewardLayout = "res_ui\\quest_re.2dt";
constexpr const char* kConfirmLayout = "res_ui\\corfim.2dt";
constexpr const char* kMiniLayout = "res_ui\\questminilist.2dt";
constexpr int kMostQuick = 3;
const ui::Color kGoalOn = ui::Argb(0xFFEFDAA4), kGoalDone = ui::Argb(0xFFFF9C68), kMiniText = ui::Argb(0xFFFFF9BA);

// The quests listed in the quick window (the player's choice for this session; ours: a quest taken is listed while
// there is room).
std::vector<std::uint32_t>& QuickList() {
    static std::vector<std::uint32_t> list;
    return list;
}
bool Listed(std::uint32_t quest) {
    const auto& l = QuickList();
    return std::find(l.begin(), l.end(), quest) != l.end();
}

std::string TextOf(const HudContext& c, const game::QuestText& t) {
    if (!t.key.empty()) return c.hasText && c.hasText(t.key) ? c.Text(t.key) : t.key;
    return t.literal;
}

// Every record of a layout as a widget of `root`, each under the last one made with its parent's id; the made ones
// in the records' order.
std::vector<ui::Widget*> Build(ui::Interface& ui, ui::Widget& root, const std::vector<assets::Ui2dtRecord>& records,
                               const std::function<void(const assets::Ui2dtRecord&, assets::UiLayoutNode&)>& adjust = {}) {
    std::vector<ui::Widget*> made(records.size(), nullptr);
    if (records.empty()) return made;
    made[0] = &root;
    std::map<int, ui::Widget*> last{{records[0].id, &root}};
    const ui::Rect origin{records[0].rect.x, records[0].rect.y, 0, 0};
    for (std::size_t i = 1; i < records.size(); ++i) {
        const assets::Ui2dtRecord& r = records[i];
        // Positions in the layout are the screen's; a child's place is from its parent's corner.
        float ox = origin.x, oy = origin.y;
        const auto parent = last.find(r.parents[0]);
        ui::Widget* under = parent != last.end() ? parent->second : &root;
        if (under != &root) {
            for (std::size_t k = 0; k < i; ++k)
                if (made[k] == under) {
                    ox = records[k].rect.x;
                    oy = records[k].rect.y;
                }
        }
        assets::UiLayoutNode n = assets::Ui2dtNode(r, ox, oy);
        if (adjust) adjust(r, n);
        made[i] = under->Add(ui.Make(n));
        last[r.id] = made[i];
    }
    return made;
}

const game::QuestDef* DefOf(const HudContext& c, std::uint32_t quest) { return c.quests != nullptr ? c.quests->Find(quest) : nullptr; }

// A goal's line with its count put in ("Hunt 15 Water Ghost (3)"; its "%d"), as the quest notice's; a goal without a text
// of its own (ours): the quest's name and "(n/m)". `met`: its count reached.
std::string GoalLine(const HudContext& c, const game::QuestDef& q, std::size_t g, const proto::QuestState& s, bool& met) {
    const game::QuestGoal& goal = q.goals[g];
    const std::uint32_t need = game::QuestView::Need(goal);
    const std::uint32_t have = g < s.progress.size() ? s.progress[g] : 0;
    met = have >= need;
    std::string line = goal.text.Empty() ? TextOf(c, q.name) : FormatText(TextOf(c, goal.text), {std::to_string(std::min(have, need))});
    if (goal.text.Empty() && need > 1) line += std::format(" ({}/{})", std::min(have, need), need);
    return line;
}

// The reward window's page: the quest's rich text inside its rect, scrolled by the wheel over it (18 a step).
class QuestPage : public ui::RichText {
public:
    using RichText::RichText;
    float scroll = 0.0f;
    void Draw(ui::Frame& frame) override {
        frame.paint.PushClip(ScreenRect());
        const ui::Rect kept = rect;
        rect.y -= scroll;
        rect.h += scroll;
        RichText::Draw(frame);
        rect = kept;
        frame.paint.PopClip();
    }
    void Mouse(ui::Frame& frame) override {
        if (frame.pointer.hovered == this && frame.input.wheel() != 0.0f) scroll = std::max(0.0f, scroll + (frame.input.wheel() > 0.0f ? -18.0f : 18.0f));
    }
    void Created(ui::Interface& ui) override {
        RichText::Created(ui);
        interactive = true;
    }
};

// --- The quest window ------------------------------------------------------------------------------------------

class QuestListWindow : public ui::MainFrame {
public:
    QuestListWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kListLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        const std::vector<ui::Widget*> made = Build(ui, *this, *records);
        for (std::size_t i = 0; i < records->size(); ++i) {
            if ((*records)[i].className == "CNIFQuestSlot") rows_.push_back({made[i]});
            if ((*records)[i].id == 6 && (*records)[i].parents[0] == records->front().id) bar_ = ui::As<ui::ScrollBar>(made[i]);
        }
        // The row's parts by their ids (questlist.2dt): 7 the small box, 8 the check box, 16 the title, 18 the map,
        // 19 the content, 11 a goal's state ("in progress and complete"), 75 the time left.
        for (Row& r : rows_) {
            r.box = r.slot->Child(7);
            r.check = ui::As<ui::CheckBox>(r.slot->Child(8));
            r.title = r.slot->Child(16);
            r.map = r.slot->Child(18);
            r.content = ui::As<ui::Button>(r.slot->Child(19));
            r.state = r.slot->Child(11);
            r.countdown = r.slot->Child(75);
            if (auto* t = ui::As<ui::Static>(r.title)) r.titleColor = t->textColor;
        }
    }

    void Update(ui::Frame& frame) override {
        ui::MainFrame::Update(frame);
        if (c_.quests == nullptr) return;
        // Redrawn when the quests change, and once a second for the countdowns.
        const int second = int(c_.time);
        if (c_.quests->Revision() == revision_ && second == second_ && (bar_ == nullptr || bar_->position() == scroll_)) return;
        revision_ = c_.quests->Revision();
        second_ = second;
        Fill();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return ui::MainFrame::OnNotice(n);
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const Row& r = rows_[i];
            if (r.quest == 0) continue;
            if (n.source == r.check) {
                auto& list = QuickList();
                if (r.check->checked && !Listed(r.quest)) {
                    if (int(list.size()) >= kMostQuick) {
                        r.check->checked = false;
                        if (c_.systemLine) c_.systemLine(c_.Text("UIIT_CTL_QUEST_HELPER_FULL"));
                    } else {
                        list.push_back(r.quest);
                    }
                } else if (!r.check->checked) {
                    list.erase(std::remove(list.begin(), list.end(), r.quest), list.end());
                }
                return true;
            }
            if (n.source == r.content) {
                OpenQuestReward(c_, r.quest, 0);
                return true;
            }
            if (n.source == r.map) {
                // The quest chosen: its NPCs marked on the minimap (1.188's CNIFQuestSlot control 18), and the map.
                c_.trackedQuest = r.quest;
                WorldMapQuest(c_, r.quest);
                return true;
            }
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    struct Row {
        ui::Widget* slot = nullptr;
        ui::CheckBox* check = nullptr;
        ui::Widget *box = nullptr, *title = nullptr, *map = nullptr, *state = nullptr, *countdown = nullptr;
        ui::Button* content = nullptr;
        ui::Color titleColor{};    // its title's colour as made
        std::uint32_t quest = 0;   // a quest's row (0: a goal's or none)
    };

    // The rows as 1.188's list lays them (its on_2): from the quest the bar points at, each quest's row (its big box,
    // check box, "name (done/most)", map and content buttons) and under it one row per goal (no box: its state "In
    // progress" or "Completed" and its line, in 0xFFEFDAA4 or 0xFFFF9C68), until the rows run out.
    void Fill() {
        std::vector<const game::QuestDef*> quests;
        for (const auto& [id, s] : c_.quests->Active()) {
            if (const game::QuestDef* q = DefOf(c_, id)) quests.push_back(q);
        }
        const auto lines = [](const game::QuestDef& q) {
            return 1 + int(std::count_if(q.goals.begin(), q.goals.end(), [](const game::QuestGoal& g) { return !g.hidden; }));
        };
        // The bar by quests: as far as the last ones fill the rows.
        int extra = int(quests.size());
        for (int fit = 0; extra > 0 && fit + lines(*quests[std::size_t(extra - 1)]) <= int(rows_.size()); --extra) fit += lines(*quests[std::size_t(extra - 1)]);
        // The range given again only when it changes (SetRange starts the bar again at the top).
        if (bar_ != nullptr && extra != barExtra_) {
            const int kept = bar_->position();
            barExtra_ = extra;
            bar_->SetRange(0, extra);
            bar_->SetPosition(std::min(kept, extra));
        }
        scroll_ = bar_ != nullptr ? std::clamp(bar_->position(), 0, extra) : 0;
        const int level = c_.state != nullptr ? c_.state->self.level : 1;
        std::size_t i = 0;
        for (std::size_t at = std::size_t(scroll_); at < quests.size() && i < rows_.size(); ++at) {
            const game::QuestDef* q = quests[at];
            const proto::QuestState& s = c_.quests->Active().at(q->id);
            ShowQuestRow(rows_[i++], *q, s, level);
            for (std::size_t g = 0; g < q->goals.size() && i < rows_.size(); ++g) {
                if (q->goals[g].hidden) continue;
                bool met = false;
                const std::string line = GoalLine(c_, *q, g, s, met);
                ShowGoalRow(rows_[i++], line, met);
            }
        }
        for (; i < rows_.size(); ++i) {
            rows_[i].slot->visible = false;
            rows_[i].quest = 0;
        }
    }

    void ShowGoalRow(Row& r, const std::string& line, bool met) {
        r.slot->visible = true;
        r.quest = 0;
        if (auto* art = ui::As<ui::Static>(r.slot)) art->noArt = true;
        for (ui::Widget* w : {r.box, static_cast<ui::Widget*>(r.check), r.map, static_cast<ui::Widget*>(r.content), r.countdown}) {
            if (w != nullptr) w->visible = false;
        }
        const ui::Color colour = met ? kGoalDone : kGoalOn;
        if (r.state != nullptr) {
            r.state->visible = true;
            r.state->SetText(c_.Text(met ? "UIIT_STT_QUEST_END" : "UIIT_STT_QUEST_ING"));
            if (auto* st = ui::As<ui::Static>(r.state)) st->textColor = colour;
        }
        if (r.title != nullptr) {
            r.title->SetText(line);
            if (auto* st = ui::As<ui::Static>(r.title)) st->textColor = colour;
        }
    }

    void ShowQuestRow(Row& r, const game::QuestDef& qd, const proto::QuestState& s, int level) {
        const game::QuestDef* q = &qd;
        r.slot->visible = true;
        r.quest = q->id;
        if (auto* art = ui::As<ui::Static>(r.slot)) art->noArt = false;
        for (ui::Widget* w : {r.box, static_cast<ui::Widget*>(r.check), r.map, static_cast<ui::Widget*>(r.content)}) {
            if (w != nullptr) w->visible = true;
        }
        if (r.state != nullptr) r.state->visible = false;
        if (auto* st = ui::As<ui::Static>(r.title)) st->textColor = r.titleColor;
        {
            const auto done = c_.quests->Finished().find(q->id);
            const int times = done != c_.quests->Finished().end() ? done->second : 0;
            if (r.title) r.title->SetText(std::format("{} ({}/{})", TextOf(c_, q->name), times, q->repeat));
            if (r.check) r.check->checked = Listed(q->id);
            // The content button by the gap between the player's level and the quest's.
            if (r.content) {
                const int gap = level - q->level;
                r.content->texture = gap > 9 ? "interface\\quest\\qst_contentviewblue_button.ddj"
                                     : gap < 0 ? "interface\\quest\\qst_contentviewred_button.ddj"
                                               : "interface\\quest\\qst_contentview_button.ddj";
            }
            // A timed quest's time left (75), "h:m:s".
            const bool timed = s.secondsLeft > 0;
            if (r.countdown) {
                r.countdown->visible = timed;
                const std::uint32_t left = s.secondsLeft > std::uint32_t(c_.time - stateTime_) ? s.secondsLeft : 0;
                r.countdown->SetText(std::format("{}:{}:{}", left / 3600, left / 60 % 60, left % 60));
            }
        }
    }

    HudContext& c_;
    std::vector<Row> rows_;
    ui::ScrollBar* bar_ = nullptr;
    std::uint32_t revision_ = ~0u;
    int second_ = -1, scroll_ = 0, barExtra_ = -1;
    double stateTime_ = 0;
};

// --- The reward window -----------------------------------------------------------------------------------------

void OpenRewardConfirm(HudContext& c, std::uint32_t quest, std::uint32_t npc, const std::vector<std::uint8_t>& picked);

class QuestRewardWindow : public ui::MainFrame {
public:
    QuestRewardWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kRewardLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        const std::vector<ui::Widget*> made = Build(ui, *this, *records, [](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
            if (r.id == 96) n.type = "CNIFQuestPage";            // the quest's page (its rich text)
            if (r.className == "CIFSlotWithHelpEx") n.type = "CIFSlotWithHelp"; // the reward's items
        });
        for (std::size_t i = 0; i < records->size(); ++i) {
            const assets::Ui2dtRecord& r = (*records)[i];
            if (r.id == 96) page_ = made[i];
            if (r.id == 38) button_ = made[i];
            if (r.className == "CNIFGoldSlot") golds_.push_back(made[i]);
            if (r.id == 121) goldWindow_ = made[i];
            if (r.id == 120) itemWindow_ = made[i];
            if (r.className == "CIFSlotWithHelpEx") {
                if (auto* slot = ui::As<ui::Slot>(made[i])) {
                    slot->pickable = false;
                    items_.push_back({r.id, slot});
                }
            }
        }
        std::sort(items_.begin(), items_.end(), [](const auto& a, const auto& b) { return a.first < b.first; }); // 301.. in order
    }

    void Show(std::uint32_t quest, std::uint32_t npc) {
        quest_ = quest;
        npc_ = npc;
        picked_.clear();
        const game::QuestDef* q = DefOf(c_, quest);
        if (q == nullptr) return;
        if (page_ != nullptr) {
            // The page reads only the original's <sml2> markup (its SN_PAYCON_* texts); our own quests' plain words are
            // put in it.
            std::string page = TextOf(c_, q->rewardText.Empty() ? q->name : q->rewardText);
            if (!page.starts_with("<sml2>")) page = "<sml2>" + page + "</sml2>";
            page_->SetText(page);
        }
        if (auto* page = dynamic_cast<QuestPage*>(page_)) page->scroll = 0.0f;
        // The numbers, the non-zero ones in the original's order, into the four rows.
        const std::pair<const char*, std::uint64_t> rows[] = {{"UIIT_STT_QUESTREWARD_EXP", q->reward.exp},
                                                              {"UIIT_STT_QUESTREWARD_GOLD", q->reward.gold},
                                                              {"UIIT_STT_QUESTREWARD_SPEXP", q->reward.skillExp},
                                                              {"UIIT_STT_QUESTREWARD_SP", q->reward.sp},
                                                              {"UIIT_STT_QUESTREWARD_AP", q->reward.points.empty() ? 0 : q->reward.points[0].amount}};
        std::size_t g = 0;
        for (const auto& [key, value] : rows) {
            if (value == 0 || g >= golds_.size()) continue;
            if (ui::Widget* label = golds_[g]->Child(44)) label->SetText(c_.Text(key));
            if (ui::Widget* number = golds_[g]->Child(50)) number->SetText(GoldText(value));
            golds_[g]->visible = true;
            ++g;
        }
        for (; g < golds_.size(); ++g) golds_[g]->visible = false;
        // The gold window only for a reward with numbers, the item window only for one with items (the original's fill).
        if (goldWindow_ != nullptr) goldWindow_->visible = std::any_of(golds_.begin(), golds_.end(), [](const ui::Widget* w) { return w->visible; });
        // The items the player may get: a reward by country (_RefQuestReward.IsCheckCountry) lists only those whose country
        // is the character's (1.188's list: the item's Country against the character's race; its gender test is set on
        // no quest).
        shown_.clear();
        const int race = c_.state != nullptr ? int(c_.state->self.race) : 0;
        for (std::size_t i = 0; i < q->reward.items.size(); ++i) {
            const assets::ItemRow* row = c_.items != nullptr ? c_.items->Find(std::string_view(q->reward.items[i].item)) : nullptr;
            if (q->reward.byCountry && row != nullptr && row->country != race) continue;
            shown_.push_back(i);
        }
        if (itemWindow_ != nullptr) itemWindow_->visible = !shown_.empty();
        // The items.
        for (std::size_t i = 0; i < items_.size(); ++i) {
            ui::Slot* slot = items_[i].second;
            const bool has = i < shown_.size();
            const std::size_t at = has ? shown_[i] : 0;
            const assets::ItemRow* row = has && c_.items != nullptr ? c_.items->Find(std::string_view(q->reward.items[at].item)) : nullptr;
            slot->icon = row != nullptr ? "icon\\" + row->iconPath : std::string();
            slot->count = has ? int(q->reward.items[at].count) : 0;
            slot->overlay.clear();
            slot->tooltipLines = [this, row, at, q] {
                if (row == nullptr) return std::vector<ui::TooltipLine>{};
                game::ItemInstance item;
                item.refId = row->refObjId;
                item.count = std::uint16_t(q->reward.items[at].count);
                return ItemTooltip(*row, item, c_);
            };
        }
        // The button: "Reward" at its finisher, "Abandon quest" for one under way, none otherwise.
        const bool active = c_.quests->Active().contains(quest);
        if (button_ != nullptr) {
            button_->visible = npc != 0 || active;
            button_->SetText(c_.Text(npc != 0 ? "UIIT_STT_QUEST_REWARD" : "UIIT_STT_QUEST_GIVEUP"));
        }
    }

    // The reward item of that codename picked and the button pressed, as a player would (the quests' tests).
    bool PickAndTake(const std::string& item) {
        const game::QuestDef* q = DefOf(c_, quest_);
        if (q == nullptr || npc_ == 0) return false;
        for (std::size_t i = 0; i < q->reward.items.size(); ++i) {
            if (q->reward.items[i].item != item) continue;
            // Picked, "Reward" pressed, then the confirm's Confirm, as the player's three clicks.
            picked_ = {std::uint8_t(i)};
            if (c_.questRequest) c_.questRequest(proto::QuestHandIn::kId, proto::Encode(proto::QuestHandIn{npc_, quest_, picked_}));
            if (c_.hud != nullptr) c_.hud->Hide(hud::kQuestReward);
            if (c_.hud != nullptr) c_.hud->Hide(hud::kQuestRewardConfirm);
            return true;
        }
        return false;
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return ui::MainFrame::OnNotice(n);
        const game::QuestDef* q = DefOf(c_, quest_);
        if (q == nullptr) return ui::MainFrame::OnNotice(n);
        const auto line = [&](const char* key) {
            if (c_.systemLine) c_.systemLine(c_.Text(key));
        };
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (n.source != items_[i].second || npc_ == 0 || q->reward.choose == 0 || i >= shown_.size()) continue;
            // An item chosen or put back; no more than the quest lets choose.
            const std::uint8_t at = std::uint8_t(shown_[i]);
            const auto it = std::find(picked_.begin(), picked_.end(), at);
            if (it != picked_.end()) {
                picked_.erase(it);
                items_[i].second->overlay.clear();
            } else if (picked_.size() >= q->reward.choose) {
                line("UIIT_STT_QUESTREWARD_MSG03");
            } else {
                picked_.push_back(at);
                items_[i].second->overlay = "interface\\ifcommon\\com_item_exchange.ddj";
            }
            return true;
        }
        if (n.source == button_) {
            if (npc_ != 0) {
                // At its finisher: the choice complete first (a line and the warning banner), then the confirm with
                // the items picked; nothing to choose, taken at once.
                const auto refuse = [&](const char* key) {
                    line(key);
                    if (c_.state != nullptr) c_.state->Notify(game::Banner::Kind::Warning, c_.Text(key), c_.time);
                };
                if (q->reward.choose > 0 && picked_.empty()) return refuse("UIIT_STT_QUESTREWARD_MSG01"), true;
                if (q->reward.choose > 0 && picked_.size() < q->reward.choose) return refuse("UIIT_STT_QUESTREWARD_MSG04"), true;
                if (q->reward.choose > 0) {
                    OpenRewardConfirm(c_, quest_, npc_, picked_);
                    return true;
                }
                if (c_.questRequest) c_.questRequest(proto::QuestHandIn::kId, proto::Encode(proto::QuestHandIn{npc_, quest_, picked_}));
                if (c_.hud != nullptr) c_.hud->Hide(hud::kQuestReward);
                return true;
            }
            // Under way: given up after the question, the quest's own warning: one that locks "If
            // you abandon this quest, you can't do it again later.", else "... you can do it later.".
            if (c_.hud != nullptr) {
                const std::uint32_t quest = quest_;
                HudContext* c = &c_;
                const char* warning = q->giveUpLocks ? "UIIT_MSG_QUEST_GIVEUP_WINDOW_1" : "UIIT_MSG_QUEST_GIVEUP_WINDOW_2";
                c_.hud->Confirm("UIIT_STT_AGREEMENT_BOX", {c_.Text(warning)}, [c, quest](bool yes) {
                    // Not while an NPC's talk is open: the original's server refuses it (its code 5, "NPC chat window
                    // is on. Therefore unable to process other requests."); ours never hears of the talk's end, so the
                    // client keeps the rule.
                    if (yes && c->hud != nullptr && c->hud->Shown(hud::kNpc)) {
                        if (c->systemLine) c->systemLine(c->Text("UIIT_STT_QUEST_GIVEUP_1"));
                        return;
                    }
                    if (yes && c->questRequest) c->questRequest(proto::QuestAbandon::kId, proto::Encode(proto::QuestAbandon{quest}));
                    if (yes && c->hud != nullptr) c->hud->Hide(hud::kQuestReward);
                });
            }
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    HudContext& c_;
    ui::Widget *page_ = nullptr, *button_ = nullptr, *goldWindow_ = nullptr, *itemWindow_ = nullptr;
    std::vector<ui::Widget*> golds_;
    std::vector<std::pair<int, ui::Slot*>> items_;
    std::uint32_t quest_ = 0, npc_ = 0;
    std::vector<std::uint8_t> picked_;
    std::vector<std::size_t> shown_; // the reward's items listed (by country), by slot
};

// --- The reward's confirm ---------------------------------------------------------------------------------------

class QuestRewardConfirm : public ui::MainFrame {
public:
    QuestRewardConfirm(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        const auto* records = c_.layout2dt ? c_.layout2dt(kConfirmLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        if (ui::Widget* title = Child(0)) title->SetText(c_.Text(records->front().textKey));
        const std::vector<ui::Widget*> made = Build(ui, *this, *records, [](const assets::Ui2dtRecord& r, assets::UiLayoutNode& n) {
            if (r.className == "CIFSlotWithHelpEx") n.type = "CIFSlotWithHelp";
        });
        for (std::size_t i = 0; i < records->size(); ++i) {
            const assets::Ui2dtRecord& r = (*records)[i];
            if (r.id == 52) made[i]->SetText(c_.Text("UIIT_STT_QUESTREWARD_MSG02"));
            if (r.id == 7) (confirm_ = made[i])->SetText(c_.Text("UIIT_CTL_CONFIRM"));
            if (r.id == 8) (cancel_ = made[i])->SetText(c_.Text("UIIT_CTL_CANCEL"));
            if (r.id == 204) scroll_ = made[i];
            if (r.className == "CIFSlotWithHelpEx") {
                if (auto* slot = ui::As<ui::Slot>(made[i])) {
                    slot->pickable = false;
                    icons_.push_back({r.id, slot});
                }
            }
        }
        std::sort(icons_.begin(), icons_.end(), [](const auto& a, const auto& b) { return a.first < b.first; }); // 26..30 in order
    }

    // The items picked, five to a page (the first page; ours: no quest lets choose more than five).
    void Show(std::uint32_t quest, std::uint32_t npc, const std::vector<std::uint8_t>& picked) {
        quest_ = quest;
        npc_ = npc;
        picked_ = picked;
        const game::QuestDef* q = DefOf(c_, quest);
        if (scroll_ != nullptr) scroll_->visible = picked.size() > icons_.size();
        for (std::size_t i = 0; i < icons_.size(); ++i) {
            ui::Slot* slot = icons_[i].second;
            const std::size_t index = i < picked.size() ? picked[i] : ~std::size_t{0};
            const bool has = q != nullptr && index < q->reward.items.size();
            const assets::ItemRow* row = has && c_.items != nullptr ? c_.items->Find(std::string_view(q->reward.items[index].item)) : nullptr;
            slot->icon = row != nullptr ? "icon\\" + row->iconPath : std::string();
            slot->count = has ? int(q->reward.items[index].count) : 0;
            slot->tooltipLines = [this, row, index, q] {
                if (row == nullptr || q == nullptr) return std::vector<ui::TooltipLine>{};
                game::ItemInstance item;
                item.refId = row->refObjId;
                item.count = std::uint16_t(q->reward.items[index].count);
                return ItemTooltip(*row, item, c_);
            };
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked || n.source == nullptr) return ui::MainFrame::OnNotice(n);
        if (n.source == confirm_) {
            // The reward taken: the hand-in with the items picked, both windows closed.
            if (c_.questRequest) c_.questRequest(proto::QuestHandIn::kId, proto::Encode(proto::QuestHandIn{npc_, quest_, picked_}));
            if (c_.hud != nullptr) {
                c_.hud->Hide(hud::kQuestRewardConfirm);
                c_.hud->Hide(hud::kQuestReward);
            }
            return true;
        }
        if (n.source == cancel_) {
            if (c_.hud != nullptr) c_.hud->Hide(hud::kQuestRewardConfirm);
            return true;
        }
        return ui::MainFrame::OnNotice(n);
    }

private:
    HudContext& c_;
    ui::Widget *confirm_ = nullptr, *cancel_ = nullptr, *scroll_ = nullptr;
    std::vector<std::pair<int, ui::Slot*>> icons_;
    std::uint32_t quest_ = 0, npc_ = 0;
    std::vector<std::uint8_t> picked_;
};

// --- The quick window ------------------------------------------------------------------------------------------

class QuestQuickWindow : public ui::Widget {
public:
    QuestQuickWindow(const assets::UiLayoutNode& node, HudContext& c) : Widget(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        ui_ = &ui;
        const auto* records = c_.layout2dt ? c_.layout2dt(kMiniLayout) : nullptr;
        if (records == nullptr || records->empty()) return;
        const std::vector<ui::Widget*> made = Build(ui, *this, *records);
        std::vector<std::pair<float, ui::Widget*>> slots;
        for (std::size_t i = 0; i < records->size(); ++i)
            if ((*records)[i].className == "CNIFMiniQuestSlot") slots.push_back({(*records)[i].rect.y, made[i]});
        std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [y, s] : slots) rows_.push_back({s, ui::As<ui::Button>(s->Child(56)), s->Child(57)});
        slotHeight_ = rows_.size() >= 2 ? rows_[1].slot->rect.y - rows_[0].slot->rect.y : 30.0f;
    }

    void Update(ui::Frame& frame) override {
        Widget::Update(frame);
        if (c_.quests == nullptr) return;
        // A quest taken is listed while there is room (ours); one no longer under way leaves.
        auto& list = QuickList();
        for (const auto& [id, s] : c_.quests->Active())
            if (!seen_.contains(id)) {
                seen_.insert(id);
                if (int(list.size()) < kMostQuick && !Listed(id)) list.push_back(id);
            }
        list.erase(std::remove_if(list.begin(), list.end(), [&](std::uint32_t q) { return !c_.quests->Active().contains(q); }), list.end());
        Fill();
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind != ui::Notice::Kind::Clicked) return false;
        for (Row& r : rows_)
            if (n.source == r.fold && r.quest != 0) {
                folded_[r.quest] = !folded_[r.quest];
                return true;
            }
        return false;
    }

private:
    struct Row {
        ui::Widget* slot = nullptr;
        ui::Button* fold = nullptr;
        ui::Widget* text = nullptr;
        std::uint32_t quest = 0;
        std::vector<ui::Widget*> goals;
    };

    void Fill() {
        const auto& list = QuickList();
        float y = rows_.empty() ? 0.0f : rows_[0].slot->rect.y;
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            Row& r = rows_[i];
            const game::QuestDef* q = i < list.size() ? DefOf(c_, list[i]) : nullptr;
            r.slot->visible = q != nullptr;
            r.quest = q != nullptr ? q->id : 0;
            for (ui::Widget* g : r.goals) g->visible = false;
            if (q == nullptr) continue;
            const proto::QuestState& s = c_.quests->Active().at(q->id);
            const bool achieved = s.status == proto::QuestStatus::Achieved;
            const bool folded = folded_[q->id];
            r.slot->rect.y = y;
            if (r.text) {
                r.text->SetText(std::format("[{}] {}", c_.Text(achieved ? "UIIT_STT_QUEST_END" : "UIIT_STT_QUEST_ING"), TextOf(c_, q->name)));
                if (auto* st = ui::As<ui::Static>(r.text)) st->textColor = kMiniText;
            }
            if (r.fold) r.fold->texture = folded ? "interface\\quest\\qst_list_open_button.ddj" : "interface\\quest\\qst_list_close_button.ddj";
            y += slotHeight_;
            if (folded) continue;
            // Its goals under it, one line each: "[On] goal (n/m)", "[Done] goal".
            std::size_t shown = 0;
            for (std::size_t g = 0; g < q->goals.size(); ++g) {
                if (q->goals[g].hidden) continue;
                bool met = false;
                const std::string line = GoalLine(c_, *q, g, s, met);
                if (shown >= r.goals.size() && ui_ != nullptr) {
                    assets::UiLayoutNode n;
                    n.type = "CIFStatic";
                    n.rect = {r.text != nullptr ? r.text->rect.x + 8.0f : 60.0f, 0.0f, 260.0f, 16.0f};
                    n.hAlign = 0;
                    r.goals.push_back(Add(ui_->Make(n)));
                }
                if (shown >= r.goals.size()) break;
                ui::Widget* w = r.goals[shown++];
                w->rect.y = y;
                w->visible = true;
                w->SetText(std::format("[{}] {}", c_.Text(met ? "UIIT_STT_QUEST_END" : "UIIT_STT_QUEST_ING"), line));
                if (auto* st = ui::As<ui::Static>(w)) st->textColor = met ? kGoalDone : kGoalOn;
                y += 16.0f;
            }
        }
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::vector<Row> rows_;
    float slotHeight_ = 30.0f;
    std::set<std::uint32_t> seen_;
    std::map<std::uint32_t, bool> folded_;
};

// The window's own node from its layout's root record (its frame and art), as `type` with `id`; none before the
// layout is read.
std::optional<assets::UiLayoutNode> RootNode(const HudContext& c, const char* layout, const char* type, int id) {
    const auto* records = c.layout2dt ? c.layout2dt(layout) : nullptr;
    if (records == nullptr || records->empty()) return std::nullopt;
    assets::UiLayoutNode node = assets::Ui2dtNode(records->front(), 0.0f, 0.0f);
    node.type = type;
    node.id = id;
    node.rect = records->front().rect;
    return node;
}

void OpenRewardConfirm(HudContext& c, std::uint32_t quest, std::uint32_t npc, const std::vector<std::uint8_t>& picked) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    ui::Widget* w = ui->Window(hud::kQuestRewardConfirm);
    if (w == nullptr) {
        const auto node = RootNode(c, kConfirmLayout, "CNIFQuestReward_ConfirmWnd", hud::kQuestRewardConfirm);
        if (!node) return;
        w = ui->Open(*node);
    } else {
        c.hud->Show(hud::kQuestRewardConfirm);
    }
    if (auto* confirm = dynamic_cast<QuestRewardConfirm*>(w)) confirm->Show(quest, npc, picked);
}

} // namespace

std::vector<ui::Widget*> Build2dt(ui::Interface& ui, ui::Widget& root, const std::vector<assets::Ui2dtRecord>& records,
                                  const std::function<void(const assets::Ui2dtRecord&, assets::UiLayoutNode&)>& adjust) {
    return Build(ui, root, records, adjust);
}
std::optional<assets::UiLayoutNode> RootNode2dt(const HudContext& c, const char* layout, const char* type, int id) { return RootNode(c, layout, type, id); }

namespace {

// The level-up's quest guide. The side icon CIFQuestInfoGuide (ginterface 46, the side icons' 0x2E):
// its two pictures in turn (icon\etc\qstinfoic_1.ddj, _2); a click opens the window and puts the icon
// away.
constexpr int kGuideIcon = 46, kGuideWindow = 45;

class QuestInfoGuide : public ui::Static {
public:
    QuestInfoGuide(const assets::UiLayoutNode& node, HudContext& c) : Static(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        Static::Created(ui);
        interactive = true;
    }
    void Update(ui::Frame&) override {
        texture = int(c_.time * 2.0) % 2 != 0 ? "icon\\etc\\qstinfoic_2.ddj" : "icon\\etc\\qstinfoic_1.ddj";
    }
    void Mouse(ui::Frame& frame) override {
        if (!frame.input.Released(ui::MouseButton::Left) || frame.pointer.hovered != this || c_.hud == nullptr) return;
        c_.hud->Show(kGuideWindow);
        c_.hud->Hide(kGuideIcon);
    }

private:
    HudContext& c_;
};

// CIFQuestInfo (ginterface 45, 315 x 142): titled UIIT_STT_QUEST_REPORT, resinfo\ifquestinfo.txt's scroll
// manager (9, 37, 298 x 96) holding one row of resinfo\ifquestinfoslot.txt at a time (280 x 96, qst_info.ddj: the name id 2,
// "NPC" id 3 and its line id 4, "Condition" id 7 and its line id 8), its bar through the rows.
class QuestInfoWindow : public ui::MainFrame {
public:
    QuestInfoWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}
    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        for (const auto& child : children()) {
            if (child->id() == 0 && child->node().type == "CIFStatic") child->SetText(c_.Text("UIIT_STT_QUEST_REPORT"));
        }
        Section(ui, *this, "resinfo\\ifquestinfo.txt");
        if (ui::Widget* list = Child(1)) {
            assets::UiLayoutNode n;
            n.type = "CIFStatic";
            n.id = 100;
            n.rect = {0.0f, 0.0f, 280.0f, 96.0f};
            auto made = ui.Make(n);
            row_ = made.get();
            list->Add(std::move(made));
            Section(ui, *row_, "resinfo\\ifquestinfoslot.txt");
        }
        Fill();
    }
    void Fill() {
        const auto& rows = c_.questGuide;
        if (ui::Widget* list = Child(1)) {
            if (auto* bar = ui::As<ui::ScrollBar>(list->Child(10))) bar->SetRange(0, std::max(0, int(rows.size()) - 1));
        }
        first_ = std::min(first_, std::max(0, int(rows.size()) - 1));
        if (row_ == nullptr) return;
        row_->visible = !rows.empty();
        if (rows.empty()) return;
        const auto& r = rows[std::size_t(first_)];
        Put(2, c_.Text(r.name));
        Put(4, c_.Text(r.npc));
        Put(8, c_.Text(r.condition));
    }
    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::ScrollChanged && n.source != nullptr && n.source->id() == 10) {
            first_ = std::max(0, n.value);
            Fill();
            return true;
        }
        return MainFrame::OnNotice(n);
    }

private:
    static void Section(ui::Interface& ui, ui::Widget& to, const char* layout) {
        const assets::UiLayout* l = ui.Layout(layout);
        if (l == nullptr) return;
        for (const auto& s : l->sections) {
            if (s.name != "Create") continue;
            for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) to.Add(ui.Make(*it));
        }
    }
    void Put(int id, const std::string& text) {
        if (ui::Widget* w = row_ != nullptr ? row_->Child(id) : nullptr) w->SetText(text);
    }

    HudContext& c_;
    ui::Widget* row_ = nullptr;
    int first_ = 0;
};

} // namespace

void ShowQuestGuide(HudContext& c, std::vector<proto::QuestGuide::Row> rows) {
    c.questGuide = std::move(rows);
    if (c.hud == nullptr) return;
    if (c.questGuide.empty()) {
        c.hud->Hide(kGuideIcon);
        return;
    }
    c.hud->Show(kGuideIcon);
    if (ui::Interface* ui = c.hud->interface()) {
        if (auto* w = dynamic_cast<QuestInfoWindow*>(ui->Window(kGuideWindow))) w->Fill();
    }
}

void RegisterQuestWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFQuestInfoGuide", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<QuestInfoGuide>(n, *c); });
    ui.Register("CIFQuestInfo", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<QuestInfoWindow>(n, *c); });
    ui.Register("CNIFQuest", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<QuestListWindow>(n, *c); });
    ui.Register("CNIFQuestPage", [](const assets::UiLayoutNode& n) { return std::make_unique<QuestPage>(n); });
    ui.Register("CNIFQuestReward", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<QuestRewardWindow>(n, *c); });
    ui.Register("CNIFQuestReward_ConfirmWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<QuestRewardConfirm>(n, *c); });
    ui.Register("CNIFMiniQuestWnd", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<QuestQuickWindow>(n, *c); });
}

void ToggleQuestWindow(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    if (c.hud->Shown(hud::kQuestList)) {
        c.hud->Hide(hud::kQuestList);
        return;
    }
    if (const auto node = RootNode(c, kListLayout, "CNIFQuest", hud::kQuestList)) ui->Open(*node);
}

void OpenQuestReward(HudContext& c, std::uint32_t quest, std::uint32_t npc) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return;
    ui::Widget* w = ui->Window(hud::kQuestReward);
    if (w == nullptr) {
        const auto node = RootNode(c, kRewardLayout, "CNIFQuestReward", hud::kQuestReward);
        if (!node) return;
        w = ui->Open(*node);
    } else {
        c.hud->Show(hud::kQuestReward);
    }
    if (auto* reward = dynamic_cast<QuestRewardWindow*>(w)) reward->Show(quest, npc);
}

void OpenQuestRewardConfirm(HudContext& c, std::uint32_t quest, std::uint32_t npc, const std::vector<std::uint8_t>& picked) {
    OpenRewardConfirm(c, quest, npc, picked);
}

bool PickQuestReward(HudContext& c, const std::string& item) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    auto* w = ui != nullptr ? dynamic_cast<QuestRewardWindow*>(ui->Window(hud::kQuestReward)) : nullptr;
    return w != nullptr && w->visible && w->PickAndTake(item);
}

void ShowQuestQuick(HudContext& c) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr || ui->Window(hud::kQuestQuick) != nullptr) return;
    // 324 from the right edge, under the minimap, at the layout's y (267).
    auto node = RootNode(c, kMiniLayout, "CNIFMiniQuestWnd", hud::kQuestQuick);
    if (!node) return;
    node->rect.x = c.hud->Screen().x - 324.0f;
    ui->Open(*node);
}

} // namespace sr::client
