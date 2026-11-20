// The fortress war on the game screen: the session's fortress events
// into game::FortressView, their system lines (the 1.188 client's texts), the official's window, and
// the test options that drive the war's requests.

#include <algorithm>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <random>
#include <vector>

#include "play/PlayScreen.h"
#include "sr/core/Log.h"
#include "sr/game/Skills.h"

namespace sr::client {

namespace proto = game::proto;

void PlayScreen::FortressOptions(ScreenContext& context) {
    // --fortress "step;step;..." (walk-official, window, apply:FORTRESS, press:FORTRESS (its row's
    // button in the window, answered yes), cancel:FORTRESS, dev:startreq|endreq|startwar|endwar,
    // goto:POINT, goto-fortress:FORTRESS, attack:heart|tower|gate, attack-player:NAME, hunt:SECONDS,
    // siege:FORTRESS, battle:FORTRESS (all of its war), until-war:FORTRESS, until-peace:FORTRESS, until-held:FORTRESS, until-owner:FORTRESS,
    // select:heart|tower|gate, action:ID (an Action page icon), drop:PLACE (an item to the ground), wait;
    // the tax: dev:starttax|endtax, walk-manager:FORTRESS (a developer's hop beside its
    // manager), tax (the manager's "Tax management" row), tax-rate:N (the slider, then Change), tax-levy[:N] (Levying,
    // the amount typed), tax-yes | tax-no (the box answered)), one each
    // --fortress-step seconds from --fortress-after seconds in the world.
    for (std::string steps = context.config.GetString("fortress", ""); !steps.empty();) {
        const std::size_t semi = steps.find(';');
        fortressScript_.push_back(steps.substr(0, semi));
        steps = semi == std::string::npos ? std::string() : steps.substr(semi + 1);
    }
    fortressAfter_ = context.config.GetDouble("fortress-after", 3.0);
    fortressStep_ = context.config.GetDouble("fortress-step", 2.0);
}

bool PlayScreen::FortressFoe(std::uint32_t entity, const game::WorldView::Entity& e) const {
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr || !guild_.InGuild()) return false;
    const std::uint32_t war = fortress_.WarArea(self->position.region);
    if (war == 0 || fortress_.WarArea(e.position.region) != war) return false;
    const proto::EntityGuild* tag = guild_.EntityTag(entity);
    return tag == nullptr || (tag->guild != guild_.Guild().id && (tag->unionId == 0 || tag->unionId != guild_.Guild().unionId));
}

bool PlayScreen::OwnStructure(std::uint32_t entity) const {
    const proto::EntityStructure* s = fortress_.Structure(entity);
    if (s == nullptr || !guild_.InGuild() || s->owner == 0) return false;
    if (s->owner == guild_.Guild().id) return true;
    const std::uint32_t unionId = guild_.Guild().unionId;
    return unionId != 0 && guild_.Union().id == unionId &&
           std::any_of(guild_.Union().guilds.begin(), guild_.Union().guilds.end(), [&](const auto& g) { return g.id == s->owner; });
}

bool PlayScreen::PlayerIsEnemy(std::uint32_t entity) const {
    const game::WorldView::Entity* e = view_.Find(entity);
    if (e == nullptr || entity == view_.Self() || IsDead(*e)) return false;
    const proto::EntityGuild* tag = guild_.EntityTag(entity);
    return (tag != nullptr && guild_.AtWarWith(tag->guild)) || FortressFoe(entity, *e) || JobFoe(entity) || FlagFoe(entity);
}

// A job's foe, as the server reckons it: a thief against
// a trader or a hunter, both in their suits, not of one party (the server checks the battlefield and the 10 s lock).
bool PlayScreen::JobFoe(std::uint32_t other) const {
    const auto a = entityJobs_.find(view_.Self()), b = entityJobs_.find(other);
    if (a == entityJobs_.end() || b == entityJobs_.end() || other == view_.Self()) return false;
    if (player_.party.InParty() && player_.party.MemberByEntity(other) != nullptr) return false;
    return game::IsHostileJob(a->second.state, b->second.state);
}

std::uint32_t PlayScreen::RandomAttackSkill(double now) const {
    if (!tables_ || !tables_->Usable()) return 0;
    if (const game::ItemInstance* weapon = player_.inventory.At(game::slot::kWeapon)) {
        if (const assets::ItemRow* row = tables_->items.Find(weapon->refId); row != nullptr && row->typeId[3] == 16) return 0; // a fortress hammer has none
    }
    std::vector<std::uint32_t> ready;
    for (const std::uint32_t id : player_.skills) {
        const assets::SkillRow* row = tables_->skills.Find(id);
        if (row == nullptr) continue;
        // The bots' swords' own, on a target (a dash wants a point of the ground, not a target).
        if (!row->codename.starts_with("SKILL_CH_SWORD")) continue;
        if (assets::FindSkillParam(*row, assets::SkillTag("tele")) || assets::FindSkillParam(*row, assets::SkillTag("tel2"))) continue;
        const game::SkillTraits traits = game::TraitsOf(*row);
        if (!traits.attack || traits.passive || traits.toggle || traits.target != game::SkillTraits::Target::Enemy) continue;
        const auto wait = player_.skillCooling.find(row->group);
        if (wait != player_.skillCooling.end() && wait->second.end > now) continue;
        ready.push_back(id);
    }
    if (ready.empty()) return 0;
    static std::mt19937 random(std::random_device{}());
    return ready[random() % ready.size()];
}

void PlayScreen::BotSkill(ScreenContext& context, std::uint32_t target) {
    if (context.time - botSkillAt_ < 1.5) return;
    if (const std::uint32_t skill = RandomAttackSkill(context.time)) {
        botSkillAt_ = context.time;
        shared_->session->UseSkill(skill, target);
    }
}

