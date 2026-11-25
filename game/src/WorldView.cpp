#include "sr/game/WorldView.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace sr::game {

void WorldView::Clear() {
    entities_.clear();
    self_ = 0;
    selfData_ = {};
}

void WorldView::Leg(Entity& e, const proto::WorldPosition& from, const proto::WorldPosition& to, float speed) {
    // The leg starts on the floor the walker stands on when the server's word is where it already
    // is (otherwise that floor is found again from the server's place, at the first step).
    e.legStart = {};
    if (e.nav.Valid()) {
        const proto::FlatPosition a = proto::ToFlat(e.position), b = proto::ToFlat(from);
        if (std::hypot(a.x - b.x, a.z - b.z) <= 2.0 && std::abs(e.position.y - from.y) <= 3.0f) e.legStart = e.nav;
    }
    e.legFrom = from;
    e.position = from; // the server's word on where it is now
    e.previous = from;
    e.legTo = to;
    e.speed = speed;
    const proto::FlatPosition a = proto::ToFlat(from), b = proto::ToFlat(to);
    e.moving = speed > 0.0f && std::hypot(b.x - a.x, b.z - a.z) > 0.01;
    if (e.moving) e.yaw = float(std::atan2(b.x - a.x, b.z - a.z));
}

void WorldView::Face(Entity& e, std::uint32_t target) {
    const auto t = entities_.find(target);
    if (t == entities_.end()) return;
    const proto::FlatPosition from = proto::ToFlat(e.position), to = proto::ToFlat(t->second.position);
    if (std::hypot(to.x - from.x, to.z - from.z) > 0.01) e.yaw = float(std::atan2(to.x - from.x, to.z - from.z));
}

