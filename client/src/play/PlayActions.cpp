// The Action page (A) on the game screen: what each icon does (the 1.188
// client's handler by the icon's id in actionwnddata.txt), the motions and marks the server
// tells, the helper marks over heads, auto run and auto looting.

#include <algorithm>
#include <cmath>

#include "play/PlayScreen.h"
#include "sr/assets/Model.h"
#include "sr/core/Log.h"
#include "sr/game/Cos.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::ChatCommand(ScreenContext& context, const std::string& key, const std::string& line) {
    // "/word NAME rest": the name (a player in sight for those that ask one) and what follows it.
    const std::size_t space = line.find(' ');
    const std::size_t nameAt = space == std::string::npos ? std::string::npos : line.find_first_not_of(' ', space);
    const std::size_t nameEnd = nameAt == std::string::npos ? std::string::npos : line.find(' ', nameAt);
    const std::string name = nameAt == std::string::npos ? std::string() : line.substr(nameAt, nameEnd == std::string::npos ? std::string::npos : nameEnd - nameAt);
    const std::size_t restAt = nameEnd == std::string::npos ? std::string::npos : line.find_first_not_of(' ', nameEnd);
    const std::string rest = restAt == std::string::npos ? std::string() : line.substr(restAt);
    const auto is = [&](std::string_view k) { return key == k; };
    const auto inSight = [&]() -> std::uint32_t {
        for (const auto& [id, e] : view_.Entities()) {
            if (id != view_.Self() && e.kind == proto::EntityKind::Character && !e.look.name.empty() && e.look.name == name) return id;
        }
        return 0;
    };
    auto* chat = dynamic_cast<ChatWindow*>(gui_.interface() != nullptr ? gui_.interface()->Window(hud::kChat) : nullptr);
    const bool live = !replay_ && shared_ && shared_->session;
    if (is("UIIT_STT_CHAT_COMMAND_REPLY1") || is("UIIT_STT_CHAT_COMMAND_REPLY2") || is("UIIT_STT_CHAT_COMMAND_REPLY3") ||
        is("UIIT_STT_CHAT_COMMAND_REPLY4")) {
        // The last whisperer's address in the input line.
        if (chat != nullptr) {
            chat->Reply();
            chat->FocusInput();
        }
    } else if (key.starts_with("UIIT_STT_CHAT_COMMAND_WHISPER") || is("UIIT_STT_CHAT_COMMAND_SECRET_TALK")) {
        // "/w NAME text": whispered; with no text, the address in the input line.
        if (name.empty()) return;
        if (!rest.empty()) {
            hud_.Request({HudRequest::Kind::Chat, int(proto::ChatKind::Whisper), rest, name});
        } else if (chat != nullptr) {
            chat->Address(name, true);
        }
    } else if (is("UIIT_STT_CHAT_COMMAND_PARTY_INVITE") || is("UIIT_STT_CHAT_COMMAND_PARTY_INVITE2")) {
        if (const std::uint32_t target = inSight(); target != 0 && live) shared_->session->PartyInvite(target, player_.party.preferred);
    } else if (is("UIIT_STT_CHAT_COMMAND_PARTY_EXPEL")) {
        for (const proto::PartyMemberRecord& m : player_.party.members) {
            if (m.name == name && live && player_.party.IsLeader()) shared_->session->KickPartyMember(m.id);
        }
    } else if (is("UIIT_STT_CHAT_COMMAND_PARTY_EXIT")) {
        if (live && player_.party.InParty()) shared_->session->LeaveParty();
    } else if (is("UIIT_STT_CHAT_COMMAND_TRADE") || is("UIIT_STT_CHAT_COMMAND_TRADE2")) {
        if (const std::uint32_t target = inSight(); target != 0 && live) {
            SelectPlayer(target);
            shared_->session->SendGame(proto::ExchangeAsk::kId, proto::Encode(proto::ExchangeAsk{target}));
        }
    } else if (is("UIIT_STT_CHAT_COMMAND_GUILD_INVITE") || is("UIIT_STT_CHAT_COMMAND_GUILD_INVITE1") || is("UIIT_STT_CHAT_COMMAND_GUILD_INVITE2")) {
        if (const std::uint32_t target = inSight(); target != 0 && live) shared_->session->SendGuild(proto::GuildInvite{target});
    } else if (is("UIIT_STT_CHAT_COMMAND_STREETSTORE")) {
        if (live) {
            shared_->session->SendGame(proto::StallOpen::kId,
                                       proto::Encode(proto::StallOpen{FormatText(gui_.data().Text("UIIT_STT_STALL_DEFAULT_TITLE"), {player_.self.name})}));
        }
    } else if (is("UIIT_STT_CHAT_COMMAND_SITDOWN")) {
        UseAction(context, 1000);
    } else if (key.starts_with("UIIT_KEY_COS_")) {
        // The pets' words: the attack pet's attack and stance, the first pet's follow and recall, the ride's board, leave
        // and dismissal.
        const OwnCos* attackPet = nullptr;
        const OwnCos* pet = nullptr;
        for (const OwnCos& c : ownCoses_) {
            if (c.kind == std::uint8_t(game::CosKind::AttackPet)) attackPet = &c;
            if (pet == nullptr && (c.kind == std::uint8_t(game::CosKind::AttackPet) || c.kind == std::uint8_t(game::CosKind::GrabPet))) pet = &c;
        }
        const auto order = [&](const OwnCos* c, proto::CosOrder o, std::uint32_t target = 0) {
            if (c != nullptr && live) shared_->session->SendGame(proto::CosCommand::kId, proto::Encode(proto::CosCommand{c->entity, o, target}));
        };
        if (is("UIIT_KEY_COS_ATTACK")) {
            order(attackPet, proto::CosOrder::Attack, player_.target.kind == game::TargetStatus::Kind::Monster ? player_.target.entity : 0);
        } else if (is("UIIT_KEY_COS_FOLLOW")) {
            order(pet, proto::CosOrder::Follow);
        } else if (is("UIIT_KEY_COS_AGGRESSIVE") || is("UIIT_KEY_COS_DEFENSIVE")) {
            order(attackPet, proto::CosOrder::Stance);
        } else if (is("UIIT_KEY_COS_CHARM")) {
            order(pet, proto::CosOrder::Charm); // its greeting gesture
        } else if (is("UIIT_KEY_COS_RIDING")) {
            CosRide(true);
        } else if (is("UIIT_KEY_COS_DISMOUNT")) {
            CosRide(false);
        } else if (is("UIIT_KEY_COS_RETURN")) {
            if (pet != nullptr && live) shared_->session->SendGame(proto::CosDismiss::kId, proto::Encode(proto::CosDismiss{pet->entity}));
        } else if (is("UIIT_KEY_COS_LEAVEOUT")) {
            CosDismiss();
        }
    } else {
        // The PvP proposal and the user search: features of the original not built.
        core::LogInfo("game: the chat command {} ({}): not built", key, line);
    }
}

