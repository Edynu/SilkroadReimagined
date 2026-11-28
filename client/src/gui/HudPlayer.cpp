// The player's bar and the buffs board.

#include <algorithm>
#include <cmath>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/Skills.h"
#include "sr/game/TimedServices.h"
#include "gui/PortraitTexture.h"

namespace sr::client {

namespace {

constexpr const char* kPmi = "interface\\playerminiinfo\\";

// The stat panel: its back, labels and values, shown and hidden together.
constexpr int kPanel[] = {21, 22, 23, 24, 25, 26, 27, 52, 53, 35, 36, 28, 29, 30, 31, 32, 33, 54, 55, 37, 38};

void Put(ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->text() != text) w->SetText(text);
}

// The abnormal states: bit, icon (icon\stateodd\s_<name>_icon.ddj) and the PARAM_<code> title.
struct StateKind {
    std::uint32_t bit;
    const char* icon;
    const char* code;
};
constexpr StateKind kStates[] = {
    {0x1, "freeze", "FZ"},           {0x2, "frostbite", "FB"},        {0x4, "electricshock", "ES"},   {0x8, "burn", "BU"},
    {0x10, "poisoning", "PS"},       {0x20, "zombi", "ZB"},           {0x40, "sleep", "SLEEP"},        {0x80, "root", "ROOT"},
    {0x100, "blunting", "SLOW"},     {0x200, "fear", "FEAR"},         {0x400, "myopia", "MYOPIA"},     {0x800, "bleeding", "BLOOD"},
    {0x1000, "stonecurse", "STONE"}, {0x2000, "dark", "DN"},          {0x4000, "stun", "STUN"},        {0x8000, "disease", "DISEASE"},  {0x10000, "confusion", "CHAOS"},
    {0x20000, "decay", "CURSE_PD"},  {0x40000, "weakness", "CURSE_MD"}, {0x80000, "powerless", "CURSE_STR"},
    {0x100000, "dissociation", "CURSE_INT"}, {0x200000, "panic", "CURSE_HP"}, {0x400000, "combustion", "CURSIE_MP"},
    {0x1000000, "incubation", "TIME_BOMB"},
};

const StateKind* StateOf(std::uint32_t bit) {
    for (const StateKind& s : kStates) {
        if (s.bit == bit) return &s;
    }
    return nullptr;
}

} // namespace

std::string StateIcon(std::uint32_t bit) {
    const StateKind* s = StateOf(bit);
    return s != nullptr ? std::string("icon\\stateodd\\s_") + s->icon + "_icon.ddj" : std::string();
}

std::string StateTitle(std::uint32_t bit) {
    const StateKind* s = StateOf(bit);
    return s != nullptr ? std::string("PARAM_") + s->code : std::string();
}

std::string EffectTooltip(const HudContext& context, const game::ActiveEffect& e) {
    // An academy experience buff: its title, "Remaining time: H hours M minutes", a received one's sender
    // (when a member), its points ("%s: %d %s").
    if (e.kind == game::ActiveEffect::Kind::TimedJob && (e.token & 0xFFFF0000u) == game::PlayerState::kAcademyToken && context.state != nullptr) {
        const game::AcademyView& a = context.state->academy;
        const std::uint32_t n = e.token & 0xFFFFu;
        const int left = std::max(0, int(std::ceil(e.started + e.duration - context.time)) / 60);
        std::string tip = context.Text(e.subject);
        tip += std::format("\n{}: {}{} {}{}", context.Text("UIIT_STT_REMAIN_TIME"), left / 60, context.Text("PARAM_HOUR"), left % 60,
                           context.Text("PARAM_MINUTE"));
        if (n == 0) {
            tip += std::format("\n{}: {} {}", context.Text("UIIT_CTL_TC_SAVE_BUFF_POINT"), a.Accumulated().points, context.Text("UIIT_STT_SILKMALL_P_POINT"));
        } else if (n - 1 < a.Reserves().size()) {
            const game::AcademyView::Reserve& r = a.Reserves()[n - 1];
            if (const std::string name = a.NameOf(r.donor); !name.empty()) tip += std::format("\n{}: {}", context.Text("UIIT_STT_LETTER_SENDER"), name);
            tip += std::format("\n{}: {} {}", context.Text("UIIT_CTL_TC_RESERVE_BUFF_POINT"), r.points, context.Text("UIIT_STT_SILKMALL_P_POINT"));
        }
        return tip;
    }
    // What it is, then the time left.
    std::string tip;
    if (e.kind == game::ActiveEffect::Kind::State) {
        tip = context.Text(StateTitle(e.id));
    } else if (context.effectName) {
        tip = context.effectName(e);
    }
    // A link the player made: whom it is on ("%s: %s").
    if (!e.target.empty()) tip += (tip.empty() ? "" : "\n") + std::format("{} : {}", context.Text("UIIT_STT_TARGETTING"), e.target);
    if (e.duration > 0.0) {
        const int left = std::max(0, int(std::ceil(e.started + e.duration - context.time)));
        const std::string remain = context.Text("UIIT_STT_REMAIN_TIME");
        std::string line;
        if (left < 60) {
            line = std::format("{} {}{}", remain, left, context.Text("PARAM_SECOND"));
        } else if (left < 3600) {
            line = std::format("{} {}{} {}{}", remain, left / 60, context.Text("PARAM_MINUTE"), left % 60, context.Text("PARAM_SECOND"));
        } else {
            line = std::format("{}: {}{} {}{} {}{}", remain, left / 86400, context.Text("PARAM_DAY"), left / 3600 % 24,
                               context.Text("PARAM_HOUR"), left / 60 % 60, context.Text("PARAM_MINUTE"));
        }
        tip += (tip.empty() ? "" : "\n") + line;
    }
    return tip;
}

