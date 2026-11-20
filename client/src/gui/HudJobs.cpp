// The jobs' leagues on the client: a league keeper's rows in the NPC window, the join and
// leave boxes, the alias window (1.188's CIFJobAlias, ifjobalias.txt), the previous job information (CIFPrevJobInfo,
// ifprevjobinfo.txt), the answers' system lines, and the character window's job rows' texts.

#include <algorithm>
#include <cmath>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/JobProtocol.h"
#include "sr/game/Jobs.h"
#include "sr/game/PlayerState.h"
#include "sr/ui/Controls.h"
#include "sr/ui/Interface.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr int kAliasWindow = 63, kPreviousWindow = 66; // ginterface.txt's ids (GDR_JOB_ALIAS, GDR_PREV_JOB_INFO)

// The text keys' job words (trader MERCHANT).
const char* JobWord(int type) { return type == 1 ? "MERCHANT" : type == 2 ? "THIEF" : type == 3 ? "HUNTER" : ""; }

// "<sml2><center>a<br>b</center></sml2>" into its lines, the tags gone (the confirmation box's paragraphs).
std::vector<std::string> PlainLines(const std::string& text) {
    std::vector<std::string> out(1);
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '<') {
            out.back() += text[i++];
            continue;
        }
        const std::size_t end = text.find('>', i);
        if (end == std::string::npos) break;
        if (text.compare(i, end - i + 1, "<br>") == 0) out.emplace_back();
        i = end + 1;
    }
    return out;
}

// The job errors the client has a text for (its error table, group 0x18); the
// others (0x480F, 0x4828, ours 0xFFFF) print nothing.
const char* JobErrorKey(proto::JobCode code) {
    switch (code) {
    case proto::JobCode::InvalidTarget: return "UIIT_STT_ERR_COMMON_INVALID_TARGET";
    case proto::JobCode::NotEnoughGold: return "UIIT_MSG_INTERACTION_FAIL_NOT_ENOUGH_MONEY";
    case proto::JobCode::LevelTooLow: return "UIIT_MSG_JOBGUILD_JOIN_ERR_LEVEL";
    case proto::JobCode::InALeague: return "UIIT_MSG_JOBGUILD_JOIN_ERR_OVERLAP";
    case proto::JobCode::SuitWorn: return "UIIT_CTL_JOBGUILD_WITHD_ERR_WEARITEM";
    case proto::JobCode::NameNotAllowed: return "UIIT_MSG_ALIAS_CREATE_ERR_NOTRULE";
    case proto::JobCode::RejoinWait: return "UIIT_MSG_JOBGUILD_JOIN_ERR_EXPIRE";
    case proto::JobCode::NameIsCharacter:
    case proto::JobCode::NameIsAlias: return "UIIT_MSG_ALIAS_CREATE_ERR_NAMEDULPLICATE";
    case proto::JobCode::NoPrevious: return "UIIT_MSG_JOBINFO_OLD_NOTEXIST";
    case proto::JobCode::QuestUnderWay: return "UIIT_MSG_JOBGUILD_WITHD_ERR_QUEST";
    default: return nullptr;
    }
}

void PostLine(HudContext& c, game::GameMessage::Kind kind, const std::string& text) {
    if (c.state != nullptr && !text.empty()) c.state->Post(kind, text, c.time);
}

void Put(ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->text() != text) w->SetText(text);
}

