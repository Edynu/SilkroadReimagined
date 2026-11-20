// Murderers on the game screen: each player's PvP state (the server's EntityPvpState), the names' colours by the
// relations and states, the click on a player (select; a second click or Alt attacks; without Alt only an enemy), the
// client's own refusals (a level below 20 at once; a murderer's return scrolls) and the texts of the server's.

#include <algorithm>

#include "play/PlayScreen.h"
#include "sr/core/Log.h"
#include "sr/game/Pk.h"

namespace sr::client {

namespace proto = game::proto;

std::uint8_t PlayScreen::PvpStateOf(std::uint32_t entity) const {
    const auto it = entityPk_.find(entity);
    return it != entityPk_.end() ? it->second : 0;
}

void PlayScreen::HandlePk(ScreenContext& context, const proto::PkEvent& e) {
    (void)context;
    if (const auto* s = std::get_if<proto::EntityPvpState>(&e)) {
        if (s->state == 0) entityPk_.erase(s->entity);
        else entityPk_[s->entity] = s->state;
        core::LogInfo("game: {} is in PvP state {}{}", s->entity, int(s->state), s->entity == view_.Self() ? " (the player)" : "");
    } else if (const auto* c = std::get_if<proto::PkCounters>(&e)) {
        // No banner nor line on a change (1.188's handlers only refresh the bar's icon).
        core::LogInfo("game: PK counts: daily {}, total {}, points {}", int(c->daily), c->total, c->points);
    }
}

ui::Color PlayScreen::PlayerNameColour(std::uint32_t id, const game::WorldView::Entity& e) const {
    // A GM's name ("[GM]" first).
    if (e.look.name.starts_with("[GM]")) return ui::Argb(0xFFFFD87A);
    const bool self = id == view_.Self();
    const bool party = !self && player_.party.InParty() && player_.party.MemberByEntity(id) != nullptr;
    // A hostile relation (free PvP's other teams, the jobs' foes): red when he may be fought.
    if (!self && !party && (FreePvpFoe(id) || JobFoe(id))) return ui::Argb(0xFFFF6262);
    // The fortress war's enemies and a guild at war with ours (the fortress's sides ours in that red).
    const proto::EntityGuild* tag = self ? nullptr : guild_.EntityTag(id);
    const bool war = !self && ((tag != nullptr && guild_.AtWarWith(tag->guild)) || FortressFoe(id, e));
    // An ally relation (the same free PvP team) keeps its colour over the states.
    const auto a = entityPvp_.find(view_.Self()), b = entityPvp_.find(id);
    const bool ally = !self && a != entityPvp_.end() && b != entityPvp_.end() && a->second.team != 0 && a->second.team == b->second.team;
    if (!ally) {
        switch (PvpStateOf(id)) {
        case std::uint8_t(game::PvpState::Assaulter): return ui::Argb(0xFFFF58FE); // pink
        case std::uint8_t(game::PvpState::Murderer): return ui::Argb(0xFFFF6262);  // red
        default: break;
        }
    }
    if (party) return ui::Argb(0xFF62F5B1);
    if (war) return ui::Argb(0xFFFF0000);
    return ui::Argb(0xFFFFFFFF);
}

void PlayScreen::ClickPlayer(ScreenContext& context, std::uint32_t other, bool ctrl, bool alt) {
    // Another player, living or dead: selected (its target window; a heal, a buff or a resurrection from the quick slots
    // goes to it). No walk: the first click only selects; the selected one clicked again, or a click with Alt (the help's
    // UIIT_STT_FORCEPK "=+ Mouse Left Button : Force PK"), attacks: without Alt only an enemy (PvP state 1 or 2, a job's
    // foe, a free PvP foe, a guild at war, the fortress war's enemy), with Alt anyone; never a member of one's party.
    // Ctrl selects and attacks an enemy at once (ours, as on monsters: the original's Ctrl is the force attack on others).
    const bool again = player_.target.entity == other || ctrl || alt;
    SelectPlayer(other);
    // A booth: walked up to and visited.
    if (!ctrl && !alt && ClickBooth(context, other)) return;
    pendingNpc_ = 0;
    pendingGate_ = 0;
    const proto::EntityGuild* tag = guild_.EntityTag(other);
    const game::WorldView::Entity* it = view_.Find(other);
    if (!again || it == nullptr || IsDead(*it) || actionsHeld_ >= 2 || replay_) return;
    // In a Capture the Flag match an enemy is exactly another flag team, nothing else tested.
    const bool flagFoe = FlagFoe(other);
    if (!flagFoe && player_.party.InParty() && player_.party.MemberByEntity(other) != nullptr) return;
    const bool freePvp = FreePvpFoe(other) || flagFoe;
    const std::uint8_t state = PvpStateOf(other);
    const bool enemy = (tag != nullptr && guild_.AtWarWith(tag->guild)) || FortressFoe(other, *it) || freePvp || JobFoe(other) ||
                       state == std::uint8_t(game::PvpState::Assaulter) || state == std::uint8_t(game::PvpState::Murderer);
    if (!enemy && !alt) return;
    // Below level 20 outside free PvP: "Cannot PK at your level." at once, nothing sent.
    if (!freePvp && int(player_.self.level) < game::kPkLevel) {
        PkRefusal(proto::SkillUseStatus::YourLevelTooLow, context.time);
        return;
    }
    shared_->session->Attack(other);
    actionSentAt_ = context.time;
    if (alt && !enemy) core::LogInfo("game: Alt + click: a forced attack on {}", it->look.name);
}

bool PlayScreen::MurdererScrollRefused(std::uint8_t place, double time) {
    if (PvpStateOf(view_.Self()) != std::uint8_t(game::PvpState::Murderer) || !tables_ || !tables_->Usable()) return false;
    const game::ItemInstance* item = player_.inventory.At(place);
    const assets::ItemRow* row = item != nullptr ? tables_->items.Find(item->refId) : nullptr;
    if (row == nullptr) return false;
    // A return scroll (TID 3/3/3/1) or the reverse return scroll (3/3/3/3): refused before asking the server (0x1875).
    const bool scroll = row->typeId == std::array<int, 4>{3, 3, 3, 1} ||
                        (row->typeId == std::array<int, 4>{3, 3, 3, 3} && row->codename.find("REVERSE_RETURN") != std::string::npos);
    if (!scroll) return false;
    const std::string text = gui_.data().Text("UIIT_MSG_STRGERR_MURDERER_CANT_USE_PORTAL_SCROLL");
    player_.Notify(game::Banner::Kind::Warning, text, time);
    player_.Post(game::GameMessage::Kind::General, text, time);
    core::LogInfo("game: a murderer may not read {}", row->codename);
    return true;
}

void PlayScreen::PkRefusal(proto::SkillUseStatus status, double time) {
    const char* key = nullptr;
    switch (status) {
    case proto::SkillUseStatus::DailyPkLimit: key = "UIIT_MSG_SKILL_USE_FAIL_DAILY_PKLIMIT_EXCEEDED"; break;
    case proto::SkillUseStatus::TotalPkLimit: key = "UIIT_MSG_SKILL_USE_FAIL_TOTAL_PKLIMIT_EXCEEDED"; break;
    case proto::SkillUseStatus::YourLevelTooLow: key = "UIIT_MSG_SKILL_USE_FAIL_YOUR_LEVEL_TOO_LOW_TO_PK"; break;
    case proto::SkillUseStatus::TargetLevelTooLow: key = "UIIT_MSG_SKILL_USE_FAIL_TARGET_LEVEL_TOO_LOW_TO_PK"; break;
    case proto::SkillUseStatus::PkProhibited: key = "UIIT_MSG_SKILL_USE_FAIL_PK_PROHIBITED_IN_THIS_SERVER"; break;
    case proto::SkillUseStatus::Duplicate: key = "UIIT_SKILL_USE_FAIL_DUPLICATE"; break;
    default: return;
    }
    // A system line.
    player_.Post(game::GameMessage::Kind::General, gui_.data().Text(key), time);
}

} // namespace sr::client
