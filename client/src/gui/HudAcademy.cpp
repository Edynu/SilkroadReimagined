// The Academy's windows over game::AcademyView:
// - the Academy page, the main window's L page, 1.188's CIFApprenticeShip from resinfo\ifapprenticeship.txt. The
//   guardian's row at the top, the two assistants' slots, then the five apprentices'; each row a face
//   (pt_no_face.ddj over it while its member is away), the name, "level(highest)", the title by its grade and its rank
//   mark (com_icon_level_N.ddj; the guardian's the academy's honour rank, com_honor_level_N.ddj), and the button
// gives it: on the player's own row Leave, or Graduate once it may (an apprentice at 40, an assistant at
//   60); on the others' rows for the guardian Ban, or Graduate past 40 / 60. The guardian's own button disbands. Invite
//   asks the selected player, Matching opens the list; the notice's subject below the slots (its contents over them
//   when clicked), the guardian's pencil to write it;
// - the judge window (CIFApprenticeShipJudge, ginterface 155): an apprentice's rating of its guardian as it graduates;
// - the honour ranking (CIFApprenticeShipHonorRanking, 154): the 50 places, ten a page;
// - the matching list (CIFMentorMatch, 153): the listed academies, the search, Join, Whisper, Register, Modify, Delete.

#include <algorithm>
#include <array>
#include <format>

#include "gui/Hud.h"
#include "gui/HudParty.h"
#include "gui/HudWindows.h"
#include "gui/PortraitTexture.h"
#include "sr/game/AcademyView.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

namespace academy = game::academy;

constexpr int kMatchWindow = 153, kRankingWindow = 154, kJudgeWindow = 155;
constexpr const char* kMatchLayout = "resinfo\\ifmentormatch.txt";

void Put(ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->text() != text) w->SetText(text);
}

void Show(ui::Widget* w, bool shown) {
    if (w != nullptr) w->visible = shown;
}

template <class T>
void Send(HudContext& c, const T& message) {
    if (c.academyRequest) c.academyRequest(T::kId, proto::Encode(message));
}

void Post(HudContext& c, const std::string& text) {
    if (c.state != nullptr) c.state->Post(game::GameMessage::Kind::General, text, c.time);
}

// The nodes of a layout's section added to a window (its first node in front, as the builder does).
void AddSection(ui::Interface& ui, ui::Widget& window, const std::string& layout, std::string_view section, bool hidden = false) {
    const assets::UiLayout* l = ui.Layout(layout);
    if (l == nullptr) return;
    for (const auto& s : l->sections) {
        if (s.name != section) continue;
        for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) {
            auto made = ui.Make(*it);
            if (hidden) made->visible = false;
            window.Add(std::move(made));
        }
    }
}

// --- The Academy page (CIFApprenticeShip) ---------------------------------------------------------------------------

// The ids of a row: the guardian's in the page itself, a member's in its CIFApprenticeShipSlot.
struct RowIds {
    int picture, status, mark, name, levelLabel, level, title, button;
};
constexpr RowIds kGuardianRow{41, 46, 15, 42, 43, 44, 45, 48};
constexpr RowIds kSlotRow{9, 20, 10, 11, 12, 13, 14, 18};
constexpr int kSlots = 7, kFirstSlot = 30; // 30, 31 the assistants' (pt_slot_co), 32..36 the apprentices' (pt_slot_re)
constexpr int kInvite = 20, kMatching = 22, kLeaderMark = 14, kNoticeSubject = 61, kNoticeEdit = 62, kNoticeContents = 140;
constexpr float kSubjectWidth = 260.0f; // the subject's room (0x104)

// What a row's button does.
enum class RowAction : std::uint8_t { None, Leave, Ban, Graduate, Disband };

// Ours: the portraits of the academy's members by their characters (frame/Portraits.h keys).
constexpr std::uint32_t kAcademyPortraitBase = 0xFE000000u;