// Its parts: the name box, Check, OK, Cancel; in modify mode the cost (level x 50,000).
// OK is enabled only once the name in the box was checked free; an edit forgets the check.
class AliasWindow : public ui::MainFrame {
public:
    AliasWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
        if (auto* edit = ui::As<ui::Edit>(Child(kEdit))) edit->limit = std::size_t(game::kAliasMaxBytes);
    }

    // Opened for `npc`: create (no alias yet) or modify.
    void Open(std::uint32_t npc, bool modify) {
        npc_ = npc;
        modify_ = modify;
        checked_.clear();
        if (ui_ != nullptr && modify && Child(kCost) == nullptr) {
            if (const assets::UiLayout* l = ui_->Layout("resinfo\\ifjobalias.txt")) {
                for (const auto& s : l->sections) {
                    if (s.name != "AliasModify") continue;
                    for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) Add(ui_->Make(*it));
                }
            }
        }
        for (int id : {40, 41, 42, kCost}) {
            if (ui::Widget* w = Child(id)) w->visible = modify;
        }
        // The window 297 x 165 to create, 297 x 180 to modify; its ground 265 x 109 or 124; the buttons below it.
        rect.h = modify ? 180.0f : 165.0f;
        if (ui::Widget* ground = Child(5)) ground->rect.h = modify ? 124.0f : 109.0f;
        for (int id : {kOk, kCancel}) {
            if (ui::Widget* b = Child(id)) b->rect.y = modify ? 128.0f : 113.0f;
        }
        Put(Child(0), c_.Text(modify ? "UIIT_PAG_ALIAS_MODIFY" : "UIIT_PAG_ALIAS_CREATE"));
        if (modify && c_.state != nullptr) Put(Child(kCost), GoldText(game::AliasChangeFee(c_.state->self.level)));
        if (auto* edit = ui::As<ui::Edit>(Child(kEdit))) {
            edit->SetValue({});
            if (ui_ != nullptr) ui_->Focus(edit);
        }
        Checked(false);
    }

    void Update(ui::Frame& frame) override {
        MainFrame::Update(frame);
        // A name other than the one checked: the check forgotten.
        if (!checked_.empty() && Name() != checked_) {
            checked_.clear();
            Checked(false);
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.source->parent() == this) {
            switch (n.control) {
            case kCheck: Check(); return true;
            case kOk: Ok(); return true;
            case kCancel: visible = false; return true;
            default: break;
            }
        }
        return MainFrame::OnNotice(n);
    }

    // Check: a name outside 2..12 bytes does nothing; one the name rules refuse is told (0x4823), not sent.
    void Check() {
        const std::string name = Name();
        if (name == checked_ && !checked_.empty()) return;
        if (!game::AliasLengthOk(name)) return;
        if (c_.words != nullptr && c_.words->Check(name) != game::NameCheck::Ok) {
            PostLine(c_, game::GameMessage::Kind::General, c_.Text("UIIT_MSG_ALIAS_CREATE_ERR_NOTRULE"));
            return;
        }
        asked_ = name;
        if (c_.jobRequest) c_.jobRequest(proto::JobAlias::kId, proto::Encode(proto::JobAlias{npc_, proto::kAliasCheck, name}));
    }

    // The check's answer: free (Check off, OK on).
    void CheckedFree() {
        if (!visible || asked_ != Name()) return;
        checked_ = asked_;
        Checked(true);
    }

    // OK: the confirmation, then the alias set and the window closed.
    void Ok() {
        if (checked_.empty() || c_.hud == nullptr || c_.state == nullptr) return;
        const std::string name = checked_;
        const std::string question =
            modify_ ? FormatText(c_.Text("UIIT_STT_ALIAS_MODIFY_WINDOW"), {c_.state->jobAlias, name, GoldText(game::AliasChangeFee(c_.state->self.level))})
                    : FormatText(c_.Text("UIIT_STT_ALIAS_CREATE_WINDOW"), {c_.state->self.name, name});
        HudContext* ctx = &c_;
        const std::uint32_t npc = npc_;
        c_.hud->Confirm("UIIT_STT_CONFIRM_BOX", PlainLines(question), [this, ctx, npc, name](bool yes) {
            if (!yes) return;
            visible = false;
            if (ctx->jobRequest) ctx->jobRequest(proto::JobAlias::kId, proto::Encode(proto::JobAlias{npc, proto::kAliasSet, name}));
        });
    }

    // The test options' way: the name typed and checked.
    void Type(const std::string& name) {
        if (auto* edit = ui::As<ui::Edit>(Child(kEdit))) edit->SetValue(name);
        Check();
    }
    bool CanConfirm() const { return !checked_.empty(); }

