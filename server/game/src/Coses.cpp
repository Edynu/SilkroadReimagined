#include "sr/server/Coses.h"

#include <algorithm>
#include <cmath>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/game/Buffs.h"
#include "sr/game/GuildMercenary.h"
#include "sr/game/Inventory.h"
#include "sr/game/Items.h"
#include "sr/game/NavMesh.h"
#include "sr/game/Skills.h"
#include "sr/game/States.h"
#include "sr/game/Stats.h"
#include "sr/server/World.h"

namespace sr::server {

namespace proto = game::proto;

namespace {
constexpr float kSummonAside = 10.0f;   // ours: a COS comes out this far to its owner's right
constexpr float kBoardReach = 30.0f;    // how near its COS a player boards it (3D; the original's bind)
constexpr float kBoardWalk = 300.0f;    // ours: from how far its owner walks to it first to board
constexpr double kCartFollowEnd = 65.0; // a ride or transport left standing stops keeping near its owner this far (the original's)

// The distance between two places in 3D (a dungeon's by its own coordinates).
double Distance3(const proto::WorldPosition& a, const proto::WorldPosition& b) {
    double dx = 0, dz = 0;
    if ((a.region & 0x8000) != 0 || (b.region & 0x8000) != 0) {
        if (a.region != b.region) return 1e9;
        dx = double(a.x) - double(b.x);
        dz = double(a.z) - double(b.z);
    } else {
        const proto::FlatPosition fa = proto::ToFlat(a), fb = proto::ToFlat(b);
        dx = fa.x - fb.x;
        dz = fa.z - fb.z;
    }
    const double dy = double(a.y) - double(b.y);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
constexpr auto kFollowPeriod = std::chrono::milliseconds(250);
constexpr double kFightLeash = 500.0; // ours: an attack pet gives a fight up this far from its owner (the monsters' own)
// The natural recovery (the attack pet's own): its
// most HP x 0.016 (double), cut to a whole number; its period ours (the players' 4 s, the NPCs' timer
// is not traced).
constexpr auto kRegenPeriod = std::chrono::seconds(4);
constexpr double kRegenShare = 0.01600000075995922;
// A grab pet: how far from its owner it goes for items (ours; the original's is not known yet),
// how near an item it picks it up (ours), its bag's places allocated when the reference tables do not say (5 pages of
// 28, the 1.188 rows' InventorySize; its own places are game::kGrabPetBag of them), its settings when it is new (ours:
// grabbing on, its owner's items, every kind).
constexpr float kGrabRange = 300.0f, kGrabReach = 10.0f;
constexpr int kDefaultPetBag = 5 * proto::kCosBagPage;
constexpr std::uint32_t kNewGrabOption = proto::kGrabOn | proto::kGrabGold | proto::kGrabEquipment | proto::kGrabOther;

proto::ItemRecord RecordOf(std::uint8_t place, const game::ItemInstance& i) {
    proto::ItemRecord r;
    r.slot = place;
    r.item = i.refId;
    r.plus = i.plus;
    r.variance = i.variance;
    r.durability = i.durability;
    r.quantity = i.count;
    for (const game::ItemInstance::Option& o : i.options) r.magic.push_back(std::uint64_t(o.id) | std::uint64_t(o.value) << 32);
    r.creator = i.creator;
    return r;
}

std::int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
bool IsPet(game::CosKind kind) { return kind == game::CosKind::AttackPet || kind == game::CosKind::GrabPet; }
// The COS that fight: an attack pet, a guild's soldier.
bool Fights(game::CosKind kind) { return kind == game::CosKind::AttackPet || kind == game::CosKind::Soldier; }
} // namespace

std::int64_t CosHost::CosNow() const { return UnixNow(); }

Coses::Coses(CosHost& host) : host_(host) {}

template <typename T>
void Coses::Send(std::uint32_t owner, const T& message) {
    if (const auto o = host_.CosOwner(owner)) host_.CosSend(o->peer, T::kId, proto::Encode(message));
}

void Coses::Refuse(std::uint32_t owner, const char* text, std::string arg) {
    Send(owner, proto::CosRefused{text, std::move(arg)});
    const auto o = host_.CosOwner(owner);
    core::LogInfo("cos: {} refused ({})", o ? o->name : std::to_string(owner), text);
}

bool Coses::PlaceTaken(std::uint32_t owner, game::CosKind kind) const {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return false;
    return std::any_of(it->second.begin(), it->second.end(), [&](std::uint32_t id) { return game::CosPlaceOf(coses_.at(id).kind) == game::CosPlaceOf(kind); });
}

std::uint32_t Coses::Place(std::uint32_t owner, const assets::ObjectRow& row, const std::string& name) {
    World& world = host_.CosWorld();
    const auto place = world.Where(owner);
    if (!place) return 0;
    // Beside its owner, facing the way it faces.
    const proto::FlatPosition at = proto::ToFlat(place->position);
    const proto::FlatPosition aside{at.x + kSummonAside * std::cos(place->yaw), at.z - kSummonAside * std::sin(place->yaw)};
    proto::WorldPosition there = proto::FromFlat(aside, place->position.y);
    there.layer = place->position.layer;
    World::MonsterLook look;
    look.model = row.refObjId;
    look.hp = row.maxHp;
    look.walkSpeed = row.walkSpeed;
    look.runSpeed = row.runSpeed;
    std::uint32_t entity = world.AddCos(look, owner, there, place->yaw);
    if (entity == 0) entity = world.AddCos(look, owner, place->position, place->yaw); // no ground aside: at its feet
    // A named pet's name in its appearance, for everyone who sees it (their clients show it under its owner's).
    if (entity != 0 && !name.empty()) world.SetName(entity, name);
    return entity;
}

Coses::Use Coses::UseItem(std::uint32_t owner, const assets::ItemRow& item, const game::ItemInstance& instance, Clock::time_point now) {
    if (const game::CosKind pet = game::PetOfItem(item); pet != game::CosKind::None) return UsePet(owner, item, instance, pet);
    if (game::IsPetHgpPotion(item)) {
        // On its attack pet out: refused at full; used up (Summoned) when it eats.
        Cos* c = AttackPetOf(owner);
        if (c == nullptr || c->hp == 0) {
            Refuse(owner, "UIIT_MSG_COSERR_YOU_DONT_HAVE_ACTIVE_COS_OBJ");
            return Use::Refused;
        }
        if (c->row.hgp >= game::kCosHgpFull) {
            Refuse(owner, "UIIT_MSG_COSPETERR_HGPFULL_NODRINK");
            return Use::Refused;
        }
        SetHgp(*c, std::min(game::kCosHgpFull, c->row.hgp + game::PetHgpPotionAmount(item)), true);
        return Use::Summoned;
    }
    if (!game::IsCosScroll(item)) return Use::NotCos;
    const auto o = host_.CosOwner(owner);
    const assets::CharacterTable* characters = host_.CosCharacters();
    if (!o || characters == nullptr) return Use::NotCos;
    const auto refuse = [&](const char* text) {
        Refuse(owner, text);
        return Use::Refused;
    };
    if (!o->alive) return refuse("UIIT_MSG_COSPETERR_CANT_DEACTIVATE_IN_DEADSTATE");
    // The original's order: its logout counting down (0x186B), on its way elsewhere (0x1869), a free-PvP state (0x18AD
    // changing, else 0x189D), then the scroll's level, stealth (0x187A), the arena (0x1879), the place, a dealing.
    if (o->loggingOut) return refuse("UIIT_MSG_STRGERR_CANT_OPERATE_REQUESTED_JOB_DURING_LOGOUT");
    if (o->travelling) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    if (o->freePvpChanging) return refuse("UIIT_MGS_ERR_FRPVP_DURING_CHANGE");
    if (o->freePvp) return refuse("UIIT_MSG_CANT_STRIP_TIME_COS_RECALL");
    if (o->level < item.requiredLevel) return refuse("UIIT_MSG_COS_HIGHER_LEVEL_REQUIRED_TO_USE_THISITEM");
    if (o->hidden) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    if (o->inArena) return refuse("UIIT_MSG_COS_CREATE_DUNGEON");
    if (!o->free || o->sitting) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM"); // a dealing; sitting (motion 4: 5)
    const std::optional<std::string> codename = game::CosOfScroll(item, o->level);
    if (!codename) return refuse("UIIT_MSG_COS_HIGHER_LEVEL_REQUIRED_TO_USE_THISITEM");
    const assets::ObjectRow* row = characters->Find(std::string_view(*codename));
    const game::CosKind kind = row != nullptr ? game::CosKindOf(*row) : game::CosKind::None;
    if (kind != game::CosKind::Ride && kind != game::CosKind::Transport) {
        core::LogWarning("cos: {} names {}, no ride or transport", item.codename, *codename);
        return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    }
    // A murderer summons no ride (0x1876); transports are not checked.
    if (kind == game::CosKind::Ride && o->murderer) return refuse("UIIT_MSG_STRGERR_MURDERER_CANT_SUMMON_VEHICLE");
    // A transport: a trader's or a thief's, a thief's not in a town.
    if (kind == game::CosKind::Transport) {
        if (o->job != std::uint8_t(game::Job::Trader) && o->job != std::uint8_t(game::Job::Thief)) return refuse("UIIT_MSG_STRGERR_YOU_CANT_ACTIVATE_CART");
        if (o->job == std::uint8_t(game::Job::Thief) && o->inTown) return refuse("UIIT_MSG_STRGERR_ROBBER_CANT_SUMMON_CART_IN_TOWN");
    }
    const auto place = host_.CosWorld().Where(owner);
    if (!place) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    if ((place->position.region & 0x8000) != 0) return refuse("UIIT_MSG_COS_CREATE_DUNGEON");
    // A second ride or transport (riding anything, one out, or its kept one still coming): 0x185C.
    if (PlaceTaken(owner, kind) || MountOf(owner) != 0 || cartLoading_.contains(owner) || carts_.contains(owner)) {
        return refuse("UIIT_MSG_STRGERR_CANT_SUMMON_MULTIPLE_COSOBJ");
    }
    if (o->fought) return refuse("UIIT_MSG_COS_CAN_NOT_CREATE_BATTLE");
    const std::uint32_t entity = Place(owner, *row);
    if (entity == 0) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    Cos& c = coses_[entity];
    c.entity = entity;
    c.owner = owner;
    c.model = row->refObjId;
    c.item = item.refObjId;
    c.kind = kind;
    c.hp = c.maxHp = row->maxHp;
    c.walkSpeed = row->walkSpeed;
    c.runSpeed = row->runSpeed;
    if (kind == game::CosKind::Transport) {
        c.bagSize = std::clamp(host_.CosTransportBagSize(row->refObjId, o->level), 0, 255); // its trade bag, by its owner's level
        // Its numbers as a monster's (its row's): the bandits' hits are rolled against them.
        c.stats = game::MonsterCombatStats(*row, 0);
        c.stats.maxHp = std::max<std::uint32_t>(1, row->maxHp);
    }
    {
        // Its row (the original's _AddNewCOS, a ride's and a transport's), in its owner's order of saves: its bag's
        // saves wait for it.
        c.character = o->character;
        CosRow made;
        made.owner = o->character;
        made.refId = row->refObjId;
        made.hp = c.hp;
        made.state = std::uint8_t(game::CosState::Active);
        made.level = std::max(1, row->level);
        host_.CosCartCreate(o->character, made, TransportModels(), [this, entity, owner, character = o->character](std::int64_t id) {
            if (id == 0) return;
            const auto it = coses_.find(entity);
            if (it == coses_.end() || it->second.owner != owner || (it->second.kind != game::CosKind::Transport && it->second.kind != game::CosKind::Ride)) {
                host_.CosCartDelete(character, id); // gone before its row was made
                return;
            }
            it->second.cart = id;
        });
    }
    owned_[owner].push_back(entity);
    Send(owner, proto::CosSummoned{entity, c.model, std::uint8_t(kind), c.hp, c.maxHp, c.item});
    if (kind == game::CosKind::Transport && c.bagSize > 0) SendBag(c);
    if (kind == game::CosKind::Transport) host_.CosTransportBack(owner); // its trade scale's rule told (the deal window's)
    core::LogInfo("cos: {} summons {} ({})", o->name, row->codename, entity);
    // Boarded at once, a ride and a transport alike (the user's account of the original, 2026-11-14); a transport is
    // left and boarded again through its Ride command.
    Ride(owner, entity, true, now);
    return Use::Summoned;
}

Coses::Use Coses::UsePet(std::uint32_t owner, const assets::ItemRow& item, const game::ItemInstance& instance, game::CosKind kind) {
    const auto o = host_.CosOwner(owner);
    const assets::CharacterTable* characters = host_.CosCharacters();
    if (!o || characters == nullptr) return Use::NotCos;
    const auto refuse = [&](const char* text) {
        Refuse(owner, text);
        return Use::Refused;
    };
    if (!o->alive) return refuse("UIIT_MSG_COSPETERR_CANT_DEACTIVATE_IN_DEADSTATE");
    if (!o->free) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    if (o->level < item.requiredLevel) return refuse("UIIT_MSG_COS_HIGHER_LEVEL_REQUIRED_TO_USE_THISITEM");
    if (loading_.contains(owner)) return Use::Kept; // its last one is still coming
    if (instance.serial == 0) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM"); // not stored yet: nothing to link the pet to
    if (!host_.CosWorld().Where(owner)) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    if (o->fought) return refuse("UIIT_MSG_COS_CAN_NOT_CREATE_BATTLE");
    if (PlaceTaken(owner, kind)) return refuse("UIIT_MSG_COS_CANT_SUMMON_DUPLICATE_PETTYPE");
    // Its first summon makes its row (the item's Desc1, its level-1 row; inactive until it is out).
    const std::string codename = game::PetCodename(item);
    const assets::ObjectRow* first = characters->Find(std::string_view(codename));
    const std::int64_t serial = instance.serial;
    const std::uint32_t itemRef = item.refObjId;
    const int rental = game::PetRentalMinutes(item);
    const std::int64_t character = o->character;
    const auto create = [this, owner, first, kind, rental, serial, itemRef, character] {
        if (first == nullptr || game::CosKindOf(*first) != kind) {
            core::LogWarning("cos: a pet's item names {}, no such pet", first != nullptr ? first->codename : std::string("?"));
            Refuse(owner, "UIIT_MSG_COSPETERR_CANT_USEITEM");
            return;
        }
        CosRow row;
        row.owner = character;
        row.refId = first->refObjId;
        row.hp = first->maxHp;
        row.state = std::uint8_t(game::CosState::Inactive);
        row.level = std::max(1, first->level);
        row.hgp = kind == game::CosKind::AttackPet ? game::kCosHgpFull : 0;
        row.option = kind == game::CosKind::GrabPet ? kNewGrabOption : 0;
        row.rentUntil = rental > 0 ? UnixNow() + std::int64_t(rental) * 60 : 0;
        loading_.insert(owner);
        host_.CosCreate(owner, row, serial, [this, owner, row, serial, itemRef](std::int64_t id) mutable {
            loading_.erase(owner);
            if (id == 0) {
                Refuse(owner, "UIIT_MSG_COSPETERR_CANT_USEITEM");
                return;
            }
            row.id = id;
            PetReady(owner, row, serial, itemRef, true);
        });
    };
    if (instance.durability == 0) {
        create();
        return Use::Kept;
    }
    loading_.insert(owner);
    host_.CosLoad(std::int64_t(instance.durability), [this, owner, serial, itemRef, create](std::optional<CosRow> row) {
        loading_.erase(owner);
        if (!row) return create(); // the item names no row (gone): a new pet, as at its first summon
        PetReady(owner, *row, serial, itemRef, true);
    });
    return Use::Kept;
}

void Coses::PetReady(std::uint32_t owner, CosRow row, std::int64_t serial, std::uint32_t item, bool asked) {
    const auto o = host_.CosOwner(owner);
    const assets::CharacterTable* characters = host_.CosCharacters();
    if (!o || characters == nullptr) return; // gone meanwhile: its row stays as it is
    const auto refuse = [&](const char* text) {
        if (asked) Refuse(owner, text);
    };
    // Its rental over: dead from now on (its item says so); a dead pet's item is not used (the 1.188 client refuses it
    // itself with this text).
    if (Expire(row)) {
        host_.CosSave(row);
        SendPetItem(owner, row);
    }
    if (row.state == std::uint8_t(game::CosState::Dead)) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    if (row.level > o->level) return refuse("UIIT_MSG_COSPETERR_CANT_PETSUM_HIGHLEVEL");
    const assets::ObjectRow* shape = characters->Find(row.refId);
    const game::CosKind kind = shape != nullptr ? game::CosKindOf(*shape) : game::CosKind::None;
    if (!IsPet(kind)) {
        core::LogWarning("cos: pet {} is characterdata {}, no pet", row.id, row.refId);
        return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    }
    if (PlaceTaken(owner, kind)) return refuse("UIIT_MSG_COS_CANT_SUMMON_DUPLICATE_PETTYPE");
    // Its name in its appearance only while its owner is in no job (the original sends the pet's name only when the
    // owner's job type is 4, none; so a suit's wearer is not known by the name over its pet).
    const std::uint32_t entity = Place(owner, *shape, o->job == 0 ? row.name : std::string());
    if (entity == 0) return refuse("UIIT_MSG_COSPETERR_CANT_USEITEM");
    // Its item's holder is its owner now (an item traded while the pet was in it).
    row.owner = o->character;
    row.state = std::uint8_t(game::CosState::Summoned);
    if (row.hp == 0 || row.hp > shape->maxHp) row.hp = shape->maxHp;
    host_.CosSave(row);
    SendPetItem(owner, row);
    Cos& c = coses_[entity];
    c.entity = entity;
    c.owner = owner;
    c.model = shape->refObjId;
    c.item = item;
    c.kind = kind;
    c.hp = row.hp;
    c.maxHp = shape->maxHp;
    c.walkSpeed = shape->walkSpeed;
    c.runSpeed = shape->runSpeed;
    c.serial = serial;
    // A grab pet's bag, as its rows were read (a new pet's is empty): its own 28 places of those its row allocates,
    // more while a watch's job runs; what lies past them (a watch's
    // places that went away) stays where it is, out of reach until a watch's places come again.
    if (kind == game::CosKind::GrabPet) {
        c.bagMost = std::clamp(row.bagSize > 0 ? row.bagSize : kDefaultPetBag, 0, 255);
        c.bagSize = std::min(game::kGrabPetBag, c.bagMost);
        for (const StoredItem& s : row.bag) {
            if (s.slot < c.bagMost) c.bag[s.slot] = host_.CosItemOf(s);
        }
    }
    std::vector<PetJobRow> jobs = std::move(row.jobs);
    row.bag.clear();
    row.jobs.clear();
    c.row = row;
    // An attack pet's stance is its row's PetOption (the original's command mode; the 1.188 client's 1 offensive); 0, a
    // new pet's, defensive.
    if (kind == game::CosKind::AttackPet) c.aggressive = row.option == 1;
    if (kind == game::CosKind::AttackPet) Arm(c, *shape);
    // Its timed jobs on again for their time left (they ran on the wall clock while it was in its item); those over
    // meanwhile taken away.
    const std::int64_t unix = host_.CosNow();
    for (const PetJobRow& j : jobs) {
        if (j.until > unix) {
            StartJob(c, j, false);
        } else {
            host_.CosEndPetJob(row.id, j.category, j.job);
        }
    }
    owned_[owner].push_back(entity);
    host_.CosWorld().SetHp(entity, c.hp);
    Send(owner, SummonedOf(c));
    if (kind == game::CosKind::GrabPet) SendBag(c);
    core::LogInfo("cos: {} summons its pet {} ({}, row {})", o->name, shape->codename, entity, row.id);
}

proto::CosSummoned Coses::SummonedOf(const Cos& c) const {
    const CosRow& row = c.row;
    const std::int64_t rentLeft = row.rentUntil != 0 ? std::max<std::int64_t>(0, row.rentUntil - UnixNow()) : 0;
    proto::CosSummoned m{c.entity, c.model, std::uint8_t(c.kind), c.hp, c.maxHp, c.item, row.name, std::uint8_t(std::clamp(row.level, 0, 255)),
                         std::uint16_t(std::clamp(row.hgp, 0, 0xFFFF)), row.option, std::uint64_t(std::max<std::int64_t>(0, row.experience)),
                         std::uint32_t(std::min<std::int64_t>(rentLeft, 0xFFFFFFFF))};
    m.aggressive = c.aggressive;
    return m;
}

void Coses::SetHgp(Cos& c, int hgp, bool tell) {
    const bool wasHungry = c.row.hgp <= game::kCosHungry;
    c.row.hgp = std::clamp(hgp, 0, game::kCosHgpFull);
    if (wasHungry != (c.row.hgp <= game::kCosHungry)) Hunger(c);
    if (!tell) return;
    host_.CosSave(c.row);
    Send(c.owner, proto::CosHgp{c.entity, std::uint16_t(c.row.hgp)});
}

void Coses::Hunger(Cos& c) {
    // At kCosHungry or below its numbers are halved, else its own.
    c.stats = c.baseStats;
    if (c.row.hgp <= game::kCosHungry) {
        for (double* v : {&c.stats.phyDefence, &c.stats.magDefence, &c.stats.hitRate, &c.stats.evasion}) *v /= 2.0;
        c.stats.phyAttackMin /= 2.0;
        c.stats.phyAttackMax /= 2.0;
        c.stats.magAttackMin /= 2.0;
        c.stats.magAttackMax /= 2.0;
    }
    // Its skills' buffs (the mall's pet skills: 'dru' 20 20, its hits' damage +20 %),
    // through the parameter keeper's sum as a player's (the skill keeper's effect on the pet).
    game::StatSheet sheet;
    for (const Cos::Job& j : c.jobs) {
        if (j.skill != nullptr) sheet.Add(j.skill->id, game::BuffChanges(*j.skill, game::BuffBasis{}));
    }
    if (!sheet.Empty()) game::ApplySheet(c.stats, sheet);
}

void Coses::Eat(Cos& c, double seconds) {
    if (c.hgpRate <= 0.0 || c.row.hgp == 0 || seconds <= 0.0) return;
    c.hgpCarry += c.hgpRate * seconds;
    if (c.hgpCarry < 1.0) return;
    const int whole = int(c.hgpCarry);
    c.hgpCarry -= whole;
    const int before = c.row.hgp, after = std::max(0, before - whole);
    // Saved and told when its whole percent comes to a tenth (40 %, 30 %, ...) from one that is not, and at none, as the
    // original's tick does.
    const int percentAfter = after / 100, percentBefore = before / 100;
    const bool step = after == 0 || (percentAfter % 10 == 0 && percentBefore % 10 != 0);
    SetHgp(c, after, step);
}

void Coses::Arm(Cos& c, const assets::ObjectRow& shape) {
    c.stats = game::MonsterCombatStats(shape, 0);
    c.stats.level = std::max(1, c.row.level);
    c.stats.maxHp = std::max<std::uint32_t>(1, shape.maxHp);
    c.hgpRate = game::PetHgpPerSecond(shape);
    c.baseStats = c.stats;
    Hunger(c);
    c.skill = nullptr;
    if (const assets::SkillTable* skills = host_.CosSkills()) {
        for (const std::uint32_t id : shape.skills) {
            const assets::SkillRow* s = id != 0 ? skills->Find(id) : nullptr;
            if (s != nullptr && s->attack) {
                c.skill = s;
                break;
            }
        }
    }
}

Coses::Cos* Coses::AttackPetOf(std::uint32_t owner) {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return nullptr;
    for (const std::uint32_t id : it->second) {
        if (Cos& c = coses_.at(id); c.kind == game::CosKind::AttackPet) return &c;
    }
    return nullptr;
}

int Coses::AttackPetLevel(std::uint32_t owner) const {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return 0;
    for (const std::uint32_t id : it->second) {
        if (const Cos& c = coses_.at(id); c.kind == game::CosKind::AttackPet && c.hp > 0) return std::max(1, c.row.level);
    }
    return 0;
}

void Coses::Fighters(std::vector<Monsters::Player>& out) const {
    for (const auto& [entity, c] : coses_) {
        // The attack pets, fought as players are; the transports by the bandits only. A COS
        // answers for its owner's job, its owner the other a bandit goes between.
        const bool transport = c.kind == game::CosKind::Transport;
        if ((!Fights(c.kind) && !transport) || c.hp == 0) continue;
        const auto at = host_.CosWorld().Where(entity);
        if (!at) continue;
        Monsters::Player p;
        p.entity = entity;
        p.position = at->position;
        p.alive = true;
        p.stats = &c.stats;
        p.partner = c.owner;
        p.ridden = c.ridden;
        p.banditsOnly = transport;
        if (const auto o = const_cast<CosHost&>(host_).CosOwner(c.owner); o && o->job != 0) p.job = o->job;
        out.push_back(p);
    }
}

void Coses::Engage(Cos& c, std::uint32_t monster) {
    if (!Fights(c.kind) || c.hp == 0 || c.skill == nullptr || monster == 0 || monster == c.target) return;
    if (!host_.CosFoe(monster)) return;
    c.target = monster;
    c.picking = 0;
}

void Coses::OwnerFights(std::uint32_t owner, std::uint32_t monster) {
    if (Cos* c = AttackPetOf(owner); c != nullptr && c->aggressive && c->target == 0) Engage(*c, monster);
    EngageSoldiers(owner, monster);
}

// Defensive, it fights only on its owner's Attack (the 1.188 client orders it onto its owner's targets only in stance
// 1; the server keeps the mode in _CharCOS and fights by no other rule; the user's
// report 2026-11-14): its defending its owner and fighting back are an offensive pet's (ours).
void Coses::OwnerHurt(std::uint32_t owner, std::uint32_t monster) {
    if (Cos* c = AttackPetOf(owner); c != nullptr && c->aggressive && c->target == 0) Engage(*c, monster);
    EngageSoldiers(owner, monster);
}

// A guild's soldiers take their master's foe, each one not in a fight already (the original's mercenary AI: its owner's
// hostile targets).
void Coses::EngageSoldiers(std::uint32_t owner, std::uint32_t monster) {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return;
    for (const std::uint32_t id : it->second) {
        if (Cos& c = coses_.at(id); c.kind == game::CosKind::Soldier && c.target == 0) Engage(c, monster);
    }
}

void Coses::ArmSoldier(Cos& c, const assets::ObjectRow& shape) {
    game::CombatStats base = game::MonsterCombatStats(shape, 0);
    base.level = std::max(1, int(shape.level));
    base.maxHp = std::max<std::uint32_t>(1, shape.maxHp);
    c.baseStats = base;
    c.stats = game::WithMercenaryAbilities(base, c.abilities);
    c.skill = nullptr;
    if (const assets::SkillTable* skills = host_.CosSkills()) {
        for (const std::uint32_t id : shape.skills) {
            const assets::SkillRow* s = id != 0 ? skills->Find(id) : nullptr;
            if (s != nullptr && s->attack) {
                c.skill = s;
                break;
            }
        }
    }
}

std::vector<std::uint32_t> Coses::SummonSoldiers(std::uint32_t owner, const assets::ObjectRow& shape, int count, int minutes, std::uint8_t abilities,
                                                 Clock::time_point now) {
    std::vector<std::uint32_t> out;
    const auto o = host_.CosOwner(owner);
    for (int i = 0; i < count; ++i) {
        const std::uint32_t entity = Place(owner, shape);
        if (entity == 0) break;
        Cos& c = coses_[entity];
        c.entity = entity;
        c.owner = owner;
        c.model = shape.refObjId;
        c.kind = game::CosKind::Soldier;
        c.walkSpeed = shape.walkSpeed;
        c.runSpeed = shape.runSpeed;
        c.aggressive = true; // its owner's foes and its own attackers (the original's mercenary takes any hostile target)
        c.abilities = abilities;
        c.until = now + std::chrono::minutes(std::max(1, minutes));
        ArmSoldier(c, shape);
        c.maxHp = c.stats.maxHp;
        c.hp = c.maxHp;
        owned_[owner].push_back(entity);
        host_.CosWorld().SetHp(entity, c.hp);
        Send(owner, SummonedOf(c));
        out.push_back(entity);
    }
    if (o) {
        core::LogInfo("cos: {} summons {} of {} guild soldiers {} for {} minutes (abilities {:#04x})", o->name, out.size(), count, shape.codename,
                      minutes, int(abilities));
    }
    return out;
}

int Coses::SoldiersOut(std::uint32_t owner) const {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return 0;
    return int(std::count_if(it->second.begin(), it->second.end(), [&](std::uint32_t id) { return coses_.at(id).kind == game::CosKind::Soldier; }));
}

int Coses::ReleaseSoldiers(std::uint32_t owner) {
    std::vector<std::uint32_t> soldiers;
    if (const auto it = owned_.find(owner); it != owned_.end()) {
        for (const std::uint32_t id : it->second) {
            if (coses_.at(id).kind == game::CosKind::Soldier) soldiers.push_back(id);
        }
    }
    for (const std::uint32_t id : soldiers) Dismiss(id, false, false, true);
    if (!soldiers.empty()) core::LogInfo("cos: entity {} releases its {} guild soldiers", owner, soldiers.size());
    return int(soldiers.size());
}

void Coses::SoldierAbilities(std::uint32_t owner, std::uint8_t abilities) {
    const auto it = owned_.find(owner);
    const assets::CharacterTable* characters = host_.CosCharacters();
    if (it == owned_.end() || characters == nullptr) return;
    for (const std::uint32_t id : it->second) {
        Cos& c = coses_.at(id);
        if (c.kind != game::CosKind::Soldier || c.abilities == abilities) continue;
        const assets::ObjectRow* shape = characters->Find(c.model);
        if (shape == nullptr) continue;
        const double share = c.maxHp > 0 ? double(c.hp) / double(c.maxHp) : 1.0;
        c.abilities = abilities;
        ArmSoldier(c, *shape);
        c.maxHp = c.stats.maxHp;
        c.hp = std::clamp<std::uint32_t>(std::uint32_t(share * double(c.maxHp) + 0.5), c.hp == 0 ? 0u : 1u, c.maxHp);
        host_.CosWorld().SetHp(id, c.hp);
        Send(owner, SummonedOf(c));
    }
}

namespace {
std::vector<proto::StateRecord> RecordsOf(const game::StateSet& states, Coses::Clock::time_point now) {
    std::vector<proto::StateRecord> records;
    for (const game::ActiveState& a : states.All()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(a.Ends() - now).count();
        records.push_back({std::uint8_t(a.kind), std::uint8_t(std::clamp(a.level, 0, 255)), std::uint32_t(std::max<long long>(0, left))});
    }
    return records;
}
} // namespace

std::optional<game::StateDefence> Coses::StateDefenceOf(std::uint32_t entity) const {
    const auto it = coses_.find(entity);
    if (it == coses_.end() || it->second.hp == 0) return std::nullopt;
    const Cos& c = it->second;
    // Its level: its row's (a pet's), else its character row's (a scroll's ride or transport).
    int level = c.row.id != 0 ? c.row.level : 1;
    if (c.row.id == 0) {
        if (const assets::CharacterTable* characters = host_.CosCharacters()) {
            if (const assets::ObjectRow* row = characters->Find(c.model)) level = std::max(1, int(row->level));
        }
    }
    return MonsterStateDefence(level, {});
}

int Coses::SusceptibilityOf(std::uint32_t entity) const {
    const auto it = coses_.find(entity);
    return it != coses_.end() ? it->second.states.Susceptibility() : 0;
}

bool Coses::Afflict(std::uint32_t entity, const std::vector<game::ActiveState>& states, Clock::time_point now) {
    const auto it = coses_.find(entity);
    if (it == coses_.end() || it->second.hp == 0 || states.empty()) return false;
    Cos& c = it->second;
    if (c.kind == game::CosKind::Ride) return false; // a horse takes no states (upstream's review of 2026-11-22)
    bool took = false;
    for (const game::ActiveState& s : states) took = c.states.Apply(s) || took;
    if (!took) return false;
    StatesChanged(c, now);
    return true;
}

void Coses::StatesChanged(Cos& c, Clock::time_point now) {
    World& world = host_.CosWorld();
    // Frozen, asleep or stunned it stands; frostbite and slow slow its walk.
    if (c.states.Locked() || c.states.Rooted()) {
        if (const auto at = world.Where(c.entity); at && world.Moving(c.entity)) world.MoveTo(c.entity, at->position);
    }
    if (const int speed = c.states.SpeedPercent(); speed != c.speedPercent) {
        c.speedPercent = speed;
        world.SetSpeedFactor(c.entity, float(speed) / 100.0f);
    }
    host_.CosStates(c.entity, RecordsOf(c.states, now));
}

void Coses::TickStates(Clock::time_point now) {
    // By entity: a burn's last tick may end a COS (Hurt, Dismiss).
    std::vector<std::uint32_t> afflicted;
    for (const auto& [entity, c] : coses_) {
        if (c.states.Mask() != 0 && c.hp > 0) afflicted.push_back(entity);
    }
    for (const std::uint32_t entity : afflicted) {
        auto it = coses_.find(entity);
        if (it == coses_.end()) continue;
        for (const game::StateTick& t : it->second.states.Ticks(now, it->second.hp, 0, it->second.stats.magAbsorption)) {
            game::HitResult hit;
            hit.damage = t.hp;
            Hurt(entity, t.source, hit);
            it = coses_.find(entity);
            if (it == coses_.end() || it->second.hp == 0) break;
        }
        if (it == coses_.end() || it->second.hp == 0) continue;
        if (!it->second.states.Expire(now).empty()) StatesChanged(it->second, now);
    }
}

void Coses::Hurt(std::uint32_t pet, std::uint32_t monster, const game::HitResult& hit) {
    const auto it = coses_.find(pet);
    if (it == coses_.end() || it->second.hp == 0) return;
    Cos& c = it->second;
    const std::uint64_t dealt = std::min<std::uint64_t>(hit.damage, c.hp);
    c.hp = hit.damage >= c.hp ? 0 : c.hp - hit.damage;
    host_.CosWorld().SetHp(pet, c.hp);
    if (dealt > 0) {
        const auto by = std::find_if(c.damageBy.begin(), c.damageBy.end(), [&](const auto& e) { return e.first == monster; });
        if (by != c.damageBy.end()) by->second += dealt;
        else c.damageBy.push_back({monster, dealt});
    }
    std::uint8_t flags = hit.critical ? proto::kDamageCritical : 0;
    if (hit.blocked) flags |= proto::kDamageBlocked;
    if (c.hp == 0) flags |= proto::kDamageKilled;
    host_.CosBroadcast(pet, proto::EntityDamaged::kId, proto::Encode(proto::EntityDamaged{pet, monster, hit.damage, c.hp, flags}));
    if (c.hp > 0) {
        if (c.target == 0 && c.aggressive) Engage(c, monster); // offensive, it fights back (defensive: only on orders)
        return;
    }
    // Dead (a character's death, then released from its owner,
    // who is told): its experience less the death penalty (killed by a monster, by its owner's state: a
    // murderer's 6 % and x 300), never under none and no level lost (a loss keeps its
    // level row); its row Dead, back into its item, which says so.
    std::uint64_t lost = 0;
    const std::span<const std::uint64_t> levels = host_.CosLevels();
    const std::span<const std::uint32_t> mobExp = host_.CosMonsterExperience();
    // Its killers credited as a monster's (a ride's, a transport's, a grab pet's: the original's COS death credit).
    if (c.kind == game::CosKind::Ride || c.kind == game::CosKind::Transport || c.kind == game::CosKind::GrabPet) host_.CosKilled(pet, c.model, c.damageBy, monster);
    if (c.row.id == 0) { // a scroll's COS (a ride, a transport: the bandits' hits): no pet's experience
        if (const auto o = host_.CosOwner(c.owner)) core::LogInfo("cos: {}'s COS {} dies", o->name, pet);
        Dismiss(pet, false, true); // a transport's cargo: every slot on the ground where it fell (Dismiss)
        return;
    }
    if (const std::size_t level = std::size_t(std::max(1, c.row.level)); level < levels.size() && level < mobExp.size()) {
        const auto owner = host_.CosOwner(c.owner);
        lost = std::min(game::PetDeathPenalty(levels[level], mobExp[level], false, owner && owner->murderer),
                        std::uint64_t(std::max<std::int64_t>(0, c.row.experience)));
        c.row.experience -= std::int64_t(lost);
    }
    if (lost > 0) Send(c.owner, proto::CosExperience{c.entity, std::uint64_t(std::max<std::int64_t>(0, c.row.experience)), 0, lost});
    if (const auto o = host_.CosOwner(c.owner)) core::LogInfo("cos: {}'s pet {} dies ({} experience lost)", o->name, c.row.id, lost);
    Dismiss(pet, false, true);
}

void Coses::Fight(Cos& c, Clock::time_point now) {
    World& world = host_.CosWorld();
    const std::uint32_t entity = c.entity;
    world.SetSpeeds(entity, c.walkSpeed, c.runSpeed); // its own row's pace in a fight (its owner's only as it follows)
    // The hits of its swing land when their time comes, on a foe still within reach (one at a time: a hit's kill can
    // take the pet away, its owner leaving with the news).
    for (std::size_t i = 0; i < c.pending.size();) {
        if (now < c.pending[i].at) {
            ++i;
            continue;
        }
        const Cos::Pending p = c.pending[i];
        c.pending.erase(c.pending.begin() + std::ptrdiff_t(i));
        const auto foe = host_.CosFoe(p.target);
        const auto self = world.Where(entity);
        if (!foe || !self || foe->hp == 0) continue;
        const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(foe->position);
        if (std::hypot(a.x - b.x, a.z - b.z) > p.reach) continue;
        // Its imbue (a mall pet skill running) joins a hit whose 'att' takes a share of
        // it, as a player's (its force, none for the pets' three, then its states).
        const assets::SkillRow* imbue = p.attack.formula && p.attack.imbuePercent > 0 ? ImbueOf(c) : nullptr;
        const game::AttackSpec force = imbue != nullptr ? game::AttackOf(*imbue) : game::AttackSpec{};
        game::HitRoll who{false, false, &c.rolls, 100, foe->hp};
        if (foe->target) {
            who.defenderStates = foe->target->states;
            who.defenderDown = foe->target->down;
        }
        const game::HitResult hit = game::RollHit(c.stats, *foe->stats, p.attack, who, random_, imbue != nullptr ? &force : nullptr);
        const Monsters::Affliction affliction = imbue != nullptr ? ImbueAffliction(c, *imbue, *foe, hit, now) : Monsters::Affliction{};
        // The lightning's jump (its 'efr' shape 6): the nearest other monster within its range of
        // the one struck, at the share its falloff leaves, the magical parts only (the imbue's force: none here), its
        // states rolled on it too.
        std::optional<std::pair<std::uint32_t, game::HitResult>> jump;
        Monsters::Affliction jumpAffliction;
        if (imbue != nullptr) {
            const assets::SkillParam area = assets::FindSkillParam(*imbue, assets::SkillTag("efr"), 1); // its attack's area (kind 1)
            if (area && area.Int(1) == 6 && (area[5] & 0x08) != 0 && area[3] != 1) {
                const double range = double(area[2]);
                std::uint32_t best = 0;
                double nearest = range;
                std::optional<CosHost::Foe> next;
                for (const std::uint32_t id : host_.CosMonstersNear(foe->position, float(range) + 100.0f)) {
                    if (id == p.target) continue;
                    const auto other = host_.CosFoe(id);
                    if (!other || other->hp == 0) continue;
                    const proto::FlatPosition q = proto::ToFlat(other->position);
                    const double d = std::hypot(q.x - b.x, q.z - b.z);
                    if (d <= nearest) {
                        nearest = d;
                        best = id;
                        next = other;
                    }
                }
                if (best != 0) {
                    game::HitRoll second{false, false, &c.rolls, 100 - std::clamp(area.Int(4), 0, 100), next->hp};
                    second.magicalOnly = true;
                    if (next->target) {
                        second.defenderStates = next->target->states;
                        second.defenderDown = next->target->down;
                    }
                    const game::HitResult h = game::RollHit(c.stats, *next->stats, p.attack, second, random_, &force);
                    jump = std::make_pair(best, h);
                    jumpAffliction = ImbueAffliction(c, *imbue, *next, h, now);
                }
            }
        }
        host_.CosStrike(entity, p.target, hit, imbue != nullptr ? &affliction : nullptr, imbue != nullptr ? imbue->id : 0u);
        if (!coses_.contains(entity)) return;
        if (jump) {
            host_.CosStrike(entity, jump->first, jump->second, &jumpAffliction, imbue->id);
            if (!coses_.contains(entity)) return;
        }
    }
    if (c.target == 0 || c.skill == nullptr || c.hp == 0) return;
    const auto foe = host_.CosFoe(c.target);
    const auto self = world.Where(c.entity);
    const auto owner = world.Where(c.owner);
    if (!foe || foe->hp == 0 || !self || !owner) {
        c.target = 0;
        return;
    }
    const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(foe->position), o = proto::ToFlat(owner->position);
    // Led too far from its owner: it gives the fight up (ours: the monsters' own give-up distance).
    if (std::hypot(o.x - b.x, o.z - b.z) > kFightLeash) {
        c.target = 0;
        return;
    }
    const double distance = std::hypot(a.x - b.x, a.z - b.z);
    const float reach = float(std::max(c.skill->range, 10)) + c.stats.bodyRadius + foe->stats->bodyRadius;
    if (distance > reach) {
        // After it: stopped short of it, on the line between them.
        const double stop = reach * 0.7, k = std::max(0.0, distance - stop) / std::max(distance, 1e-6);
        world.MoveTo(c.entity, proto::FromFlat({a.x + (b.x - a.x) * k, a.z + (b.z - a.z) * k}, foe->position.y), false);
        return;
    }
    if (world.Moving(c.entity)) world.MoveTo(c.entity, self->position); // stops where it stands
    if (now < c.nextSwing) return;
    host_.CosBroadcast(c.entity, proto::EntitySwing::kId, proto::Encode(proto::EntitySwing{c.entity, c.target, c.skill->id}));
    game::AttackSpec attack = game::AttackOf(*c.skill);
    if (c.row.hgp <= game::kCosHungry) { // hungry: its attack halved too (its skill's own numbers)
        attack.flatMin /= 2;
        attack.flatMax /= 2;
    }
    const int duration = std::max(500, c.skill->durationMs);
    for (int h = 0; h < attack.hits; ++h) {
        const float at = 0.3f + 0.4f * float(h) / float(std::max(1, attack.hits)); // as a monster's (ours: the clips' hit events are the client's)
        c.pending.push_back({now + std::chrono::milliseconds(int(float(duration) * at)), c.target, attack, reach * 1.5f});
    }
    c.nextSwing = now + std::chrono::milliseconds(std::max(duration, c.skill->cooldownMs) + std::uniform_int_distribution<int>(0, 300)(random_));
}

void Coses::GainExperience(std::uint32_t owner, std::uint64_t experience) {
    Cos* found = AttackPetOf(owner);
    const auto o = host_.CosOwner(owner);
    if (found == nullptr || !o || experience == 0 || found->hp == 0) return;
    Cos& c = *found;
    const game::PetGrowth g = game::GrowPet(std::max(1, c.row.level), std::uint64_t(std::max<std::int64_t>(0, c.row.experience)), experience, o->level,
                                            host_.CosLevels());
    c.row.experience = std::int64_t(std::min<std::uint64_t>(g.experience, std::uint64_t(INT64_MAX)));
    if (g.levelsGained > 0) {
        c.row.level = g.level;
        // Its next level's row: its numbers, its health full again.
        const assets::CharacterTable* characters = host_.CosCharacters();
        const assets::ObjectRow* now = characters != nullptr ? characters->Find(c.model) : nullptr;
        const std::string next = now != nullptr ? game::PetCodenameAtLevel(now->codename, g.level) : std::string();
        if (const assets::ObjectRow* shape = !next.empty() ? characters->Find(std::string_view(next)) : nullptr) {
            c.row.refId = shape->refObjId;
            c.model = shape->refObjId;
            c.maxHp = std::max<std::uint32_t>(1, shape->maxHp);
            c.walkSpeed = shape->walkSpeed;
            c.runSpeed = shape->runSpeed;
            Arm(c, *shape);
            host_.CosWorld().SetModel(c.entity, c.model);
            host_.CosWorld().SetSpeeds(c.entity, c.walkSpeed, c.runSpeed);
        } else {
            core::LogWarning("cos: pet {} reaches level {}, no row {}", c.row.id, g.level, next);
            c.stats.level = g.level;
        }
        c.hp = c.maxHp;
        c.row.hp = c.hp;
        host_.CosWorld().SetHp(c.entity, c.hp);
        host_.CosBroadcast(c.entity, proto::EntityLevelUp::kId, proto::Encode(proto::EntityLevelUp{c.entity, std::uint8_t(std::clamp(g.level, 0, 255))}));
        Send(owner, SummonedOf(c)); // its new row, level and health to its owner
        SendPetItem(owner, c.row);
        core::LogInfo("cos: {}'s pet {} reaches level {}", o->name, c.row.id, g.level);
    }
    c.row.hp = c.hp;
    host_.CosSave(c.row);
    Send(owner, proto::CosExperience{c.entity, std::uint64_t(c.row.experience), experience});
}

namespace {
// A COS of `have` answers for `want` (None: any; Transport: a ride or a transport).
bool KindFits(game::CosKind have, game::CosKind want) {
    if (want == game::CosKind::None) return true;
    if (want == game::CosKind::Transport) return have == game::CosKind::Transport || have == game::CosKind::Ride;
    return have == want;
}
} // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>> Coses::Health(std::uint32_t owner, game::CosKind kind) const {
    if (const auto it = owned_.find(owner); it != owned_.end()) {
        for (const std::uint32_t id : it->second) {
            const Cos& c = coses_.at(id);
            if (c.hp != 0 && c.maxHp != 0 && KindFits(c.kind, kind)) return std::pair{c.hp, c.maxHp};
        }
    }
    return std::nullopt;
}

std::uint32_t Coses::HealPet(std::uint32_t owner, std::uint32_t hp, game::CosKind kind) {
    // a COS of its owner's, alive (else 0x1871 / 3); given at once, even at full
    // (the original checks no gauge: the kit is used all the same).
    Cos* best = nullptr;
    if (const auto it = owned_.find(owner); it != owned_.end()) {
        for (const std::uint32_t id : it->second) {
            Cos& c = coses_.at(id);
            if (c.hp == 0 || c.maxHp == 0 || !KindFits(c.kind, kind)) continue;
            if (best == nullptr || double(c.hp) / double(c.maxHp) < double(best->hp) / double(best->maxHp)) best = &c;
        }
    }
    if (best == nullptr) {
        Refuse(owner, "UIIT_MSG_COSERR_YOU_DONT_HAVE_ACTIVE_COS_OBJ");
        return 0;
    }
    Cos& c = *best;
    c.hp = std::uint32_t(std::min<std::uint64_t>(c.maxHp, std::uint64_t(c.hp) + hp));
    c.row.hp = c.hp;
    host_.CosWorld().SetHp(c.entity, c.hp);
    host_.CosBroadcast(c.entity, proto::EntityDamaged::kId, proto::Encode(proto::EntityDamaged{c.entity, 0, 0, c.hp, 0}));
    if (c.row.id != 0) host_.CosSave(c.row);
    return c.entity;
}

void Coses::Recover(Cos& c, Clock::time_point now) {
    if (c.nextRegen == Clock::time_point{}) c.nextRegen = now + kRegenPeriod; // its first tick a period after it came out
    if (now < c.nextRegen) return;
    c.nextRegen = now + kRegenPeriod;
    // Alive, standing (its motion 0 or 4: ours, not walking and in no fight), short of its most.
    if (c.hp == 0 || c.hp >= c.maxHp || c.target != 0 || !c.pending.empty() || host_.CosWorld().Moving(c.entity)) return;
    const auto gain = std::uint32_t(double(c.maxHp) * kRegenShare);
    if (gain == 0) return;
    c.hp = std::min(c.maxHp, c.hp + gain);
    c.row.hp = c.hp; // saved with its row's next save
    host_.CosWorld().SetHp(c.entity, c.hp);
    host_.CosBroadcast(c.entity, proto::EntityDamaged::kId, proto::Encode(proto::EntityDamaged{c.entity, 0, 0, c.hp, 0}));
}

void Coses::SendBag(const Cos& c) {
    proto::CosBag m;
    m.entity = c.entity;
    m.size = std::uint8_t(c.bagSize);
    for (const auto& [place, item] : c.bag) {
        if (place < c.bagSize) m.items.push_back(RecordOf(place, item)); // past its places: kept, not shown
    }
    Send(c.owner, m);
}

void Coses::SaveBag(Cos& c, const std::vector<std::uint8_t>& bagTouched, const std::vector<std::uint8_t>& inventoryTouched) {
    if (c.row.id != 0) host_.CosSaveBag(c.owner, c.row.id, c.bag, bagTouched, inventoryTouched);
}

void Coses::SetOption(Cos& c, std::uint32_t option) {
    c.row.option = option & proto::kGrabMask;
    if ((c.row.option & proto::kGrabOn) == 0) c.picking = 0;
    host_.CosSave(c.row);
    Send(c.owner, proto::CosOption{c.entity, c.row.option});
}

int Coses::BagPlaceFor(const Cos& c, std::uint32_t refId, int count) const {
    const assets::ItemTable* items = host_.CosItems();
    const assets::ItemRow* row = items != nullptr ? items->Find(refId) : nullptr;
    if (row != nullptr && game::MaxStack(*row) > 1) {
        for (const auto& [place, item] : c.bag) {
            if (place < c.bagSize && item.refId == refId && int(item.count) + count <= game::MaxStack(*row)) return place;
        }
    }
    for (int place = 0; place < c.bagSize; ++place) {
        if (!c.bag.contains(std::uint8_t(place))) return place;
    }
    return -1;
}

bool Coses::Grab(Cos& c) {
    if (c.kind != game::CosKind::GrabPet || (c.row.option & proto::kGrabOn) == 0) {
        c.picking = 0;
        return false;
    }
    World& world = host_.CosWorld();
    const auto self = world.Where(c.entity);
    if (!self) return false;
    // Going for one: picked up once there.
    if (c.picking != 0) {
        const auto there = world.Where(c.picking);
        if (!there) {
            c.picking = 0; // gone (picked up by another, expired)
            return false;
        }
        const proto::FlatPosition a = proto::ToFlat(self->position), b = proto::ToFlat(there->position);
        if (std::hypot(a.x - b.x, a.z - b.z) > kGrabReach) {
            if (!world.Moving(c.entity)) world.MoveTo(c.entity, there->position);
            return true;
        }
        const std::uint32_t target = std::exchange(c.picking, 0u);
        std::vector<std::uint8_t> touched;
        const CosHost::Picked picked = host_.CosPickUp(c.owner, c.entity, target, [&](game::ItemInstance& item) {
            const int place = BagPlaceFor(c, item.refId, item.count);
            if (place < 0) return false;
            if (auto it = c.bag.find(std::uint8_t(place)); it != c.bag.end()) {
                it->second.count = std::uint16_t(it->second.count + item.count);
            } else {
                c.bag[std::uint8_t(place)] = item;
            }
            touched.push_back(std::uint8_t(place));
            return true;
        });
        if (picked == CosHost::Picked::Taken && !touched.empty()) {
            SaveBag(c, touched);
            SendBag(c);
        } else if (picked == CosHost::Picked::NoRoom) {
            // Its bag full: grabbing off (the 1.188 client's text says so).
            SetOption(c, c.row.option & ~proto::kGrabOn);
            Refuse(c.owner, "UIIT_MSG_COSPETERR_CANT_PICKITEM1");
        }
        return true;
    }
    // The nearest one it may take, not another pet's already.
    const std::vector<CosHost::GroundSeen> seen = host_.CosGroundNear(c.owner, kGrabRange);
    const proto::FlatPosition me = proto::ToFlat(self->position);
    const CosHost::GroundSeen* best = nullptr;
    double bestDistance = 0;
    for (const CosHost::GroundSeen& g : seen) {
        if ((g.kind & c.row.option) == 0 || (!g.mine && (c.row.option & proto::kGrabAnyone) == 0)) continue;
        if (std::any_of(coses_.begin(), coses_.end(), [&](const auto& other) { return other.second.picking == g.entity; })) continue;
        const proto::FlatPosition p = proto::ToFlat(g.position);
        const double d = std::hypot(p.x - me.x, p.z - me.z);
        if (best == nullptr || d < bestDistance) {
            best = &g;
            bestDistance = d;
        }
    }
    if (best == nullptr) return false;
    c.picking = best->entity;
    world.MoveTo(c.entity, best->position);
    return true;
}

void Coses::MoveInBag(std::uint32_t owner, const proto::CosBagMove& m) {
    const auto it = coses_.find(m.entity);
    if (it == coses_.end() || it->second.owner != owner || it->second.kind != game::CosKind::GrabPet) return;
    Cos& c = it->second;
    const auto o = host_.CosOwner(owner);
    game::Inventory* inventory = host_.CosInventory(owner);
    const assets::ItemTable* items = host_.CosItems();
    if (!o || !o->alive || !o->free || inventory == nullptr || items == nullptr) return;
    // Where the item is and where it goes: the bag's places, or the inventory's bag places (not the worn ones).
    const auto inBag = [&](std::uint8_t place) { return place < c.bagSize; };
    const auto inInventory = [&](std::uint8_t place) { return place >= game::slot::kWorn && place < inventory->size; };
    if ((m.fromPet && !inBag(m.from)) || (!m.fromPet && !inInventory(m.from)) || (m.toPet && !inBag(m.to)) || (!m.toPet && !inInventory(m.to))) return;
    if (m.fromPet == m.toPet && (!m.fromPet || m.from == m.to)) return; // the inventory's own moves are its own (ItemMove)
    auto& fromSide = m.fromPet ? c.bag : inventory->places;
    auto& toSide = m.toPet ? c.bag : inventory->places;
    const auto source = fromSide.find(m.from);
    if (source == fromSide.end()) return;
    const assets::ItemRow* row = items->Find(source->second.refId);
    if (row == nullptr) return;
    // A pet's summoning item never into a pet's bag (the 1.188 client's text).
    if (m.toPet && !m.fromPet && game::IsCosSummoner(*row)) return Refuse(owner, "UIIT_MSG_COSPETERR_CANT_MOVETO_SUMITEM");
    std::vector<std::uint8_t> bagTouched, inventoryTouched;
    const auto touch = [&](bool pet, std::uint8_t place) { (pet ? bagTouched : inventoryTouched).push_back(place); };
    const auto dest = toSide.find(m.to);
    if (dest == toSide.end()) {
        toSide[m.to] = source->second; // onto an empty place
        fromSide.erase(m.from);
    } else if (dest->second.refId == source->second.refId && game::MaxStack(*row) > 1 &&
               int(dest->second.count) + int(source->second.count) <= game::MaxStack(*row)) {
        dest->second.count = std::uint16_t(dest->second.count + source->second.count); // onto a stack of it
        fromSide.erase(m.from);
    } else if (m.fromPet == m.toPet) {
        std::swap(source->second, dest->second); // within the bag: the two change places
    } else {
        return Refuse(owner, "UIIT_MSG_COSPETERR_CANT_USEITEM"); // ours: a full place on the other side
    }
    touch(m.fromPet, m.from);
    touch(m.toPet, m.to);
    if (!inventoryTouched.empty()) host_.CosInventoryChanged(owner, inventoryTouched);
    SaveBag(c, bagTouched, inventoryTouched);
    SendBag(c);
}

bool Coses::Handle(std::uint32_t owner, proto::MessageId id, std::span<const std::byte> payload, Clock::time_point now) {
    switch (id) {
    case proto::CosRide::kId: {
        proto::CosRide m;
        if (!proto::Decode(payload, m)) return false;
        // A ride is not left standing: getting off it is its end (only a transport is boarded and left again).
        if (const auto it = coses_.find(m.entity); !m.ride && it != coses_.end() && it->second.owner == owner && it->second.kind == game::CosKind::Ride) {
            Dismiss(m.entity);
            return true;
        }
        Ride(owner, m.entity, m.ride, now);
        return true;
    }
    case proto::CosDismiss::kId: {
        proto::CosDismiss m;
        if (!proto::Decode(payload, m)) return false;
        const auto it = coses_.find(m.entity);
        if (it == coses_.end() || it->second.owner != owner) return true; // not its own: nothing happens
        // A guild's soldier: all of them released (the original's 0x725A, which its client sends for a mercenary's unsummon).
        if (it->second.kind == game::CosKind::Soldier) {
            ReleaseSoldiers(owner);
            return true;
        }
        // A pet put away (the original's 0x7116): refused in battle (0x440D), dead (0x4411), in a free-PvP change
        // (0x4420), from 100 or more away (4). A ride or transport terminated (0x70C6): no such check.
        if (IsPet(it->second.kind)) {
            const auto o = host_.CosOwner(owner);
            if (o && o->fought) return Refuse(owner, "UIIT_MSG_COSPETERR_CANT_PETRE_BATTLE"), true;
            if (o && !o->alive) return Refuse(owner, "UIIT_MSG_COSPETERR_CANT_DEACTIVATE_IN_DEADSTATE"), true;
            if (o && o->freePvpChanging) return Refuse(owner, "UIIT_MGS_ERR_FRPVP_DURING_CHANGE"), true;
            const auto a = host_.CosWorld().Where(owner), b = host_.CosWorld().Where(m.entity);
            if (a && b && Distance3(a->position, b->position) >= 100.0) return true;
        }
        Dismiss(m.entity, false, false, true); // terminated: a transport's cargo on the ground
        return true;
    }
    case proto::CosCommand::kId: {
        proto::CosCommand m;
        if (!proto::Decode(payload, m)) return false;
        const auto it = coses_.find(m.entity);
        if (it == coses_.end() || it->second.owner != owner || !IsPet(it->second.kind)) return true;
        Cos& c = it->second;
        if (m.order == proto::CosOrder::Follow) {
            c.picking = 0; // its pick-up given up, and its fight
            c.target = 0;
            c.pending.clear();
            if (!Follow(c, game::kCosFollowNear)) Dismiss(m.entity);
        } else if (m.order == proto::CosOrder::Attack) {
            if (c.kind == game::CosKind::AttackPet) {
                c.target = 0;
                Engage(c, m.target);
            }
        } else if (m.order == proto::CosOrder::Stance && c.kind == game::CosKind::AttackPet) {
            c.aggressive = !c.aggressive;
            c.row.option = c.aggressive ? 1 : 0; // kept in its row (PetOption, the original's command mode)
            host_.CosSave(c.row);
            Send(owner, proto::CosStance{c.entity, c.aggressive});
        } else if (m.order == proto::CosOrder::Charm && c.target == 0 && c.hp > 0) {
            // Its greeting gesture (the original's AI gives it in its idle and following states, not in a fight).
            host_.CosBroadcast(c.entity, proto::EntityEmote::kId, proto::Encode(proto::EntityEmote{c.entity, 0}));
        }
        return true;
    }
    case proto::CosSetOption::kId: {
        proto::CosSetOption m;
        if (!proto::Decode(payload, m)) return false;
        const auto it = coses_.find(m.entity);
        if (it != coses_.end() && it->second.owner == owner && it->second.kind == game::CosKind::GrabPet) SetOption(it->second, m.option);
        return true;
    }
    case proto::CosBagMove::kId: {
        proto::CosBagMove m;
        if (!proto::Decode(payload, m)) return false;
        MoveInBag(owner, m);
        return true;
    }
    case proto::CosRename::kId: {
        proto::CosRename m;
        if (!proto::Decode(payload, m)) return false;
        Rename(owner, m);
        return true;
    }
    case proto::CosUseOnPet::kId: {
        proto::CosUseOnPet m;
        if (!proto::Decode(payload, m)) return false;
        UseOnPet(owner, m);
        return true;
    }
    case proto::CosSell::kId: {
        proto::CosSell m;
        if (!proto::Decode(payload, m)) return false;
        Sell(owner, m.npc);
        return true;
    }
    default: return false;
    }
}

void Coses::Rename(std::uint32_t owner, const proto::CosRename& m) {
    const auto it = coses_.find(m.entity);
    if (it == coses_.end() || it->second.owner != owner || it->second.row.id == 0) return; // a pet of its own, out
    if (!it->second.row.name.empty()) return Refuse(owner, "UIIT_MSG_COSPET_NOTREVISION_NAME");
    if (!host_.CosNameAllowed(m.name)) return Refuse(owner, "UIIT_MSG_COSPETERR_PETNAME_NOTPUT");
    const std::uint32_t entity = m.entity;
    const std::int64_t id = it->second.row.id;
    host_.CosName(id, m.name, [this, owner, entity, id, name = m.name](CosNaming result) {
        if (result == CosNaming::Taken) return Refuse(owner, "UIIT_MSG_COSPETERR_PETNAME_SUMENESS");
        if (result == CosNaming::HasOne) return Refuse(owner, "UIIT_MSG_COSPET_NOTREVISION_NAME");
        if (result != CosNaming::Named) return Refuse(owner, "UIIT_MSG_COSPETERR_PETNAME_NOTPUT");
        // Named in its row; the pet out (if it still is) and its item say so.
        const auto c = coses_.find(entity);
        if (c == coses_.end() || c->second.row.id != id) return;
        c->second.row.name = name;
        // Told to its owner always; to everyone else who sees it, and kept for those who come into sight of it later,
        // only while its owner is in no job (as its appearance; a pet just out has no observers until the
        // world's next pass, its owner is told directly).
        Send(owner, proto::CosRenamed{entity, name});
        const auto o = host_.CosOwner(owner);
        if (o && o->job == 0) {
            host_.CosWorld().SetName(entity, name);
            const std::vector<std::byte> news = proto::Encode(proto::CosRenamed{entity, name});
            host_.CosWorld().ForEachObserver(entity, [&](std::uint64_t peer) {
                if (peer != o->peer) host_.CosSend(peer, proto::CosRenamed::kId, news);
            });
        }
        SendPetItem(owner, c->second.row);
        if (o) core::LogInfo("cos: {} names its pet {} \"{}\"", o->name, id, name);
    });
}

void Coses::UseOnPet(std::uint32_t owner, const proto::CosUseOnPet& m) {
    const assets::ItemTable* items = host_.CosItems();
    const game::Inventory* bag = host_.CosInventory(owner);
    const auto o = host_.CosOwner(owner);
    if (items == nullptr || bag == nullptr || !o || m.place < game::slot::kWorn || m.target < game::slot::kWorn || m.place == m.target) return;
    const game::ItemInstance* tool = bag->At(m.place);
    const game::ItemInstance* target = bag->At(m.target);
    const assets::ItemRow* toolRow = tool != nullptr ? items->Find(tool->refId) : nullptr;
    const assets::ItemRow* targetRow = target != nullptr ? items->Find(target->refId) : nullptr;
    if (toolRow == nullptr || targetRow == nullptr) return;
    const game::PetTreatment treatment = game::PetTreatmentOf(*toolRow);
    if (treatment == game::PetTreatment::None) return;
    // On a pet's item of the kind it is for (the 1.188 client's own check), that has a pet.
    const game::CosKind wanted = treatment == game::PetTreatment::Extension ? game::CosKind::GrabPet : game::CosKind::AttackPet;
    if (!game::IsCosSummoner(*targetRow) || game::PetOfItem(*targetRow) != wanted || target->durability == 0) {
        return Refuse(owner, "UIIT_MSG_COSPETERR_CANT_USE_WRONGOBJECT");
    }
    if (o->level < toolRow->requiredLevel) return Refuse(owner, "UIIT_MSG_COS_HIGHER_LEVEL_REQUIRED_TO_USE_THISITEM");
    if (loading_.contains(owner)) return; // a pet's row of its is on its way
    loading_.insert(owner);
    const std::int64_t toolSerial = tool->serial, targetSerial = target->serial;
    const int minutes = toolRow->params[0];
    host_.CosLoad(std::int64_t(target->durability), [this, owner, treatment, minutes, toolSerial, targetSerial, character = o->character](std::optional<CosRow> row) {
        loading_.erase(owner);
        // Both items still where they were found (by their ids: they may have moved meanwhile).
        const game::Inventory* bag = host_.CosInventory(owner);
        if (!row || bag == nullptr || row->owner != character) return;
        int toolPlace = -1;
        bool hasTarget = false;
        for (const auto& [place, item] : bag->places) {
            if (place < game::slot::kWorn) continue;
            if (item.serial == toolSerial) toolPlace = place;
            if (item.serial == targetSerial && std::int64_t(item.durability) == row->id) hasTarget = true;
        }
        if (toolPlace < 0 || !hasTarget) return;
        if (Expire(*row)) host_.CosSave(*row); // its rental ran out meanwhile: dead now, as the clock wants it
        if (row->state != std::uint8_t(game::CosState::Dead)) return Refuse(owner, "UIIT_MSG_COSPETERR_CANT_USEITEM");
        row->state = std::uint8_t(game::CosState::Inactive);
        if (treatment == game::PetTreatment::Extension) {
            row->rentUntil = UnixNow() + std::int64_t(std::max(minutes, 0)) * 60;
        } else if (const assets::CharacterTable* characters = host_.CosCharacters()) {
            if (const assets::ObjectRow* shape = characters->Find(row->refId)) row->hp = shape->maxHp;
        }
        host_.CosSave(*row);
        host_.CosTakeOne(owner, std::uint8_t(toolPlace));
        SendPetItem(owner, *row);
        core::LogInfo("cos: pet {} {} ({})", row->id, treatment == game::PetTreatment::Extension ? "renewed" : "revived", row->name);
    });
}

bool Coses::Expire(CosRow& row) {
    if (row.rentUntil == 0 || UnixNow() < row.rentUntil || row.state == std::uint8_t(game::CosState::Dead)) return false;
    row.state = std::uint8_t(game::CosState::Dead);
    return true;
}

void Coses::SendPetItem(std::uint32_t owner, const CosRow& row) {
    proto::CosPetItem p;
    p.id = row.id;
    p.model = row.refId;
    p.state = row.state;
    p.name = row.name;
    p.level = std::uint8_t(std::clamp(row.level, 0, 255));
    const std::int64_t left = row.rentUntil != 0 ? row.rentUntil - UnixNow() : 0;
    p.rentSeconds = std::uint32_t(std::clamp<std::int64_t>(left, 0, 0xFFFFFFFF));
    Send(owner, proto::CosPetItems{{p}});
}

bool Coses::KeepsItem(std::uint32_t owner, std::int64_t serial, const char* text) {
    const auto it = owned_.find(owner);
    if (it == owned_.end() || serial == 0) return false;
    for (std::uint32_t id : it->second) {
        const Cos& c = coses_.at(id);
        if (c.row.id == 0 || c.serial != serial) continue;
        Refuse(owner, text);
        return true;
    }
    return false;
}

bool Coses::Out(std::uint32_t owner, std::int64_t serial) const {
    const auto it = owned_.find(owner);
    if (it == owned_.end() || serial == 0) return false;
    for (std::uint32_t id : it->second) {
        const Cos& c = coses_.at(id);
        if (c.row.id != 0 && c.serial == serial) return true;
    }
    return false;
}

bool Coses::Ride(std::uint32_t owner, std::uint32_t entity, bool ride, Clock::time_point) {
    const auto it = coses_.find(entity);
    if (it == coses_.end() || it->second.owner != owner) return false;
    Cos& c = it->second;
    if (c.ridden == ride) return true;
    if (c.kind != game::CosKind::Ride && c.kind != game::CosKind::Transport) return false; // pets carry nobody
    World& world = host_.CosWorld();
    if (ride) {
        const auto o = host_.CosOwner(owner);
        if (!o || !o->alive || !o->free) return false;
        // The original's order: sitting or getting up (5), in battle (0xB), then its bind: a hunter on a transport (10),
        // hidden (body mode 6 / 7: 0xC), farther than 30 (4).
        if (o->sitting) return Refuse(owner, "UIIT_MSG_CMSERR_CANT_RIDE_ON_IT"), false;
        if (o->fought) {
            Refuse(owner, "UIIT_MSG_COS_CAN_NOT_RIDE_BATTLE");
            return false;
        }
        if (c.kind == game::CosKind::Transport && o->job == std::uint8_t(game::Job::Hunter)) {
            Refuse(owner, "UIIT_MSG_CMSERR_HUNTER_CANT_RIDE_CART");
            return false;
        }
        if (o->hidden) return Refuse(owner, "UIIT_MSG_CMSERR_CANT_RIDE_ON_IT"), false;
        const auto a = world.Where(owner), b = world.Where(entity);
        if (!a || !b) return false;
        // Within 30 (3D, one plane): farther, its owner walks to it first and boards there (ours: the original's client
        // walks up before it asks; Tick's boarding walks).
        if (a->position.layer != b->position.layer || Distance3(a->position, b->position) > kBoardReach) {
            if (a->position.layer == b->position.layer && Distance3(a->position, b->position) <= kBoardWalk &&
                world.MoveTo(owner, b->position) == World::MoveRefused::None) {
                boarding_[owner] = entity;
                return false;
            }
            Refuse(owner, "UIIT_MSG_CMSERR_CANT_RIDE_ON_IT");
            return false;
        }
        boarding_.erase(owner);
        world.SetSpeeds(owner, c.walkSpeed, c.runSpeed);
        world.Carry(owner, entity);
    } else {
        world.Carry(owner, 0);
        world.SetSpeeds(owner, 0, 0);
        // Ours: a transport left steps aside to its owner's right (where it was summoned), not left standing in the
        // rider (a ride is gone: Dismiss).
        if (const auto place = c.kind == game::CosKind::Transport ? world.Where(owner) : std::nullopt) {
            const proto::FlatPosition at = proto::ToFlat(place->position);
            proto::WorldPosition aside =
                proto::FromFlat({at.x + kSummonAside * std::cos(place->yaw), at.z - kSummonAside * std::sin(place->yaw)}, place->position.y);
            world.MoveTo(entity, aside, true);
        }
    }
    c.ridden = ride;
    host_.CosRiding(owner, ride);
    const proto::CosRiding news{owner, entity, ride};
    host_.CosBroadcast(owner, proto::CosRiding::kId, proto::Encode(news));
    world.SetExtra(owner, proto::CosRiding::kId, ride ? proto::Encode(news) : std::vector<std::byte>{});
    if (const auto o = host_.CosOwner(owner)) core::LogInfo("cos: {} {} {}", o->name, ride ? "boards" : "leaves", entity);
    return true;
}

void Coses::Dismiss(std::uint32_t entity, bool comesBack, bool dead, bool terminate, bool moving) {
    const auto it = coses_.find(entity);
    if (it == coses_.end()) return;
    const std::uint32_t owner = it->second.owner;
    const bool wasRidden = it->second.ridden;
    if (it->second.ridden) Ride(owner, entity, false, Clock::now());
    // A pet back into its item: its row saved as it is now (Summoned when it comes out again where its owner arrives).
    if (Cos& c = it->second; c.row.id != 0) {
        c.row.hp = c.hp;
        c.row.state = std::uint8_t(dead ? game::CosState::Dead : comesBack ? game::CosState::Summoned : game::CosState::Inactive);
        host_.CosSave(c.row);
        if (comesBack && !dead) returning_[owner].push_back({c.row.id, c.serial});
        if (!comesBack || dead) SendPetItem(owner, c.row);
    }
    // A transport killed or terminated: every slot drops whole where it stands (as the original's: its death and its
    // owner's terminate, nothing else). Any other end (its owner's leaving, a travel without it, its rider's death) keeps
    // the goods in its row.
    if (Cos& c = it->second; c.kind == game::CosKind::Transport && !c.bag.empty() && (dead || terminate)) {
        if (const auto at = host_.CosWorld().Where(entity)) host_.CosCargoDropped(owner, at->position, std::exchange(c.bag, {}));
    }
    // A transport's row: gone with it when it is killed or sent away (the original's _DeleteCharCOS), else kept with its
    // goods for its owner's next entry (the original's departure despawns it with no database job).
    if (const Cos& c = it->second; c.kind == game::CosKind::Transport && c.cart != 0) {
        if (dead || terminate) host_.CosCartDelete(c.character, c.cart);
        else host_.CosCartRelease(c.character, c.cart, c.hp, wasRidden, c.bag);
    }
    // A ride's: kept only through a move to another game server (made again there, ridden); any other end is its end
    // (the original deletes a riding horse at its rider's getting off, logout, death).
    if (const Cos& c = it->second; c.kind == game::CosKind::Ride && c.cart != 0) {
        if (moving && !dead && !terminate) host_.CosCartRelease(c.character, c.cart, c.hp, true, c.bag);
        else host_.CosCartDelete(c.character, c.cart);
    }
    host_.CosWorld().Leave(entity);
    Send(owner, proto::CosDismissed{entity});
    const bool transport = it->second.kind == game::CosKind::Transport;
    coses_.erase(it);
    if (transport) host_.CosTransportGone(owner);
    if (auto o = owned_.find(owner); o != owned_.end()) {
        std::erase(o->second, entity);
        if (o->second.empty()) owned_.erase(o);
    }
}

std::optional<Coses::Cart> Coses::TransportCart(std::uint32_t owner) {
    for (auto& [entity, c] : coses_) {
        if (c.owner == owner && c.kind == game::CosKind::Transport) return Cart{entity, c.model, &c.bag, c.bagSize, c.cart};
    }
    return std::nullopt;
}

bool Coses::CarriesGoods(std::uint32_t owner) const {
    for (const auto& [entity, c] : coses_) {
        if (c.owner == owner && c.kind == game::CosKind::Transport && !c.bag.empty()) return true;
    }
    return false;
}

void Coses::CartChanged(std::uint32_t entity) {
    const auto it = coses_.find(entity);
    if (it == coses_.end()) return;
    SendBag(it->second);
    // Written in its owner's order of saves, with his gold (a purchase's price, a sale's payout).
    if (it->second.kind == game::CosKind::Transport) host_.CosCartSave(it->second.character);
}

void Coses::OwnerLevelChanged(std::uint32_t owner) {
    const auto o = host_.CosOwner(owner);
    const auto it = owned_.find(owner);
    if (!o || it == owned_.end()) return;
    for (const std::uint32_t entity : it->second) {
        Cos& c = coses_.at(entity);
        if (c.kind != game::CosKind::Transport) continue;
        const int size = std::clamp(host_.CosTransportBagSize(c.model, o->level), 0, 255);
        if (size <= c.bagSize) continue; // it only grows
        c.bagSize = size;
        SendBag(c);
        core::LogInfo("cos: {}'s transport {} holds {} goods now", o->name, entity, size);
    }
}

const std::vector<std::uint32_t>& Coses::TransportModels() {
    if (!transportModelsRead_) {
        transportModelsRead_ = true;
        if (const assets::CharacterTable* characters = host_.CosCharacters()) {
            for (const auto& [codename, row] : characters->rows())
                if (game::CosKindOf(row) == game::CosKind::Transport || game::CosKindOf(row) == game::CosKind::Ride) transportModels_.push_back(row.refObjId);
            std::sort(transportModels_.begin(), transportModels_.end());
        }
    }
    return transportModels_;
}

void Coses::RestoreCart(std::uint32_t owner, const CosRow& row, Clock::time_point now) {
    const auto o = host_.CosOwner(owner);
    const assets::CharacterTable* characters = host_.CosCharacters();
    const assets::ObjectRow* shape = characters != nullptr ? characters->Find(row.refId) : nullptr;
    const game::CosKind kind = shape != nullptr ? game::CosKindOf(*shape) : game::CosKind::None;
    if (!o || (kind != game::CosKind::Transport && kind != game::CosKind::Ride)) return;
    if (PlaceTaken(owner, kind)) {
        core::LogInfo("cos: {}'s kept COS {} stays kept (one out already)", o->name, row.id);
        return;
    }
    const std::uint32_t entity = Place(owner, *shape);
    if (entity == 0) return;
    Cos& c = coses_[entity];
    c.entity = entity;
    c.owner = owner;
    c.model = shape->refObjId;
    c.kind = kind;
    c.maxHp = shape->maxHp;
    c.hp = row.hp != 0 ? std::min(row.hp, shape->maxHp) : shape->maxHp;
    c.walkSpeed = shape->walkSpeed;
    c.runSpeed = shape->runSpeed;
    if (kind == game::CosKind::Transport) {
        c.bagSize = std::clamp(host_.CosTransportBagSize(shape->refObjId, o->level), 0, 255);
        c.stats = game::MonsterCombatStats(*shape, 0);
        c.stats.maxHp = std::max<std::uint32_t>(1, shape->maxHp);
    }
    c.character = o->character;
    c.cart = row.id;
    for (const StoredItem& s : row.bag) {
        game::ItemInstance item = host_.CosItemOf(s);
        item.creator = s.creator;
        c.bag[s.slot] = std::move(item);
    }
    owned_[owner].push_back(entity);
    Send(owner, proto::CosSummoned{entity, c.model, std::uint8_t(c.kind), c.hp, c.maxHp, c.item});
    if (c.bagSize > 0) SendBag(c);
    core::LogInfo("cos: {}'s {} {} back with {} stacks of goods (row {})", o->name, kind == game::CosKind::Ride ? "ride" : "transport", entity, c.bag.size(),
                  row.id);
    // A ride is always ridden (getting off it is its end); a transport as it was.
    if (kind == game::CosKind::Ride || row.state == std::uint8_t(game::CosState::Active)) Ride(owner, entity, true, now);
    if (kind == game::CosKind::Transport) host_.CosTransportBack(owner); // its goods count again: their stars, the bandits' caravan
}

void Coses::DismissAll(std::uint32_t owner, bool comesBack, bool keepTransport, bool moving) {
    // Pets still waiting to come back, their owner leaving for good: inactive.
    if (!comesBack) {
        if (const auto r = returning_.find(owner); r != returning_.end()) {
            for (const PetLink& link : r->second) {
                host_.CosLoad(link.id, [this](std::optional<CosRow> row) {
                    if (!row || row->state != std::uint8_t(game::CosState::Summoned)) return;
                    row->state = std::uint8_t(game::CosState::Inactive);
                    host_.CosSave(*row);
                });
            }
            returning_.erase(r);
        }
    }
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return;
    const std::vector<std::uint32_t> list = it->second;
    for (std::uint32_t entity : list) {
        if (const auto c = coses_.find(entity);
            keepTransport && c != coses_.end() && (c->second.kind == game::CosKind::Transport || c->second.kind == game::CosKind::Ride)) {
            travelling_[owner].list.push_back({entity, c->second.ridden}); // a rider stays on it through the travel
            continue;
        }
        Dismiss(entity, comesBack, false, false, moving);
    }
}

void Coses::OwnerDied(std::uint32_t owner) {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return;
    const std::vector<std::uint32_t> list = it->second;
    for (std::uint32_t entity : list) {
        const auto c = coses_.find(entity);
        if (c == coses_.end() || !c->second.ridden) continue;
        // The COS ridden goes, a transport too, its goods with it, none dropped (as the original's: the death releases
        // the ride slot with its database job 1, _DeleteCharCOS: a transport's row and goods deleted). One walked beside is
        // not touched (the loop's test). Rare: a rider is never a monster's target (its mount is).
        if (Cos& ridden = c->second; ridden.cart != 0) host_.CosCartDelete(ridden.character, std::exchange(ridden.cart, 0));
        Dismiss(entity, false);
    }
}

void Coses::BringTransport(std::uint32_t owner, const proto::WorldPosition& at) {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return;
    World& world = host_.CosWorld();
    for (const std::uint32_t entity : it->second) {
        const Cos& c = coses_.at(entity);
        if (c.kind != game::CosKind::Transport && c.kind != game::CosKind::Ride) continue;
        // Ridden: where its rider arrives, the two as one (the user's report 2026-11-14: rider and horse came out apart,
        // then he got on); else beside it.
        const proto::FlatPosition p = proto::ToFlat(at);
        proto::WorldPosition beside = c.ridden ? at : proto::FromFlat({p.x + game::kCosFollowNear, p.z}, at.y);
        beside.layer = at.layer;
        world.Teleport(entity, beside);
    }
}

bool Coses::KeptOutOfTown(std::uint32_t owner, const proto::WorldPosition& target) {
    const auto o = host_.CosOwner(owner);
    if (!o || o->job != std::uint8_t(game::JobType::Thief) || !host_.CosTown(target.region)) return false;
    const std::uint32_t mount = MountOf(owner);
    const auto c = coses_.find(mount);
    if (c == coses_.end() || c->second.kind != game::CosKind::Transport) return false;
    const auto at = host_.CosWorld().Where(mount);
    if (!at || host_.CosTown(at->position.region)) return false; // already in one (the scroll's way in)
    Refuse(owner, "UIIT_MSG_JSERR_ROBBER_CANT_ENTER_TOWN_WITH_CART");
    return true;
}

void Coses::Entered(std::uint32_t owner, std::vector<PetLink> pets) {
    const auto o = host_.CosOwner(owner);
    // Its transport kept (its row): read now, made beside it once it stands (Tick).
    if (o) {
        cartLoading_.insert(owner);
        host_.CosCartLoad(o->character, TransportModels(), [this, owner, character = o->character](std::vector<CosRow> rows) {
            cartLoading_.erase(owner);
            const auto now = host_.CosOwner(owner);
            if (!now || now->character != character || rows.empty()) return;
            carts_[owner] = std::move(rows);
        });
    }
    std::erase_if(pets, [](const PetLink& p) { return p.id == 0; });
    if (pets.empty()) return;
    // Each pet item's row to its owner's client (its tooltip, its slot); a rental run out meanwhile: dead now.
    for (const PetLink& link : pets) {
        host_.CosLoad(link.id, [this, owner, character = o ? o->character : 0](std::optional<CosRow> row) {
            if (!row || row->owner != character || !host_.CosOwner(owner)) return;
            if (Expire(*row)) host_.CosSave(*row);
            SendPetItem(owner, *row);
        });
    }
    std::vector<PetLink>& list = returning_[owner];
    list.insert(list.end(), pets.begin(), pets.end());
}

// The original's 0x70C7 kind 3 (its handler's order): the NPC within reach (else 3), the player's ride or transport
// out (else 5), the row's CanSell, no goods aboard, not ridden; then its SellPrice held to 1..10,000,000 paid and the
// COS let go (its row deleted, as a terminate's).
void Coses::Sell(std::uint32_t owner, std::uint32_t npc) {
    if (!host_.CosNpcNear(owner, npc)) return;
    const auto o = owned_.find(owner);
    if (o == owned_.end()) return;
    const auto vehicle = std::find_if(o->second.begin(), o->second.end(), [&](std::uint32_t id) {
        const game::CosKind kind = coses_.at(id).kind;
        return kind == game::CosKind::Ride || kind == game::CosKind::Transport;
    });
    if (vehicle == o->second.end()) return;
    const std::uint32_t entity = *vehicle;
    const Cos& c = coses_.at(entity);
    const assets::CharacterTable* characters = host_.CosCharacters();
    const assets::ObjectRow* row = characters != nullptr ? characters->Find(c.model) : nullptr;
    if (row == nullptr || !row->canSell) return Refuse(owner, "UIIT_MSG_COSERR_CANT_SELL_REQUESTED_COS");
    if (!c.bag.empty()) return Refuse(owner, "UIIT_MSG_COSERR_CANT_SELL_COS_HAVE_ITEMS");
    if (c.ridden) return Refuse(owner, "UIIT_MSG_COSERR_CANT_SELL_RIDING_COS");
    const std::uint64_t price = std::uint64_t(std::clamp(row->sellPrice, 1, 10'000'000));
    core::LogInfo("cos: {} sold back for {}", row->codename, price);
    host_.CosSellPay(owner, price);
    Dismiss(entity, false, false, true);
}

std::uint32_t Coses::MountOf(std::uint32_t owner) const {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return 0;
    for (std::uint32_t id : it->second) {
        if (coses_.at(id).ridden) return id;
    }
    return 0;
}

bool Coses::TransportOut(std::uint32_t owner) const {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return false;
    for (std::uint32_t id : it->second) {
        const game::CosKind kind = coses_.at(id).kind;
        if (kind == game::CosKind::Ride || kind == game::CosKind::Transport) return true;
    }
    return false;
}

bool Coses::Tether(std::uint32_t owner, proto::WorldPosition& target, bool& toward) {
    const auto it = owned_.find(owner);
    if (it == owned_.end()) return true;
    const auto me = host_.CosWorld().Where(owner);
    if (!me) return true;
    for (std::uint32_t id : it->second) {
        const Cos& c = coses_.at(id);
        if (c.kind != game::CosKind::Transport || c.ridden) continue;
        const auto at = host_.CosWorld().Where(id);
        if (!at) continue;
        // Along the walk X(t) = from + t (to - from), t in 0..1 (a walk that way: as far as a region's walk), the
        // steps go while its owner is within the line or getting nearer its transport.
        const proto::FlatPosition t = proto::ToFlat(at->position), f = proto::ToFlat(me->position), p = proto::ToFlat(target);
        double dx = p.x - f.x, dz = p.z - f.z;
        if (toward) {
            const double n = std::max(std::hypot(dx, dz), 1e-6);
            dx = dx / n * double(game::kNavRegionSize);
            dz = dz / n * double(game::kNavRegionSize);
        }
        const double ox = f.x - t.x, oz = f.z - t.z, l = game::kCosLeash;
        const double a = dx * dx + dz * dz, b = 2.0 * (dx * ox + dz * oz), cc = ox * ox + oz * oz - l * l;
        if (a < 1e-9) continue;
        double stop = 1.0; // where along it the steps stop (1: the whole walk goes)
        const double disc = b * b - 4.0 * a * cc;
        if (cc <= 0.0) {
            stop = (-b + std::sqrt(std::max(0.0, disc))) / (2.0 * a); // inside: until it crosses the line going out
        } else if (b >= 0.0) {
            stop = 0.0; // beyond the line and leading farther: no step goes
        } else if (disc < 0.0) {
            stop = -b / (2.0 * a); // nearer and nearer, never inside: as near as it comes
        } else {
            stop = (-b + std::sqrt(disc)) / (2.0 * a); // in, then out at the far side
        }
        if (stop >= 1.0) {
            if (!toward) continue;
            stop = 1.0;
        }
        if (stop <= 0.0) {
            Refuse(owner, "UIIT_MSG_COSERR_TOO_FAR_FROM_TRADECART", std::to_string(game::kCosLeashMetres));
            return false;
        }
        if (toward && stop >= 1.0) continue;
        target = proto::FromFlat({float(f.x + dx * stop), float(f.z + dz * stop)}, me->position.y);
        target.layer = me->position.layer;
        toward = false;
        Refuse(owner, "UIIT_MSG_COSERR_TOO_FAR_FROM_TRADECART", std::to_string(game::kCosLeashMetres));
    }
    return true;
}

int Coses::LevelOf(std::uint32_t entity) const {
    const auto it = coses_.find(entity);
    return it != coses_.end() ? std::max(1, it->second.row.level) : 0;
}

std::uint32_t Coses::OwnerOf(std::uint32_t entity) const {
    const auto it = coses_.find(entity);
    return it != coses_.end() ? it->second.owner : 0;
}

const game::CombatStats* Coses::StatsOf(std::uint32_t entity) const {
    const auto it = coses_.find(entity);
    if (it == coses_.end() || it->second.hp == 0 || it->second.stats.maxHp == 0) return nullptr;
    return &it->second.stats;
}

std::uint32_t Coses::HpOf(std::uint32_t entity) const {
    const auto it = coses_.find(entity);
    return it != coses_.end() ? it->second.hp : 0u;
}

// Rides and transports that came along on a travel: at the first tick their owner stands at the other end (placed by the
// world's update, which delivers its WorldEntered at once; this tick runs before the next one), told to it again (its
// client cleared its own at WorldEntered) with the riding of the one it is on, so it shows mounted from the start.
void Coses::Boardings(Clock::time_point now) {
    World& world = host_.CosWorld();
    for (auto it = boarding_.begin(); it != boarding_.end();) {
        const auto [owner, entity] = *it;
        if (world.Moving(owner)) {
            ++it;
            continue;
        }
        it = boarding_.erase(it);
        const auto c = coses_.find(entity);
        const auto a = world.Where(owner), b = world.Where(entity);
        if (c == coses_.end() || c->second.owner != owner || c->second.ridden || !a || !b) continue;
        // Arrived beside it: boarded (its checks again); stopped short of it (another walk, a wall): not.
        if (Distance3(a->position, b->position) > kBoardReach) continue;
        Ride(owner, entity, true, now);
    }
}

void Coses::Arrived() {
    World& world = host_.CosWorld();
    for (auto it = travelling_.begin(); it != travelling_.end();) {
        const std::uint32_t owner = it->first;
        if (!host_.CosOwner(owner)) {
            it = travelling_.erase(it);
            continue;
        }
        const auto place = world.Where(owner);
        if (!place || (world.HasNavigation() && !world.NavigationLoaded(place->position.region))) {
            ++it;
            continue;
        }
        for (const Travelling& t : it->second.list) {
            const auto c = coses_.find(t.entity);
            if (c == coses_.end() || c->second.owner != owner) continue;
            Send(owner, proto::CosSummoned{t.entity, c->second.model, std::uint8_t(c->second.kind), c->second.hp, c->second.maxHp, c->second.item});
            if (c->second.kind == game::CosKind::Transport && c->second.bagSize > 0) SendBag(c->second);
            if (c->second.ridden) Send(owner, proto::CosRiding{owner, t.entity, true});
        }
        it = travelling_.erase(it);
    }
}

bool Coses::Follow(Cos& c, double far) {
    World& world = host_.CosWorld();
    const auto owner = world.Where(c.owner);
    const auto self = world.Where(c.entity);
    if (!owner || !self) return true;
    const proto::FlatPosition a = proto::ToFlat(owner->position), b = proto::ToFlat(self->position);
    const double dx = a.x - b.x, dz = a.z - b.z, d = std::hypot(dx, dz);
    // Frozen, asleep, stunned or rooted: it stays where it is (a teleport's relocation below still takes it).
    const bool held = c.states.Locked() || c.states.Rooted();
    // Back to kCosFollowNear short of its owner, on the line between them.
    const double k = std::max(0.0, d - game::kCosFollowNear) / std::max(d, 1e-6);
    const proto::WorldPosition to = proto::FromFlat({b.x + dx * k, b.z + dz * k}, owner->position.y);
    // A ride or transport left standing keeps its spacing (the original's): within 65 of its owner it keeps near him (at
    // its run pace, to kCosFollowNear of him); 65 or more away, or on another plane, it stands where it is: never moved
    // to him (its owner comes back for it; the leash holds him within 100 m).
    if (c.kind == game::CosKind::Transport || c.kind == game::CosKind::Ride) {
        const bool apart = owner->position.layer != self->position.layer ||
                           (((owner->position.region | self->position.region) & 0x8000) != 0 && owner->position.region != self->position.region);
        if (apart || d >= kCartFollowEnd || d <= game::kCosFollowNear + 5.0 || held) return true;
        // A thief's transport stays out of the towns: it waits at the edge.
        if (c.kind == game::CosKind::Transport && host_.CosTown(to.region) && !host_.CosTown(self->position.region)) {
            if (const auto o = host_.CosOwner(c.owner); o && o->job == std::uint8_t(game::JobType::Thief)) return true;
        }
        world.MoveTo(c.entity, to, false);
        return true;
    }
    if (d <= far) return true;
    // A thief's transport stays out of the towns: it waits at the edge.
    if (c.kind == game::CosKind::Transport && host_.CosTown(to.region) && !host_.CosTown(self->position.region)) {
        if (const auto o = host_.CosOwner(c.owner); o && o->job == std::uint8_t(game::JobType::Thief)) return true;
    }
    // Relocated to its owner when the original's follow would: its owner beyond its own and the adjacent sectors, or on
    // another plane, or its walk back stuck (ours: a walk that ends short of its owner or is refused). False: it could
    // not be put there either, it is gone.
    const auto relocate = [&] { return world.Teleport(c.entity, owner->position); };
    const int rx = (owner->position.region & 0xFF) - (self->position.region & 0xFF), rz = ((owner->position.region >> 8) & 0x7F) - ((self->position.region >> 8) & 0x7F);
    const bool dungeon = ((owner->position.region | self->position.region) & 0x8000) != 0;
    if (owner->position.layer != self->position.layer || (dungeon && owner->position.region != self->position.region) || std::abs(rx) > 1 ||
        std::abs(rz) > 1) {
        return relocate();
    }
    if (held) return true;
    // A pet keeps its owner's pace (the original's): within 80 of him his speed now, beyond his run, 1.25 times it when
    // he runs; its own row's again in a fight (Fight).
    bool walk = d < game::kCosFollowFar * 2.0;
    if (const auto pace = world.PaceOf(c.owner)) {
        const float speed = d <= 80.0 ? (pace->now > 0.0f ? pace->now : pace->walking ? pace->walk : pace->run) : pace->run * (pace->walking ? 1.0f : 1.25f);
        if (speed > 0.0f) {
            world.SetSpeeds(c.entity, speed, speed);
            walk = false;
        }
    }
    if (world.MoveTo(c.entity, to, walk) != World::MoveRefused::None) return relocate();
    // Stuck: its walk stopped well short of the one asked for (a wall, a cliff; a walk is at most one region less 40, the
    // world's own cap, game::kNavRegionSize), and so short of its owner.
    const proto::FlatPosition wanted = proto::ToFlat(to);
    const double asked = std::min(std::hypot(wanted.x - b.x, wanted.z - b.z), double(game::kNavRegionSize) - 40.0);
    double walked = 0.0;
    if (const auto end = world.Heading(c.entity)) {
        const proto::FlatPosition e = proto::ToFlat(*end);
        walked = std::hypot(e.x - b.x, e.z - b.z);
    }
    if (walked < asked - game::kCosFollowNear) return relocate();
    return true;
}

void Coses::Tick(Clock::time_point now) {
    // Attack pets in a fight act every update (by their entities: a kill's news may take one away).
    std::vector<std::uint32_t> fighting;
    for (const auto& [entity, c] : coses_) {
        if (Fights(c.kind) && (c.target != 0 || !c.pending.empty())) fighting.push_back(entity);
    }
    for (const std::uint32_t entity : fighting) {
        if (const auto it = coses_.find(entity); it != coses_.end()) Fight(it->second, now);
    }
    for (auto& [entity, c] : coses_) Recover(c, now);
    TickStates(now);
    Arrived();
    Boardings(now);
    if (now < nextFollow_) return;
    const double elapsed = lastFollow_ == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - lastFollow_).count();
    lastFollow_ = now;
    nextFollow_ = now + kFollowPeriod;
    // Attack pets out eat their HGP.
    for (auto& [entity, c] : coses_) {
        if (c.kind == game::CosKind::AttackPet && c.hp > 0) Eat(c, std::min(elapsed, 5.0));
    }
    World& world = host_.CosWorld();
    // Pets coming back: once their owner stands where the ground is in, their rows read; those still Summoned come out.
    for (auto it = returning_.begin(); it != returning_.end();) {
        const std::uint32_t owner = it->first;
        const auto place = world.Where(owner);
        const auto o = host_.CosOwner(owner);
        if (!o) {
            it = returning_.erase(it); // not a player here (any more): its rows stay as they are
            continue;
        }
        if (!place || (world.HasNavigation() && !world.NavigationLoaded(place->position.region))) {
            ++it;
            continue;
        }
        for (const PetLink& link : it->second) {
            host_.CosLoad(link.id, [this, owner, link, character = o->character](std::optional<CosRow> row) {
                if (!row || row->state != std::uint8_t(game::CosState::Summoned) || row->owner != character) return;
                // Never a second of a pet already out (under another entity of its owner's: a re-entry from another
                // connection; the user's report 2026-11-13, cloned pets).
                for (const auto& [entity, out] : coses_) {
                    if (out.row.id == row->id) return;
                }
                PetReady(owner, *row, link.serial, 0, false);
            });
        }
        it = returning_.erase(it);
    }
    // The pets' timed jobs over by now (their ends on the wall clock: the keepers' ticks).
    const std::int64_t unix = host_.CosNow();
    for (auto& [entity, c] : coses_) {
        if (!c.jobs.empty()) EndJobs(c, unix);
    }
    std::vector<std::uint32_t> lost, unsummoned, expired, served;
    for (auto& [entity, c] : coses_) {
        // A guild's soldier whose minutes are up.
        if (c.kind == game::CosKind::Soldier && now >= c.until) {
            served.push_back(entity);
            continue;
        }
        // A grab pet whose rental ran out: back into its item, dead.
        if (c.row.id != 0 && c.row.rentUntil != 0 && UnixNow() >= c.row.rentUntil) {
            expired.push_back(entity);
            continue;
        }
        // A pet whose item left its owner's bag (dropped, sold, stored, traded): back in it.
        if (c.row.id != 0 && !host_.CosHasItem(c.owner, c.serial)) {
            unsummoned.push_back(entity);
            continue;
        }
        if (c.ridden) continue;
        if (!world.Where(c.owner)) {
            lost.push_back(entity); // its owner left the world without a word
            continue;
        }
        if (c.target != 0) continue; // an attack pet in a fight goes where its fight takes it
        if (Grab(c)) continue; // a grab pet going for an item follows once it has it
        if (!Follow(c, game::kCosFollowFar)) lost.push_back(entity);
    }
    for (std::uint32_t entity : expired) {
        const std::uint32_t owner = coses_.at(entity).owner;
        Refuse(owner, "UIIT_STT_COSNEWUI_ITEM_EXPIRERENT"); // "The pet's rental period has ended." in the chat
        Dismiss(entity, false, true);
    }
    for (std::uint32_t entity : unsummoned) Dismiss(entity);
    for (std::uint32_t entity : served) Dismiss(entity);
    for (std::uint32_t entity : lost) Dismiss(entity);
    // Kept transports read at their owners' entry: made once the owner stands where the ground is in; the newest row,
    // the older ones (kept and never brought back) gone with their goods.
    for (auto it = carts_.begin(); it != carts_.end();) {
        const std::uint32_t owner = it->first;
        const auto o = host_.CosOwner(owner);
        if (!o) {
            it = carts_.erase(it); // not a player here any more: the rows stay
            continue;
        }
        const auto place = world.Where(owner);
        if (!place || (world.HasNavigation() && !world.NavigationLoaded(place->position.region))) {
            ++it;
            continue;
        }
        const std::vector<CosRow> rows = std::move(it->second);
        it = carts_.erase(it);
        for (std::size_t k = 1; k < rows.size(); ++k) {
            core::LogWarning("cos: {}'s older kept transport {} ({} stacks of goods) deleted", o->name, rows[k].id, rows[k].bag.size());
            host_.CosCartDelete(o->character, rows[k].id);
        }
        RestoreCart(owner, rows.front(), now);
    }
}

