// The target window.

#include <algorithm>
#include <cmath>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace {

constexpr const char* kTargetArt = "interface\\targetwindow\\";
constexpr int kPlayer = 0, kJobPlayer = 1, kMonster = 2, kCommon = 4, kStructure = 8, kClose = 11, kBuffs = 100;

void Put(ui::Widget* w, const std::string& text) {
    if (w != nullptr && w->text() != text) w->SetText(text);
}

void SetTexture(ui::Widget* w, const std::string& path) {
    if (auto* s = ui::As<ui::Static>(w)) s->texture = path;
}

void SetColour(ui::Widget* w, ui::Color colour) {
    if (auto* s = ui::As<ui::Static>(w)) s->textColor = colour;
}

// A part of window_all.ddj (1024 x 512) by its pixels: another look of a kind.
void Look(ui::Widget* w, float x0, float y0, float x1, float y1) {
    auto* s = ui::As<ui::Static>(w);
    if (s == nullptr) return;
    s->customUv = true;
    s->uv0 = {x0 / 1024.0f, y0 / 512.0f};
    s->uv1 = {x1 / 1024.0f, y1 / 512.0f};
    s->rect.w = x1 - x0;
    s->rect.h = y1 - y0;
}

void LayoutLook(ui::Widget* w) {
    if (auto* s = ui::As<ui::Static>(w)) {
        s->customUv = false;
        s->rect.w = s->node().rect.w;
        s->rect.h = s->node().rect.h;
    }
}

void Health(ui::Widget* w, const game::TargetStatus& t, float width, bool full = false) {
    auto* g = ui::As<ui::Gauge>(w);
    if (g == nullptr) return;
    g->rect.w = width;
    // The bar snaps to the value (its speed makes the easing whole, observed).
    g->SetValue(full ? 1.0f : t.maxHp > 0 ? float(std::min(t.hp, t.maxHp)) / float(t.maxHp) : 0.0f, true);
}

// The race mark (24 x 24) of China or Europe.
std::string RaceMark(std::uint8_t race) {
    return race == 1 ? "interface\\ifcommon\\com_kindred_europe.ddj" : "interface\\ifcommon\\com_kindred_china.ddj";
}

} // namespace

void TargetWindow::Created(ui::Interface&) {
    // The kinds and the buff row wait hidden; the close button stays.
    for (int id : {kPlayer, kJobPlayer, kMonster, kCommon, kStructure, kBuffs}) {
        if (Widget* w = Child(id)) w->visible = false;
    }
    if (Widget* d = Find(kStructure)) {
        if (Widget* button = d->Child(11)) button->visible = false; // the fortress war's "remove" (shown by Update)
    }
}