void PlayScreen::HandleFortress(ScreenContext& context, const proto::FortressEvent& event) {
    if (const auto* list = std::get_if<proto::FortressStructureList>(&event)) {
        for (const proto::FortressStructureRecord& r : list->structures) {
            core::LogInfo("game: fortress: structure {} ({}): {} of {} HP, state {}, repair {} gold, upgrade {} gold {} GP level {}, wait {} min",
                          r.zone, r.refObjId, r.hp, r.maxHp, r.state, r.repairCost, r.upgradeGold, r.upgradePoints, int(r.upgradeLevel), r.waitMinutes);
        }
    }
    // A new tax rate (kind 7): the NPC or gate talked to shows it (its waiver stays the server's word).
    if (const auto* n = std::get_if<proto::FortressNotice>(&event); n != nullptr && n->kind == proto::FortressNoticeKind::TaxRate && npcTax_.fortress == n->fortress) {
        const std::int16_t rate = std::int16_t(std::uint16_t(n->value));
        npcTax_.rate = rate;
    }
    if (const auto* h = std::get_if<proto::StructureHealth>(&event)) {
        if (h->entity == player_.target.entity) player_.target.hp = h->hp; // a repair kit's pulse on the selected structure
    }
    if (const auto* s = std::get_if<proto::EntityStructure>(&event)) {

        core::LogInfo("game: fortress: {} is a structure of fortress {} (kind {}, owner {}, {} HP)", s->entity, s->fortress, int(s->kind), s->owner, s->maxHp);
    } else if (const auto* l = std::get_if<proto::FortressList>(&event)) {
        for (const proto::FortressRecord& f : l->fortresses) {
            // The holder's emblem for the world map's crest box.
            if (f.crest != 0) guild_.AskCrest(proto::kCrestGuild, f.holder != 0 ? f.holder : f.owner, f.crest);
            core::LogInfo("game: fortress: {} owner '{}'{}{}", f.nameKey, f.ownerName, f.holderName.empty() ? "" : ", held by '" + f.holderName + "'",
                          f.atWar ? " (at war)" : "");
        }
    }
    for (const game::FortressView::Line& line : fortress_.Apply(event)) {
        const std::string shown = FortressNoticeText(hudContext_, line);
        core::LogInfo("game: fortress: {}", shown);
        player_.Post(game::GameMessage::Kind::General, shown, context.time);
    }
    for (const proto::FortressAnswer& a : fortress_.TakeAnswers()) {
        core::LogInfo("game: fortress: request {} for {}: {} 0x{:04x}", int(a.action), a.fortress, a.ok ? "ok" : "refused", a.error);
        // A marker's use answered: the markers go.
        if (a.action == proto::FortressAction::UseItem && zoneWaiting_) ClearZoneMarkers();
    }
    // The manager's "Jobs for Fortress War" (CIFFortressBusiness) and "About Guild" (CNIFGuildIntroWnd), the smith's or the
    // trainer's window (CIFFortressMakeItemWnd): HudFortressWork.cpp.
    if (const auto s = fortress_.TakeSchedule()) {
        core::LogInfo("game: fortress: the war's schedule: last {}-{} {}:{}, next {}-{} {}:{}, {}", int(s->last[0]), int(s->last[1]), int(s->last[3]), int(s->last[4]),
                      int(s->next[0]), int(s->next[1]), int(s->next[3]), int(s->next[4]), s->applied ? "applied" : "none applied");
        OpenFortressBusiness(hudContext_, *s);
    }
    if (fortress_.TakeDungeonShow()) {
        const proto::FortressDungeon& d = fortress_.Dungeon();
        core::LogInfo("game: fortress: the Cave of Discipline: {} made today, {}, {} inside{}", int(d.count), d.open ? "open" : "closed", int(d.players),
                      d.admitted ? ", admitted" : "");
    }
    if (fortress_.TakeForgeShow()) {
        const proto::FortressForge& w = fortress_.Forge();
        const assets::ItemRow* row = w.has && tables_ && tables_->Usable() ? tables_->items.Find(w.item) : nullptr;
        const std::int64_t left = w.finishes - std::int64_t(std::time(nullptr));
        core::LogInfo("game: fortress: the {}'s work: {}", w.side == 0 ? "smith" : "trainer",
                      !w.has ? std::string("none")
                             : (row != nullptr ? row->codename : std::to_string(w.item)) + " x" + std::to_string(w.count) +
                                   (w.done ? std::string(", done") : ", " + std::to_string(std::max<std::int64_t>(0, left)) + " s left"));
        OpenFortressMakeItem(hudContext_, w, std::exchange(hudContext_.forgeAsked, false));
    }
    if (const auto intro = fortress_.TakeIntroduction()) {
        core::LogInfo("game: fortress: About Guild: {}", intro->text);
        OpenGuildIntroduction(hudContext_, *intro, hudContext_.fortressNpc);
    }
    // The official's window, asked open by its row.
    if (fortress_.TakeApplyShow()) OpenFortressApply(hudContext_);
    if (fortress_.TakeStructuresShow()) OpenFortressMap(hudContext_);
    if (fortress_.TakeTaxShow()) OpenTaxManagement(hudContext_);
}