class AcademyPage : public ui::Widget {
public:
    AcademyPage(const assets::UiLayoutNode& node, HudContext& context) : Widget(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui_ = &ui;
        // The notice's subject row (its section NotifySubBox) and its contents (NotifyContents, shown when the subject is
        // clicked, over the slots).
        AddSection(ui, *this, "resinfo\\ifapprenticeship.txt", "NotifySubBox");
        AddSection(ui, *this, "resinfo\\ifapprenticeship.txt", "NotifyContents", true);
        // The guardian's button: "Disband" (on child 48).
        Put(Child(kGuardianRow.button), context_.Text("UIIT_STT_PARTY_DISSOLVE"));
        // The contents' text (ours: CIFGuildNotifyContents' own controls are made in its code, not read).
        if (ui::Widget* contents = Child(kNoticeContents)) {
            assets::UiLayoutNode n;
            n.type = "CIFEdit";
            n.id = 1;
            n.rect = {12.0f, 10.0f, 340.0f, 180.0f};
            auto text = ui.Make(n);
            if (auto* e = ui::As<ui::Edit>(text.get())) {
                e->multiline = true;
                e->readOnly = true;
                e->limit = int(academy::kNoticeTextMax);
            }
            contents->Add(std::move(text));
        }
    }

    void Update(ui::Frame& frame) override {
        game::PlayerState* state = context_.state;
        if (state == nullptr) return;
        const game::AcademyView& a = state->academy;
        const std::string& self = state->self.name;
        const bool guardian = a.IsGuardian(self);

        const proto::AcademyMember* g = a.Guardian();
        Fill(*this, kGuardianRow, g, self, guardian);
        guardianAction_ = g != nullptr && guardian ? RowAction::Disband : RowAction::None;
        Show(Child(kGuardianRow.button), guardianAction_ != RowAction::None);
        Show(Child(kLeaderMark), g != nullptr);
        if (auto* mark = ui::As<ui::Static>(Child(kGuardianRow.mark))) {
            const std::uint8_t rank = a.Info().rank;
            mark->texture = a.InAcademy() && rank >= 1 && rank <= 4 ? std::format("interface\\ifcommon\\com_honor_level_{}.ddj", rank) : std::string();
            mark->visible = !mark->texture.empty();
        }

        const auto assistants = a.OfClass(academy::Class::Assistant);
        const auto apprentices = a.OfClass(academy::Class::Apprentice);
        for (int i = 0; i < kSlots; ++i) {
            ui::Widget* slot = Child(kFirstSlot + i);
            if (slot == nullptr) continue;
            const proto::AcademyMember* m = nullptr;
            if (i < academy::kMostAssistants) {
                if (std::size_t(i) < assistants.size()) m = assistants[std::size_t(i)];
            } else if (std::size_t(i - academy::kMostAssistants) < apprentices.size()) {
                m = apprentices[std::size_t(i - academy::kMostAssistants)];
            }
            slots_[std::size_t(i)] = m != nullptr ? m->name : std::string();
            classes_[std::size_t(i)] = m != nullptr ? m->memberClass : 0;
            actions_[std::size_t(i)] = Fill(*slot, kSlotRow, m, self, guardian);
            ui::Widget* button = slot->Child(kSlotRow.button);
            Show(button, actions_[std::size_t(i)] != RowAction::None);
            const RowAction act = actions_[std::size_t(i)];
            Put(button, act == RowAction::Graduate ? context_.Text("UIIT_CTL_TC_GRADUATION")
                        : act == RowAction::Ban    ? context_.Text("UIIT_CTL_BAN_PARTY")
                        : act == RowAction::Leave  ? context_.Text("UIIT_CTL_LEAVE_PARTY")
                                                   : std::string());
        }

        // The notice's subject, or that there is none; its pencil the guardian's.
        // 1.188 writes it into control 61 (the whole row, 4,306 358x28) cut to 260 pixels with "...",
        // centred in the row: between the "Notice" label and the pencil.
        const std::string& title = a.Info().noticeTitle;
        std::string subject = !a.InAcademy() ? std::string() : title.empty() ? context_.Text("UIIT_MSG_GUILD_COMMON_NOTEXIST") : title;
        if (ui::Widget* row = Child(kNoticeSubject)) {
            const int font = row->node().fontIndex;
            if (frame.paint.TextWidth(subject, font) > kSubjectWidth) {
                while (!subject.empty() && frame.paint.TextWidth(subject + "...", font) > kSubjectWidth) {
                    do subject.pop_back();
                    while (!subject.empty() && (static_cast<unsigned char>(subject.back()) & 0xC0) == 0x80);
                    if (!subject.empty() && static_cast<unsigned char>(subject.back()) >= 0xC0) subject.pop_back();
                }
                subject += "...";
            }
            const auto& area = row->node().rect;
            const float w = std::ceil(frame.paint.TextWidth(subject, font)), h = ui::TextLineHeight(font);
            row->rect = {std::trunc(area.x + (area.w - w) * 0.5f), std::trunc(area.y + (area.h - h) * 0.5f), w + 2.0f, h};
        }
        Put(Child(kNoticeSubject), subject);
        Show(Child(kNoticeEdit), guardian);
        if (ui::Widget* contents = Child(kNoticeContents)) {
            if (!a.InAcademy()) contents->visible = false;
            if (auto* e = ui::As<ui::Edit>(contents->Child(1)); e != nullptr && e->text() != a.Info().notice) e->SetValue(a.Info().notice);
        }
        if (ui::Widget* b = Child(kInvite)) b->enabled = guardian;
    }