// --- The mall's items on a pet ---------------------------------------------------------

Coses::PetItemUse Coses::UseMallItemOnPet(std::uint32_t owner, const assets::ItemRow& item, const game::ItemInstance& stack,
                                          const game::ItemInstance& petItem) {
    // The pet of that item out (a summoning item with its pet's row; the owner's
    // COS of that row): else 0x18B8.
    Cos* found = nullptr;
    if (const auto it = owned_.find(owner); it != owned_.end() && petItem.durability != 0) {
        for (const std::uint32_t id : it->second) {
            Cos& c = coses_.at(id);
            if (c.row.id != 0 && c.row.id == std::int64_t(petItem.durability) && c.serial == petItem.serial) found = &c;
        }
    }
    if (found == nullptr) return {0x18B8, 0};
    Cos& c = *found;
    const game::PetItemJobs jobs = game::PetItemJobsOf(item);
    if (!jobs) return {2, 0}; // ours: no job to run (none of the 1.188 rows)
    const std::int64_t unix = host_.CosNow();
    // Refused, first of all: this item's composite job running on the pet
    // (category 5, its JobID the item): 0x1894.
    for (const Cos::Job& j : c.jobs) {
        if (j.row.category == game::kPetJobItem && j.row.job == item.refObjId) return {0x1894, 0};
    }
    // Which pet takes it: an attack pet an item of Param6 bit 1; a grab pet one of bit 2 with more than two
    // minutes of its rental left (3 / 2); any other COS: 2.
    const std::int64_t rentLeft = c.row.rentUntil - unix;
    const auto fits = [&]() -> std::uint16_t {
        if (c.kind == game::CosKind::GrabPet) {
            if (!jobs.grabPet) return 3;
            return rentLeft / 60 <= game::kPetWatchLeastMinutes ? 2 : 0;
        }
        if (c.kind == game::CosKind::AttackPet) return jobs.attackPet ? 0 : 3;
        return 2;
    };
    // The jobs in the order the 1.188 rows list them: PSU1, then BCS1.
    const assets::SkillTable* skills = host_.CosSkills();
    const assets::SkillRow* skill = nullptr;
    std::vector<std::size_t> replaced;
    if (!jobs.skill.empty()) {
        // Its skill (the pet's level's row when the job asks); none: 2.
        skill = skills != nullptr ? skills->Find(std::string_view(game::PetSkillCodename(jobs, c.row.level))) : nullptr;
        if (skill == nullptr) return {2, 0};
        // That skill running on it (a timed job of category 0): 0x1894.
        for (const Cos::Job& j : c.jobs) {
            if (j.row.category == game::kPetJobSkill && j.row.job == skill->id) return {0x1894, 0};
        }
        // Its cast's checks on the pet: a higher rank of its group, or one
        // of its overlap groups held (the pets' three imbues share group 1: one element at a time): 2. An equal or
        // higher rank of its group ends the one running.
        std::vector<const assets::SkillRow*> running;
        std::vector<std::size_t> at;
        for (std::size_t i = 0; i < c.jobs.size(); ++i) {
            if (c.jobs[i].skill == nullptr) continue;
            running.push_back(c.jobs[i].skill);
            at.push_back(i);
        }
        const game::BuffAdmission admission = game::AdmitBuff(*skill, running);
        if (!admission.admitted) return {2, 0};
        for (const std::size_t r : admission.replaces) replaced.push_back(at[r]);
        if (const std::uint16_t code = fits()) return {code, 0};
    }
    if (jobs.bagPlaces > 0) {
        // A bag's job running ('BCS1', INC_INVENTORY_SIZE): 0x1894.
        for (const Cos::Job& j : c.jobs) {
            if (j.row.category == game::kPetJobItem && j.row.data[0] == game::kPetJobBagTag && j.row.data[1] == game::kPetJobBagKind) return {0x1894, 0};
        }
        if (const std::uint16_t code = fits()) return {code, 0};
        // Its places beyond those the bag allocates: 2.
        if (c.kind != game::CosKind::GrabPet || c.bagMost < c.bagSize + jobs.bagPlaces) return {2, 0};
    }
    // each job a timed job of the pet's, its end on the wall clock. A grab
    // pet's: as many minutes as the stack holds, at most its rental's, and the use takes that many; an
    // attack pet's skill: the skill's own time (its 'dura'), one taken.
    const bool byStack = c.kind == game::CosKind::GrabPet;
    const int minutes = byStack ? game::PetWatchMinutes(int(stack.count), rentLeft) : 0;
    if (byStack && minutes <= 0) return {2, 0};
    PetItemUse use{0, byStack ? minutes : 1};
    // A replaced rank ended first (back to front: their places stay right).
    std::sort(replaced.rbegin(), replaced.rend());
    for (const std::size_t i : replaced) EndJob(c, i);
    if (skill != nullptr) {
        PetJobRow r;
        r.category = game::kPetJobSkill;
        r.job = skill->id;
        r.until = unix + (byStack ? std::int64_t(minutes) * 60 : std::int64_t(skill->buffDurationMs / 1000));
        r.data[1] = 1; // TimeToKeep is the end (the skill keeper's mode 1)
        r.serial = stack.serial;
        host_.CosSavePetJob(c.row.id, r);
        StartJob(c, r, true);
    }
    if (jobs.bagPlaces > 0) {
        PetJobRow r;
        r.category = game::kPetJobItem;
        r.job = item.refObjId;
        r.until = unix + std::int64_t(minutes) * 60;
        r.data[0] = game::kPetJobBagTag;
        r.data[1] = game::kPetJobBagKind;
        r.data[2] = jobs.bagPlaces;
        r.serial = stack.serial;
        host_.CosSavePetJob(c.row.id, r);
        StartJob(c, r, true);
    }
    if (const auto o = host_.CosOwner(owner)) {
        core::LogInfo("cos: {} uses {} on pet {} ({} taken; its bag {}, {} skills on it)", o->name, item.codename, c.row.id, use.taken, c.bagSize,
                      std::count_if(c.jobs.begin(), c.jobs.end(), [](const Cos::Job& j) { return j.skill != nullptr; }));
    }
    return use;
}