// --- The player's bar ------------------------------------------------------------------------------

void PlayerBar::Created(ui::Interface& ui) {
    Static::Created(ui);
    interactive = true; // a press anywhere on it selects the player
    // Both gauges start empty, with their own fills.
    if (auto* hp = ui::As<ui::Gauge>(Child(0))) {
        hp->texture = std::string(kPmi) + "pmi_hp.ddj";
        hp->SetValue(0.0f, true);
    }
    if (auto* mp = ui::As<ui::Gauge>(Child(1))) {
        mp->texture = std::string(kPmi) + "pmi_mp.ddj";
        mp->SetValue(0.0f, true);
    }
    // Hidden until the state says otherwise: the orbs, the berserk button and effects, the party
    // leader's crown, the race mark (the first update shows it), the status icons, the low HP / MP
    // glows, the pet's bar, the self-selection highlight, the + button.
    for (int id : {7, 8, 9, 10, 11, 13, 14, 15, 40, 41, 42, 43, 44, 45, 46, 50, 51, 60, 70, 77, 78, 100, 200, 34}) {
        if (Widget* w = Child(id)) w->visible = false;
    }
    if (Widget* pk = Child(50)) pk->interactive = true; // the PK icon's tooltip
    // The portrait (12): the player's head drawn into a target of its own, under the 48 x 48 mask; the screen draws it
    // (frame/Portraits.h).
    if (auto* face = ui::As<ui::PictureClip>(Child(12))) {
        face->texture = PortraitTexture(kSelfPortrait);
        face->mask = std::string(kPmi) + "pmi_face.ddj";
    }
    ShowPanel(false);
}

void PlayerBar::ShowPanel(bool shown) {
    panel_ = shown;
    for (int id : kPanel) {
        if (Widget* w = Child(id)) w->visible = shown;
    }
}

void PlayerBar::Warning(ui::Frame& frame, int id, float ratio) {
    // Below 30 % (and not dead): the glow's eight 128 x 32 cells, 100 ms each, looping.
    auto* glow = ui::As<ui::Static>(Child(id));
    if (glow == nullptr) return;
    glow->visible = ratio > 0.0f && ratio < 0.3f;
    if (!glow->visible) return;
    glow->texture = std::string(kPmi) + (id == 77 ? "pmi_hp_cha_effect_caution.ddj" : "pmi_mp_cha_effect_caution.ddj");
    const int cell = int(phase_ / 100.0) % 8;
    glow->customUv = true;
    glow->uv0 = {float(cell % 4) * 0.25f, float(cell / 4) * 0.5f};
    glow->uv1 = {glow->uv0.x + 0.25f, glow->uv0.y + 0.5f};
    (void)frame;
}