    bool OnNotice(const ui::Notice& notice) override {
        if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || context_.hud == nullptr || context_.state == nullptr) return false;
        if (notice.source->parent() == this) {
            switch (notice.control) {
            case kInvite: {
                const game::TargetStatus& t = context_.state->target;
                if (t.kind == game::TargetStatus::Kind::Player && t.entity != 0 && t.entity != context_.state->self.entity) {
                    Send(context_, proto::AcademyInvite{t.entity});
                }
                return true;
            }
            case kMatching: context_.hud->Toggle(kMatchWindow); return true;
            case kNoticeEdit: OpenAcademyNoticeWrite(context_); return true;
            case kNoticeSubject:
                if (ui::Widget* contents = Child(kNoticeContents); contents != nullptr && context_.state->academy.InAcademy()) contents->visible = !contents->visible;
                return true;
            case 48:
                if (guardianAction_ == RowAction::Disband) Act(RowAction::Disband, {}, 0);
                return true;
            default: return false;
            }
        }
        if (notice.control != kSlotRow.button) return false;
        for (int i = 0; i < kSlots; ++i) {
            if (notice.source->parent() != Child(kFirstSlot + i)) continue;
            Act(actions_[std::size_t(i)], slots_[std::size_t(i)], classes_[std::size_t(i)]);
            return true;
        }
        return false;
    }