void Coses::StartJob(Cos& c, const PetJobRow& row, bool tell) {
    Cos::Job job;
    job.row = row;
    if (row.category == game::kPetJobSkill) {
        const assets::SkillTable* skills = host_.CosSkills();
        job.skill = skills != nullptr ? skills->Find(row.job) : nullptr;
        if (job.skill == nullptr) {
            core::LogWarning("cos: pet {}'s job names skill {}, none", c.row.id, row.job);
            return;
        }
    }
    c.jobs.push_back(job);
    if (row.category == game::kPetJobItem && row.data[0] == game::kPetJobBagTag && row.data[1] == game::kPetJobBagKind) {
        // CBuffCharacterStatusForPet on: its places + the job's, its owner told the bag.
        c.bagSize = std::clamp(c.bagSize + row.data[2], 0, std::max(c.bagMost, c.bagSize));
        if (tell) SendBag(c);
        return;
    }
    if (job.skill != nullptr) {
        // On the pet as a buff (its changes on its numbers; those who see it shown its effect).
        if (c.kind == game::CosKind::AttackPet) Hunger(c);
        std::vector<std::uint32_t> skills;
        for (const Cos::Job& j : c.jobs) {
            if (j.skill != nullptr) skills.push_back(j.skill->id);
        }
        host_.CosWorld().SetBuffs(c.entity, skills);
        host_.CosBroadcast(c.entity, proto::EntityBuffs::kId, proto::Encode(proto::EntityBuffs{c.entity, skills}));
    }
}