void WorldView::Apply(const TitleEvent& event) {
    if (const auto* m = std::get_if<proto::WorldEntered>(&event)) {
        if (m->status != proto::WorldStatus::Ok) return;
        Clear();
        self_ = m->entity;
        selfData_ = *m;
        Entity& e = entities_[m->entity];
        e.id = m->entity;
        e.look = m->character;
        e.position = e.previous = e.legTo = m->position;
        e.yaw = m->yaw;
        e.lookVersion = ++nextLook_;
    } else if (const auto* m = std::get_if<proto::EntityAppeared>(&event)) {
        Entity& e = entities_[m->entity];
        e.id = m->entity;
        e.look = m->character;
        e.lookVersion = ++nextLook_;
        e.yaw = m->yaw;
        e.kind = m->kind;
        e.amount = m->amount;
        e.owner = m->owner;
        e.plus = m->plus;
        e.rarity = m->rarity;
        e.fresh = m->fresh;
        e.nav = {};
        e.settled = false;
        if (m->moving) {
            Leg(e, m->position, m->to, m->speed);
        } else {
            e.position = e.previous = e.legTo = m->position;
            e.moving = false;
        }
    } else if (const auto* m = std::get_if<proto::EquipmentChanged>(&event)) {
        // What it wears: the item taken off goes, the one put on comes.
        if (const auto it = entities_.find(m->entity); it != entities_.end()) {
            auto& worn = it->second.look.equipment;
            if (m->previous != 0) {
                if (const auto old = std::find_if(worn.begin(), worn.end(), [&](const proto::EquippedItem& e) { return e.item == m->previous; });
                    old != worn.end()) {
                    worn.erase(old);
                }
            }
            if (m->item != 0) worn.push_back({m->item, m->plus});
            it->second.lookVersion = ++nextLook_;
        }
    } else if (const auto* m = std::get_if<proto::EntityLook>(&event)) {
        // A transform: its whole look (a monster's model, another's dress), or its own again.
        if (const auto it = entities_.find(m->entity); it != entities_.end()) {
            it->second.look.model = m->model;
            it->second.look.height = m->height;
            it->second.look.equipment = m->equipment;
            it->second.look.avatars = m->avatars;
            it->second.lookVersion = ++nextLook_;
        }
    } else if (const auto* cos = std::get_if<proto::CosEvent>(&event)) {
        // A pet named: its name over it from now on, for everyone who sees it.
        if (const auto* m = std::get_if<proto::CosRenamed>(cos)) {
            if (const auto it = entities_.find(m->entity); it != entities_.end()) it->second.look.name = m->name;
        }
    } else if (const auto* m = std::get_if<proto::EntityGone>(&event)) {
        if (m->entity != self_) entities_.erase(m->entity);
    } else if (const auto* m = std::get_if<proto::EntityBlink>(&event)) {
        // A teleport skill: a dash runs from where it stands to the end at the
        // dash's speed, facing it; a put stands there at once, facing the way it went.
        if (const auto it = entities_.find(m->entity); it != entities_.end()) {
            Entity& e = it->second;
            const proto::FlatPosition a = proto::ToFlat(e.position), b = proto::ToFlat(m->to);
            if (m->kind == proto::BlinkKind::Dash) {
                Leg(e, e.position, m->to, proto::kBlinkDashSpeed);
                e.dashing = e.moving;
                e.dashTo = m->to;
            } else {
                Leg(e, m->to, m->to, 0.0f);
                e.dashing = false;
            }
            if (std::hypot(b.x - a.x, b.z - a.z) > 0.01) e.yaw = float(std::atan2(b.x - a.x, b.z - a.z));
            e.turnTo = 0;
        }
    } else if (const auto* m = std::get_if<proto::EntityMoved>(&event)) {
        const auto it = entities_.find(m->entity);
        if (it != entities_.end()) {
            Entity& e = it->second;
            // The server's stand at a running dash's end: the dash goes on there.
            if (e.dashing && m->speed <= 0.0f) {
                const proto::FlatPosition a = proto::ToFlat(m->to), b = proto::ToFlat(e.dashTo);
                if (std::hypot(a.x - b.x, a.z - b.z) < 1.0) return;
            }
            e.dashing = false;
            // Another walker already on its way, re-aimed (a monster's chase, a pet following): on from where it is drawn
            // when the server's start is within half a second of its walk, not pulled back to it (the leg comes late by
            // the network's delay; the user's report 2026-11-14: chasers stopped and started). Farther: the server's word.
            // The player's own walk too: a click while running re-aims it from where it is drawn (the user's report
            // 2026-11-22: clicking on and on stuttered, each answer's start a step off the drawn place).
            const proto::FlatPosition here = proto::ToFlat(e.position), said = proto::ToFlat(m->from);
            const bool carryOn = e.moving && m->speed > 0.0f && e.position.region == m->from.region &&
                                 std::hypot(here.x - said.x, here.z - said.z) <= std::max(10.0, double(m->speed) * 0.5);
            if (carryOn) {
                const proto::WorldPosition previous = e.previous;
                Leg(e, e.position, m->to, m->speed);
                e.previous = previous; // the frame's blend goes on unbroken
            } else {
                Leg(e, m->from, m->to, m->speed);
            }
            if (e.moving) {
                e.turnTo = 0; // a new walk: whatever it swung at is behind it
            } else if (e.turnTo != 0) {
                Face(e, e.turnTo);
                e.turnTo = 0;
            }
        }
    } else if (const auto* m = std::get_if<proto::EntityDamaged>(&event)) {
        if (const auto it = entities_.find(m->target); it != entities_.end()) it->second.look.hp = m->hp;
    } else if (const auto* m = std::get_if<proto::GroundItemFree>(&event)) {
        if (const auto it = entities_.find(m->entity); it != entities_.end()) it->second.owner = 0;
    } else if (const auto* m = std::get_if<proto::EntityTurned>(&event)) {
        // A turn where it stands (a walker's leg keeps the facing of its way).
        if (const auto it = entities_.find(m->entity); it != entities_.end() && !it->second.moving) {
            it->second.yaw = proto::YawOf(m->heading);
            it->second.turnTo = 0;
        }
    } else if (const auto* m = std::get_if<proto::EntitySwing>(&event)) {
        // A swing turns the attacker toward its target: now when it stands, else when its walk stops.
        if (const auto a = entities_.find(m->attacker); a != entities_.end() && m->target != m->attacker) {
            if (a->second.moving) {
                a->second.turnTo = m->target;
            } else {
                Face(a->second, m->target);
            }
        }
    } else if (std::holds_alternative<proto::WorldLeft>(event)) {
        Clear();
    }
}