private:
    // The button's question, then its request (1.188's texts).
    void Act(RowAction action, const std::string& name, std::uint8_t memberClass) {
        Hud* hud = context_.hud;
        HudContext* c = &context_;
        const bool own = context_.state != nullptr && name == context_.state->self.name;
        switch (action) {
        case RowAction::Leave:
            hud->Confirm("UIIT_STT_CONFIRM_BOX", {context_.Text("UIIT_STT_TC_OUT_TRAININGCAMP")}, [c](bool yes) {
                if (yes) Send(*c, proto::AcademyLeave{});
            });
            return;
        case RowAction::Ban:
            hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(context_.Text("UIIT_STT_TC_BAN_TRAININGCAMP"), {name})}, [c, name](bool yes) {
                if (yes) Send(*c, proto::AcademyBan{name});
            });
            return;
        case RowAction::Disband:
            hud->Confirm("UIIT_STT_CONFIRM_BOX", {context_.Text("UIIT_STT_TC_DISSOLUTION")}, [c](bool yes) {
                if (yes) Send(*c, proto::AcademyDisband{});
            });
            return;
        case RowAction::Graduate:
            if (!own) {
                // The guardian graduates a member: "Apprentices will not be rewarded..." (UIIT_MSG_TC_GRADUATION_COMPULSION).
                hud->Confirm("UIIT_STT_CONFIRM_BOX", {FormatText(context_.Text("UIIT_MSG_TC_GRADUATION_COMPULSION"), {name})}, [c, name](bool yes) {
                    if (yes) Send(*c, proto::AcademyGraduateForce{name});
                });
            } else if (memberClass == std::uint8_t(academy::Class::Apprentice)) {
                hud->Show(kJudgeWindow); // the rating, then the graduation
            } else {
                hud->Confirm("UIIT_STT_CONFIRM_BOX", {context_.Text("UIIT_STT_TC_GRADUATION")}, [c](bool yes) {
                    if (yes) Send(*c, proto::AcademyGraduate{0});
                });
            }
            return;
        default: return;
        }
    }

    // A row filled; what its button does.
    RowAction Fill(ui::Widget& row, const RowIds& ids, const proto::AcademyMember* m, const std::string& self, bool guardian) {
        const bool own = m != nullptr && m->name == self;
        Put(row.Child(ids.name), m != nullptr ? m->name : std::string());
        Show(row.Child(ids.levelLabel), m != nullptr);
        Put(row.Child(ids.level), m != nullptr ? std::format("{}({})", m->level, m->maxLevel) : std::string());
        Put(row.Child(ids.title), m != nullptr ? context_.Text(game::AcademyGradeKey(m->grade)) : std::string());
        if (&row != this) {
            if (auto* mark = ui::As<ui::Static>(row.Child(ids.mark))) {
                mark->texture = m != nullptr && m->grade >= 1 && m->grade <= 7 ? std::format("interface\\ifcommon\\com_icon_level_{}.ddj", m->grade) : std::string();
                mark->visible = !mark->texture.empty();
            }
        }
        // The face under pt_face.ddj (by the member's model); away: pt_no_face.ddj over it.
        if (auto* face = ui::As<ui::PictureClip>(row.Child(ids.picture))) {
            face->mask = "interface\\party\\pt_face.ddj";
            face->texture = m == nullptr ? std::string() : PortraitTexture(own ? kSelfPortrait : kAcademyPortraitBase + m->character);
            face->visible = m != nullptr;
        }
        if (auto* status = ui::As<ui::Static>(row.Child(ids.status))) {
            status->texture = m != nullptr && m->online == 0 ? "interface\\party\\pt_no_face.ddj" : "";
            status->visible = !status->texture.empty();
        }
        if (m == nullptr || m->memberClass == std::uint8_t(academy::Class::Guardian)) return RowAction::None;
        const bool apprentice = m->memberClass == std::uint8_t(academy::Class::Apprentice);
        if (own) return m->maxLevel >= (apprentice ? academy::kApprenticeGraduates : academy::kAssistantGraduates) ? RowAction::Graduate : RowAction::Leave;
        if (guardian) return m->maxLevel > (apprentice ? academy::kApprenticeGraduates : academy::kAssistantGraduates) ? RowAction::Graduate : RowAction::Ban;
        return RowAction::None;
    }

    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::array<std::string, kSlots> slots_{};
    std::array<std::uint8_t, kSlots> classes_{};
    std::array<RowAction, kSlots> actions_{};
    RowAction guardianAction_ = RowAction::None;
};

// --- The judge window (CIFApprenticeShipJudge) --------------------------------------------------------------------

// Five ratings in a row (Very dissatisfied .. Very satisfied, 80 apart); Appraise sends the choice with
// the graduation, Cancel closes.
class JudgeWindow : public ui::MainFrame {
public:
    JudgeWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        if (auto* r = ui::As<ui::RadioButton>(Child(20))) {
            r->origin = {0.0f, 12.0f};
            r->spacing = 80.0f;
            r->labelOffset = 18.0f;
            r->labelWidth = 62.0f;
            r->SetUnits(ui, {context_.Text("UIIT_STT_TC_APPRAISAL_VERY_DESPAIR"), context_.Text("UIIT_STT_TC_APPRAISAL_DESPAIR"),
                             context_.Text("UIIT_STT_TC_APPRAISAL_NORMAL"), context_.Text("UIIT_STT_TC_APPRAISAL_SATISFACTION"),
                             context_.Text("UIIT_STT_TC_APPRAISAL_VERY_SATISFACTION")},
                        true);
            r->selected = 2; // ours: the middle to begin with
            // A label too long for its 62 pixels on two lines, as the control's 40 rows leave room for (ours: 1.188's own
            // drawing of them is not read).
            for (int i = 0; i < 5; ++i) {
                ui::Widget* label = r->Child(200 + i);
                if (label == nullptr) continue;
                const std::string text = label->text();
                const std::size_t space = text.find(' ');
                if (space == std::string::npos || text.size() <= 10) continue;
                label->SetText(text.substr(0, space));
                label->rect.y -= 6.0f;
                assets::UiLayoutNode n;
                n.type = "CIFStatic";
                n.id = 210 + i;
                n.rect = {label->rect.x, label->rect.y + 13.0f, label->rect.w, label->rect.h};
                auto rest = ui.Make(n);
                rest->SetText(text.substr(space + 1));
                r->Add(std::move(rest));
            }
        }
    }

    bool OnNotice(const ui::Notice& notice) override {
        const bool mine = notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this;
        if (!mine || (notice.control != 30 && notice.control != 31)) return ui::MainFrame::OnNotice(notice);
        if (notice.control == 30) {
            const auto* r = ui::As<ui::RadioButton>(Child(20));
            Send(context_, proto::AcademyGraduate{std::uint8_t(std::clamp(r != nullptr ? r->selected : 2, 0, 4))});
        }
        visible = false;
        return true;
    }