void TargetWindow::Update(ui::Frame& frame) {
    if (context_.state == nullptr || !context_.state->target.Selected()) return;
    const game::TargetStatus& t = context_.state->target;
    const game::CharacterStatus& self = context_.state->self;
    int shown = kCommon;
    switch (t.kind) {
    case game::TargetStatus::Kind::Monster: shown = kMonster; break;
    case game::TargetStatus::Kind::Player: shown = t.job == 0 ? kPlayer : kJobPlayer; break;
    case game::TargetStatus::Kind::Structure: shown = kStructure; break;
    default: shown = kCommon; break;
    }
    for (int id : {kPlayer, kJobPlayer, kMonster, kCommon, kStructure}) {
        if (Widget* w = Child(id)) w->visible = id == shown;
    }
    Widget* kind = Child(shown);
    if (kind == nullptr) return;
    // The 1.188 kinds are 236 wide: the close button at their right end, the bars 208 long (the older
    // layout's 168 and 176 are for its 196-wide frames).
    float closeX = 216.0f;
    if (shown == kMonster) {
        // The level difference picks the gem and the name's colour.
        const int d = int(t.level) - int(self.level);
        const int step = d <= -7 ? 0 : d <= -4 ? 1 : d <= 0 ? 2 : d <= 5 ? 3 : 4;
        static constexpr const char* kGems[] = {"tw_gem_weak2.ddj", "tw_gem_weak1.ddj", "tw_gem_normal.ddj", "tw_gem_strong1.ddj", "tw_gem_strong2.ddj"};
        static constexpr std::uint32_t kColours[] = {0xFF87D2FF, 0xFFA5E0CE, 0xFFFFFFFF, 0xFFFFB387, 0xFFFF8787};
        SetTexture(kind->Child(10), std::string(kTargetArt) + kGems[step]);
        Put(kind->Child(0), t.name);
        SetColour(kind->Child(0), ui::Argb(kColours[step]));
        Health(kind->Child(1), t, 208.0f);
        Put(kind->Child(4), FormatText(context_.Text("UIIT_STT_LV_LEVEL"), {std::to_string(t.level)}));
        // The rarity: its icon and its name, "(Party)" after it; rarity 2 shows none.
        static constexpr const char* kIcons[] = {"tw_icon_normal.ddj", "tw_icon_champion.ddj", "", "tw_icon_unique.ddj",
                                                 "tw_icon_giant.ddj",  "tw_icon_titan.ddj",    "tw_icon_elite.ddj"};
        static constexpr const char* kNames[] = {"UIIT_CTL_WARENETWORK_DETAIL_NORMAL", "UIIT_STT_MOB_CHAMPION", "", "UIIT_STT_MOB_UNIQUE",
                                                 "UIIT_STT_MOB_GIANT", "UIIT_STT_MOB_TITAN", "UIIT_STT_MOB_ELITE"};
        const bool known = t.rarity <= 6 && t.rarity != 2;
        Widget* icon = kind->Child(3);
        Widget* rank = kind->Child(2);
        if (icon != nullptr) {
            icon->visible = known;
            if (known) SetTexture(icon, std::string(kTargetArt) + kIcons[t.rarity]);
            icon->rect.x = 85.0f;
            icon->rect.y = 54.0f;
        }
        if (rank != nullptr) {
            rank->visible = known;
            std::string text = known ? context_.Text(kNames[t.rarity]) : std::string();
            if (known && t.party) text += context_.Text("UIIT_STT_PARTYMOB_TARGET");
            Put(rank, text);
            rank->rect.x = 105.0f;
            rank->rect.y = 56.0f;
            // A party monster's line is centred, the icon 20 to its left.
            if (known && t.party && t.rarity != 0) {
                const float width = frame.paint.TextWidth(text, rank->node().fontIndex);
                rank->rect.x = std::floor(kind->rect.w / 2.0f - width / 2.0f + 10.0f);
                if (icon != nullptr) icon->rect.x = rank->rect.x - 20.0f;
            }
        }
    } else if (shown == kCommon) {
        // Three looks: the NPC's (wider, its bar full unless a guard), a pet's or a guild
        // soldier's (no bar), the layout's.
        Widget* gauge = kind->Child(1);
        if (t.kind == game::TargetStatus::Kind::Npc) {
            Look(kind, 741, 113, 977, 164);
            if (gauge != nullptr) gauge->visible = true;
            Health(gauge, t, 208.0f, !t.guard);
            if (Widget* name = kind->Child(0)) name->rect.w = 177.0f;
            SetTexture(kind->Child(10), std::string(kTargetArt) + "tw_gem_player.ddj");
        } else if (t.noBar) {
            Look(kind, 543, 330, 739, 366);
            if (gauge != nullptr) gauge->visible = false;
            if (Widget* name = kind->Child(0)) name->rect.w = 122.0f;
            SetTexture(kind->Child(10), std::string(kTargetArt) + "tw_gem_animal.ddj");
            closeX = 176.0f; // this look is 196 wide
        } else {
            LayoutLook(kind);
            if (gauge != nullptr) gauge->visible = true;
            Health(gauge, t, 208.0f);
            if (Widget* name = kind->Child(0)) name->rect.w = 177.0f;
            SetTexture(kind->Child(10), std::string(kTargetArt) + (t.kind == game::TargetStatus::Kind::Pet ? "tw_gem_animal.ddj" : "tw_gem_normal.ddj"));
        }
        Put(kind->Child(0), t.name);
    } else if (shown == kPlayer) {
        SetTexture(kind->Child(10), RaceMark(t.race));
        Put(kind->Child(1), t.name);
    } else if (shown == kJobPlayer) {
        // "*" before the name, the job's icon, its rank name and level.
        static constexpr const char* kJobs[] = {"", "MERCHANT", "THIEF", "HUNTER"};
        static constexpr const char* kJobIcons[] = {"", "tw_job_merchant.ddj", "tw_job_thief.ddj", "tw_job_hunter.ddj"};
        const int job = std::clamp<int>(t.job, 1, 3);
        SetTexture(kind->Child(20), RaceMark(t.race));
        Put(kind->Child(1), "*" + t.name);
        SetTexture(kind->Child(10), std::string(kTargetArt) + kJobIcons[job]);
        Put(kind->Child(11), context_.Text(std::format("UIIT_STT_CLASS_{}{}_{}", t.race == 1 ? "EU_" : "", kJobs[job], t.jobLevel)));
        Put(kind->Child(12), std::format("{}{}", t.jobLevel, context_.Text("UIIT_STT_GRADE")));
    } else if (shown == kStructure) {
        // The 236-wide look of the NPCs' (the layout's own frame is the 196-wide look of the older kinds).
        Look(kind, 741, 113, 977, 164);
        Put(kind->Child(0), t.name);
        if (Widget* name = kind->Child(0)) name->rect.w = 177.0f;
        Health(kind->Child(1), t, 208.0f);
        SetTexture(kind->Child(10), std::string(kTargetArt) + "tw_gem_player.ddj");
        closeX = 216.0f;
        // The remove button: a barricade, a guard or a summoned monster of the player's own guild (ours:
        // the war's positions are the server's check).
        const game::proto::EntityStructure* s = context_.fortress != nullptr ? context_.fortress->Structure(t.entity) : nullptr;
        const bool removable = s != nullptr && (s->kind == std::uint8_t(game::StructureKind::Barricade) || s->kind == std::uint8_t(game::StructureKind::Guard) ||
                                                s->kind == std::uint8_t(game::StructureKind::Summoned));
        if (Widget* button = kind->Child(11)) {
            button->visible = removable && context_.guild != nullptr && context_.guild->InGuild() && s->owner == context_.guild->Guild().id;
        }

    }
    // The target's states under its look (the layout puts the row at 37, under the 36-high player look;
    // ours: one pixel under each look until the viewer's placing is read).
    if (Widget* buffs = Child(kBuffs)) {
        buffs->visible = !t.states.empty();
        buffs->rect.x = 0.0f;
        buffs->rect.y = kind->rect.h + 1.0f;
    }
    // Centred at the top, 7 down, every time.
    rect.w = kind->rect.w;
    rect.h = kind->rect.h;
    rect.x = std::floor((frame.interface.screen().x - rect.w) / 2.0f);
    rect.y = 7.0f;
    if (Widget* close = Child(kClose)) close->rect.x = closeX;
}