std::pair<std::string, std::string> PlayScreen::ActionSlotLook(std::uint32_t action) const {
    // What the next use does (the original's quick slots swap their icon and text on each change): sit or stand by
    // the motion, walk or run by the pace, free PvP on or off by the player's team.
    const auto own = entityMotions_.find(view_.Self());
    const bool sitting = own != entityMotions_.end() &&
                         (own->second.motion == proto::kMotionSit || (own->second.motion == proto::kMotionChanging && own->second.to == proto::kMotionSit));
    const bool walking = own != entityMotions_.end() && own->second.walking;
    const auto pvp = entityPvp_.find(view_.Self());
    const bool freePvp = pvp != entityPvp_.end() && pvp->second.team != 0;
    switch (action) {
    case 1000: return sitting ? std::pair{"icon\\action\\icon_cha_stand.ddj", "UIIT_CTL_STAND"} : std::pair{"icon\\action\\icon_cha_sit.ddj", "UIIT_CTL_SIT"};
    case 1001: return walking ? std::pair{"icon\\action\\icon_cha_run.ddj", "UIIT_CTL_RUN"} : std::pair{"icon\\action\\icon_cha_walk.ddj", "UIIT_CTL_WALK"};
    case 1017:
        return freePvp ? std::pair{"icon\\action\\icon_cha_frpvp_off.ddj", "UIIT_STT_FRPVP_OFF"} : std::pair{"icon\\action\\icon_cha_frpvp_on.ddj", "UIIT_STT_FRPVP_ON"};
    default: break;
    }
    // The others as the Action page shows them.
    if (hudContext_.actions != nullptr) {
        for (const assets::ActionRow& row : *hudContext_.actions) {
            if (row.id == action) return {"icon\\" + row.icon.substr(row.icon.rfind("icon\\", 0) == 0 ? 5 : 0), row.textKey};
        }
    }
    return {};
}