private:
    HudContext& context_;
};

// --- The honour ranking (CIFApprenticeShipHonorRanking) -----------------------------------------------------------

class RankingWindow : public ui::MainFrame {
public:
    RankingWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        for (const char* section : {"RankingListBtn", "RankingSlotList"}) AddSection(ui, *this, "resinfo\\ifapprenticeshiphonorranking.txt", section);
        if (ui::Widget* square = Child(1)) square->background = ui::Argb(0xFF000000);
    }

    void Update(ui::Frame&) override {
        if (context_.state == nullptr) return;
        const auto& rows = context_.state->academy.Ranking().rows;
        const int pages = std::max(1, int((rows.size() + academy::kRankingRows - 1) / academy::kRankingRows));
        int page = 0;
        if (auto* spin = ui::As<ui::Spin>(Child(4))) {
            spin->max = pages - 1;
            spin->value = std::clamp(spin->value, 0, pages - 1);
            page = spin->value;
        }
        for (int i = 0; i < academy::kRankingRows; ++i) {
            ui::Widget* slot = Child(30 + i);
            if (slot == nullptr) continue;
            const std::size_t at = std::size_t(page * academy::kRankingRows + i);
            const proto::AcademyRankRow* r = at < rows.size() ? &rows[at] : nullptr;
            Put(slot->Child(1), r != nullptr ? std::to_string(r->place) : std::string());
            if (auto* mark = ui::As<ui::Static>(slot->Child(2))) {
                mark->texture = r != nullptr && r->rank >= 1 && r->rank <= 4 ? std::format("interface\\ifcommon\\com_honor_level_{}.ddj", r->rank) : std::string();
                mark->visible = !mark->texture.empty();
            }
            Put(slot->Child(3), r != nullptr ? r->guardian : std::string());
            Put(slot->Child(4), r != nullptr ? std::to_string(r->level) : std::string());
            Put(slot->Child(5), r != nullptr ? std::to_string(r->graduates) : std::string());
            Put(slot->Child(6), r != nullptr ? r->guild : std::string());
        }
    }

    bool OnNotice(const ui::Notice& notice) override {
        if (notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this && notice.control == 3) {
            visible = false;
            return true;
        }
        return ui::MainFrame::OnNotice(notice);
    }

private:
    HudContext& context_;
};

// --- The matching list (CIFMentorMatch) ---------------------------------------------------------------------------

class MatchWindow : public ui::MainFrame {
public:
    MatchWindow(const assets::UiLayoutNode& node, HudContext& context) : MainFrame(node), context_(context) {}

    void Created(ui::Interface& ui) override {
        ui::MainFrame::Created(ui);
        ui_ = &ui;
        for (const char* section : {"SearchInfo", "SlotListButton", "SlotList"}) AddSection(ui, *this, kMatchLayout, section);
        if (ui::Widget* square = Child(6)) square->background = ui::Argb(0xFF000000);
        // The honour class: 1..5, then all (chosen); whom they take: apprentices, assistants, all.
        if (auto* combo = ui::As<ui::ComboBox>(Child(45))) {
            combo->items = {"1", "2", "3", "4", "5", context_.Text("UIIT_CTL_PARTYMATCH_PSEARCH_OBJECTALL")};
            combo->Select(5);
        }
        if (auto* combo = ui::As<ui::ComboBox>(Child(46))) {
            combo->items = {context_.Text("UIIT_STT_TC_STUDENT"), context_.Text("UIIT_STT_TC_ASSISTANT_GUARDIAN"),
                            context_.Text("UIIT_CTL_PARTYMATCH_PSEARCH_OBJECTALL")};
            combo->Select(2);
        }
        if (auto* e = ui::As<ui::Edit>(Child(47))) e->limit = 12;
        Show(Child(26), false); // twelve rows a page: no scrolling
    }