void PlayerBar::Update(ui::Frame& frame) {
    const game::PlayerState* state = context_.state;
    if (state == nullptr || state->self.entity == 0) return;
    const game::CharacterStatus& s = state->self;
    // The race mark and the name.
    if (auto* race = ui::As<ui::Static>(Child(15))) {
        race->texture = s.race == 1 ? "interface\\ifcommon\\com_kindred_europe.ddj" : "interface\\ifcommon\\com_kindred_china.ddj";
        race->visible = true;
    }
    Put(Child(3), s.name);
    Put(Child(4), FormatText(context_.Text("UIIT_STT_LV_LEVEL"), {std::to_string(s.level)}));
    // HP and MP: the gauges ease, the numbers do not.
    const std::uint32_t hp = std::min(s.hp, s.maxHp), mp = std::min(s.mp, s.maxMp);
    if (s.maxHp > 0) {
        if (auto* g = ui::As<ui::Gauge>(Child(0))) g->SetValue(float(hp) / float(s.maxHp));
        if (auto* g = ui::As<ui::Gauge>(Child(1))) g->SetValue(s.maxMp > 0 ? float(mp) / float(s.maxMp) : 0.0f);
    }
    Put(Child(5), std::format("{} / {}", hp, s.maxHp));
    Put(Child(6), std::format("{} / {}", mp, s.maxMp));
    // The highlight when the player selected himself; + with stat points to spend; the crown.
    if (Widget* w = Child(200)) w->visible = state->target.entity != 0 && state->target.entity == s.entity;
    if (Widget* w = Child(34)) w->visible = s.statPoints != 0;
    if (Widget* w = Child(14)) w->visible = s.partyLeader;
    // The berserk orbs fill from the bottom one up; the button at the full five.
    // In the mode they all burn and go out one by one as its time runs: each with the flames of
    // pmi_jahwan_burn (16 x 28 cells, 8 a row, 12 frames of 100 ms) in its deco slot,
    // the face's glow (45) while it lasts.
    burnClock_ += frame.dt;
    const int burning = int(burnClock_ / 0.1f) % 12;
    for (int k = 0; k < 5; ++k) {
        if (Widget* orb = Child(11 - k)) orb->visible = k < std::max(s.berserk, s.berserkLit);
        if (auto* deco = ui::As<ui::Static>(Child(44 - k))) {
            deco->visible = k < s.berserkLit;
            deco->texture = std::string(kPmi) + "pmi_jahwan_burn.ddj";
            deco->customUv = true;
            const float u = float(burning % 8) / 8.0f, v = float(burning / 8) * 28.0f / 64.0f;
            deco->uv0 = {u, v};
            deco->uv1 = {u + 1.0f / 8.0f, v + 28.0f / 64.0f};
        }
    }
    if (Widget* w = Child(45)) w->visible = s.berserkLit > 0;
    if (Widget* w = Child(13)) w->visible = s.berserk >= game::CharacterStatus::kBerserkBars;
    // The stat panel's numbers, every frame. The balances from the highest
    // level reached, at most 120 %.
    Put(Child(37), std::to_string(s.strength));
    Put(Child(38), std::to_string(s.intelligence));
    Put(Child(28), std::format("{} ~ {}", s.physicalAttack[0], s.physicalAttack[1]));
    Put(Child(29), std::to_string(s.physicalDefence));
    Put(Child(30), std::format("{} ~ {}", s.magicalAttack[0], s.magicalAttack[1]));
    Put(Child(31), std::to_string(s.magicalDefence));
    Put(Child(32), std::to_string(s.hitRatio));
    Put(Child(33), std::to_string(s.parryRatio));
    const double top = std::max<double>(1, s.highestLevel);
    const int physical = std::min(120, int(std::trunc((s.strength + 2.0 * top + 14.0) * 100.0 / float(((top - 1.0) * 7.0 + 56.0) * 0.857142866))));
    const int magical = std::min(120, int(std::trunc(s.intelligence * 100.0 / ((top + 7.0) * 5.0 * 0.8))));
    Put(Child(54), std::format("{} %", physical));
    Put(Child(55), std::format("{} %", magical));
    // The low HP / MP glows.
    phase_ = std::fmod(phase_ + double(frame.dt) * 1000.0, 800.0);
    Warning(frame, 77, s.maxHp > 0 ? float(hp) / float(s.maxHp) : 1.0f);
    Warning(frame, 78, s.maxMp > 0 ? float(mp) / float(s.maxMp) : 1.0f);
    // The stat panel's button tells what it does.
    if (Widget* b = Child(20); b != nullptr && frame.pointer.hovered == b) {
        frame.interface.ShowTooltip(context_.Text("UIIT_STT_CHARACTER_ABILITY_VIEW_OFF"), b->ScreenRect());
    }
    // The PK icon (GDR_PMI_PK, id 50, icon\etc\stwin_pk.ddj; after each count's message): shown while any of the three
    // counts is not 0; its tooltip "Daily PK %d / %d" with
    // (DailyPK, 15) and "Murderer level %d (%d level)" with (the penalty points, TotalPK).
    if (Widget* pk = Child(50)) {
        pk->visible = state->pkDaily != 0 || state->pkTotal != 0 || state->pkPoints != 0;
        if (pk->visible && frame.pointer.hovered == pk) {
            const std::string tip = FormatText(context_.Text("UIIT_TOOLTIP_PK_DAY"), {std::to_string(state->pkDaily), "15"}) + "\n" +
                                    FormatText(context_.Text("UIIT_TOOLTIP_PK_PENALTY_TIME"), {std::to_string(state->pkPoints), std::to_string(state->pkTotal)});
            frame.interface.ShowTooltip(tip, pk->ScreenRect());
        }
    }
    // In a fortress at war: the battle score (0x385F 0x11) and the rank's name (writes them into the
    // window's tooltip object, inferred), its rank the buff of siegefortressbattlerank.txt (skills 21234..21239).
    if (frame.pointer.hovered == this && context_.fortress != nullptr && context_.world != nullptr) {
        const game::WorldView::Entity* me = context_.world->Find(context_.world->Self());
        const game::proto::FortressScore& score = context_.fortress->Score();
        if (me != nullptr && score.fortress != 0 && context_.fortress->WarArea(me->position.region) == score.fortress) {
            std::string tip = FormatText(context_.Text("UIIT_STT_FORT_PK_NUM_01"), {std::to_string(score.kills)}) + "\n" +
                              FormatText(context_.Text("UIIT_STT_FORT_PK_NUM_02"), {std::to_string(score.deaths)});
            if (const assets::SkillRow* rank = score.rank > 0 && context_.skills != nullptr ? context_.skills->Find(21233u + score.rank) : nullptr) {
                tip = context_.Text(rank->nameKey) + "\n" + tip;
            }
            frame.interface.ShowTooltip(tip, ScreenRect());
        }
    }
}

