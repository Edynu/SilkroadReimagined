#include "sr/game/PlayerState.h"

#include <algorithm>
#include <variant>

namespace sr::game {

void PlayerState::SetLevel(std::uint8_t level) {
    self.level = level;
    self.experienceToNext = level < levels_.size() ? levels_[level] : 0;
}

void PlayerState::Apply(const TitleEvent& event) {
    // The party's events (its view keeps the player's entity from the entries too); the player's
    // bar shows whether it leads one.
    if (party.Apply(event, now)) {
        self.party = party.party;
        self.partyLeader = party.IsLeader();
        return;
    }
    if (const auto* entered = std::get_if<proto::WorldEntered>(&event)) {
        if (entered->status != proto::WorldStatus::Ok) return;
        // Afresh: the character as the server keeps it. Until its stats come (C9) a character is
        // at its full health, so what it has is also its most.
        const proto::CharacterSummary& c = entered->character;
        CharacterStatus s;
        s.entity = entered->entity;
        s.model = c.model;
        s.name = c.name;
        s.hp = c.hp;
        s.mp = c.mp;
        s.maxHp = std::max(entered->maxHp, c.hp);
        s.maxMp = std::max(entered->maxMp, c.mp);
        s.experience = c.experience;
        s.strength = c.strength;
        s.intelligence = c.intelligence;
        s.statPoints = c.statPoints;
        s.race = self.entity == entered->entity ? self.race : 0; // the client's tables say it (CreationRules)
        // What the entry does not carry stays: the server's Progress came just before it (skill
        // points, the stat panel's numbers); a teleport or a rising keeps the character's level and
        // experience as they are now (the entry's summary is the one it entered the world with).
        const CharacterStatus was = self;
        const bool same = was.entity == entered->entity;
        s.skillExperience = was.skillExperience;
        s.skillPoints = was.skillPoints;
        s.physicalAttack[0] = was.physicalAttack[0];
        s.physicalAttack[1] = was.physicalAttack[1];
        s.magicalAttack[0] = was.magicalAttack[0];
        s.magicalAttack[1] = was.magicalAttack[1];
        s.physicalDefence = was.physicalDefence;
        s.magicalDefence = was.magicalDefence;
        s.hitRatio = was.hitRatio;
        s.parryRatio = was.parryRatio;
        s.berserk = was.berserk; // told just before the entry (BerserkPoints), kept through a teleport
        std::uint8_t level = c.level;
        if (same || (was.entity == 0 && was.level > 1)) {
            level = was.level;
            s.experience = was.experience;
            s.strength = was.strength;
            s.intelligence = was.intelligence;
            s.statPoints = was.statPoints;
        }
        self = s;
        SetLevel(level);
        self.highestLevel = std::max(level, was.highestLevel);
        self.party = party.party; // a teleport keeps the party
        self.partyLeader = party.IsLeader();
        target = {};
        effects.clear();
        marks.clear();
        ShowTimedServices(); // the mall's services stay over a teleport
        ShowAcademyBuffs();  // and the academy's buffs
        return;
    }
    if (const auto* points = std::get_if<proto::BerserkPoints>(&event)) {
        self.berserk = std::min<std::uint8_t>(points->points, CharacterStatus::kBerserkBars);
        return;
    }
    if (const auto* v = std::get_if<proto::Vitals>(&event)) {
        self.hp = v->hp;
        self.maxHp = v->maxHp;
        self.mp = v->mp;
        self.maxMp = v->maxMp;
        return;
    }
    if (const auto* book = std::get_if<proto::SkillBookList>(&event)) {
        masteries.clear();
        for (const proto::MasteryRecord& m : book->masteries) masteries.push_back({m.mastery, int(m.level)});
        skills = book->skills;
        ++skillsVersion;
        return;
    }
    if (const auto* p = std::get_if<proto::Progress>(&event)) {
        if (p->level != self.level) SetLevel(p->level);
        self.highestLevel = std::max(self.highestLevel, p->level);
        self.experience = p->experience;
        self.skillExperience = p->skillExperience;
        self.skillPoints = p->skillPoints;
        self.strength = p->strength;
        self.intelligence = p->intelligence;
        self.statPoints = p->statPoints;
        self.physicalAttack[0] = p->phyAttackMin;
        self.physicalAttack[1] = p->phyAttackMax;
        self.magicalAttack[0] = p->magAttackMin;
        self.magicalAttack[1] = p->magAttackMax;
        self.physicalDefence = p->phyDefence;
        self.magicalDefence = p->magDefence;
        self.hitRatio = p->hitRate;
        self.parryRatio = p->parryRate;
        return;
    }
    if (const auto* hit = std::get_if<proto::EntityDamaged>(&event)) {
        // Health from a hit: the player's own, and the selected one's.
        if (hit->target == self.entity) self.hp = hit->hp;
        if (hit->target == target.entity) target.hp = hit->hp;
        return;
    }
    if (const auto* list = std::get_if<proto::QuickSlotList>(&event)) {
        quickSlots.slots = {};
        for (const proto::QuickSlotRecord& r : list->slots) {
            if (r.slot < QuickSlots::kCount) quickSlots.slots[r.slot] = {QuickSlot::Kind(r.kind), r.value};
        }
        return;
    }
    if (const auto* potion = std::get_if<proto::AutoPotionSettings>(&event)) {
        autoPotion = FromMessage(*potion);
        ++autoPotionVersion;
        return;
    }
    if (const auto* jobs = std::get_if<proto::JobEvent>(&event)) {
        if (const auto* info = std::get_if<proto::JobInfoMessage>(jobs)) {
            job = {JobType(std::min<int>(info->type, 3)), info->level, info->exp, info->contribution, info->reward};
            jobAlias = info->alias;
            if (info->entry) jobContributionAtEntry = info->contribution;
        } else if (const auto* seen = std::get_if<proto::EntityJob>(jobs); seen != nullptr && seen->entity == self.entity) {
            jobState = seen->state;
        }
        return;
    }
    if (const auto* pk = std::get_if<proto::PkEvent>(&event)) {
        if (const auto* c = std::get_if<proto::PkCounters>(pk)) {
            pkDaily = c->daily;
            pkTotal = c->total;
            pkPoints = c->points;
        } else if (const auto* s = std::get_if<proto::EntityPvpState>(pk); s != nullptr && s->entity == self.entity) {
            pvpState = s->state;
        }
        return;
    }
    if (const auto* gone = std::get_if<proto::EntityGone>(&event)) {
        if (gone->entity == target.entity) target = {};
        return;
    }
    if (const auto* grown = std::get_if<proto::InventoryGrown>(&event)) inventory.size = std::min(grown->size, Inventory::kMaxSize);
    if (const auto* list = std::get_if<proto::InventoryList>(&event)) {
        inventory = {};
        inventory.size = std::min(list->size, Inventory::kMaxSize);
        inventory.gold = list->gold;
        for (const proto::ItemRecord& r : list->items) inventory.places[r.slot] = Inventory::FromRecord(r);
        ++inventoryVersion;
        return;
    }
    if (const auto* list = std::get_if<proto::StorageList>(&event)) {
        Storage& s = list->guild != 0 ? guildStorage : storage;
        s = {};
        s.size = std::min(list->size, Storage::kMaxSize);
        s.gold = list->gold;
        for (const proto::ItemRecord& r : list->items) s.places[r.slot] = Inventory::FromRecord(r);
        (list->guild != 0 ? guildStorageOpen : storageOpen) = true;
        ++inventoryVersion;
        return;
    }
    if (const auto* moved = std::get_if<proto::StorageMoved>(&event)) {
        if (moved->status != proto::ItemStatus::Ok) {
            itemRefusals.push_back(moved->status);
            return;
        }
        for (std::uint8_t place : moved->inventoryEmptied) inventory.places.erase(place);
        for (const proto::ItemRecord& r : moved->inventoryChanged) inventory.places[r.slot] = Inventory::FromRecord(r);
        Storage& s = moved->guild != 0 ? guildStorage : storage;
        for (std::uint8_t place : moved->storageEmptied) s.places.erase(place);
        for (const proto::ItemRecord& r : moved->storageChanged) s.places[r.slot] = Inventory::FromRecord(r);
        ++inventoryVersion;
        return;
    }
    if (const auto* shop = std::get_if<proto::ShopAnswer>(&event)) {
        if (shop->opened) shopNpc = shop->entity;
        buyback = shop->buyback;
        if (shop->status != proto::ItemStatus::Ok) itemRefusals.push_back(shop->status);
        for (std::uint8_t place : shop->emptied) inventory.places.erase(place);
        for (const proto::ItemRecord& r : shop->changed) inventory.places[r.slot] = Inventory::FromRecord(r);
        ++inventoryVersion;
        return;
    }
    // An NPC's repair: the pieces mended (a refusal's line is the game screen's).
    if (const auto* repaired = std::get_if<proto::NpcRepaired>(&event)) {
        for (const proto::ItemRecord& r : repaired->changed) inventory.places[r.slot] = Inventory::FromRecord(r);
        ++inventoryVersion;
        return;
    }
    if (const auto* stored = std::get_if<proto::StorageGoldChanged>(&event)) {
        (stored->guild != 0 ? guildStorage : storage).gold = stored->gold;
        ++inventoryVersion;
        return;
    }
    if (const auto* gold = std::get_if<proto::GoldChanged>(&event)) {
        inventory.gold = gold->gold;
        ++inventoryVersion;
        return;
    }
    // A stall's sale: the buyer's item and gold now; the owner's item gone and gold now.
    if (const auto* stall = std::get_if<proto::StallEvent>(&event)) {
        if (const auto* bought = std::get_if<proto::StallBought>(stall); bought != nullptr && bought->code == proto::StallCode::Ok) {
            inventory.places[bought->item.slot] = Inventory::FromRecord(bought->item);
            inventory.gold = bought->gold;
            ++inventoryVersion;
        } else if (const auto* sold = std::get_if<proto::StallSold>(stall); sold != nullptr && sold->place != 0) {
            inventory.places.erase(sold->place);
            inventory.gold = sold->gold;
            ++inventoryVersion;
        }
        return;
    }
    // A deal made in an exchange: what came, what went, the gold now.
    if (const auto* exchange = std::get_if<proto::ExchangeEvent>(&event)) {
        const auto* closed = std::get_if<proto::ExchangeClosed>(exchange);
        if (closed == nullptr || closed->done == 0) return;
        for (std::uint8_t place : closed->left) inventory.places.erase(place);
        for (const proto::ItemRecord& r : closed->arrived) inventory.places[r.slot] = Inventory::FromRecord(r);
        inventory.gold = closed->gold;
        ++inventoryVersion;
        return;
    }
    // The avatars worn: after the inventory, then each move of one.
    if (const auto* list = std::get_if<proto::AvatarList>(&event)) {
        inventory.avatars.clear();
        for (const proto::ItemRecord& r : list->items) inventory.avatars[r.slot] = Inventory::FromRecord(r);
        ++inventoryVersion;
        return;
    }
    if (const auto* moved = std::get_if<proto::AvatarMoved>(&event)) {
        if (moved->status != proto::ItemStatus::Ok) {
            itemRefusals.push_back(moved->status);
            return;
        }
        for (std::uint8_t place : moved->bagEmptied) inventory.places.erase(place);
        for (std::uint8_t place : moved->avatarsEmptied) inventory.avatars.erase(place);
        for (const proto::ItemRecord& r : moved->bag) inventory.places[r.slot] = Inventory::FromRecord(r);
        for (const proto::ItemRecord& r : moved->avatars) inventory.avatars[r.slot] = Inventory::FromRecord(r);
        ++inventoryVersion;
        return;
    }
    if (const auto* mall = std::get_if<proto::MallEvent>(&event)) {
        if (const auto* b = std::get_if<proto::SilkBalanceMessage>(mall)) {
            silk = b->own;
            giftSilk = b->gift;
            silkPoints = b->point;
            silkKnown = true;
        } else if (const auto* bought = std::get_if<proto::MallBought>(mall); bought != nullptr && bought->code == 0) {
            for (const proto::ItemRecord& r : bought->items) inventory.places[r.slot] = Inventory::FromRecord(r);
            ++inventoryVersion;
        } else if (const auto* played = std::get_if<proto::PopPlayed>(mall); played != nullptr && played->code == 0) {
            for (const proto::ItemRecord& r : played->item) inventory.places[r.slot] = Inventory::FromRecord(r);
            ++inventoryVersion;
        } else if (const auto* exchanged = std::get_if<proto::PopExchanged>(mall); exchanged != nullptr && exchanged->code == 0) {
            for (const proto::ItemRecord& r : exchanged->items) inventory.places[r.slot] = Inventory::FromRecord(r);
            ++inventoryVersion;
        } else if (const auto* d = std::get_if<proto::DurabilityChanged>(mall)) {
            if (auto it = inventory.places.find(d->place); it != inventory.places.end()) it->second.durability = d->durability;
            ++inventoryVersion;
        } else if (const auto* g = std::get_if<proto::StorageGrown>(mall); g != nullptr && g->target == 1) {
            inventory.size = std::min(g->size, Inventory::kMaxSize);
            ++inventoryVersion;
        } else if (g != nullptr && g->target == 2) {
            storage.size = std::min(g->size, Storage::kMaxSize);
            ++inventoryVersion;
        } else if (const auto* t = std::get_if<proto::TimedServices>(mall)) {
            timedServices = *t;
            timedServicesAt = now;
            ++timedServicesVersion;
            ShowTimedServices();
        }
        return;
    }
    if (const auto* moved = std::get_if<proto::ItemMoved>(&event)) {
        if (moved->status != proto::ItemStatus::Ok) {
            itemRefusals.push_back(moved->status);
            itemRefusalCodes.resize(itemRefusals.size() - 1, 0);
            itemRefusalCodes.push_back(moved->code);
            return;
        }
        for (std::uint8_t place : moved->emptied) inventory.places.erase(place);
        for (const proto::ItemRecord& r : moved->changed) inventory.places[r.slot] = Inventory::FromRecord(r);
        ++inventoryVersion;
        return;
    }
    if (const auto* answer = std::get_if<proto::ChatAnswer>(&event)) {
        if (answer->index >= pendingChat_.size() || !pendingChat_[answer->index].waiting) return;
        PendingChat& sent = pendingChat_[answer->index];
        sent.waiting = false;
        if (answer->result != proto::ChatResult::Ok) {
            ChatEntry refused;
            refused.kind = sent.kind;
            refused.name = sent.target;
            refused.result = answer->result;
            refused.seconds = answer->seconds;
            AddChat(std::move(refused));
            return;
        }
        // Taken: the player's own line (a global one comes back to everyone as a line).
        if (sent.kind == proto::ChatKind::Global) return;
        ChatEntry own;
        own.kind = sent.kind;
        own.sender = self.entity;
        own.name = sent.kind == proto::ChatKind::Whisper ? sent.target : self.name;
        own.text = std::move(sent.text);
        own.own = true;
        AddChat(std::move(own));
        return;
    }
    if (const auto* line = std::get_if<proto::ChatLine>(&event)) {
        // The player's own lines came as the answer; only a global one is shown from here.
        const bool byEntity = line->kind == proto::ChatKind::All || line->kind == proto::ChatKind::Gm;
        const bool mine = byEntity ? line->sender != 0 && line->sender == self.entity : !self.name.empty() && line->name == self.name;
        if (mine && line->kind != proto::ChatKind::Global) return;
        // Types 1..5 and 11 from a name on the PC's block list.
        const bool blockable = line->kind != proto::ChatKind::Global;
        if (!mine && blockable && std::find(chatBlocked.begin(), chatBlocked.end(), line->name) != chatBlocked.end()) return;
        ChatEntry entry;
        entry.kind = line->kind;
        entry.sender = line->sender;
        entry.name = line->name;
        entry.text = line->text;
        entry.own = mine;
        AddChat(std::move(entry));
        return;
    }
    if (std::holds_alternative<proto::ServerMove>(event)) {
        // A change of game server is a logout and a login: nothing the last one
        // kept running goes on; what its database holds is told again with the entry.
        timedServices = {};
        target = {};
        effects.clear();
        marks.clear();
        skillCooling.clear();
        groupCooling.clear();
        itemCooling.clear();
    }
    if (std::holds_alternative<proto::WorldLeft>(event)) {
        timedServices = {};
        target = {};
        effects.clear();
        marks.clear();
        self.party = 0; // out of the world, out of the party
        self.partyLeader = false;
    }
}

std::uint8_t PlayerState::QueueChat(proto::ChatKind kind, std::string target, std::string text) {
    const std::uint8_t index = nextChat_;
    nextChat_ = std::uint8_t(nextChat_ >= pendingChat_.size() - 1 ? 0 : nextChat_ + 1);
    pendingChat_[index] = {kind, std::move(target), std::move(text), true};
    return index;
}

void PlayerState::AddChat(ChatEntry entry) {
    chat.push_back(std::move(entry));
    ++chatPosted;
    while (chat.size() > kKeptChat) chat.pop_front();
}

void PlayerState::Post(GameMessage::Kind kind, std::string text, double time) {
    messages.push_back({kind, std::move(text), time});
    ++messagesPosted;
    while (messages.size() > kKeptMessages) messages.pop_front();
}

void PlayerState::ShowAcademyBuffs() {
    // The accumulation (board kind 7, tc_amends_buff.ddj) and the received buffs (kind 8, tc_reserve_buff.ddj; the
    // icons), their day's gauge; the HUD writes their tooltips.
    std::erase_if(effects, [](const ActiveEffect& e) { return e.kind == ActiveEffect::Kind::TimedJob && (e.token & 0xFFFF0000u) == kAcademyToken; });
    const auto add = [this](std::uint32_t token, const char* icon, const char* subject, double ends, bool timed) {
        ActiveEffect e;
        e.kind = ActiveEffect::Kind::TimedJob;
        e.token = kAcademyToken | token;
        e.icon = icon;
        e.subject = subject;
        e.duration = timed ? double(academy::kBuffMinutes) * 60.0 : 0.0;
        e.started = ends - e.duration;
        effects.push_back(std::move(e));
    };
    if (const AcademyView::Accumulation& a = academy.Accumulated(); a.shown) {
        add(0, "icon\\skill\\etc\\tc_amends_buff.ddj", "UIIT_CTL_TC_SAVE_BUFF", a.ends, !a.over);
    }
    for (std::size_t i = 0; i < academy.Reserves().size(); ++i) {
        add(std::uint32_t(1 + i), "icon\\skill\\etc\\tc_reserve_buff.ddj", "UIIT_CTL_TC_RESERVE_BUFF", academy.Reserves()[i].ends, true);
    }
}

void PlayerState::ShowTimedServices() {
    // The time services and the premium on the buff board (the premium list window's entries, ours there): each a
    // timed job keyed kTimedServiceToken | its category, its item's icon and name drawn by the HUD.
    std::erase_if(effects, [](const ActiveEffect& e) { return e.kind == ActiveEffect::Kind::TimedJob && (e.token & 0xFFFF0000u) == kTimedServiceToken; });
    const double elapsed = std::max(0.0, now - timedServicesAt);
    for (const proto::TimedService& s : timedServices.services) {
        ActiveEffect e;
        e.kind = ActiveEffect::Kind::TimedJob;
        e.id = s.item;
        e.token = kTimedServiceToken | s.category;
        e.started = now - elapsed;
        e.duration = double(s.secondsLeft);
        e.bonus = s.bonus;
        effects.push_back(std::move(e));
    }
}

} // namespace sr::game