private:
    static constexpr int kEdit = 13, kCheck = 20, kOk = 30, kCancel = 31, kCost = 43;

    std::string Name() const {
        const ui::Widget* w = const_cast<AliasWindow*>(this)->Child(kEdit);
        return w != nullptr ? w->text() : std::string();
    }
    void Checked(bool free) {
        if (ui::Widget* b = Child(kCheck)) b->enabled = !free;
        if (ui::Widget* b = Child(kOk)) b->enabled = free;
    }

    HudContext& c_;
    ui::Interface* ui_ = nullptr;
    std::uint32_t npc_ = 0;
    bool modify_ = false;
    std::string asked_, checked_;
};

// the three old jobs, trader, hunter, thief from the top; OK closes it.
class PreviousWindow : public ui::MainFrame {
public:
    PreviousWindow(const assets::UiLayoutNode& node, HudContext& c) : MainFrame(node), c_(c) {}

    void Show(const proto::JobPrevious& m) {
        const int race = c_.state != nullptr ? c_.state->self.race : 0;
        const struct {
            int type, level;
            std::uint32_t exp;
        } rows[] = {{1, m.traderLevel, m.traderExp}, {3, m.hunterLevel, m.hunterExp}, {2, m.thiefLevel, m.thiefExp}};
        for (int r = 0; r < 3; ++r) {
            const auto& row = rows[r];
            float fill = 0.0f;
            Put(Child(25 + r), FormatText("%d %s", {std::to_string(row.level), c_.Text("UIIT_STT_GRADE")}));
            Put(Child(30 + r), JobClassName(c_, row.type, row.level, race));
            Put(Child(50 + r), JobExpLine(c_, row.type, row.level, row.exp, fill));
            if (auto* g = ui::As<ui::Gauge>(Child(45 + r))) g->SetValue(fill);
        }
    }

    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::Clicked && n.source != nullptr && n.source->parent() == this && n.control == 60) {
            visible = false;
            return true;
        }
        return MainFrame::OnNotice(n);
    }

private:
    HudContext& c_;
};

AliasWindow* OpenAlias(HudContext& c) { return c.hud != nullptr ? dynamic_cast<AliasWindow*>(c.hud->Show(kAliasWindow)) : nullptr; }
AliasWindow* ShownAlias(HudContext& c) {
    return c.hud != nullptr && c.hud->Shown(kAliasWindow) ? dynamic_cast<AliasWindow*>(c.hud->Show(kAliasWindow)) : nullptr;
}

} // namespace

std::string JobClassName(const HudContext& c, int type, int level, int race) {
    if (type < 1 || type > 3) return {};
    return c.Text(std::format("UIIT_STT_CLASS_{}{}_{}", race == 1 ? "EU_" : "", JobWord(type), std::clamp(level, 1, game::kJobMaxLevel)));
}

std::string JobExpLine(const HudContext& c, int type, int level, std::uint64_t exp, float& fill) {
    // The job level's own requirement (leveldata.txt's JobExp column, _RefLevel's): "%d%% (%d)", the percent cut;
    // -1 (past the table) a full gauge and "100%".
    std::int64_t need = -1;
    if (c.jobExp != nullptr && type >= 1 && type <= 3 && level >= 1 && std::size_t(level) < c.jobExp->size()) need = (*c.jobExp)[std::size_t(level)][std::size_t(type - 1)];
    if (need <= 0) {
        fill = 1.0f;
        return "100%";
    }
    const double part = double(exp) / double(need);
    fill = float(std::clamp(part, 0.0, 1.0));
    return FormatText("%d%% (%d)", {std::to_string(std::int64_t(std::trunc(part * 100.0))), std::to_string(exp)});
}