void PlayScreen::UseAction(ScreenContext& context, std::uint32_t action) {
    const auto send = [this](proto::MessageId id, std::vector<std::byte> payload) {
        if (!replay_) shared_->session->SendGame(id, std::move(payload));
    };
    switch (action) {
    case 1000: // Sit / Stand (N): the original's 0x704F with 4
        send(proto::SetMotion::kId, proto::Encode(proto::SetMotion{proto::kMotionSit}));
        return;
    case 1001: { // Walk / Run: 2 or 3 by the pace it moves at now
        const auto own = entityMotions_.find(view_.Self());
        const bool walking = own != entityMotions_.end() && own->second.walking;
        send(proto::SetMotion::kId, proto::Encode(proto::SetMotion{walking ? proto::kMotionRun : proto::kMotionWalk}));
        return;
    }
    case 1003: // Trace: the selected player
        if (player_.target.entity != 0 && player_.target.kind == game::TargetStatus::Kind::Player && player_.target.entity != view_.Self()) {
            autoRun_ = false;
            send(proto::Trace::kId, proto::Encode(proto::Trace{player_.target.entity}));
        }
        return;
    case 1004: // Auto Run: on or off
        autoRun_ = !autoRun_;
        autoRunNext_ = 0.0;
        if (autoRun_) send(proto::Trace::kId, proto::Encode(proto::Trace{0}));
        core::LogInfo("game: auto run {}", autoRun_ ? "on" : "off");
        return;
    case 1006: AskExchange(context); return;              // Exchange: with the selected player (PlayExchange.cpp)
    case 1007: hud_.OpenPage(hud::kPageParty); return;   // the party
    case 1008: AutoLoot(context); return;                 // Auto Looting (G)
    case 1009: OpenStall(context); return;                // Stall (PlayStall.cpp)
    case 1013: OpenStallNetwork(context); return;         // Stall network (hidden in 1.188's data; the key F)
    case 1011: {                                          // Helper: the helper's mark on or off (0x7402)
        const auto own = entityMarks_.find(view_.Self());
        const std::uint8_t marks = own != entityMarks_.end() ? own->second : 0;
        send(proto::SetMarks::kId, proto::Encode(proto::SetMarks{std::uint8_t(marks ^ proto::kMarkHelper)}));
        return;
    }
    case 1010: ToggleAlchemyWindow(hudContext_); return;  // Alchemy: its window (case 0x3F2)
    case 1012: hud_.Show(hud::kAutoPotion); return;      // the auto potion
    case 1017: ToggleFreePvpWindow(); return;             // Free PvP: its window
    case 1016: hud_.Show(hud::kPartyMatch); return;      // party matching
    case 1015: {                                         // Return to fortress
        const std::uint32_t mine = guild_.InGuild() ? guild_.Guild().id : 0;
        for (const proto::FortressRecord& f : fortress_.Fortresses()) {
            if (mine != 0 && (f.holder != 0 ? f.holder : f.owner) == mine) {
                shared_->session->SendGame(proto::FortressReturn::kId, proto::Encode(proto::FortressReturn{f.id}));
                return;
            }
        }
        player_.Post(game::GameMessage::Kind::General, gui_.data().Text("UIIT_MSG_FORT_PORTALSTONE_FAIL_03"), context.time);
        return;
    }
    case 4000: // the emotes: greeting 0, laugh 6, pokun 1, yes 5, rush 2, joy 3, no 4
    case 4001:
    case 4002:
    case 4003:
    case 4004:
    case 4005:
    case 4006: {
        static constexpr std::uint8_t kGestures[] = {0, 6, 1, 5, 2, 3, 4};
        autoRun_ = false;
        send(proto::Emote::kId, proto::Encode(proto::Emote{kGestures[action - 4000]}));
        return;
    }
    default: core::LogInfo("game: action {} is not built yet", action); return;
    }
}

// Auto Looting (the Action page's 1008, the key G): the nearest item on the ground within
// 50 of the player, asked for (the server walks there and picks it up; whose it may be is the server's).
void PlayScreen::AutoLoot(ScreenContext&) {
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr || replay_ || IsDead(*self)) return;
    const proto::FlatPosition me = proto::ToFlat(self->position);
    std::uint32_t best = 0;
    double nearest = 50.0;
    for (const auto& [id, e] : view_.Entities()) {
        if (e.kind != proto::EntityKind::GroundItem) continue;
        const proto::FlatPosition p = proto::ToFlat(e.position);
        const double d = std::hypot(p.x - me.x, p.z - me.z);
        if (d <= nearest) {
            nearest = d;
            best = id;
        }
    }
    if (best == 0) return;
    autoRun_ = false;
    shared_->session->PickUp(best);
}