void WorldView::Step(double seconds) {
    for (auto& [id, e] : entities_) {
        e.previous = e.position;
        if (!e.moving) {
            // Standing where it appeared (an NPC at its spawn's height, which the original's tables give loosely: Jangan's
            // Adventurer Flora at 11 by the market's steps): put down once on the floor nearest that height when the
            // navigation there is in, as the walkers' heights come from it.
            if (!e.settled && !e.nav.Valid() && nav_ != nullptr && nav_->Holds(e.position.region)) {
                e.settled = true;
                const NavPosition at = nav_->Drop(e.position.region, e.position.x, e.position.y, e.position.z);
                if (at.Valid()) {
                    e.nav = at;
                    e.position.y = e.previous.y = e.legTo.y = at.y;
                }
            }
            continue;
        }
        const proto::FlatPosition a = proto::ToFlat(e.position), b = proto::ToFlat(e.legTo);
        const double dx = b.x - a.x, dz = b.z - a.z, left = std::hypot(dx, dz), step = double(e.speed) * seconds;
        if (step >= left) {
            proto::WorldPosition end = e.legTo;
            Walk(e, end);
            e.position = end;
            e.moving = false;
            e.dashing = false;
            if (e.turnTo != 0) {
                Face(e, e.turnTo); // it swung at something on the way: it faces it now
                e.turnTo = 0;
            }
            continue;
        }
        const double t = step / left;
        const float y = e.position.y + float(t) * (e.legTo.y - e.position.y);
        proto::WorldPosition p = proto::FromFlat({a.x + dx * t, a.z + dz * t}, y);
        Walk(e, p);
        e.position = p;
    }
}

// The leg walked on the navigation from its start to `to`'s x and z; `to.y` becomes the floor
// reached. The leg's start is the floor the walker stood on, or (entered, appeared, a far
// correction) the leg's first place dropped, as the original drops when it cannot walk.
void WorldView::Walk(Entity& e, proto::WorldPosition& to) const {
    if (nav_ == nullptr) return;
    if (!e.legStart.Valid() || !nav_->Holds(e.legStart.region)) {
        e.legStart = nav_->Drop(e.legFrom.region, e.legFrom.x, e.legFrom.y, e.legFrom.z);
    }
    if (!e.legStart.Valid() || !nav_->Holds(to.region)) {
        e.nav = {};
        return;
    }
    NavPosition next;
    nav_->Walk(e.legStart, to.region, to.x, to.z, next);
    if (next.Valid() && next.region == to.region && std::hypot(next.x - to.x, next.z - to.z) < 0.5f) {
        e.nav = next;
    } else {
        // Stopped short of the server's leg (our navigation and its differ there): on at the
        // server's place, the floor nearest the height reached.
        e.nav = nav_->Drop(to.region, to.x, next.Valid() ? next.y : e.position.y, to.z);
    }
    if (e.nav.Valid()) to.y = e.nav.y;
}

const WorldView::Entity* WorldView::Find(std::uint32_t id) const {
    const auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : &it->second;
}

proto::WorldPosition WorldView::Drawn(const Entity& e, double alpha) {
    const proto::FlatPosition a = proto::ToFlat(e.previous), b = proto::ToFlat(e.position);
    const double t = std::clamp(alpha, 0.0, 1.0);
    return proto::FromFlat({a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t}, e.previous.y + float(t) * (e.position.y - e.previous.y));
}

std::uint64_t WorldView::Hash() const {
    std::vector<std::uint32_t> ids;
    for (const auto& [id, e] : entities_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (std::uint32_t id : ids) {
        const Entity& e = entities_.at(id);
        mix(id);
        mix(e.position.region);
        mix(std::bit_cast<std::uint32_t>(e.position.x));
        mix(std::bit_cast<std::uint32_t>(e.position.y));
        mix(std::bit_cast<std::uint32_t>(e.position.z));
        mix(e.moving ? 1 : 0);
    }
    return h;
}

} // namespace sr::game