std::vector<JobNpcRowInfo> JobNpcRows(const HudContext& c, std::uint32_t services) {
    // The rows by the player's league, never its suit, level or gold.
    std::vector<JobNpcRowInfo> rows;
    const int keeper = (services & proto::kNpcJobTrader) != 0 ? 1 : (services & proto::kNpcJobThief) != 0 ? 2 : (services & proto::kNpcJobHunter) != 0 ? 3 : 0;
    if (keeper == 0 || c.state == nullptr || !c.text) return rows;
    const int league = int(c.state->job.type);
    const std::string menu = keeper == 1 ? "UIIT_STT_NPC_CHATTING_TRADERMENU_" : keeper == 2 ? "UIIT_STT_NPC_CHATTING_THIEFMENU_" : "UIIT_STT_NPC_CHATTING_HUNTERMENU_";
    const auto add = [&](const std::string& key, JobNpcRow row) { rows.push_back({c.text(key), row}); };
    if (league == 0) {
        add(menu + "JOIN", JobNpcRow::Join);
    } else if (league == keeper) {
        add(menu + "WITHD", JobNpcRow::Leave);
        if (c.state->jobAlias.empty()) add(menu + "ALIASCREATE", JobNpcRow::AliasCreate);
        else add(menu + "ALIASMODIFY", JobNpcRow::AliasModify);
        if (keeper != 1) add("UIIT_STT_NPC_CHATTING_HUNTERMENU_OUTCOME", JobNpcRow::Outcome); // the thief's keeper takes the hunter's words
    }
    if (keeper == 1) add("UIIT_STT_NPC_CHATTING_TRADERMENU_DONATIONRANK", JobNpcRow::DonationRank);
    else add("UIIT_STT_NPC_CHATTING_HUNTERMENU_CONTRIBUTERANK", JobNpcRow::ContributionRank);
    add(menu + "JOBRANK", JobNpcRow::ActivityRank);
    add("UIIT_STT_NPC_CHATTING_JOBINFO_OLD", JobNpcRow::Previous);
    return rows;
}

void JobNpcChoice(ui::Interface& ui, HudContext& c, JobNpcRow row, std::uint32_t npc, std::uint32_t services) {
    (void)ui;
    if (c.hud == nullptr || c.state == nullptr) return;
    HudContext* ctx = &c;
    const std::uint8_t keeper = (services & proto::kNpcJobTrader) != 0 ? 1 : (services & proto::kNpcJobThief) != 0 ? 2 : 3;
    switch (row) {
    case JobNpcRow::Join:
        // Asked first (box kind 4: UIIT_STT_JOBGUILD_JOIN_WINDOW), then 0x70E1 with the NPC's league.
        if (c.state->job.type != game::JobType::None) return;
        c.hud->Confirm("UIIT_STT_CONFIRM_BOX", PlainLines(c.Text("UIIT_STT_JOBGUILD_JOIN_WINDOW")), [ctx, npc, keeper](bool yes) {
            if (yes && ctx->jobRequest) ctx->jobRequest(proto::JobJoin::kId, proto::Encode(proto::JobJoin{npc, keeper}));
        });
        return;
    case JobNpcRow::Leave:
        if (c.state->job.type == game::JobType::None) return;
        c.hud->Confirm("UIIT_STT_CONFIRM_BOX", PlainLines(c.Text("UIIT_STT_JOBGUILD_WITHD_WINDOW")), [ctx, npc](bool yes) {
            if (yes && ctx->jobRequest) ctx->jobRequest(proto::JobLeave::kId, proto::Encode(proto::JobLeave{npc}));
        });
        return;
    case JobNpcRow::AliasCreate:
    case JobNpcRow::AliasModify:
        if (AliasWindow* w = OpenAlias(c)) w->Open(npc, row == JobNpcRow::AliasModify);
        return;
    case JobNpcRow::Previous:
        if (c.jobRequest) c.jobRequest(proto::JobPreviousQuery::kId, proto::Encode(proto::JobPreviousQuery{npc}));
        return;
    case JobNpcRow::TradeBoard: OpenTradeBoard(c); return;
    case JobNpcRow::ActivityRank:
    case JobNpcRow::DonationRank:
    case JobNpcRow::ContributionRank: {
        // The NPC's league's ranking: from the cache when it holds rows, else asked (0x70E4).
        const std::uint8_t kind = row == JobNpcRow::ActivityRank ? 0 : 1;
        if (const auto it = c.jobRanks.find(keeper * 2 + kind); it != c.jobRanks.end() && !it->second.rows.empty()) {
            JobRankingArrived(c, it->second);
            return;
        }
        if (c.jobRequest) c.jobRequest(proto::JobRankQuery::kId, proto::Encode(proto::JobRankQuery{npc, keeper, kind}));
        return;
    }
    case JobNpcRow::Outcome:
    case JobNpcRow::OutcomeCollect:
        // Asked (0x70E5 action 0), then collected (action 1) from the talk's new row.
        if (c.jobRequest) c.jobRequest(proto::JobOutcome::kId, proto::Encode(proto::JobOutcome{npc, std::uint8_t(row == JobNpcRow::OutcomeCollect ? 1 : 0)}));
        return;
    default: return;
    }
}