// --- The target's states ---------------------------------------------------------------------------

void TargetBuffs::Created(ui::Interface&) { interactive = true; }

void TargetBuffs::Update(ui::Frame& frame) {
    hovered_ = -1;
    if (context_.state == nullptr) return;
    const std::vector<game::ActiveEffect>& states = context_.state->target.states;
    rect.w = float(std::min<std::size_t>(states.size(), 8)) * 23.0f;
    rect.h = float((states.size() + 7) / 8) * 27.0f;
    if (frame.pointer.hovered != this) return;
    const ui::Vec2 o = Origin();
    for (std::size_t i = 0; i < states.size(); ++i) {
        const ui::Rect r{o.x + float(i % 8) * 23.0f, o.y + float(i / 8) * 27.0f, 20, 20};
        if (frame.pointer.position.x >= r.x && frame.pointer.position.x <= r.Right() && frame.pointer.position.y >= r.y &&
            frame.pointer.position.y <= r.Bottom()) {
            hovered_ = int(i);
            const std::string tip = EffectTooltip(context_, states[i]);
            if (!tip.empty()) frame.interface.ShowTooltip(tip, r);
        }
    }
}

void TargetBuffs::Draw(ui::Frame& frame) {
    if (context_.state == nullptr) return;
    const ui::Vec2 o = Origin();
    const ui::Color tint = ui::WithAlpha(ui::kWhite, Opacity());
    const std::vector<game::ActiveEffect>& states = context_.state->target.states;
    for (std::size_t i = 0; i < states.size(); ++i) {
        const std::string icon = states[i].kind == game::ActiveEffect::Kind::State ? StateIcon(states[i].id) : std::string();
        if (!icon.empty()) frame.paint.Image(icon, {o.x + float(i % 8) * 23.0f, o.y + float(i / 8) * 27.0f, 20, 20}, {0, 0}, {1, 1}, tint);
    }
}

bool TargetWindow::OnNotice(const ui::Notice& notice) {
    // The structure's remove button: a guard or a barricade asks "Do you want to remove [%s]?"
    // (UIIT_MSG_FORT_STRUCTURE_DELETE_WINDOW, boxes 100, 101), a summoned monster "Do you want exterminate the summoned
    // monster?" (_SUMMONMON_DISMISS_WINDOW, 102); then 0x705E 0x16 / 0x17 {object, fortress}.
    if (notice.kind == ui::Notice::Kind::Clicked && notice.control == 11 && notice.source != nullptr && notice.source->parent() == Child(kStructure) &&
        context_.state != nullptr && context_.fortress != nullptr && context_.fortressRequest && context_.hud != nullptr) {
        const std::uint32_t object = context_.state->target.entity;
        if (const game::proto::EntityStructure* s = context_.fortress->Structure(object)) {
            const bool monster = s->kind == std::uint8_t(game::StructureKind::Summoned);
            const std::string question = monster ? context_.Text("UIIT_MSG_FORT_SUMMONMON_DISMISS_WINDOW")
                                                 : FormatText(context_.Text("UIIT_MSG_FORT_STRUCTURE_DELETE_WINDOW"), {context_.state->target.name});
            HudContext* c = &context_;
            const std::uint32_t fortress = s->fortress;
            context_.hud->Confirm("UIIT_STT_CONFIRM_BOX", {question}, [c, object, fortress](bool yes) {
                if (yes && c->fortressRequest) c->fortressRequest(game::proto::FortressRemove::kId, game::proto::Encode(game::proto::FortressRemove{object, fortress}));
            });
        }
        return true;
    }
    // The close button hides the window; the selection stays.
    if (notice.kind == ui::Notice::Kind::Clicked && notice.control == kClose && notice.source != nullptr && notice.source->parent() == this) {
        visible = false;
        return true;
    }
    return false;
}

} // namespace sr::client