void Coses::EndJob(Cos& c, std::size_t index) {
    if (index >= c.jobs.size()) return;
    const Cos::Job job = c.jobs[index];
    c.jobs.erase(c.jobs.begin() + std::ptrdiff_t(index));
    host_.CosEndPetJob(c.row.id, job.row.category, job.row.job);
    if (job.row.category == game::kPetJobItem && job.row.data[0] == game::kPetJobBagTag && job.row.data[1] == game::kPetJobBagKind) {
        // Off: its places less the job's; what lies past them stays there, out of reach.
        c.bagSize = std::max(0, c.bagSize - job.row.data[2]);
        c.picking = 0;
        SendBag(c);
    } else if (job.skill != nullptr) {
        if (c.kind == game::CosKind::AttackPet) Hunger(c);
        std::vector<std::uint32_t> skills;
        for (const Cos::Job& j : c.jobs) {
            if (j.skill != nullptr) skills.push_back(j.skill->id);
        }
        host_.CosWorld().SetBuffs(c.entity, skills);
        host_.CosBroadcast(c.entity, proto::EntityBuffs::kId, proto::Encode(proto::EntityBuffs{c.entity, skills}));
    }
    if (const auto o = host_.CosOwner(c.owner)) core::LogInfo("cos: {}'s pet {}: its job {} ends (its bag {})", o->name, c.row.id, job.row.job, c.bagSize);
}