bool PlayScreen::UseFortressItem(std::uint8_t place) {
    if (replay_ || !tables_ || !tables_->Usable() || !shared_ || !shared_->session) return false;
    const game::ItemInstance* item = player_.inventory.At(place);
    const assets::ItemRow* row = item != nullptr ? tables_->items.Find(item->refId) : nullptr;
    if (row == nullptr || row->typeId[0] != 3 || row->typeId[1] != 3) return false;
    const auto& t = row->typeId;
    const bool groundPoint = t[2] == 3 && (t[3] == 6 || t[3] == 7 || t[3] == 9);
    const bool manual = t[2] == 3 && t[3] == 8;
    const bool onTarget = (t[2] == 1 && t[3] == 10) || (t[2] == 6 && t[3] == 2);
    if (!groundPoint && !manual && !onTarget) return false;
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr) return true;
    // The fortress whose area the player stands in (0: none).
    std::uint32_t fortress = 0;
    for (const proto::FortressRecord& f : fortress_.Fortresses()) {
        if (std::find(f.regions.begin(), f.regions.end(), self->position.region) != f.regions.end()) fortress = f.id;
    }
    // The stock client's checks: Param2 the place (1 in a fortress's world, 2 outside), Param3 the time
    // (1 in the war, 2 outside it), Param5 the positions that may (0: anyone).
    const auto refuse = [&](game::FortressError e) {
        if (const char* key = game::FortressErrorKey(std::uint16_t(e)); key != nullptr && *key != 0) {
            player_.Post(game::GameMessage::Kind::General, hudContext_.Text(key), 0.0);
        }
        core::LogInfo("game: fortress: the item {} refused here (0x{:04x})", row->codename, std::uint16_t(e));
        return true;
    };
    const int placeRule = row->params[1], timeRule = row->params[2], mask = row->params[4];
    const bool atWar = fortress != 0 && fortress_.WarOn() && fortress_.WarArea(self->position.region) == fortress;
    if (placeRule == 1 && fortress == 0) return refuse(game::FortressError::ItemNotInFortress);
    if (placeRule == 2 && fortress != 0) return refuse(game::FortressError::ItemInFortress);
    if (timeRule == 1 && !atWar) return refuse(game::FortressError::ItemNotInWar);
    if (timeRule == 2 && fortress_.WarOn()) return refuse(game::FortressError::ItemInWar);
    const proto::GuildMemberRecord* me = guild_.Self();
    if (mask != 0 && (me == nullptr || (me->position & mask) == 0)) return refuse(game::FortressError::ItemPosition);
    auto& session = *shared_->session;
    if (onTarget) {
        const std::uint32_t target = player_.target.entity;
        if (target == 0) return refuse(game::FortressError::ItemWrongTarget);
        proto::FortressUseItem m{place, fortress, 0, self->position.region, self->position.x, self->position.y, self->position.z, target};
        session.SendGame(proto::FortressUseItem::kId, proto::Encode(m));
        return true;
    }
    if (manual) {
        // The stock client's: a barricade by one not of the holder's guild 0x18E4, a camp by a defender
        // 0x18E3, then the free zones in reach marked (none: 0x18D8); the click on one sends the use with its zone.
        const bool camp = row->paramTexts[0].find("HEADQUARTERS") != std::string::npos;
        const proto::FortressRecord* f = fortress_.Fortress(fortress);
        const std::uint32_t holder = f != nullptr ? (f->holder != 0 ? f->holder : f->owner) : 0u;
        const std::uint32_t mine = guild_.InGuild() ? guild_.Guild().id : 0u;
        if (!camp && (mine == 0 || mine != holder)) return refuse(game::FortressError::ItemAttacker);
        if (camp && mine != 0 && mine == holder) return refuse(game::FortressError::ItemDefender);
        if (!ShowZoneMarkers(place, *row, fortress)) return refuse(game::FortressError::ItemTooFar);
        return true;
    }
    fortressPick_ = place; // the next ground click is its point
    core::LogInfo("game: fortress: {} waits for its ground point", row->codename);
    return true;
}

bool PlayScreen::ShowZoneMarkers(std::uint8_t place, const assets::ItemRow& manual, std::uint32_t fortress) {
    ClearZoneMarkers();
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    const proto::FortressRecord* f = fortress_.Fortress(fortress);
    const assets::WorldMapData* data = gui_.data().WorldMap();
    if (self == nullptr || f == nullptr || data == nullptr || !tables_ || !tables_->Usable() || markerFiles_ == nullptr) return false;
    // The manual's kind of zone (its Param1: STRUCTURE_BARRICADE_01 / STRUCTURE_HEADQUARTERS_01) and its reach: Range (itemdata
    // col 94) less 5, in 3D.
    const bool camp = manual.paramTexts[0].find("HEADQUARTERS") != std::string::npos;
    const double reach = double(manual.Stat(94)) - 5.0;
    const proto::FlatPosition me = proto::ToFlat(self->position);
    for (const assets::FortressZoneRow& z : data->zones) {
        if (!z.placed || (z.event == "HEADQUARTERS") != camp) continue;
        if (std::find(f->regions.begin(), f->regions.end(), z.region) == f->regions.end()) continue;
        const proto::WorldPosition at{z.region, z.x, z.y, z.z};
        const proto::FlatPosition p = proto::ToFlat(at);
        const double dy = double(at.y) - double(self->position.y);
        if (std::sqrt((p.x - me.x) * (p.x - me.x) + (p.z - me.z) * (p.z - me.z) + dy * dy) > reach) continue;
        // Taken: a structure standing on the zone's point (the stock compares the objects' places with it).
        bool taken = false;
        for (const auto& [id, e] : view_.Entities()) {
            if (fortress_.Structure(id) == nullptr) continue;
            const proto::FlatPosition q = proto::ToFlat(e.position);
            if (std::hypot(q.x - p.x, q.z - p.z) < 1.0) taken = true;
        }
        if (taken) continue;
        // Its look: the zone's own structure; a camp's foundation (_00) shows the camp it becomes (_01; the stock looks it up
        // through the structure's next level; ours by its name).
        std::string code = z.structure;
        if (camp && code.ends_with("_00")) code.replace(code.size() - 3, 3, "_01");
        const assets::ObjectRow* look = tables_->characters.Find(code);
        if (look == nullptr) continue;
        ZoneMarker m;
        m.zone = z.id;
        m.at = at;
        m.yaw = z.yaw;
        m.model = assets::CharacterTable::ResolveModel(*markerFiles_, *look);
        m.height = float(look->scalePercent) / 100.0f;
        if (m.model.empty()) continue;
        zoneMarkers_.push_back(std::move(m));
        if (zoneMarkers_.size() >= kMarkerSlots) break;
    }
    if (zoneMarkers_.empty()) return false;
    zonePlace_ = place;
    zoneFortress_ = fortress;
    zoneWaiting_ = false;
    zoneSince_ = zoneNow_;
    std::string where;
    for (const ZoneMarker& m : zoneMarkers_) where += std::format(" {} ({:.0f},{:.0f},{:.0f})", m.zone, m.at.x, m.at.y, m.at.z);
    core::LogInfo("game: fortress: {} marks {} free {} zones from {:.0f},{:.0f},{:.0f}:{}", manual.codename, zoneMarkers_.size(), camp ? "war camp" : "barricade",
                  self->position.x, self->position.y, self->position.z, where);
    return true;
}

void PlayScreen::ClearZoneMarkers() {
    zoneMarkers_.clear();
    zoneWaiting_ = false;
}

void PlayScreen::PlaceZoneMarkers(ScreenContext& context) {
    markerFiles_ = &context.files;
    zoneNow_ = context.time;
    if (!world_) return;
    // An answer never coming (a lost connection): put away after 10 s.
    if (zoneWaiting_ && zoneNow_ - zoneSince_ > 10.0) ClearZoneMarkers();
    for (std::size_t i = 0; i < zoneMarkers_.size(); ++i) {
        const ZoneMarker& m = zoneMarkers_[i];
        const glm::vec3 at = world_->OriginRelative(m.at);
        WorldScreen::StageLook look;
        look.model = m.model;
        look.height = m.height;
        world_->SetStage(kViewSlot + 1 + i, look, at.x, at.y, at.z, m.yaw);
    }
    for (std::size_t i = zoneMarkers_.size(); i < zoneShown_; ++i) world_->ClearStage(kViewSlot + 1 + i);
    zoneShown_ = zoneMarkers_.size();
}