// Auto Run (the Action page's 1004): on until a click, the icon again or death, the character runs
// on ahead the way the camera looks (ours: the original moves by a heading; ours asks for a point 200
// ahead every 0.4 s, and the server's walk stops at walls).
void PlayScreen::AutoRun(ScreenContext& context) {
    if (!autoRun_ || replay_) return;
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr || IsDead(*self)) {
        autoRun_ = false;
        return;
    }
    if (context.time < autoRunNext_) return;
    autoRunNext_ = context.time + 0.4;
    const proto::FlatPosition at = proto::ToFlat(self->position);
    const double ahead = 200.0;
    const proto::WorldPosition target =
        proto::FromFlat({at.x + std::sin(double(cameraYaw_)) * ahead, at.z + std::cos(double(cameraYaw_)) * ahead}, self->position.y);
    shared_->session->MoveTo(target);
}

// The helper marks over heads (SYSTEM_HELPERMARK), kept while a walker shows with the mark; again when it
// moves to another slot. The list's loop carries res\etc\helpermark.bsr at the walker's height + (0, 13, 0):
// a model without meshes whose own effect (system\system_helpermark.efp at its root + (0, 3, 0)) is the mark,
// so that effect is what is played there (the guild war's mark is the same: guild_war_mark.bsr).
void PlayScreen::ActionMarks(ScreenContext& context) {
    if (!world_ || !tables_ || !tables_->Usable()) return;
    gfx::EffectSystem& effects = world_->Effects();
    // What each one wears over its head: its free PvP team's cape (SYSTEM_FREEPVP_RED .. _YELLOW, in
    // place of the other marks), else the helper's mark.
    static constexpr const char* kCapes[] = {"SYSTEM_FREEPVP_RED", "SYSTEM_FREEPVP_GRAY", "SYSTEM_FREEPVP_BLUE", "SYSTEM_FREEPVP_WHITE",
                                             "SYSTEM_FREEPVP_YELLOW"};
    const auto listOf = [&](std::uint32_t id) -> std::string {
        if (const auto p = entityPvp_.find(id); p != entityPvp_.end() && p->second.team >= 1 && p->second.team <= proto::kFreePvpTeams) {
            return kCapes[p->second.team - 1];
        }
        const auto marks = entityMarks_.find(id);
        if (marks != entityMarks_.end() && (marks->second & proto::kMarkHelper) != 0) return "SYSTEM_HELPERMARK";
        // An NPC with something about the player's quests: the 1.188 client's kinds
        //  1 SYSTEM_EXCLAMATION_START, 2 _GOING, 3 _END.
        if (const auto mark = QuestMarkOf(id)) {
            return *mark == proto::QuestMark::End ? "SYSTEM_EXCLAMATION_END" : *mark == proto::QuestMark::Going ? "SYSTEM_EXCLAMATION_GOING" : "SYSTEM_EXCLAMATION_START";
        }
        return "";
    };
    for (auto it = markEffects_.begin(); it != markEffects_.end();) {
        const auto slot = slots_.find(it->first);
        if (slot != slots_.end() && slot->second == it->second.slot && listOf(it->first) == it->second.list) {
            ++it;
            continue;
        }
        for (const gfx::EffectSystem::Handle h : it->second.effects) effects.Stop(h);
        it = markEffects_.erase(it);
    }
    std::vector<std::uint32_t> wearers;
    for (const auto& [id, marks] : entityMarks_) wearers.push_back(id);
    for (const auto& [id, pvp] : entityPvp_) wearers.push_back(id);
    if (!quests_.Marks().empty()) {
        for (const auto& [id, e] : view_.Entities())
            if (e.kind == proto::EntityKind::Character && IsNpc(e) && QuestMarkOf(id)) wearers.push_back(id);
    }
    for (const std::uint32_t id : wearers) {
        if (markEffects_.contains(id)) continue;
        const std::string list = listOf(id);
        const auto* stages = list.empty() ? nullptr : tables_->skillAnimations.Stages(list);
        if (stages == nullptr) continue;
        const auto slot = slots_.find(id);
        if (slot == slots_.end() || !world_->StageReady(slot->second)) continue;
        MarkEffect& m = markEffects_[id];
        m.slot = slot->second;
        m.list = list;
        for (const assets::SkillEffectStage& stage : *stages) {
            for (const assets::ResourceData::ParticleModifier& p : CarriedEffects(context, stage.object)) {
                gfx::EffectSystem::Request request;
                request.path = p.effect;
                request.kind = gfx::EffectSystem::Kind::World;
                request.seconds = -1.0f; // while the mark is on
                request.anchor = StageEffects::Anchor(slot->second, stage.start, true);
                request.anchor.offset += glm::vec3(p.offset[0], p.offset[1], p.offset[2]);
                if (const gfx::EffectSystem::Handle h = effects.Start(request); h != 0) m.effects.push_back(h);
            }
        }
    }
}