void PlayerBar::Mouse(ui::Frame& frame) {
    // A left press anywhere on the bar: the player selects himself.
    if (frame.input.Pressed(ui::MouseButton::Left) && frame.pointer.hovered == this && context_.hud != nullptr) {
        context_.hud->Request({HudRequest::Kind::TargetSelf});
    }
}

bool PlayerBar::OnNotice(const ui::Notice& notice) {
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || notice.source->parent() != this) return false;
    switch (notice.control) {
    case 34: // +: the main window on its Character page
        if (context_.hud != nullptr) context_.hud->OpenPage(hud::kPageCharacter);
        return true;
    case 20: ShowPanel(!panel_); return true;
    case 13:
        if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::Berserk});
        return true;
    default: return false;
    }
}

// --- The character page ----------------------------------------------------------------------------

void PlayerInfo::Update(ui::Frame&) {
    if (context_.state == nullptr) return;
    const game::CharacterStatus& s = context_.state->self;
    const auto put = [&](int id, std::string text) {
        if (ui::Widget* w = Child(id)) w->SetText(std::move(text));
    };
    // The level alone (the name is on the frame's title); the experience as raw numbers: what it has
    // and what the level needs.
    put(2, FormatText(context_.Text("UIIT_STT_LV_LEVEL"), {std::to_string(s.level)}));
    put(5, std::to_string(s.experience));
    put(7, std::to_string(s.experienceToNext));
    put(9, std::to_string(s.statPoints));
    // The academy's honour: the player's points ("%d"), or not in one.
    const game::AcademyView& academy = context_.state->academy;
    const game::proto::AcademyMember* me = academy.InAcademy() ? academy.Find(s.name) : nullptr;
    put(51, me != nullptr ? std::to_string(me->honor) : context_.Text("UIIT_STT_TC_CAMP_NOT_JOIN"));
    put(18, std::to_string(s.strength));
    put(19, std::to_string(s.intelligence));
    // The + buttons: always there, pressable while there are points (their _disable face otherwise).
    for (int id : {14, 15}) {
        if (ui::Widget* w = Child(id)) w->enabled = s.statPoints > 0;
    }
    const std::uint32_t hp = std::min(s.hp, s.maxHp), mp = std::min(s.mp, s.maxMp);
    put(12, std::format("{} / {}", hp, s.maxHp));
    put(13, std::format("{} / {}", mp, s.maxMp));
    if (auto* g = ui::As<ui::Gauge>(Child(20))) g->SetValue(s.maxHp > 0 ? float(hp) / float(s.maxHp) : 0.0f);
    if (auto* g = ui::As<ui::Gauge>(Child(21))) g->SetValue(s.maxMp > 0 ? float(mp) / float(s.maxMp) : 0.0f);
    put(34, std::format("{} ~ {}", s.physicalAttack[0], s.physicalAttack[1]));
    put(35, std::to_string(s.physicalDefence));
    put(37, std::to_string(s.hitRatio));
    put(42, std::format("{} ~ {}", s.magicalAttack[0], s.magicalAttack[1]));
    put(43, std::to_string(s.magicalDefence));
    put(45, std::to_string(s.parryRatio));
    // The balances as the player's bar works them out (from the highest level reached, at most 120 %).
    const double top = std::max<double>(1, s.highestLevel);
    const int physical = std::min(120, int(std::trunc((s.strength + 2.0 * top + 14.0) * 100.0 / float(((top - 1.0) * 7.0 + 56.0) * 0.857142866))));
    const int magical = std::min(120, int(std::trunc(s.intelligence * 100.0 / ((top + 7.0) * 5.0 * 0.8))));
    put(36, std::format("{} %", physical));
    put(44, std::format("{} %", magical));
    // The job rows: the alias ("<Nothing>" without), the league's icon, grade name and level,
    // its experience against the job level's requirement.
    const game::PlayerState& p = *context_.state;
    const int league = int(p.job.type);
    const std::string nothing = "<" + context_.Text("UIIT_STT_NONE") + ">";
    put(70, p.jobAlias.empty() ? nothing : p.jobAlias);
    if (auto* icon = dynamic_cast<ui::Static*>(Child(71))) {
        icon->texture = league == 1   ? "interface\\ifcommon\\com_job_merchant.ddj"
                        : league == 2 ? "interface\\ifcommon\\com_job_thief.ddj"
                        : league == 3 ? "interface\\ifcommon\\com_job_hunter.ddj"
                                      : std::string();
    }
    put(72, league == 0 ? nothing : JobClassName(context_, league, p.job.level, s.race));
    put(73, league == 0 ? std::string() : FormatText("%d %s", {std::to_string(p.job.level), context_.Text("UIIT_STT_GRADE")}));
    float fill = 0.0f;
    const std::string exp = league == 0 ? std::string("0%") : JobExpLine(context_, league, p.job.level, p.job.exp, fill);
    put(75, exp);
    if (auto* g = ui::As<ui::Gauge>(Child(74))) g->SetValue(fill);
}