bool PlayScreen::ClickZoneMarker(float x, float y, float width, float height) {
    if (zoneMarkers_.empty() || zoneWaiting_) return false;
    std::size_t hit = zoneMarkers_.size();
    float best = 1e9f;
    for (std::size_t i = 0; i < zoneMarkers_.size() && i < zoneShown_; ++i) {
        float distance = 0.0f;
        if (world_->RayHitsStage(x, y, width, height, kViewSlot + 1 + i, distance) && distance < best) {
            best = distance;
            hit = i;
        }
    }
    if (hit == zoneMarkers_.size()) {
        // Elsewhere: the markers go and the click goes on.
        for (std::size_t i = 0; i < zoneMarkers_.size(); ++i) {
            float px = 0.0f, py = 0.0f;
            const bool on = world_->Project(zoneMarkers_[i].at, 4.0f, width, height, px, py);
            glm::mat4 placed(1.0f);
            const bool staged = world_->StageBone(kViewSlot + 1 + i, {}, placed);
            core::LogInfo("game: fortress: the click at {:.0f},{:.0f} misses the zone {}'s marker ({}: {:.0f},{:.0f}; staged {} at {:.1f},{:.1f},{:.1f})", x, y,
                          zoneMarkers_[i].zone, on ? "on the screen" : "off it", px, py, staged, placed[3].x, placed[3].y, placed[3].z);
        }
        core::LogInfo("game: fortress: the zone markers put away");
        ClearZoneMarkers();
        return false;
    }
    // A marker: the others go, the manual is used on its zone, and it waits for the answer.
    const ZoneMarker chosen = zoneMarkers_[hit];
    zoneMarkers_ = {chosen};
    zoneWaiting_ = true;
    zoneSince_ = zoneNow_;
    core::LogInfo("game: fortress: the zone {} picked", chosen.zone);
    if (!replay_ && shared_ && shared_->session) {
        proto::FortressUseItem m{zonePlace_, zoneFortress_, chosen.zone, chosen.at.region, chosen.at.x, chosen.at.y, chosen.at.z};
        shared_->session->SendGame(proto::FortressUseItem::kId, proto::Encode(m));
    }
    return true;
}

bool PlayScreen::FortressPicked(const proto::WorldPosition& at) {
    const std::uint8_t place = *fortressPick_;
    fortressPick_.reset();
    const game::WorldView::Entity* self = view_.Find(view_.Self());
    if (self == nullptr || replay_ || !shared_ || !shared_->session) return false;
    // A point too near the player (under 0.5) is refused there (0x18D9).
    const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(at);
    if (std::hypot(a.x - b.x, a.z - b.z) < 0.5) {
        player_.Post(game::GameMessage::Kind::General, hudContext_.Text(game::FortressErrorKey(std::uint16_t(game::FortressError::ItemNotWalkable))), 0.0);
        return false;
    }
    std::uint32_t fortress = 0;
    for (const proto::FortressRecord& f : fortress_.Fortresses()) {
        if (std::find(f.regions.begin(), f.regions.end(), at.region) != f.regions.end()) fortress = f.id;
    }
    proto::FortressUseItem m{place, fortress, 0, at.region, at.x, at.y, at.z};
    // A guard's and a tablet's point asks first (UIIT_MSG_FORT_GUARD_SET_WINDOW,
    // _SUMMONMON_SET_WINDOW); a flag goes at once.
    const game::ItemInstance* item = player_.inventory.At(place);
    const assets::ItemRow* row = item != nullptr && tables_ && tables_->Usable() ? tables_->items.Find(item->refId) : nullptr;
    const int kind = row != nullptr ? row->typeId[3] : 0;
    // A point beyond the item's Range (itemdata col 94) less 5, in 3D, is refused there too (0x18D8).
    if (row != nullptr && row->Stat(94) > 0.0f) {
        const double dy = double(at.y) - double(self->position.y);
        if (std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z) + dy * dy) > double(row->Stat(94)) - 5.0) {
            player_.Post(game::GameMessage::Kind::General, hudContext_.Text(game::FortressErrorKey(std::uint16_t(game::FortressError::ItemTooFar))), 0.0);
            return false;
        }
    }
    if (kind == 6 || kind == 7) {
        auto* session = shared_->session.get();
        hud_.Confirm("UIIT_STT_CONFIRM_BOX", {hudContext_.Text(kind == 7 ? "UIIT_MSG_FORT_GUARD_SET_WINDOW" : "UIIT_MSG_FORT_SUMMONMON_SET_WINDOW")}, [session, m](bool yes) {
            if (yes) session->SendGame(proto::FortressUseItem::kId, proto::Encode(m));
        });
        return true;
    }
    shared_->session->SendGame(proto::FortressUseItem::kId, proto::Encode(m));
    return true;
}