    void Update(ui::Frame&) override {
        game::PlayerState* state = context_.state;
        if (state == nullptr) return;
        const game::AcademyView& a = state->academy;
        // Opened: the first page.
        if (visible && !asked_) {
            asked_ = true;
            Ask(0);
        }
        if (!visible) asked_ = false;
        const proto::AcademyMatchList& list = a.Matches();
        if (auto* spin = ui::As<ui::Spin>(Child(25))) {
            spin->max = std::max(0, int(list.pages) - 1);
            if (seen_ != a.version) spin->value = list.page;
            else if (spin->value != page_) Ask(spin->value);
        }
        if (seen_ != a.version) {
            seen_ = a.version;
            page_ = list.page;
            if (selected_ >= int(list.rows.size())) selected_ = -1;
        }
        for (int i = 0; i < academy::kMatchRows; ++i) {
            ui::Widget* row = Child(70 + i);
            if (row == nullptr) continue;
            const proto::AcademyMatchRow* e = std::size_t(i) < list.rows.size() ? &list.rows[std::size_t(i)] : nullptr;
            for (int id = 10; id <= 18; ++id) Show(row->Child(id), e != nullptr);
            Show(row->Child(5), true);
            if (auto* bar = ui::As<ui::Bar>(row->Child(5))) bar->pieces = e != nullptr && i == selected_ ? "interface\\ifcommon\\com_bar01select_" : "";
            if (e == nullptr) continue;
            Put(row->Child(10), std::to_string(e->entry));
            Put(row->Child(11), std::to_string(e->level));
            Put(row->Child(12), context_.Text(e->race == 1 ? "UIIT_CTL_PARTYMATCH_AUTOMATCH_RACE_EU" : "UIIT_CTL_PARTYMATCH_AUTOMATCH_RACE_CH"));
            Put(row->Child(13), e->guardian);
            Put(row->Child(14), e->title);
            Put(row->Child(15), std::to_string(e->graduates));
            Put(row->Child(16), std::format("{}/{}", e->members, 1 + academy::kMostAssistants + academy::kMostApprentices));
            if (auto* mark = ui::As<ui::Static>(row->Child(17))) {
                mark->texture = e->rank >= 1 && e->rank <= 4 ? std::format("interface\\ifcommon\\com_honor_level_{}.ddj", e->rank) : std::string();
            }
            Put(row->Child(18), std::to_string(e->rank));
        }
        // Register while the guardian has no entry; Modify and Delete while it has; Join outside an academy.
        const bool guardian = a.IsGuardian(state->self.name);
        const bool listed = a.Info().matchEntry != 0;
        if (ui::Widget* b = Child(18)) b->enabled = guardian && !listed;
        if (ui::Widget* b = Child(19)) b->enabled = guardian && listed;
        if (ui::Widget* b = Child(20)) b->enabled = guardian && listed;
        if (ui::Widget* b = Child(15)) b->enabled = !a.InAcademy() && selected_ >= 0;
        if (ui::Widget* b = Child(16)) b->enabled = selected_ >= 0;
    }

    void Mouse(ui::Frame& frame) override {
        ui::MainFrame::Mouse(frame);
        if (!frame.input.Pressed(ui::MouseButton::Left)) return;
        for (int i = 0; i < academy::kMatchRows; ++i) {
            ui::Widget* row = Child(70 + i);
            if (row != nullptr && row->Hit(frame.pointer.position)) selected_ = i;
        }
    }