bool PlayerInfo::OnNotice(const ui::Notice& notice) {
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || notice.source->parent() != this) return false;
    if (notice.control != 14 && notice.control != 15) return false;
    if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::RaiseStat, notice.control == 14 ? 0 : 1});
    return true;
}

// --- The buffs board -------------------------------------------------------------------------------

void BuffBoard::Created(ui::Interface&) {
    interactive = true;
    Layout();
}

void BuffBoard::Layout() {
    // Eight a row, 23 apart; rows 27 apart; the buffs first (one row kept even with none), the
    // debuffs and states 2 lower in the rows under them.
    places_.clear();
    if (context_.state == nullptr) return;
    int buffs = 0, debuffs = 0;
    for (const game::ActiveEffect& e : context_.state->effects) (e.debuff ? debuffs : buffs)++;
    const int buffRows = std::max(0, buffs - 1) / 8 + 1;
    const int debuffRows = debuffs == 0 ? 0 : (debuffs - 1) / 8 + 1;
    int b = 0, d = 0;
    for (const game::ActiveEffect& e : context_.state->effects) {
        if (!e.debuff) {
            places_.push_back({float(b % 8) * 23.0f, float(b / 8) * 27.0f});
            ++b;
        } else {
            places_.push_back({float(d % 8) * 23.0f, float(buffRows + d / 8) * 27.0f + 2.0f});
            ++d;
        }
    }
    rect.w = 184.0f;
    rect.h = float(buffRows + debuffRows) * 27.0f;
}