void Coses::EndJobs(Cos& c, std::int64_t unix) {
    for (std::size_t i = c.jobs.size(); i-- > 0;) {
        if (c.jobs[i].row.until <= unix) EndJob(c, i);
    }
}

const assets::SkillRow* Coses::ImbueOf(const Cos& c) const {
    for (const Cos::Job& j : c.jobs) {
        if (j.skill != nullptr && game::IsImbue(*j.skill)) return j.skill;
    }
    return nullptr;
}

Monsters::Affliction Coses::ImbueAffliction(Cos& c, const assets::SkillRow& imbue, const CosHost::Foe& foe, const game::HitResult& hit, Clock::time_point now) {
    // Its states rolled on the monster after the hit (none on a blocked hit), through the pet's
    // chance history, as a player's imbue's are.
    Monsters::Affliction a;
    a.magical = hit.magical;
    if (hit.blocked || !foe.target) return a;
    const std::vector<game::StateGift> gifts = game::StateGiftsOf(imbue);
    if (gifts.empty()) return a;
    const int pulse = assets::FindSkillParam(imbue, assets::SkillTag("puls")).Int(0);
    a.states = game::RollStates(gifts, foe.target->defence, foe.target->susceptibility, c.rolls, random_, c.entity, imbue.id, pulse, now);
    return a;
}

std::optional<Coses::PetJobsView> Coses::JobsOf(std::uint32_t entity) const {
    const auto it = coses_.find(entity);
    if (it == coses_.end()) return std::nullopt;
    PetJobsView v;
    v.bagSize = it->second.bagSize;
    for (const Cos::Job& j : it->second.jobs) {
        if (j.skill != nullptr) v.skills.push_back(j.skill->id);
    }
    return v;
}

} // namespace sr::server