    bool OnNotice(const ui::Notice& notice) override {
        if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || notice.source->parent() != this || context_.state == nullptr ||
            ui_ == nullptr) {
            return ui::MainFrame::OnNotice(notice);
        }
        game::AcademyView& a = context_.state->academy;
        const auto& rows = a.Matches().rows;
        const proto::AcademyMatchRow* chosen = selected_ >= 0 && std::size_t(selected_) < rows.size() ? &rows[std::size_t(selected_)] : nullptr;
        switch (notice.control) {
        case 15: // Join: the guardian asked, the progress box until it answers
            if (chosen == nullptr) return true;
            a.joiningGuardian = chosen->guardian;
            a.joiningAt = context_.time;
            Send(context_, proto::AcademyMatchJoin{chosen->entry});
            return true;
        case 16:
            if (chosen != nullptr) {
                if (auto* chat = dynamic_cast<ChatWindow*>(ui_->Window(hud::kChat))) chat->Address(chosen->guardian, true);
            }
            return true;
        case 18: OpenAcademyMatchRegister(*ui_, context_, false); return true;
        case 19: OpenAcademyMatchRegister(*ui_, context_, true); return true;
        case 20: {
            HudContext* c = &context_;
            context_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {context_.Text("UIIT_STT_PARTYMATCH_DELETE_CONFIRM")}, [c](bool yes) {
                if (yes) Send(*c, proto::AcademyMatchDelete{});
            });
            return true;
        }
        case 55: Ask(0); return true;     // Search
        case 56: Ask(page_); return true; // Refresh
        default: return ui::MainFrame::OnNotice(notice);
        }
    }

private:
    void Ask(int page) {
        proto::AcademyMatchListAsk ask;
        ask.page = std::uint8_t(std::clamp(page, 0, 255));
        if (const auto* combo = ui::As<ui::ComboBox>(Child(45)); combo != nullptr && combo->selected >= 0 && combo->selected < 5) ask.honorClass = std::uint8_t(combo->selected + 1);
        if (const auto* combo = ui::As<ui::ComboBox>(Child(46)); combo != nullptr && combo->selected >= 0 && combo->selected < 2) ask.kind = std::uint8_t(combo->selected + 1);
        ask.name = Child(47) != nullptr ? Child(47)->text() : std::string();
        Send(context_, ask);
        page_ = page;
    }

    HudContext& context_;
    ui::Interface* ui_ = nullptr;
    std::uint64_t seen_ = ~0ull;
    int page_ = 0, selected_ = -1;
    bool asked_ = false;
};

// The notice's contents: a frame (its text ours, above).
class NoticeContents : public ui::FrameBorder {
public:
    using FrameBorder::FrameBorder;
};

} // namespace

std::uint32_t AcademyPortraitKey(std::uint32_t character) { return kAcademyPortraitBase + character; }

void RegisterAcademyWindows(ui::Interface& ui, HudContext& context) {
    HudContext* c = &context;
    ui.Register("CIFApprenticeShip", [c](const assets::UiLayoutNode& n) { return std::make_unique<AcademyPage>(n, *c); });
    ui.Register("CIFApprenticeShipJudge", [c](const assets::UiLayoutNode& n) { return std::make_unique<JudgeWindow>(n, *c); });
    ui.Register("CIFApprenticeShipHonorRanking", [c](const assets::UiLayoutNode& n) { return std::make_unique<RankingWindow>(n, *c); });
    ui.Register("CIFMentorMatch", [c](const assets::UiLayoutNode& n) { return std::make_unique<MatchWindow>(n, *c); });
    ui.Register("CIFGuildNotifyContents", [](const assets::UiLayoutNode& n) { return std::make_unique<NoticeContents>(n); });
    ui.Register("CIFApprenticeShipHonorSlot", [](const assets::UiLayoutNode& n) -> std::unique_ptr<ui::Widget> { return std::make_unique<ui::Bar>(n); });
}

void OpenAcademyRanking(HudContext& context) {
    if (context.hud != nullptr && !context.hud->Shown(kRankingWindow)) context.hud->Show(kRankingWindow);
}

std::string AcademyLineText(const std::function<std::string(const std::string&)>& text, const game::AcademyView::Line& line) {
    if (line.key.empty() || !text) return {};
    const auto arg = [&](std::size_t i) -> std::string {
        if (i >= line.args.size()) return {};
        const std::string& a = line.args[i];
        return a.starts_with('@') ? text(a.substr(1)) : a;
    };
    const std::string format = text(line.key);
    switch (line.args.size()) {
    case 0: return format;
    case 1: return FormatText(format, {arg(0)});
    default: return FormatText(format, {arg(0), arg(1)});
    }
}

} // namespace sr::client