void BuffBoard::Update(ui::Frame& frame) {
    Layout();
    // The skill buffs gone since the last frame: their end's flash and sound (not on entering the world, a board
    // made afresh: nothing was there before).
    if (context_.state != nullptr) {
        std::unordered_map<std::uint32_t, ui::Vec2> now;
        const auto& effects = context_.state->effects;
        for (std::size_t i = 0; i < effects.size() && i < places_.size(); ++i) {
            if (effects[i].kind == game::ActiveEffect::Kind::Skill && !effects[i].debuff && effects[i].token != 0) now[effects[i].token] = places_[i];
        }
        for (const auto& [token, place] : lastBuffs_) {
            if (now.contains(token)) continue;
            endings_.push_back({place, context_.time});
            if (context_.playAtPlayer) context_.playAtPlayer("prim\\snd\\ui\\buf_disappear.wav");
        }
        lastBuffs_ = std::move(now);
    }
    std::erase_if(endings_, [&](const Ending& e) { return context_.time - e.started >= 1.0; });
    hovered_ = -1;
    if (frame.pointer.hovered != this || context_.state == nullptr) return;
    const ui::Vec2 o = Origin();
    for (std::size_t i = 0; i < places_.size(); ++i) {
        const ui::Rect r{o.x + places_[i].x, o.y + places_[i].y, 20, 24};
        if (frame.pointer.position.x >= r.x && frame.pointer.position.x <= r.Right() && frame.pointer.position.y >= r.y &&
            frame.pointer.position.y <= r.Bottom()) {
            hovered_ = int(i);
        }
    }
    if (hovered_ < 0) return;
    // The tooltip: what it is, then the time left.
    const std::string tip = EffectTooltip(context_, context_.state->effects[std::size_t(hovered_)]);
    if (!tip.empty()) frame.interface.ShowTooltip(tip, {o.x + places_[std::size_t(hovered_)].x, o.y + places_[std::size_t(hovered_)].y, 20, 24});
}