// The quest mark over an NPC in sight (by its codename), none for others.
std::optional<proto::QuestMark> PlayScreen::QuestMarkOf(std::uint32_t entity) const {
    if (quests_.Marks().empty() || !tables_ || !tables_->Usable()) return std::nullopt;
    const game::WorldView::Entity* e = view_.Find(entity);
    if (e == nullptr || e->kind != proto::EntityKind::Character || !IsNpc(*e)) return std::nullopt;
    const assets::ObjectRow* row = tables_->characters.Find(e->look.model);
    return row != nullptr ? quests_.MarkOf(row->codename) : std::nullopt;
}

// A carried model's own effects (its .bsr's ambient particles), read once.
const std::vector<assets::ResourceData::ParticleModifier>& PlayScreen::CarriedEffects(ScreenContext& context, const std::string& model) {
    auto [it, fresh] = carriedEffects_.try_emplace(model);
    if (fresh) {
        assets::ResourceData resource;
        std::string error;
        if (const auto file = context.files.Read(model); file && assets::DecodeBsr(file->Bytes(), resource, error)) {
            for (const assets::ResourceData::ModifierSet& set : resource.modifierSets) {
                if (set.kind == 2) it->second.insert(it->second.end(), set.particles.begin(), set.particles.end());
            }
        }
    }
    return it->second;
}

// --- Free -------------------------------------------------------------------

void PlayScreen::ToggleFreePvpWindow() {
    if (hud_.Shown(hud::kFreePvp)) {
        hud_.Hide(hud::kFreePvp);
        return;
    }
    OpenFreePvpWindow(hudContext_);
}

// A foe in free PvP, as the server reckons it: both in teams, the teams apart or either the
// Giraffe, not of one party.
bool PlayScreen::FreePvpFoe(std::uint32_t other) const {
    const auto a = entityPvp_.find(view_.Self()), b = entityPvp_.find(other);
    if (a == entityPvp_.end() || b == entityPvp_.end() || a->second.team == 0 || b->second.team == 0) return false;
    if (player_.party.InParty() && player_.party.MemberByEntity(other) != nullptr) return false;
    return a->second.team != b->second.team || a->second.team == proto::kFreePvpTeams || b->second.team == proto::kFreePvpTeams;
}

void PlayScreen::HandleFreePvp(ScreenContext& context, const proto::EntityFreePvp& e) {
    FreePvpState& s = entityPvp_[e.entity];
    const bool joined = e.changing == 0 && s.team != e.team;
    s.team = e.team;
    s.to = e.to;
    s.changing = e.changing != 0;
    if (e.entity == view_.Self() && e.changing != 0) pvpChangeFrom_ = e.team, pvpChangeTo_ = e.to;
    if (e.team == 0 && e.changing == 0) entityPvp_.erase(e.entity);
    hudContext_.freePvpTeam = entityPvp_.contains(view_.Self()) ? entityPvp_[view_.Self()].team : 0;
    hudContext_.freePvpChanging = entityPvp_.contains(view_.Self()) && entityPvp_[view_.Self()].changing;
    if (e.entity == view_.Self() && joined) {
        // Ours: the window's line said as a system line (the team's description; out: "You have exited the Free PvP mode.").
        static constexpr const char* kTeams[] = {"UIIT_STT_FRPVP_RED", "UIIT_STT_FRPVP_GRAY", "UIIT_STT_FRPVP_BLUE", "UIIT_STT_FRPVP_WHITE",
                                                 "UIIT_STT_FRPVP_YELLOW"};
        const char* key = e.team >= 1 && e.team <= proto::kFreePvpTeams ? kTeams[e.team - 1] : "UIIT_MGS_ERR_FRPVP_OFF_CANCEL";
        player_.Post(game::GameMessage::Kind::General, gui_.data().Text(key), context.time);
        core::LogInfo("game: free PvP team {}", e.team);
    }
}

} // namespace sr::client
