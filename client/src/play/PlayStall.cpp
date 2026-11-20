// Stalls on the game screen: the Action page's Stall (1009: the stall window and its name
// box) and Stall Network (1013, hidden in 1.188's data, shown again; the key F), the
// booths of those around (the skin's model, else cj_store.bsr / euro_streetstall01.bsr by race,
// the owner in the vendor's clip, the stall's name over it), a click on a booth walking up to it and visiting,
// the server's news as system lines, and the test steps (of --guild and --fortress) that drive it all.

#include <cmath>
#include <cstdlib>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "play/PlayScreen.h"
#include "sr/core/Log.h"

namespace sr::client {

namespace proto = game::proto;
using Code = proto::StallCode;

namespace {
constexpr std::uint32_t kBoothKey = 0x40000000u; // the booths' ground-model keys (entity | this)
constexpr float kStallWalkTo = 60.0f;            // ours: a click on a booth walks this near, then visits
} // namespace

void PlayScreen::OpenStall(ScreenContext& context) {
    if (replay_) return;
    if (!stall_.In() && ActionBarRefuses(context.time)) return; // a job suit's change, a flag pole's action
    if (stall_.In()) {
        hud_.Show(hud::kStall);
        return;
    }
    // The name box first, "[name]'s stall." in it (with UIIT_STT_STALL_DEFAULT_TITLE); its OK opens it.
    HudContext* c = &hudContext_;
    AskGuildText(hudContext_, gui_.data().Text("UIIT_STT_INSERT_STALL_NAME"), proto::kStallNameLength, [c](std::string name) {
        if (c->stallRequest) c->stallRequest(proto::StallOpen::kId, proto::Encode(proto::StallOpen{std::move(name)}));
    }, FormatText(gui_.data().Text("UIIT_STT_STALL_DEFAULT_TITLE"), {player_.self.name}));
}

void PlayScreen::OpenStallNetwork(ScreenContext&) { hud_.Toggle(hud::kStallNetwork); }

void PlayScreen::HandleStall(ScreenContext& context, const proto::StallEvent& event) {
    const auto text = [this](const char* key) { return gui_.data().Text(key); };
    const auto post = [&](const std::string& line) { player_.Post(game::GameMessage::Kind::General, line, context.time); };
    // Those around: a booth up, renamed, or down (0x30B8 / 0x30BB / 0x30B9).
    if (const auto* around = std::get_if<proto::EntityStall>(&event)) {
        if (around->name.empty()) {
            entityStalls_.erase(around->entity);
        } else {
            entityStalls_[around->entity] = *around;
        }
        return;
    }
    if (std::holds_alternative<proto::StallView>(event)) {
        hud_.Show(hud::kStall);
        if (stall_.Owner() == view_.Self() || std::get<proto::StallView>(event).owner == view_.Self()) hud_.OpenPage(hud::kPageInventory);
    }
    if (const auto* page = std::get_if<proto::NetworkPage>(&event); page != nullptr && page->code == Code::NotTownSearch) {
        hud_.Hide(hud::kStallNetwork); // the client's own rule: the window goes
    }
    for (const game::StallView::Line& line : stall_.Apply(event, view_.Self())) {
        using Kind = game::StallView::Line::Kind;
        switch (line.kind) {
        case Kind::Refused:
            if (const char* key = StallCodeKey(line.code)) post(text(key));
            core::LogInfo("game: stall: refused (0x{:X})", std::uint16_t(line.code));
            break;
        case Kind::Opened:
            post(text(line.open ? "UIIT_STT_STALL_CONDITION_START" : "UIIT_STT_STALL_CONDITION_END"));
            core::LogInfo("game: stall {}", line.open ? "open for business" : "being modified");
            break;
        case Kind::Registered:
            if (line.code == Code::Ok) {
                post(text("UIIT_MSG_WARENETWORK_REGIST_SUCCESS"));
            } else {
                hud_.Confirm("UIIT_STT_CONFIRM_BOX", {text("UIIT_MSG_WARENETWORK_USE_CITY")}, [](bool) {});
            }
            core::LogInfo("game: stall {} in the network", line.code == Code::Ok ? "registered" : "not registered");
            break;
        case Kind::Sold: {
            // Through the network: "[%s] has purchased [%s] through stall network." and the commission paid. A sale in
            // the stall shows in its window (the item gone).
            const assets::ItemRow* row = tables_ ? tables_->items.Find(line.item.item) : nullptr;
            if (line.network) post(FormatText(text("UIIT_MSG_WARENETWORK_BUY_SUCCESS"), {line.name, row != nullptr ? text(row->nameKey.c_str()) : std::string()}));
            if (line.gold > 0) post(FormatText(text("UIIT_MSG_WARENETWORK_CHARGE"), {GoldText(line.gold)}));
            core::LogInfo("game: stall: sold to {}{} (commission {})", line.name, line.network ? " through the network" : "", line.gold);
            break;
        }
        case Kind::Bought:
            if (line.code == Code::Ok) {
                const assets::ItemRow* row = tables_ ? tables_->items.Find(line.item.item) : nullptr;
                if (line.network) post(FormatText(text("UIIT_MSG_WARENETWORK_BUY_SUCCESS"), {player_.self.name, row != nullptr ? text(row->nameKey.c_str()) : std::string()}));
                core::LogInfo("game: stall: bought {} at place {}{}", line.item.item, line.item.slot, line.network ? " through the network" : "");
            } else {
                if (const char* key = StallCodeKey(line.code)) post(text(key));
                core::LogInfo("game: stall: buy refused (0x{:X})", std::uint16_t(line.code));
            }
            break;
        case Kind::Left:
            hud_.Hide(hud::kStall);
            if (line.code != Code::Ok) {
                if (const char* key = StallCodeKey(line.code)) post(text(key));
            }
            core::LogInfo("game: stall: out of {}'s stall (0x{:X})", line.name, std::uint16_t(line.code));
            break;
        }
    }
}

// The booths of those around: at the owner, turned with it; the skin's model or the plain
// booth by race. Gone with the stall or the owner.
void PlayScreen::SyncBooths() {
    if (!world_ || !tables_ || !tables_->Usable()) return;
    for (auto it = boothsShown_.begin(); it != boothsShown_.end();) {
        const auto stall = entityStalls_.find(it->first);
        if (stall == entityStalls_.end() || stall->second.skin != it->second || view_.Find(it->first) == nullptr) {
            world_->ClearGroundItem(kBoothKey | it->first);
            it = boothsShown_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [entity, stall] : entityStalls_) {
        const game::WorldView::Entity* e = view_.Find(entity);
        if (e == nullptr || boothsShown_.contains(entity)) continue;
        std::string model;
        if (stall.skin != 0) {
            if (const assets::ItemRow* skin = tables_->items.Find(stall.skin); skin != nullptr && !skin->modelPath.empty()) model = "res\\" + skin->modelPath;
        }
        if (model.empty()) {
            const assets::ObjectRow* row = tables_->characters.Find(e->look.model);
            const bool europe = row != nullptr && row->codename.rfind("CHAR_EU", 0) == 0;
            model = europe ? "res\\item\\europe\\item\\euro_streetstall01.bsr" : "res\\item\\china\\item\\cj_store.bsr";
        }
        const glm::vec3 p = world_->OriginRelative(e->position);
        world_->SetGroundItem(kBoothKey | entity, model, p.x, p.y, p.z, e->yaw);
        boothsShown_[entity] = stall.skin;
    }
}

// A click on a player whose booth stands: walked up to it, then visited (as an NPC's talk).
bool PlayScreen::ClickBooth(ScreenContext& context, std::uint32_t entity) {
    if (replay_ || entity == view_.Self() || !entityStalls_.contains(entity)) return false;
    pendingStall_ = entity;
    pendingStallUntil_ = context.time + 8.0;
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const game::WorldView::Entity* it = view_.Find(entity);
    if (self != nullptr && it != nullptr) {
        const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(it->position);
        const double dx = a.x - b.x, dz = a.z - b.z, length = std::max(1.0, std::hypot(dx, dz));
        if (length > proto::kStallReach) {
            shared_->session->MoveTo(proto::FromFlat({b.x + dx / length * kStallWalkTo, b.z + dz / length * kStallWalkTo}, it->position.y));
        }
    }
    return true;
}

void PlayScreen::StallVisits(double now) {
    if (pendingStall_ == 0 || replay_) return;
    if (StandingNear(pendingStall_, proto::kStallReach)) {
        shared_->session->SendGame(proto::StallVisit::kId, proto::Encode(proto::StallVisit{pendingStall_}));
        pendingStall_ = 0;
    } else if (now > pendingStallUntil_) {
        pendingStall_ = 0;
    }
}

// The test steps: stall:NAME (opens one), stall-put:SLOT,PLACE,PRICE, stall-take:SLOT, stall-open[:1 registered],
// stall-modify, stall-greet:TEXT, stall-chat:TEXT, stall-close, visit:NAME (walks to its booth and visits),
// stall-buy:SLOT, stall-leave, use-item:PLACE, network (the window), search:CLASS,TYPE[,DEGREE] (indices of the combos), page:N,
// network-buy:N (the shown page's row N, through the window's question answered yes).
bool PlayScreen::StallScriptStep(ScreenContext& context, const std::string& verb, const std::string& arg) {
    const auto send = [&](const auto& message) {
        using T = std::decay_t<decltype(message)>;
        if (!replay_) shared_->session->SendGame(T::kId, proto::Encode(message));
    };
    const auto numbers = [&] {
        std::vector<std::uint64_t> out;
        for (std::size_t at = 0; at <= arg.size();) {
            const std::size_t comma = arg.find(',', at);
            out.push_back(std::strtoull(arg.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str(), nullptr, 10));
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        return out;
    };
    if (verb == "stall") {
        send(proto::StallOpen{arg.empty() ? FormatText(gui_.data().Text("UIIT_STT_STALL_DEFAULT_TITLE"), {player_.self.name}) : arg});
    } else if (verb == "stall-put") {
        const auto n = numbers();
        if (n.size() >= 3) send(proto::StallPut{std::uint8_t(n[0]), std::uint8_t(n[1]), n[2]});
    } else if (verb == "stall-take") {
        send(proto::StallTake{std::uint8_t(std::strtoul(arg.c_str(), nullptr, 10))});
    } else if (verb == "stall-open") {
        send(proto::StallSetOpen{1, std::uint8_t(arg == "1" ? 1 : 0)});
    } else if (verb == "stall-modify") {
        send(proto::StallSetOpen{0, 0});
    } else if (verb == "stall-greet") {
        send(proto::StallSetText{1, arg});
    } else if (verb == "stall-chat") {
        send(proto::StallChat{arg});
    } else if (verb == "stall-close") {
        send(proto::StallClose{});
    } else if (verb == "visit") {
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind == proto::EntityKind::Character && e.look.name == arg && id != view_.Self()) {
                if (!ClickBooth(context, id)) core::LogWarning("game: visit: {} has no stall", arg);
                return true;
            }
        }
        core::LogWarning("game: visit: {} is not in sight", arg);
    } else if (verb == "stall-buy") {
        send(proto::StallBuy{std::uint8_t(std::strtoul(arg.c_str(), nullptr, 10))});
    } else if (verb == "attack-player") {
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind == proto::EntityKind::Character && e.look.name == arg && id != view_.Self()) {
                SelectPlayer(id);
                if (!replay_) shared_->session->Attack(id);
                return true;
            }
        }
        core::LogWarning("game: attack-player: {} is not in sight", arg);
    } else if (verb == "pvp") {
        send(proto::FreePvp{std::uint8_t(std::strtoul(arg.c_str(), nullptr, 10))}); // a free PvP team (0 none)
    } else if (verb == "use-item") {
        // A bag place (a stall decoration, ...); a murderer's return scroll refused first, as the player's use is (PlayPk.cpp).
        if (const auto place = std::uint8_t(std::strtoul(arg.c_str(), nullptr, 10)); !MurdererScrollRefused(place, context.time)) send(proto::UseItem{place});
    } else if (verb == "reverse-box") {
        // The reverse return scroll used as a player does (its box), then
        // reverse-choose:N (1 recall, 2 death, 3 the map) and reverse-point:ID (the map's pick).
        if (const std::uint8_t place = ReverseScrollPlace(); place != 0) UseReverseReturn(place);
        else core::LogWarning("game: reverse-box: no reverse return scroll in the bag");
    } else if (verb == "reverse-choose") {
        if (const std::uint8_t place = ReverseScrollPlace(); place != 0) {
            if (ui::Interface* ui = hud_.interface()) {
                for (ui::Widget* w : ui->Windows()) {
                    if (dynamic_cast<ReverseReturnBox*>(w) != nullptr) ui->Close(w);
                }
            }
            ReverseChoice(place, int(std::strtol(arg.c_str(), nullptr, 10)));
        }
    } else if (verb == "reverse-point") {
        // The map's pick: as a click on a marker's destination (the map closed, the ID sent).
        if (const std::uint8_t place = ReverseScrollPlace(); place != 0) {
            hud_.Hide(hud::kWorldMap);
            if (!replay_) shared_->session->UseItem(place, proto::kReverseToPoint, std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10)));
        }
    } else if (verb == "stall-leave") {
        send(proto::StallLeave{});
    } else if (verb == "network") {
        hud_.Show(hud::kStallNetwork);
    } else if (verb == "search") {
        const auto n = numbers();
        if (ui::Interface* ui = hud_.interface(); ui != nullptr && n.size() >= 2) {
            StallNetworkSearch(*ui, int(n[0]), int(n[1]), n.size() > 2 ? int(n[2]) : 0);
        }
    } else if (verb == "page") {
        send(proto::NetworkPageAsk{std::uint16_t(std::strtoul(arg.c_str(), nullptr, 10))});
    } else if (verb == "network-buy") {
        if (ui::Interface* ui = hud_.interface()) {
            StallNetworkBuy(*ui, int(std::strtoul(arg.c_str(), nullptr, 10)));
            hud_.AnswerConfirm(true);
        }
    } else {
        return false;
    }
    return true;
}

} // namespace sr::client