namespace {

constexpr int kRankWindow = 64, kContributionWindow = 65; // ginterface JobActiveRank, JobContributionRank

// The digits in groups of three ("12,345"), as the client's.
std::string Grouped(std::uint64_t n) {
    const std::string digits = std::to_string(n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// CIFJobRank (64) and CIFJobContributionRank (65): last week's ranking of a league, ten rows a page (slots 20..29 /
// 30..39), the spinner (40 / 50) paging the
// cache; the contribution window's own entry (10..16) or its "none" line (60). Centred when opened; nothing hidden at
// Create.
class RankWindow : public ui::MainFrame {
public:
    RankWindow(const assets::UiLayoutNode& node, HudContext& c, bool contribution) : MainFrame(node), c_(c), contribution_(contribution) {}
    void Created(ui::Interface& ui) override {
        MainFrame::Created(ui);
        ui_ = &ui;
    }
    bool OnNotice(const ui::Notice& n) override {
        if (n.kind == ui::Notice::Kind::ValueChanged && n.control == SpinId()) {
            Fill();
            return true;
        }
        return MainFrame::OnNotice(n);
    }
    bool Page(int page) {
        auto* spin = ui::As<ui::Spin>(Child(SpinId()));
        if (spin == nullptr) return false;
        spin->value = std::clamp(page, 0, spin->max);
        Fill();
        return true;
    }
    void Set(int type) {
        type_ = type;
        static constexpr const char* kWords[4] = {"", "TRADER", "THIEF", "HUNTER"};
        const char* word = kWords[std::clamp(type, 0, 3)];
        if (contribution_) {
            if (ui::Widget* t = Child(0)) t->SetText(c_.Text(type == 1 ? "UIIT_STT_JOBGUILD_TRADER_MENU_DONATIONRANK" : "UIIT_STT_JOBGUILD_HUNTER_MENU_CONTRIBUTERANK"));
            if (ui::Widget* w = Child(20)) w->SetText(c_.Text(type == 1 ? "UIIT_STT_JOBGUILD_CONTRIBUTERANK" : "UIIT_STT_JOBGUILD_CONTRIBUTERANK2"));
            if (ui::Widget* w = Child(28)) w->SetText(c_.Text(type == 1 ? "UIIT_STT_DONATION" : "UIIT_STT_CONTRIBUTE"));
            Own();
        } else if (ui::Widget* t = Child(0)) {
            t->SetText(c_.Text(std::format("UIIT_STT_JOBGUILD_{}_MENU_JOBRANK", word)));
        }
        const auto it = c_.jobRanks.find(type * 2 + (contribution_ ? 1 : 0));
        const std::size_t n = it != c_.jobRanks.end() ? it->second.rows.size() : 0;
        if (auto* spin = ui::As<ui::Spin>(Child(SpinId()))) {
            spin->max = std::max(0, int((n + 9) / 10) - 1);
            spin->value = 0;
        }
        Fill();
        if (ui_ != nullptr && c_.hud != nullptr) ui_->Centre(*this, c_.hud->Screen());
    }

private:
    int SpinId() const { return contribution_ ? 50 : 40; }
    // The own entry: in this league, with an alias and job exp; else the "does not exist" line.
    void Own() {
        const game::PlayerState* s = c_.state;
        const bool shown = s != nullptr && int(s->job.type) == type_ && !s->jobAlias.empty() && s->job.exp != 0;
        for (int id = 10; id <= 16; ++id) {
            if (ui::Widget* w = Child(id)) w->visible = shown;
        }
        if (ui::Widget* w = Child(60)) w->visible = !shown;
        if (!shown) return;
        if (ui::Widget* w = Child(10)) w->SetText(FormatText(c_.Text(type_ == 1 ? "UIIT_STT_JOBGUILD_MYCONTRIBUTE" : "UIIT_STT_JOBGUILD_MYCONTRIBUTE2"), {s->jobAlias}));
        if (ui::Widget* w = Child(13)) w->SetText(c_.Text(type_ == 1 ? "UIIT_STT_DONATION" : "UIIT_STT_CONTRIBUTE"));
        if (ui::Widget* w = Child(14)) w->SetText(s->jobAlias);
        if (ui::Widget* w = Child(15)) w->SetText(std::to_string(s->job.level));
        if (ui::Widget* w = Child(16)) w->SetText(std::to_string(s->jobContributionAtEntry)); // plain, as of entering
    }
    void Fill() {
        const auto it = c_.jobRanks.find(type_ * 2 + (contribution_ ? 1 : 0));
        const auto* spin = ui::As<ui::Spin>(Child(SpinId()));
        const std::size_t first = std::size_t(spin != nullptr ? spin->value : 0) * 10;
        const int base = contribution_ ? 30 : 20;
        for (int i = 0; i < 10; ++i) {
            ui::Widget* slot = Child(base + i);
            if (slot == nullptr) continue;
            const std::size_t k = first + std::size_t(i);
            const game::proto::JobRankRecord* r = it != c_.jobRanks.end() && k < it->second.rows.size() ? &it->second.rows[k] : nullptr;
            const auto put = [&](int id, const std::string& text) {
                if (ui::Widget* w = slot->Child(id)) w->SetText(r != nullptr ? text : std::string());
            };
            if (r == nullptr) {
                for (int id = 10; id <= 14; ++id) put(id, {});
                continue;
            }
            put(10, std::to_string(r->rank));
            put(11, r->alias);
            put(12, std::to_string(r->level));
            if (contribution_) {
                put(13, Grouped(r->value));
                continue;
            }
            // The exp over its own level's requirement, rounded; a requirement of -1: 100.
            std::int64_t need = -1;
            if (c_.jobExp != nullptr && r->level >= 1 && std::size_t(r->level) < c_.jobExp->size()) need = (*c_.jobExp)[r->level][std::size_t(type_ - 1)];
            const long percent = need <= 0 ? 100 : std::lround(double(r->value) * 100.0 / double(need));
            put(13, FormatText("%d%%", {std::to_string(percent)}));
            put(14, JobClassName(c_, type_, r->level, r->country == 1 ? 1 : 0));
        }
    }

    HudContext& c_;
    bool contribution_ = false;
    ui::Interface* ui_ = nullptr;
    int type_ = 0;
};

} // namespace

void JobRankingArrived(HudContext& c, const proto::JobRanking& ranking) {
    if (ranking.type < 1 || ranking.type > 3 || c.hud == nullptr) return;
    c.jobRanks[ranking.type * 2 + ranking.kind] = ranking;
    const int id = ranking.kind == 0 ? kRankWindow : kContributionWindow;
    if (c.hud->Shown(id)) return; // an open window keeps its rows
    if (auto* w = dynamic_cast<RankWindow*>(c.hud->Show(id))) w->Set(ranking.type);
}

bool PageJobRanking(HudContext& c, int page) {
    if (c.hud == nullptr) return false;
    for (const int id : {kRankWindow, kContributionWindow}) {
        if (!c.hud->Shown(id)) continue;
        if (auto* w = dynamic_cast<RankWindow*>(c.hud->Show(id)); w != nullptr && w->Page(page)) return true;
    }
    return false;
}

void JobOutcomeAnswered(HudContext& c, const proto::JobOutcomeAnswer& a) {
    if (c.hud == nullptr || c.state == nullptr) return;
    if (a.action == 0) {
        // "You have worked hard for a week.\n[%s]will receive following reward.\nReward : [%s]gold" in the talk.
        auto* npc = c.hud->Shown(hud::kNpc) ? dynamic_cast<NpcWindow*>(c.hud->Show(hud::kNpc)) : nullptr;
        if (npc == nullptr || npc->npc() == 0) return;
        npc->ShowOutcome(FormatText(c.Text("UIIT_STT_OUTCOME_WINDOW"), {c.state->self.name, Grouped(a.amount)}), a.amount != 0);
        return;
    }
    // Collected: the talk closes, "A reward of [%s]gold has been received." as a Game line.
    if (ui::Widget* npc = c.hud->Shown(hud::kNpc) ? c.hud->Show(hud::kNpc) : nullptr) npc->visible = false;
    PostLine(c, game::GameMessage::Kind::Game, FormatText(c.Text("UIIT_MSG_OUTCOM_COMPLETE"), {Grouped(a.amount)}));
}

void JobAnswered(HudContext& c, const proto::JobAnswer& a) {
    // Every refusal is the error table's line in the system messages, never a box.
    if (a.code != proto::JobCode::Ok) {
        // The rankings' and the outcome's refusals other than the target are silent.
        if ((a.request == proto::JobRequest::Rank || a.request == proto::JobRequest::Outcome) && a.code != proto::JobCode::InvalidTarget) return;
        if (const char* key = JobErrorKey(a.code)) PostLine(c, game::GameMessage::Kind::General, c.Text(key));
        return;
    }
    switch (a.request) {
    case proto::JobRequest::Join: PostLine(c, game::GameMessage::Kind::Game, c.Text("UIIT_MSG_JOBGUILD_JOIN_COMPELET")); break;
    case proto::JobRequest::Leave: PostLine(c, game::GameMessage::Kind::Game, c.Text("UIIT_MSG_JOBGUILD_WITHD_COMPELET")); break;
    case proto::JobRequest::Alias:
        if (a.action == proto::kAliasCheck) {
            if (AliasWindow* w = ShownAlias(c)) w->CheckedFree(); // no text
        } else if (c.state != nullptr) {
            // Told before the new league comes: the alias held until now says which.
            PostLine(c, game::GameMessage::Kind::Game, c.Text(c.state->jobAlias.empty() ? "UIIT_MSG_ALIAS_CREATE_COMPLETE" : "UIIT_MSG_ALIAS_MODIFY_COMPLETE"));
        }
        break;
    default: break;
    }
}

void ShowPreviousJobs(HudContext& c, const proto::JobPrevious& m) {
    if (c.hud == nullptr) return;
    if (auto* w = dynamic_cast<PreviousWindow*>(c.hud->Show(kPreviousWindow))) w->Show(m);
}

bool TypeJobAlias(HudContext& c, const std::string& name) {
    AliasWindow* w = ShownAlias(c);
    if (w == nullptr) return false;
    w->Type(name);
    return true;
}

bool ConfirmJobAlias(HudContext& c) {
    AliasWindow* w = ShownAlias(c);
    if (w == nullptr || !w->CanConfirm()) return false;
    w->Ok();
    return true;
}

void RegisterJobWindows(ui::Interface& ui, HudContext& context) {
    ui.Register("CIFJobRank", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<RankWindow>(n, *c, false); });
    ui.Register("CIFJobContributionRank", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<RankWindow>(n, *c, true); });
    // A ranking's row: its bar (com_bar01_) under its texts.
    ui.Register("CIFJobRankSlot", [](const assets::UiLayoutNode& n) -> std::unique_ptr<ui::Widget> { return std::make_unique<ui::Bar>(n); });
    ui.Register("CIFJobContributionRankSlot", [](const assets::UiLayoutNode& n) -> std::unique_ptr<ui::Widget> { return std::make_unique<ui::Bar>(n); });
    ui.Register("CIFJobAlias", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<AliasWindow>(n, *c); });
    ui.Register("CIFPrevJobInfo", [c = &context](const assets::UiLayoutNode& n) { return std::make_unique<PreviousWindow>(n, *c); });
}

} // namespace sr::client