void PlayScreen::FortressScript(ScreenContext& context) {
    if (fortressScript_.empty()) return;
    context.holdCapture = true; // a capture waits for the last step (--capture-wait frames at most)
    if (fortressNext_ < 0.0) fortressNext_ = context.time + fortressAfter_;
    if (context.time < fortressNext_) return;
    fortressNext_ = context.time + fortressStep_;
    const std::string step = fortressScript_.front();
    fortressScript_.erase(fortressScript_.begin());
    const std::size_t colon = step.find(':');
    const std::string verb = step.substr(0, colon), arg = colon == std::string::npos ? std::string() : step.substr(colon + 1);
    auto& session = *shared_->session;
    const auto send = [&](const auto& m) { session.SendGame(std::decay_t<decltype(m)>::kId, proto::Encode(m)); };
    // The nearest fortress official in sight (its codename *_FORTRESS_OFFICIAL).
    const auto official = [&]() -> std::uint32_t {
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (self == nullptr || !tables_ || !tables_->Usable()) return 0;
        const proto::FlatPosition me = proto::ToFlat(self->position);
        std::uint32_t best = 0;
        double nearest = 1e18;
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind != proto::EntityKind::Character || !IsNpc(e)) continue;
            const assets::ObjectRow* row = tables_->characters.Find(e.look.model);
            if (row == nullptr || !row->codename.ends_with("_FORTRESS_OFFICIAL")) continue;
            const proto::FlatPosition p = proto::ToFlat(e.position);
            const double d = std::hypot(p.x - me.x, p.z - me.z);
            if (d < nearest) {
                nearest = d;
                best = id;
            }
        }
        return best;
    };
    // The nearest standing structure of a kind in sight.
    const auto structure = [&](const std::string& kind) -> std::uint32_t {
        const game::StructureKind want = kind == "heart" ? game::StructureKind::Heart : kind == "tower" ? game::StructureKind::GuardTower : game::StructureKind::Gate;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (self == nullptr) return 0;
        const proto::FlatPosition me = proto::ToFlat(self->position);
        std::uint32_t best = 0;
        double nearest = 1e18;
        for (const auto& [id, e] : view_.Entities()) {
            const proto::EntityStructure* s = fortress_.Structure(id);
            if (s == nullptr || game::StructureKind(s->kind) != want || IsDead(e)) continue;
            const proto::FlatPosition p = proto::ToFlat(e.position);
            const double d = std::hypot(p.x - me.x, p.z - me.z);
            if (d < nearest) {
                nearest = d;
                best = id;
            }
        }
        if (best == 0) core::LogWarning("game: --fortress {}: no {} in sight", step, kind);
        return best;
    };
    core::LogInfo("game: --fortress {}", step);
    const std::uint32_t fortressId = std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10));
    // The nearest living player in sight not of the guild, within `within`, attacked (a hop beside it when
    // far: our walks go straight and the fortress's walls stand between). False: nobody to fight.
    const auto huntPlayers = [&](double within) -> bool {
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (self == nullptr) return false;
        const proto::FlatPosition me = proto::ToFlat(self->position);
        const auto& members = guild_.Guild().members;
        std::uint32_t best = 0;
        double nearest = within;
        for (const auto& [id, e] : view_.Entities()) {
            if (id == view_.Self() || e.kind != proto::EntityKind::Character || IsNpc(e) || IsDead(e) || e.look.name.empty()) continue;
            if (std::any_of(members.begin(), members.end(), [&](const auto& m) { return m.name == e.look.name; })) continue;
            const proto::FlatPosition p = proto::ToFlat(e.position);
            const double d = std::hypot(p.x - me.x, p.z - me.z);
            if (d < nearest) {
                nearest = d;
                best = id;
            }
        }
        if (best == 0) return false;
        if (nearest > 120.0) send(proto::FortressDev{proto::FortressDevAction::GoTo, fortressId, best});
        if (best != siegeTarget_ || context.time - siegeAttackAt_ > 8.0) {
            SelectPlayer(best);
            session.Attack(best);
            siegeTarget_ = best;
            siegeAttackAt_ = context.time;
        }
        BotSkill(context, best);
        return true;
    };
    // The nearest gate while one stands, then the guard towers, then the heart (a hop beside it when far).
    const auto besiege = [&]() {
        std::uint32_t target = player_.target.entity;
        const proto::EntityStructure* s = target != 0 ? fortress_.Structure(target) : nullptr;
        const game::WorldView::Entity* e = target != 0 ? view_.Find(target) : nullptr;
        if (s == nullptr || e == nullptr || IsDead(*e) || s->owner == guild_.Guild().id) {
            target = structure("gate");
            if (target == 0) target = structure("tower");
            if (target == 0) target = structure("heart");
            if (target != 0) SelectMonster(target);
        }
        if (target == 0 || fortress_.Structure(target) == nullptr) return;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        const game::WorldView::Entity* it = view_.Find(target);
        if (self != nullptr && it != nullptr) {
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(it->position);
            if (std::hypot(a.x - b.x, a.z - b.z) > 150.0) send(proto::FortressDev{proto::FortressDevAction::GoTo, fortressId, target});
        }
        // Again now and then (a teleport or a refusal stops a fight; each new order restarts the swing).
        if (target != siegeTarget_ || context.time - siegeAttackAt_ > 8.0) {
            siegeTarget_ = target;
            siegeAttackAt_ = context.time;
            session.Attack(target);
        }
    };
    if (verb == "walk-official") {
        // To the nearest official, until within its reach (the step comes again each second).
        const game::WorldView::Entity* npc = view_.Find(official());
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (npc != nullptr && self != nullptr) {
            const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(npc->position);
            const double d = std::hypot(a.x - b.x, a.z - b.z);
            if (d > 200.0 && ++walkTries_ < 30) {
                proto::WorldPosition to = npc->position;
                to.x += 12.0f;
                session.MoveTo(to);
                fortressScript_.insert(fortressScript_.begin(), step);
                fortressNext_ = context.time + 1.0;
            } else {
                walkTries_ = 0;
            }
        } else {
            core::LogWarning("game: --fortress walk-official: no fortress official in sight");
        }
    } else if (verb == "walk") {
        // To X,Z of the region it stands in, until within 30 (the step again each second, 30 times at most),
        // or until it stands in another region (a teleport zone on the way).
        float x = 0, z = 0;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        if (self != nullptr && std::sscanf(arg.c_str(), "%f,%f", &x, &z) == 2) {
            if (walkTries_ == 0) walkRegion_ = self->position.region;
            const double d = std::hypot(double(self->position.x) - x, double(self->position.z) - z);
            core::LogInfo("game: --fortress walk: {:.0f} from {:.0f},{:.0f}", d, x, z);
            if (self->position.region != walkRegion_) {
                core::LogInfo("game: --fortress walk: taken to {:#06x}", self->position.region);
                walkTries_ = 0;
            } else if (d > 30.0 && ++walkTries_ < 30) {
                session.MoveTo({self->position.region, x, self->position.y, z});
                fortressScript_.insert(fortressScript_.begin(), step);
                fortressNext_ = context.time + 1.0;
            } else {
                walkTries_ = 0;
            }
        }
    } else if (verb == "window") {
        send(proto::FortressApplyOpen{official()});
    } else if (verb == "apply" || verb == "defend") {
        send(proto::FortressApply{official(), fortressId, verb == "defend" ? proto::FortressSide::Defend : proto::FortressSide::Attack});
    } else if (verb == "press") {
        // The row's button in the open window, its question answered yes.
        if (PressFortressApply(hudContext_, fortressId)) hud_.AnswerConfirm(true);
    } else if (verb == "cancel") {
        send(proto::FortressCancel{official(), fortressId});
    } else if (verb == "dev") {
        const proto::FortressDevAction action = arg == "startreq" ? proto::FortressDevAction::StartRequests
                                                : arg == "endreq" ? proto::FortressDevAction::EndRequests
                                                : arg == "startwar" ? proto::FortressDevAction::StartWar
                                                : arg == "starttax" ? proto::FortressDevAction::StartTax
                                                : arg == "endtax" ? proto::FortressDevAction::EndTax
                                                                    : proto::FortressDevAction::EndWar;
        send(proto::FortressDev{action, 0, 0});
    } else if (verb == "use-item") {
        // use-item:CODENAME[,ZONE]: a fortress item of the bag used for the fortress whose war's area the player stands in
        // (a manual on ZONE, 0: the nearest free zone of its kind; tablets, guards, flags at the player's feet).
        const std::size_t comma = arg.find(',');
        const std::string code = arg.substr(0, comma);
        const std::uint32_t zone = comma == std::string::npos ? 0u : std::uint32_t(std::strtoul(arg.c_str() + comma + 1, nullptr, 10));
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        std::uint32_t fortress = self != nullptr ? fortress_.WarArea(self->position.region) : 0;
        for (const proto::FortressRecord& f : fortress_.Fortresses()) {
            if (fortress == 0 && self != nullptr && std::find(f.regions.begin(), f.regions.end(), self->position.region) != f.regions.end()) fortress = f.id;
        }
        for (const auto& [place, item] : player_.inventory.places) {
            const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(item.refId) : nullptr;
            if (row == nullptr || row->codename != code || place < game::slot::kWorn || self == nullptr) continue;
            // Tablets, guards and flags a little beside the player (the stock client's pick refuses one under 0.5); kits and the
            // bomb on the selection.
            send(proto::FortressUseItem{place, fortress, zone, self->position.region, self->position.x + 10.0f, self->position.y, self->position.z,
                                        player_.target.entity});

            break;
        }
    } else if (verb == "manage" || verb == "hire" || verb == "bless" || verb == "introduce") {
        // manage:ACTION[,VALUE] (an action of FortressManage at the nearest fortress manager: 3 hired, 5 schedule, 0x19, 0x1B...),
        // hire:BIT (1 aide, 2 smith, 4 trainer), bless:ID, introduce:TEXT.
        std::uint32_t npc = 0;
        if (const game::WorldView::Entity* self = view_.Find(view_.Self()); self != nullptr && tables_ && tables_->Usable()) {
            double nearest = 1e18;
            for (const auto& [id, e] : view_.Entities()) {
                const assets::ObjectRow* row = e.kind == proto::EntityKind::Character ? tables_->characters.Find(e.look.model) : nullptr;
                if (row == nullptr || row->codename.find("_FORTRESS_MANAGER") == std::string::npos) continue;
                const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(e.position);
                if (const double d = std::hypot(a.x - b.x, a.z - b.z); d < nearest) {
                    nearest = d;
                    npc = id;
                }
            }
        }
        proto::FortressManage m{npc};
        hudContext_.fortressNpc = npc;
        if (verb == "hire") {
            m.action = proto::FortressAction::Hire;
            m.value = std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10));
        } else if (verb == "bless") {
            m.action = proto::FortressAction::Bless;
            m.value = std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10));
        } else if (verb == "introduce") {
            m.action = proto::FortressAction::Introduce;
            m.text = arg;
        } else {
            const std::size_t comma = arg.find(',');
            m.action = proto::FortressAction(std::strtoul(arg.c_str(), nullptr, 0));
            if (comma != std::string::npos) m.value = std::uint32_t(std::strtoul(arg.c_str() + comma + 1, nullptr, 10));
        }
        send(m);
    } else if (verb == "forge" || verb == "forge-query" || verb == "forge-cancel" || verb == "forge-collect") {
        // forge:ITEM,COUNT[,trainer] (a work begun at the nearest smith, or trainer), forge-query[:trainer], forge-cancel[:trainer],
        // forge-collect[:trainer[,COUNT]].
        const bool trainer = arg.find("trainer") != std::string::npos;
        std::uint32_t npc = 0;
        if (const game::WorldView::Entity* self = view_.Find(view_.Self()); self != nullptr && tables_ && tables_->Usable()) {
            double nearest = 1e18;
            for (const auto& [id, e] : view_.Entities()) {
                const assets::ObjectRow* row = e.kind == proto::EntityKind::Character ? tables_->characters.Find(e.look.model) : nullptr;
                if (row == nullptr || row->codename.find(trainer ? "_FORTRESS_TRAINER" : "_FORTRESS_SMITH") == std::string::npos) continue;
                const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(e.position);
                if (const double d = std::hypot(a.x - b.x, a.z - b.z); d < nearest) {
                    nearest = d;
                    npc = id;
                }
            }
        }
        using A = proto::FortressAction;
        proto::FortressManage m{npc};
        const int op = verb == "forge" ? 1 : verb == "forge-cancel" ? 2 : verb == "forge-collect" ? 3 : 0;
        hudContext_.fortressNpc = npc;
        if (op == 0) hudContext_.forgeAsked = true; // the answer opens the window (as the row's)
        m.action = A(int(trainer ? A::TrainQuery : A::ForgeQuery) + op);
        if (op == 1) {
            m.value = std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10));
            const std::size_t comma = arg.find(',');
            m.value2 = comma == std::string::npos ? 1u : std::uint32_t(std::strtoul(arg.c_str() + comma + 1, nullptr, 10));
        } else if (op == 3) {
            const std::size_t comma = arg.rfind(',');
            m.value2 = comma == std::string::npos ? 0u : std::uint32_t(std::strtoul(arg.c_str() + comma + 1, nullptr, 10));
        }
        send(m);
    } else if (verb == "dungeon") {
        // dungeon:query|create|enter|leave (the Cave of Discipline at the nearest fortress manager; leave anywhere inside).
        std::uint32_t npc = 0;
        if (const game::WorldView::Entity* self = view_.Find(view_.Self()); self != nullptr && tables_ && tables_->Usable()) {
            double nearest = 1e18;
            for (const auto& [id, e] : view_.Entities()) {
                const assets::ObjectRow* row = e.kind == proto::EntityKind::Character ? tables_->characters.Find(e.look.model) : nullptr;
                if (row == nullptr || row->codename.find("_FORTRESS_MANAGER") == std::string::npos) continue;
                const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(e.position);
                if (const double d = std::hypot(a.x - b.x, a.z - b.z); d < nearest) {
                    nearest = d;
                    npc = id;
                }
            }
        }
        const proto::FortressAction action = arg == "create"  ? proto::FortressAction::DungeonCreate
                                             : arg == "enter" ? proto::FortressAction::DungeonEnter
                                             : arg == "leave" ? proto::FortressAction::DungeonLeave
                                                              : proto::FortressAction::DungeonQuery;
        send(proto::FortressManage{npc, action});
    } else if (verb == "remove") {
        // remove:ENTITY or the selected structure: the target window's remove button (a barricade, a guard, a monster).
        const std::uint32_t object = arg.empty() ? player_.target.entity : std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10));
        if (const proto::EntityStructure* s = fortress_.Structure(object)) send(proto::FortressRemove{object, s->fortress});
    } else if (verb == "goto") {
        session.DevGoto(fortressId);
    } else if (verb == "goto-fortress") {
        send(proto::FortressDev{proto::FortressDevAction::GoTo, fortressId, 0});
    } else if (verb == "siege") {
        // The war fought for a fortress (each second): the nearest guard tower while one stands, then
        // the heart, until the player's guild holds it.
        const proto::FortressRecord* f = fortress_.Fortress(fortressId);
        if (f != nullptr && f->atWar && (f->holder == 0 || f->holder != guild_.Guild().id)) {
            besiege();
            fortressScript_.insert(fortressScript_.begin(), step);
            fortressNext_ = context.time + 1.0;
        }
    } else if (verb == "battle") {
        // The whole war, each second: back up when dead and back into the fortress (a hop), then a fortress
        // that is ours is defended against whoever is in sight; one that is not is taken, gates, towers, heart.
        const proto::FortressRecord* f = fortress_.Fortress(fortressId);
        if (f != nullptr && f->atWar) {
            const game::WorldView::Entity* self = view_.Find(view_.Self());
            if (self != nullptr && IsDead(*self)) {
                session.Resurrect(1);
            } else if (self != nullptr && fortress_.WarArea(self->position.region) != f->id) {
                send(proto::FortressDev{proto::FortressDevAction::GoTo, fortressId, 0});
            } else if ((f->holder != 0 ? f->holder : f->owner) == guild_.Guild().id) {
                huntPlayers(1e18);
            } else {
                besiege();
            }
            fortressScript_.insert(fortressScript_.begin(), step);
            fortressNext_ = context.time + 1.0;
        }
    } else if (verb == "select" || verb == "attack") {
        if (const std::uint32_t target = structure(arg)) {
            SelectMonster(target);
            if (verb == "attack") session.Attack(target);
        }
    } else if (verb == "until-owner" || verb == "until-held") {
        // Waits (each second) until the fortress has an owner, or a guild holding it in its war.
        const proto::FortressRecord* f = fortress_.Fortress(fortressId);
        if (f == nullptr || (verb == "until-owner" ? f->owner : f->holder) == 0) {
            fortressScript_.insert(fortressScript_.begin(), step);
            fortressNext_ = context.time + 1.0;
        }
    } else if (verb == "until-war" || verb == "until-peace") {
        // Waits (each second) until the fortress's war has begun, or is over.
        const proto::FortressRecord* f = fortress_.Fortress(fortressId);
        if (f == nullptr || f->atWar != (verb == "until-war")) {
            fortressScript_.insert(fortressScript_.begin(), step);
            fortressNext_ = context.time + 1.0;
        }
    } else if (verb == "hunt") {
        // Each second for arg seconds: the nearest living player in sight not of the guild, attacked.
        huntPlayers(1e18);
        if (++walkTries_ < std::atoi(arg.c_str())) {
            fortressScript_.insert(fortressScript_.begin(), step);
            fortressNext_ = context.time + 1.0;
        } else {
            walkTries_ = 0;
        }
    } else if (verb == "aide" || verb == "repair" || verb == "upgrade" || verb == "aide-build") {
        // The nearest battle aide in sight (its codename *_FORTRESS_BATTLEAIDE*): aide asks for the structures;
        // repair:ZONE, upgrade:ZONE,BRANCH ask for those; aide-build:ZONE a barricade on ZONE (the original's 0x705E 0x0A,
        // which the stock client never sends: ours from this step only).
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        std::uint32_t aide = 0;
        double nearest = 1e18;
        for (const auto& [id, e] : view_.Entities()) {
            if (self == nullptr || e.kind != proto::EntityKind::Character || !IsNpc(e) || !tables_ || !tables_->Usable()) continue;
            const assets::ObjectRow* row = tables_->characters.Find(e.look.model);
            if (row == nullptr || row->codename.find("_FORTRESS_BATTLEAIDE") == std::string::npos) continue;
            const proto::FlatPosition p = proto::ToFlat(e.position), me = proto::ToFlat(self->position);
            if (const double d = std::hypot(p.x - me.x, p.z - me.z); d < nearest) {
                nearest = d;
                aide = id;
            }
        }
        if (aide == 0) {
            core::LogWarning("game: --fortress {}: no battle aide in sight", step);
        } else if (verb == "aide") {
            send(proto::FortressStructuresQuery{aide, 0});
        } else if (verb == "aide-build") {
            // The zone's fortress: the one whose area holds its place (the client's eventzonedata.txt and objectstring.ifo).
            const std::uint32_t zone = std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10));
            std::uint32_t fortress = 0;
            if (const assets::WorldMapData* data = gui_.data().WorldMap(); data != nullptr) {
                for (const assets::FortressZoneRow& z : data->zones) {
                    for (const proto::FortressRecord& f : fortress_.Fortresses()) {
                        if (z.id == zone && std::find(f.regions.begin(), f.regions.end(), z.region) != f.regions.end()) fortress = f.id;
                    }
                }
            }
            send(proto::FortressBuild{aide, fortress, zone});
        } else {
            unsigned zone = 0, branch = 0;
            std::sscanf(arg.c_str(), "%u,%u", &zone, &branch);
            if (verb == "repair") send(proto::FortressRepair{aide, 0, zone});
            else send(proto::FortressUpgrade{aide, 0, zone, std::uint8_t(branch)});
        }
    } else if (verb == "manual") {
        // manual:CODENAME: the bag's first of that manual used as from the bag: its zones marked.
        for (const auto& [place, item] : player_.inventory.places) {
            const assets::ItemRow* row = tables_ && tables_->Usable() ? tables_->items.Find(item.refId) : nullptr;
            if (row == nullptr || row->codename != arg || place < game::slot::kWorn) continue;
            UseFortressItem(std::uint8_t(place));
            break;
        }
    } else if (verb == "zone-click" || verb == "zone-miss") {
        // zone-click[:ZONE]: a click on that zone's marker (else the nearest one's) where it stands on the screen; zone-miss: a
        // click beside the markers (they go). Both through the player's click.
        const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
        const std::uint32_t zone = std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10));
        const ZoneMarker* chosen = nullptr;
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        double nearest = 1e18;
        for (const ZoneMarker& m : zoneMarkers_) {
            const proto::FlatPosition p = proto::ToFlat(m.at), me = self != nullptr ? proto::ToFlat(self->position) : p;
            const double d = std::hypot(p.x - me.x, p.z - me.z);
            if ((zone != 0 && m.zone == zone) || (zone == 0 && d < nearest)) {
                nearest = d;
                chosen = &m;
            }
        }
        float x = 0.0f, y = 0.0f;
        if (verb == "zone-miss") {
            ClickWorld(context, width * 0.5f, height * 0.9f, false);
        } else if (chosen == nullptr || !world_ || !world_->Project(chosen->at, 4.0f, width, height, x, y)) {
            core::LogWarning("game: --fortress {}: no marker on the screen", step);
        } else {
            core::LogInfo("game: --fortress {}: a click at {:.0f},{:.0f} on the zone {}'s marker", step, x, y, chosen->zone);
            ClickWorld(context, x, y, false);
        }
    } else if (verb == "walk-manager") {
        send(proto::FortressDev{proto::FortressDevAction::GoTo, fortressId, 0xFFFFFFFEu});
    } else if (verb == "tax") {
        // The nearest fortress manager talked to; its "Tax management" row chosen once the menu is in (next step's turn).
        const game::WorldView::Entity* self = view_.Find(view_.Self());
        std::uint32_t manager = 0;
        double nearest = 1e18;
        for (const auto& [id, e] : view_.Entities()) {
            if (self == nullptr || e.kind != proto::EntityKind::Character || !IsNpc(e) || !tables_ || !tables_->Usable()) continue;
            const assets::ObjectRow* row = tables_->characters.Find(e.look.model);
            if (row == nullptr || row->codename.find("_FORTRESS_MANAGER") == std::string::npos) continue;
            const proto::FlatPosition p = proto::ToFlat(e.position), me = proto::ToFlat(self->position);
            if (const double d = std::hypot(p.x - me.x, p.z - me.z); d < nearest) {
                nearest = d;
                manager = id;
            }
        }
        if (manager == 0) {
            core::LogWarning("game: --fortress tax: no fortress manager in sight");
        } else {
            send(proto::NpcTalk{manager});
            fortressScript_.insert(fortressScript_.begin(), "tax-row");
            fortressNext_ = context.time + 1.0;
        }
    } else if (verb == "tax-row") {
        auto* npc = dynamic_cast<NpcWindow*>(hud_.Show(hud::kNpc));
        if (npc == nullptr || !npc->ChooseText(gui_.data().Text("SN_FORTRESS_MANAGER_TAX"))) core::LogWarning("game: --fortress tax: no Tax management row");
    } else if (verb == "tax-rate") {
        if (!PressTaxManagement(hudContext_, "rate", std::strtoll(arg.c_str(), nullptr, 10))) core::LogWarning("game: --fortress tax-rate: no tax window");
    } else if (verb == "tax-levy") {
        if (!PressTaxManagement(hudContext_, "levy", 0)) core::LogWarning("game: --fortress tax-levy: no tax window");
        levyAmount_ = arg.empty() ? -1 : std::strtoll(arg.c_str(), nullptr, 10); // typed when the box is answered
    } else if (verb == "tax-yes" || verb == "tax-no") {
        AnswerTaxBox(hudContext_, verb == "tax-yes", std::exchange(levyAmount_, -1));
    } else if (verb == "walk-aide") {
        // Beside the fortress's battle aide (a developer's hop: the fortress's walls stand between).
        send(proto::FortressDev{proto::FortressDevAction::GoTo, fortressId, 0xFFFFFFFFu});
    } else if (verb == "action") {
        UseAction(context, std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10))); // an icon of the Action window
    } else if (verb == "drop") {
        send(proto::DropItem{std::uint8_t(std::strtoul(arg.c_str(), nullptr, 10))}); // an inventory place's item to the ground
    } else if (verb == "attack-player") {
        for (const auto& [id, e] : view_.Entities()) {
            if (e.kind == proto::EntityKind::Character && e.look.name == arg) {
                SelectPlayer(id);
                session.Attack(id);
            }
        }
    } else if (verb == "make" || verb == "make-cancel" || verb == "make-complete" || verb == "make-tab") {
        // The smith's or the trainer's open window (CIFFortressMakeItemWnd): make:ITEM,COUNT (the item's row's Make, its box
        // answered with COUNT), make-cancel (Cancel, its box answered), make-complete (Complete), make-tab:N.
        const std::size_t comma = arg.find(',');
        const std::uint32_t item = std::uint32_t(std::strtoul(arg.c_str(), nullptr, 10));
        const int count = comma == std::string::npos ? 1 : std::atoi(arg.c_str() + comma + 1);
        const std::string action = verb == "make" ? "make" : verb == "make-cancel" ? "cancel" : verb == "make-complete" ? "complete" : "tab";
        if (!PressFortressMakeItem(hudContext_, action, item, count)) core::LogWarning("game: fortress: no make-item window for {}", step);
    } else if (verb == "intro-type") {
        // intro-type:TEXT: typed into the open About Guild window and confirmed (the commander's).
        if (!TypeGuildIntroduction(hudContext_, arg, true)) core::LogWarning("game: fortress: no introduction window for {}", step);
    } else if (verb == "yes" || verb == "no") {
        hud_.AnswerConfirm(verb == "yes"); // the question shown (a gap's, an exit's, a talisman's)
    } else if (verb != "wait" && !ExchangeScriptStep(verb, arg) && !StallScriptStep(context, verb, arg) && !InstanceScriptStep(context, verb, arg) &&
               !AlchemyScriptStep(verb, arg) && !CosScriptStep(verb, arg) && !ServiceScriptStep(verb, arg) && !FlagScriptStep(context, verb, arg) &&
               !OpenMarketScriptStep(verb, arg)) {
        core::LogWarning("game: --fortress {}: not a step", step);
    }
}

} // namespace sr::client