void BuffBoard::Draw(ui::Frame& frame) {
    if (context_.state == nullptr) return;
    const ui::Vec2 o = Origin();
    const float opacity = Opacity();
    const ui::Color tint = ui::WithAlpha(ui::kWhite, opacity);
    const auto& effects = context_.state->effects;
    for (std::size_t i = 0; i < effects.size() && i < places_.size(); ++i) {
        const game::ActiveEffect& e = effects[i];
        const ui::Vec2 at{o.x + places_[i].x, o.y + places_[i].y};
        // The time gauge: its black frame, then the light blue fill shrinking from the right; none
        // for what has no end.
        // A time service with a daily bonus (the premium's hours a day, spent in battle): 1.188's two gauges in one place
        // (0x806D50 kind 3; 0x8056A0): s_stateodd_time02_gauge for its term (the item's Param1 seconds), the plain gauge
        // over it for the bonus left of its day's (yesterday's and today's over Param3 / 1000, at most full).
        const assets::ItemRow* service = e.kind == game::ActiveEffect::Kind::TimedJob && (e.token & 0xFFFF0000u) == game::PlayerState::kTimedServiceToken &&
                                                 context_.items != nullptr
                                             ? context_.items->Find(e.id)
                                             : nullptr;
        const double daily = service != nullptr ? double(std::max(0, service->params[2]) / 1000) : 0.0;
        if (daily > 0.0 && e.duration > 0.0) {
            const double term = std::max(1.0, double(std::max(0, service->params[0])));
            const float term01 = float(std::clamp((e.started + e.duration - context_.time) / term, 0.0, 1.0));
            const float bonus01 = float(std::clamp(double((e.bonus >> 16) + (e.bonus & 0xFFFFu)) / daily, 0.0, 1.0));
            frame.paint.Image("icon\\stateodd\\s_stateodd_time.ddj", {at.x, at.y + 20, 20, 4}, {0, 0}, {1, 1}, tint);
            if (term01 > 0.0f) frame.paint.Image("icon\\stateodd\\s_stateodd_time02_gauge.ddj", {at.x, at.y + 20, 20 * term01, 4}, {0, 0}, {term01, 1}, tint);
            if (bonus01 > 0.0f) frame.paint.Image("icon\\stateodd\\s_stateodd_time_gauge.ddj", {at.x, at.y + 20, 20 * bonus01, 4}, {0, 0}, {bonus01, 1}, tint);
        } else if (e.duration > 0.0) {
            const float left = float(std::clamp((e.started + e.duration - context_.time) / e.duration, 0.0, 1.0));
            frame.paint.Image("icon\\stateodd\\s_stateodd_time.ddj", {at.x, at.y + 20, 20, 4}, {0, 0}, {1, 1}, tint);
            if (left > 0.0f) {
                frame.paint.Image("icon\\stateodd\\s_stateodd_time_gauge.ddj", {at.x, at.y + 20, 20 * left, 4}, {0, 0}, {left, 1}, tint);
            }
        }
        std::string icon;
        if (e.kind == game::ActiveEffect::Kind::State) {
            icon = StateIcon(e.id);
        } else if (context_.effectIcon) {
            icon = context_.effectIcon(e);
        }
        if (!icon.empty()) frame.paint.Image(icon, {at.x, at.y, 20, 20}, {0, 0}, {1, 1}, tint);
        // A link skill's buff ('lnks', the skill row's link parameter): window_all.ddj's small frame (u 0.9316..0.9551,
        // v 0..0.0527) round its slot, 2 out on each side, its gauge inside, as the 1.188 board's slot.
        if (e.kind == game::ActiveEffect::Kind::Skill && context_.skills != nullptr) {
            if (const assets::SkillRow* skill = context_.skills->Find(e.id); skill != nullptr && game::TraitsOf(*skill).link) {
                const float h = e.duration > 0.0 ? 24.0f : 20.0f;
                frame.paint.Image("interface\\ifcommon\\window_all.ddj", {at.x - 2.0f, at.y - 2.0f, 24.0f, h + 4.0f}, {0.9316f, 0.0f}, {0.9551f, 0.0527f}, tint);
            }
        }
    }
    // The ended buffs' flash: the sheet's cells are 50 x 52 in 512 x 64, centred on the slot.
    for (const Ending& e : endings_) {
        const int cell = std::clamp(int((context_.time - e.started) / 0.1), 0, 9);
        const ui::Rect r{o.x + e.place.x + 10.0f - 25.0f, o.y + e.place.y + 12.0f - 26.0f, 50.0f, 52.0f};
        frame.paint.Image("icon\\buf_effect.ddj", r, {float(cell) * 50.0f / 512.0f, 0.0f}, {float(cell + 1) * 50.0f / 512.0f, 52.0f / 64.0f}, tint);
    }
}

void BuffBoard::Mouse(ui::Frame& frame) {
    // A right click on a buff asks to end it; debuffs and states cannot be.
    if (!frame.input.Released(ui::MouseButton::Right) || frame.pointer.held != this || hovered_ < 0 || context_.state == nullptr) return;
    const game::ActiveEffect& e = context_.state->effects[std::size_t(hovered_)];
    if (e.kind == game::ActiveEffect::Kind::Skill && !e.debuff && context_.hud != nullptr) {
        context_.hud->Request({HudRequest::Kind::CancelBuff, hovered_});
    }
    // A time service's entry: its pause asked (the premium list's confirm box); a bonus scroll's: its end asked; the
    // premium package's: its uses' window (1.188's composite item window, the board's kind 5).
    if (e.kind == game::ActiveEffect::Kind::TimedJob && (e.token & 0xFFFF0000u) == game::PlayerState::kTimedServiceToken && context_.hud != nullptr) {
        if (std::uint8_t(e.token & 0xFF) == game::kTimedPremium) {
            ShowCompositeItems(context_);
        } else {
            context_.hud->Request({HudRequest::Kind::CancelBuff, hovered_});
        }
    }
    // The academy's accumulation an hour after it began (or its day over): its pass asked (the same handler,
    // board kind 7, 3,600,000 ms; UIIT_CTL_TC_PASS_BUFF's box, then the original's 0x7483; the server checks the hour).
    if (e.kind == game::ActiveEffect::Kind::TimedJob && e.token == game::PlayerState::kAcademyToken &&
        (e.duration == 0.0 || context_.time - e.started >= double(game::academy::kPassAfterMinutes) * 60.0) && context_.hud != nullptr) {
        context_.hud->Request({HudRequest::Kind::CancelBuff, hovered_});
    }
}

} // namespace sr::client
