#include "sr/game/ItemCatalogue.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cctype>
#include <string_view>
#include <tuple>

#include "sr/assets/GameData.h"
#include "sr/game/Alchemy.h"
#include "sr/game/Cos.h"
#include "sr/game/FlagWorld.h"
#include "sr/game/Guild.h"
#include "sr/game/GuildRecall.h"
#include "sr/game/Items.h"
#include "sr/game/Jobs.h"
#include "sr/game/MagicPop.h"
#include "sr/game/Mall.h"
#include "sr/game/MallServices.h"
#include "sr/game/Potions.h"
#include "sr/game/QuestItems.h"
#include "sr/game/SpecialItems.h"
#include "sr/game/TimedServices.h"

namespace sr::game {

namespace {

// What still stands in the way of a quest item's use;
// empty: its handler, its quest and the quests before it are built.
std::string_view QuestItemGap(std::string_view codename) {
    // The six traps' captures (EASTEU_5, _11_1, _17_1, FINDTHIEF_2, BAEKRYOUNG_4, FLYSHIP1_3) are whole: the catch, its COS kept
    // over a relog and a move to the other game server, the bandits' hits on it, the client's selection, the daily resupply.
    // The hwan tests' trials (QuestDef::Trial) are built: QNO_*_HWAN_1_1, _1_5, _2_1, _2_5 from their NPC's talk; and 1_4_1's
    // trap (one roll, two catches: `capture-outcomes`): the whole line plays (2026-11-17). None left.
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 0> kGaps{};
    for (const auto& [code, gap] : kGaps) {
        if (code == codename) return gap;
    }
    return {};
}

using Tids = std::array<int, 4>;

bool Is(const assets::ItemRow& row, int t2, int t3, int t4) { return row.typeId == Tids{3, t2, t3, t4}; }
bool Is(const assets::ItemRow& row, int t2, int t3) { return row.typeId[0] == 3 && row.typeId[1] == t2 && row.typeId[2] == t3; }

std::string_view Trimmed(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// UseItem's checks (GameServer.cpp, `UseItem` and the UseMallService / UseMallItemOn it hands the mall's items to), in
// their order, on the data alone: the runtime's refusals (dead, the reuse lock, a running service, ...) come around
// them there. Nothing when UseItem has no branch for the row ("not usable yet").
bool UseRoute(const assets::ItemRow& row, const ItemRouteData& data, ItemRouting& out) {
    const auto& t = row.typeId;
    const auto set = [&](ItemRoute r, std::string why = {}) {
        out.route = r;
        out.why = std::move(why);
        return true;
    };
    // UseMallService (3/3/13): the resurrection scroll, the tickets, the premium, the expansions, the open market ticket;
    // the Extension Gear and the growth potion are refused there ("needs a target") and taken by UseItemOn.
    if (Is(row, 3, 13)) {
        switch (t[3]) {
        case 6: return set(ItemRoute::Resurrection);
        case 4:
        case 5: return set(ItemRoute::TimeTicket);
        case 14: return set(ItemRoute::Premium);
        case 17:
            if (row.params[1] != 0 && row.params[1] != 1) return set(ItemRoute::Unhandled, "its Param2 names no bag or storage: refused (code 3)");
            return set(ItemRoute::Expansion);
        case 19: return set(ItemRoute::TimeTicket, "the Open Market ticket: a category 11 timed job (more listings, longer, a lower sale fee)");
        case 16: return set(ItemRoute::SpiritExtension);
        case 15:
            // UseMallItemOn: the potions of growth as before; the watches and pet skills run their Param2 jobs on the pet of
            // the item named (Coses::UseMallItemOnPet).
            if (row.codename.find("GROWTH") != std::string::npos) return set(ItemRoute::PetGrowth);
            if (const PetItemJobs jobs = PetItemJobsOf(row); jobs && (jobs.attackPet || jobs.grabPet)) {
                std::string why = jobs.grabPet ? "a grab pet's" : "an attack pet's";
                if (!jobs.skill.empty()) why += std::string(" skill ") + jobs.skill + (jobs.skillByLevel ? "_NNN (the pet's level)" : "");
                if (jobs.bagPlaces > 0) why += ", its bag +" + std::to_string(jobs.bagPlaces) + " places";
                why += jobs.grabPet ? " for as many minutes as the stack holds (at most the rental's left; that many taken)" : " for the skill's time";
                return set(ItemRoute::PetItem, std::move(why));
            }
            return set(ItemRoute::Unhandled, "a pet item (its parts " + std::string(Trimmed(row.paramTexts[1])) + ") that names no pet: refused (2)");
        default: break;
        }
    }
    // Instances::UseItem: a hole (3/3/12/7, its world Desc2 one of _RefGame_World's) or a talisman of the collection book.
    if (Is(row, 3, 12, 7)) {
        if (Trimmed(row.paramTexts[1]).empty()) return set(ItemRoute::Unhandled, "a hole naming no world (Desc2)");
        return set(ItemRoute::Hole);
    }
    if (data.talismans != nullptr ? data.talismans->contains(row.codename) : Is(row, 3, 12, 5)) return set(ItemRoute::Talisman);
    // FlagWorlds::UseItem: So-Ok's card.
    if (row.codename == flag::kCard) return set(ItemRoute::FlagCard);
    // Coses::UseItem: a pet's item, the HGP potion, a ride's or a transport's scroll.
    if (PetOfItem(row) != CosKind::None) return set(ItemRoute::PetSummon);
    if (IsPetHgpPotion(row)) return set(ItemRoute::PetFood);
    if (IsCosScroll(row)) {
        if (data.characters != nullptr) {
            const std::string name(Trimmed(row.paramTexts[0]));
            // The ladder's first step stands for them all (the same kind of COS at every level).
            std::string first = name;
            if (const std::string ladder(Trimmed(row.paramTexts[1])); !ladder.empty() && ladder != "xxx") {
                first += "_" + ladder.substr(0, ladder.find(','));
                while (!first.empty() && first.back() == ' ') first.pop_back();
            }
            const assets::ObjectRow* summoned = data.characters->Find(std::string_view(first));
            const CosKind kind = summoned != nullptr ? CosKindOf(*summoned) : CosKind::None;
            if (kind != CosKind::Ride && kind != CosKind::Transport) {
                return set(ItemRoute::Unhandled, "it names " + first + (summoned == nullptr ? ", not in the characters" : ", no ride or transport") +
                                                     ": refused (UIIT_MSG_COSPETERR_CANT_USEITEM)");
            }
        }
        return set(ItemRoute::CosSummon);
    }
    // A fortress war's item: refused here, used through FortressUseItem with its place or zone.
    if (t[0] == 3 && t[1] == 3 && ((t[2] == 3 && t[3] >= 6 && t[3] <= 11 && t[3] != 10) || (t[2] == 1 && t[3] == 10) || (t[2] == 6 && t[3] == 2))) {
        return set(ItemRoute::Fortress);
    }
    // The scrolls and balloons (3/3/13/1..3): the skill Desc1 names.
    if (Is(row, 3, 13) && t[3] >= 1 && t[3] <= 3) {
        // As the server looks it up: Desc1 as written.
        const assets::SkillRow* skill = data.skills != nullptr ? data.skills->Find(std::string_view(row.paramTexts[0])) : nullptr;
        if (data.skills != nullptr && skill == nullptr) {
            return set(ItemRoute::Unhandled, "its skill \"" + row.paramTexts[0] + "\" is not in the skill table: refused (no skill)");
        }
        // A quest's trap (its skill's 'trap' and 'qest'; Skills::SetTrap): set at the user's feet, its catch the quest's event.
        if (skill != nullptr && assets::FindSkillParam(*skill, assets::SkillTag("trap")) && assets::FindSkillParam(*skill, assets::SkillTag("qest"))) {
            return set(ItemRoute::BuffScroll, "a quest's trap: set at the user's feet for its skill's time, its catch of the monster its 'qest' names the "
                                              "holder's quest event");
        }
        return set(ItemRoute::BuffScroll);
    }
    if (IsMonsterMask(row)) return set(ItemRoute::MonsterMask);
    if ((IsPotion(row) && t[3] >= 1 && t[3] <= 3) || (IsCure(row) && (t[3] == 1 || t[3] == 6))) return set(ItemRoute::Recovery);
    // The mall's pill bag: the universal pill's path with its own six params.
    if (IsCure(row) && t[3] == 8) {
        return set(ItemRoute::Recovery, "the bag as the universal pill with its own Param1..6 (the server never reads the pills Desc1 / Desc2 name): "
                                        "freeze and frostbite ended outright, the other states untouched (as the original)");
    }
    if (IsPetHpPotion(row)) return set(ItemRoute::PetRecovery);
    if (IsCure(row) && t[3] == 7) return set(ItemRoute::PetCure);
    if (Is(row, 3, 3, 4)) return set(ItemRoute::StallDecoration);
    if (IsRepairHammer(row)) return set(ItemRoute::RepairHammer);
    if (Is(row, 3, 3, 3)) return set(ItemRoute::ReverseReturn); // by its type alone: the quest's certificate too
    if (Is(row, 3, 3, 1) && row.paramTexts[2] == "THIEFDEN") return set(ItemRoute::ThiefDen);
    if (Is(row, 3, 3, 1) && row.paramTexts[2] == "RESURRECT") return set(ItemRoute::ReturnScroll);
    if (IsFirework(row)) return set(ItemRoute::Firework);
    if (IsBerserkPotion(row)) return set(ItemRoute::BerserkPotion);
    if (Is(row, 3, 3, 10)) {
        const BonusScroll b = BonusScrollOf(row);
        if (!b) return set(ItemRoute::Unhandled, "a bonus scroll of an unknown key (" + std::to_string(std::uint32_t(row.params[1])) + "): refused");
        return set(ItemRoute::BonusScroll); // every key's meaning from the keeper's table
    }
    // The guild recall: the original's checks and wait; the members on both game servers.
    if (IsGuildRecall(row)) return set(ItemRoute::GuildRecall);
    if (IsQuestUseItem(row)) {
        // What takes it in the original (game/QuestItems.h); a fortress's world refuses
        // it first (0x18F3).
        const QuestItemUse u = QuestItemUseOf(row.codename);
        switch (u.taker) {
        case QuestItemTaker::None:
            set(ItemRoute::QuestUse, "nothing takes it in the original's 1.188 server (no quest or running event names it): every use "
                                     "refused (code 2), as the original's");
            return true;
        case QuestItemTaker::GuildSupport:
            set(ItemRoute::QuestUse, "the guild support event's party summon scroll (always running): not in a town, level 30, not "
                                     "in the arena; a strong monster of the user's level for each party member, 20..100 away (SummonEvents)");
            return true;
        case QuestItemTaker::Pandora:
            set(ItemRoute::QuestUse, "the Pandora box (always running): the box, then for 20 minutes four strong monsters "
                                     "50 away whenever none stands, the box's cast of MSKILL_EVENT_PANDORA_ATTACK01 after each wave "
                                     "(SummonEvents)");
            return true;
        case QuestItemTaker::Quest: break;
        }
        // Its quest and the quests before it are ours (OriginalQuests*.cpp) and its handler
        // an `on use` rule (game/QuestItems.cpp): what still stands in the way, by item (2026-11-16); none: the path is whole.
        const std::string quests(u.quests), what(u.what);
        const std::string_view gap = QuestItemGap(row.codename);
        set(ItemRoute::QuestUse, "taken only for a holder of " + quests + " (" + std::string(u.kind) + ": " + what + ")" +
                                     (gap.empty() ? std::string("; its quest and chain built") : "; " + std::string(gap)));
        out.gap = !gap.empty();
        return true;
    }
    return false;
}

} // namespace

ItemRouting ItemRouteOf(const assets::ItemRow& row, const ItemRouteData& data) {
    ItemRouting out;
    const auto& t = row.typeId;
    // Worn: the equipment's moves (ItemMove into places 0..12, AvatarMove).
    switch (KindOf(row)) {
    case ItemKind::Weapon:
    case ItemKind::Shield:
    case ItemKind::Armour:
    case ItemKind::Accessory:
        out.route = ItemRoute::Equip;
        if (t[3] == 16 && KindOf(row) == ItemKind::Weapon) out.why = "a fortress weapon: worn only in a fortress position its Param5 allows (0x18E0)";
        return out;
    case ItemKind::JobSuit:
        if (!IsJobSuit(row)) {
            // The event suit, the PvP cape, the whole-body suits (TID4 4..7): no job's rules, no job state.
            out.route = ItemRoute::Equip;
            out.why = "worn in the job place as ordinary equipment (TID4 " + std::to_string(t[3]) + "): no league, town or 10 s change, no job state";
            return out;
        }
        out.route = ItemRoute::JobSuit;
        return out;
    case ItemKind::Avatar:
        if (AvatarPlaceFor(row) < 0) break;
        out.route = ItemRoute::Avatar;
        return out;
    default: break;
    }
    if (UseRoute(row, data, out)) {
        // A use refused by its data on an item the client never uses (CanUse 0): not usable.
        if (out.route == ItemRoute::Unhandled && row.canUse == 0) out.route = ItemRoute::NotUsable;
        return out;
    }
    // The other messages that take an item from the bag.
    if (IsGenderSwitchTool(row)) return {ItemRoute::GenderSwitch, {}};
    if (PetTreatmentOf(row) != PetTreatment::None) return {ItemRoute::PetTreatment, {}};
    if (IsSkinChangeScroll(row)) return {ItemRoute::SkinChange, {}};
    if (WithdrawModeOf(row) != WithdrawMode::None) return {ItemRoute::SkillWithdraw, {}};
    if (IsMagicCube(row)) return {ItemRoute::MagicCube, {}};
    if (Is(row, 3, 3, 5)) return {ItemRoute::GlobalChat, {}};
    if (CrestItemOf(row.codename)) return {ItemRoute::GuildCrest, {}};
    if (alchemy::IsSocketStone(row) || alchemy::IsSocketMaker(row) || alchemy::IsSocketEraser(row)) return {ItemRoute::Socket, {}};
    if (alchemy::KindOf(row) != alchemy::Kind::None && alchemy::KindOf(row) != alchemy::Kind::Equipment) return {ItemRoute::Alchemy, {}};
    if (pop::IsCard(row) || pop::IsCoupon(row)) return {ItemRoute::MagicPop, {}};
    // Every 3/3/8, the events' foods (TID4 0) too: the original's tests read TID1..3 alone, and the foods carry the
    // goods' own flags (CanUse 0, CanTrade 0, a goods'
    // price): trade goods to it as to ours.
    if (IsSpecialtyGood(row)) return {ItemRoute::TradeGood, {}};
    if (IsAmmunition(row)) return {ItemRoute::Ammunition, {}};
    if (Is(row, 3, 5, 0)) return {ItemRoute::Gold, {}};
    if (Is(row, 3, 9, 0) && row.canUse == 0) return {ItemRoute::QuestItem, {}};
    if (const std::uint16_t code = OriginalUseRefusal(row); code != 0 && row.canUse != 0) {
        // The guild's mercenary scrolls (3/3/12/1): requirement type 10, which the use's gate does not know.
        if (const auto unknown = std::find_if(row.requirementTypes.begin(), row.requirementTypes.end(), [](int type) { return !RequirementTypeKnown(type); });
            unknown != row.requirementTypes.end()) {
            return {ItemRoute::OriginalRefuses, "the original's use gate does not know its requirement type " + std::to_string(*unknown) +
                                                    " (refused 2) before the summon: no mercenary is ever summoned; "
                                                    "ours answers the same"};
        }
        char hex[16];
        std::snprintf(hex, sizeof hex, "%#x", unsigned(code));
        return {ItemRoute::OriginalRefuses, std::string("the original's server refuses its use (") + hex + "): ours answers the same"};
    }
    if (row.canUse != 0) {
        out.route = ItemRoute::Unhandled;
        out.why = "UseItem has no branch for it: refused (\"not usable yet\")";
        return out;
    }
    out.route = ItemRoute::NotUsable;
    return out;
}

ItemState ItemStateOf(const ItemRouting& routing) {
    switch (routing.route) {
    case ItemRoute::Unhandled: return ItemState::Unhandled;
    case ItemRoute::Declined: return ItemState::Declined;
    case ItemRoute::NotUsable: return ItemState::Carried;
    default: return routing.gap ? ItemState::Partly : ItemState::Implemented;
    }
}

const char* ItemStateName(ItemState state) {
    switch (state) {
    case ItemState::Implemented: return "implemented";
    case ItemState::Partly: return "partly";
    case ItemState::Unhandled: return "unhandled";
    case ItemState::Declined: return "declined";
    case ItemState::Carried: return "carried";
    }
    return "?";
}

const char* ItemRouteName(ItemRoute route) {
    switch (route) {
    case ItemRoute::Equip: return "equip";
    case ItemRoute::JobSuit: return "job-suit";
    case ItemRoute::Avatar: return "avatar";
    case ItemRoute::Ammunition: return "ammunition";
    case ItemRoute::TimeTicket: return "time-ticket";
    case ItemRoute::Premium: return "premium";
    case ItemRoute::Expansion: return "expansion";
    case ItemRoute::Resurrection: return "resurrection";
    case ItemRoute::SpiritExtension: return "spirit-extension";
    case ItemRoute::PetGrowth: return "pet-growth";
    case ItemRoute::PetItem: return "pet-item";
    case ItemRoute::Hole: return "dimension-hole";
    case ItemRoute::Talisman: return "talisman";
    case ItemRoute::FlagCard: return "flag-card";
    case ItemRoute::PetSummon: return "pet";
    case ItemRoute::PetFood: return "pet-food";
    case ItemRoute::CosSummon: return "cos-scroll";
    case ItemRoute::Fortress: return "fortress";
    case ItemRoute::BuffScroll: return "buff-scroll";
    case ItemRoute::MonsterMask: return "monster-mask";
    case ItemRoute::Recovery: return "recovery";
    case ItemRoute::PetRecovery: return "pet-recovery";
    case ItemRoute::PetCure: return "pet-cure";
    case ItemRoute::StallDecoration: return "stall-decoration";
    case ItemRoute::RepairHammer: return "repair-hammer";
    case ItemRoute::ReverseReturn: return "reverse-return";
    case ItemRoute::ThiefDen: return "thief-den";
    case ItemRoute::ReturnScroll: return "return-scroll";
    case ItemRoute::Firework: return "firework";
    case ItemRoute::BerserkPotion: return "berserk-potion";
    case ItemRoute::BonusScroll: return "bonus-scroll";
    case ItemRoute::QuestUse: return "quest-use";
    case ItemRoute::GuildRecall: return "guild-recall";
    case ItemRoute::GenderSwitch: return "gender-switch";
    case ItemRoute::PetTreatment: return "pet-treatment";
    case ItemRoute::SkinChange: return "skin-change";
    case ItemRoute::SkillWithdraw: return "skill-withdraw";
    case ItemRoute::MagicCube: return "magic-cube";
    case ItemRoute::GlobalChat: return "global-chat";
    case ItemRoute::GuildCrest: return "guild-crest";
    case ItemRoute::Alchemy: return "alchemy";
    case ItemRoute::Socket: return "socket";
    case ItemRoute::MagicPop: return "magic-pop";
    case ItemRoute::TradeGood: return "trade-good";
    case ItemRoute::QuestItem: return "quest-item";
    case ItemRoute::Gold: return "gold";
    case ItemRoute::Declined: return "declined";
    case ItemRoute::OriginalRefuses: return "original-refuses";
    case ItemRoute::Unhandled: return "unhandled";
    case ItemRoute::NotUsable: return "not-usable";
    }
    return "?";
}

const char* ItemRouteText(ItemRoute route) {
    switch (route) {
    case ItemRoute::Equip: return "Worn in its place (weapon, shield, a piece of armour, an accessory) when the inventory's rules allow: level, race, sex, not broken.";
    case ItemRoute::JobSuit: return "Worn in the job place: its job's league, the alias, in a town, no party, the job level; then a 10 s change.";
    case ItemRoute::Avatar: return "Worn in the avatar places (hat, dress, attachment, flag; the Devil's Spirit only while awake).";
    case ItemRoute::Ammunition: return "Worn beside the bow or the crossbow; each shot takes one, the bag's next stack follows.";
    case ItemRoute::TimeTicket: return "A timed service of its category for its days (an experience or skill experience bonus, hours of bonus a day); one of a category at a time.";
    case ItemRoute::Premium: return "The premium package: its parts (a ticket, daily uses, buffs, the booth, its quest) for its time; one at a time.";
    case ItemRoute::Expansion: return "The bag (Param2 0) or the storage (1) grows by Param1 places, to its limit.";
    case ItemRoute::Resurrection: return "Used while dead only: risen where it fell with its HP and MP (Param1 each, else Param2 %) and Param3 % of the death's experience.";
    case ItemRoute::SpiritExtension: return "On an awakened Devil's Spirit in the bag: its awakening longer by Param1 seconds.";
    case ItemRoute::PetGrowth: return "On a growth pet's item whose pet is out: the pet's experience +100 % for Param1 seconds; one at a time.";
    case ItemRoute::PetItem: return "On a pet's item whose pet is out: a watch gives the grab pet 28 places more and its skill for the stack's minutes (at most the rental's), a pet skill the attack pet its imbue for 30 minutes; one of a kind at a time.";
    case ItemRoute::Hole: return "Opens a Forgotten World instance (Desc2) of its grade for its user's party, the levels Param3..Param4.";
    case ItemRoute::Talisman: return "Registered in the collection book under its theme (the item is used up).";
    case ItemRoute::FlagCard: return "So-Ok's Subscription Card: the account registered for Capture the Flag.";
    case ItemRoute::PetSummon: return "Summons its pet (the attack pet or the pick pet) beside its owner; the item stays, linked to the pet.";
    case ItemRoute::PetFood: return "Fed to the attack pet out: its hunger (HGP) up by Param1 %; refused when full.";
    case ItemRoute::CosSummon: return "Summons a ride or a transport (Desc1, by the level ladder Desc2) beside its owner, boarded at once; used up.";
    case ItemRoute::Fortress: return "A fortress war item, used through FortressUseItem with its place, zone or target (tablet, guard, manual, flag, scroll, repair kit, shock bomb).";
    case ItemRoute::BuffScroll: return "The skill its Desc1 names, as a buff on the reader; a second one starts the time again.";
    case ItemRoute::MonsterMask: return "Its skill turns the user into the monster it carries (its level at most the user's).";
    case ItemRoute::Recovery: return "HP and MP (at once by percent, or in five steps by amount) or a cure; its kind locked for its reuse time.";
    case ItemRoute::PetRecovery: return "On its user's COS (a ride, a transport, the attack pet): its HP back.";
    case ItemRoute::PetCure: return "On its user's COS (the one named, else the first out): its bad states shortened by Param1..6 levels; used up.";
    case ItemRoute::StallDecoration: return "The player's stall wears it from now on (kept); the item is used up.";
    case ItemRoute::RepairHammer: return "Everything worn and in the bag mended to its most, free; nothing to mend: refused.";
    case ItemRoute::ReverseReturn: return "To one of three after Param1 ms: the recall point (the last return scroll's place), the place of death, a map point.";
    case ItemRoute::ThiefDen: return "A thief's only: after Param1 ms to the thieves' town; a transport out comes along.";
    case ItemRoute::ReturnScroll: return "After Param1 ms to the return point (the race's start town until a gate designates one); its place kept as the recall point.";
    case ItemRoute::Firework: return "Used up at once; those around see its show (the effect list named after it).";
    case ItemRoute::BerserkPotion: return "The berserk gauge full (five points); refused in berserk (0x18CE); its COOLTIME, if any, waits.";
    case ItemRoute::BonusScroll: return "For Param1 s a bonus by its key: kills' experience or skill experience +Param3 %, or the berserk time +Param3 s; one of a key at a time (0x1894).";
    case ItemRoute::QuestUse: return "Taken by the quest (or event) under way whose handler wants it, then used up; refused (2) when none takes it.";
    case ItemRoute::GuildRecall: return "A guild master's, in a town, its guild at level Param3 (5): every member online on either game server asked for Param2 / 1,000 s (30) to come to it; Param1 / 1,000 s (an hour) before the next.";
    case ItemRoute::GenderSwitch: return "On a piece of armour in the bag (degree up to Param1): it becomes its other sex's counterpart, all else kept.";
    case ItemRoute::PetTreatment: return "On a pet's item: a dead attack pet revived, or a pick pet's time extended.";
    case ItemRoute::SkinChange: return "Its window: a new model of the same race (another sex with no armour on) and scale.";
    case ItemRoute::SkillWithdraw: return "Taken by the skill and mastery withdrawal window, one a level: the SP back (the quest's potion 80 % and gold).";
    case ItemRoute::MagicCube: return "Keeps up to Param9 B elixirs of its kind (Param10); one dropped on it goes in, taken out to the bag.";
    case ItemRoute::GlobalChat: return "A global chat line takes one.";
    case ItemRoute::GuildCrest: return "The guild window's crest takes one (a guild's or a union's, registered or deleted).";
    case ItemRoute::Alchemy: return "A material of the alchemy window: strengthening (elixir, powder, stones), making a stone (tablet, elements), breaking down (materials, Rondos).";
    case ItemRoute::Socket: return "Socket alchemy: a stone put into a made socket (its skills while worn), the maker of sockets, the eraser.";
    case ItemRoute::MagicPop: return "Magic POP: a card played at its NPC; a coupon its prize.";
    case ItemRoute::TradeGood: return "A specialty good: bought into the transport at a specialty NPC, sold at another town's at its price there.";
    case ItemRoute::QuestItem: return "Never used: its quest gives it (a drop, the start) and counts it; the quest's end takes it.";
    case ItemRoute::Gold: return "Gold's look on the ground: picking it up adds its amount.";
    case ItemRoute::Declined: return "Our server refuses it on purpose.";
    case ItemRoute::OriginalRefuses: return "The original's server refuses its use; ours answers the same code.";
    case ItemRoute::Unhandled: return "Usable by its data, but nothing of our server takes it: refused.";
    case ItemRoute::NotUsable: return "Not usable (CanUse 0) and nothing takes it: carried, stored, sold or traded.";
    }
    return "";
}

const char* ItemRouteHandler(ItemRoute route) {
    switch (route) {
    case ItemRoute::Equip: return "GameServer::MoveItem -> Inventory::Move (game::CanEquip)";
    case ItemRoute::JobSuit: return "GameServer::MoveItem -> SuitRefusal";
    case ItemRoute::Avatar: return "GameServer::MoveAvatar";
    case ItemRoute::Ammunition: return "GameServer::MoveItem; TakeAmmunition at each shot";
    case ItemRoute::TimeTicket:
    case ItemRoute::Premium:
    case ItemRoute::Expansion:
    case ItemRoute::Resurrection:
    case ItemRoute::Declined: return "GameServer::UseItem -> UseMallService";
    case ItemRoute::SpiritExtension:
    case ItemRoute::PetGrowth: return "GameServer::UseMallItemOn (UseItemOn)";
    case ItemRoute::PetItem: return "GameServer::UseMallItemOn (UseItemOn) -> Coses::UseMallItemOnPet";
    case ItemRoute::Hole:
    case ItemRoute::Talisman: return "GameServer::UseItem -> Instances::UseItem";
    case ItemRoute::FlagCard: return "GameServer::UseItem -> FlagWorlds::UseItem";
    case ItemRoute::PetSummon: return "GameServer::UseItem -> Coses::UseItem -> UsePet";
    case ItemRoute::PetFood:
    case ItemRoute::CosSummon: return "GameServer::UseItem -> Coses::UseItem";
    case ItemRoute::Fortress: return "Fortresses::Handle (FortressUseItem)";
    case ItemRoute::BuffScroll: return "GameServer::UseItem -> Skills::AddBuff";
    case ItemRoute::MonsterMask: return "GameServer::UseItem -> Skills::TransformInto";
    case ItemRoute::Recovery: return "GameServer::UseItem -> UseRecovery (and the auto potion)";
    case ItemRoute::PetRecovery: return "GameServer::UseItem -> UsePetRecovery";
    case ItemRoute::PetCure: return "GameServer::UseItem -> UsePetCure";
    case ItemRoute::StallDecoration: return "GameServer::UseItem (the stall's skin)";
    case ItemRoute::RepairHammer: return "GameServer::UseItem -> UseHammer";
    case ItemRoute::ReverseReturn:
    case ItemRoute::ThiefDen:
    case ItemRoute::ReturnScroll: return "GameServer::UseItem (returnAt, then the travel)";
    case ItemRoute::Firework:
    case ItemRoute::BerserkPotion:
    case ItemRoute::BonusScroll:
    case ItemRoute::OriginalRefuses: return "GameServer::UseItem -> UseSpecialItem";
    case ItemRoute::QuestUse: return "GameServer::UseItem -> UseSpecialItem -> Quests::UseItem (QuestDef::onUse), SummonEvents::UseItem";
    case ItemRoute::GuildRecall: return "GameServer::UseItem -> UseGuildRecall -> Guilds::Recall (GuildRecallAnswer)";
    case ItemRoute::GenderSwitch: return "GameServer::UseItemOnService (UseItemOn)";
    case ItemRoute::PetTreatment: return "Coses::UseOnPet (CosUseOnPet)";
    case ItemRoute::SkinChange: return "GameServer::ChangeSkin (SkinChange)";
    case ItemRoute::SkillWithdraw: return "GameServer::Withdraw (Withdraw)";
    case ItemRoute::MagicCube: return "GameServer::MoveCube (CubeMove)";
    case ItemRoute::GlobalChat: return "GameServer::Chat (the global line)";
    case ItemRoute::GuildCrest: return "Guilds::Handle (the crest's request)";
    case ItemRoute::Alchemy:
    case ItemRoute::Socket: return "Alchemy::Handle";
    case ItemRoute::MagicPop: return "MagicPop::Handle";
    case ItemRoute::TradeGood: return "GameServer::BuyGood -> Trades::Buy; Trades::Handle";
    case ItemRoute::QuestItem: return "Quests (its goals count it, QuestTake)";
    case ItemRoute::Gold: return "GameServer: the drop's gold (picked up as gold)";
    case ItemRoute::Unhandled: return "GameServer::UseItem: refused";
    case ItemRoute::NotUsable: return "";
    }
    return "";
}

const char* ItemRouteNote(ItemRoute route) {
    switch (route) {
    case ItemRoute::Equip:
    case ItemRoute::Ammunition:
    case ItemRoute::Gold: return "inventory";
    case ItemRoute::JobSuit:
    case ItemRoute::ThiefDen:
    case ItemRoute::TradeGood: return "jobs";
    case ItemRoute::Avatar: return "dress";
    case ItemRoute::TimeTicket:
    case ItemRoute::Premium:
    case ItemRoute::Expansion:
    case ItemRoute::Resurrection:
    case ItemRoute::SpiritExtension:
    case ItemRoute::PetGrowth:
    case ItemRoute::BuffScroll:
    case ItemRoute::RepairHammer:
    case ItemRoute::GenderSwitch:
    case ItemRoute::SkinChange:
    case ItemRoute::SkillWithdraw:
    case ItemRoute::MagicCube:
    case ItemRoute::Declined: return "mall-items";
    case ItemRoute::Hole:
    case ItemRoute::Talisman: return "forgotten-world";
    case ItemRoute::FlagCard: return "so-ok";
    case ItemRoute::PetSummon:
    case ItemRoute::PetFood:
    case ItemRoute::CosSummon:
    case ItemRoute::PetRecovery:
    case ItemRoute::PetTreatment:
    case ItemRoute::PetCure: return "cos";
    case ItemRoute::Firework:
    case ItemRoute::BerserkPotion:
    case ItemRoute::BonusScroll:
    case ItemRoute::QuestUse:
    case ItemRoute::GuildRecall:
    case ItemRoute::PetItem:
    case ItemRoute::OriginalRefuses: return "special-items";
    case ItemRoute::Fortress: return "fortress-war";
    case ItemRoute::MonsterMask: return "special-skills";
    case ItemRoute::Recovery: return "auto-potion";
    case ItemRoute::StallDecoration: return "stall";
    case ItemRoute::ReverseReturn:
    case ItemRoute::ReturnScroll: return "reverse-return";
    case ItemRoute::GlobalChat: return "chat";
    case ItemRoute::GuildCrest: return "guilds";
    case ItemRoute::Alchemy:
    case ItemRoute::Socket: return "alchemy";
    case ItemRoute::MagicPop: return "magic-pop";
    case ItemRoute::QuestItem: return "quests";
    case ItemRoute::Unhandled:
    case ItemRoute::NotUsable: return "";
    }
    return "";
}

CatalogueKind CatalogueKindOf(const assets::ItemRow& row) {
    const auto& t = row.typeId;
    if (t[0] != 3) return {};
    if (t[1] == 1) {
        switch (t[2]) {
        case 1: return {"armour", "garment"};
        case 2: return {"armour", "protector"};
        case 3: return {"armour", "armor"};
        case 9: return {"armour", "robe"};
        case 10: return {"armour", "light armor"};
        case 11: return {"armour", "heavy armor"};
        case 4: return {"shield", t[3] == 1 ? "china" : t[3] == 2 ? "europe" : "unknown"};
        case 5:
        case 12: return {"accessory", t[3] == 1 ? "earring" : t[3] == 2 ? "necklace" : t[3] == 3 ? "ring" : "unknown"};
        case 6:
            switch (t[3]) {
            case 2: return {"weapon", "sword"};
            case 3: return {"weapon", "blade"};
            case 4: return {"weapon", "spear"};
            case 5: return {"weapon", "glaive"};
            case 6: return {"weapon", "bow"};
            case 7: return {"weapon", "one-handed sword"};
            case 8: return {"weapon", "two-handed sword"};
            case 9: return {"weapon", "axe"};
            case 10: return {"weapon", "warlock staff"};
            case 11: return {"weapon", "two-handed staff"};
            case 12: return {"weapon", "crossbow"};
            case 13: return {"weapon", "dagger"};
            case 14: return {"weapon", "harp"};
            case 15: return {"weapon", "cleric rod"};
            case 16: return {"fortress", "weapon"};
            default: return {"weapon", "unknown"};
            }
        case 7:
            switch (t[3]) {
            case 1: return {"job", "trader suit"};
            case 2: return {"job", "thief suit"};
            case 3: return {"job", "hunter suit"};
            case 4: return {"job", "event suit"};
            case 5: return {"job", "pvp cape"};
            case 6: return {"job", "trader suit (whole body)"};
            case 7: return {"job", "hunter suit (whole body)"};
            default: return {"job", "unknown"};
            }
        case 13:
            switch (t[3]) {
            case 1: return {"avatar", "hat"};
            case 2: return {"avatar", "dress"};
            case 3: return {"avatar", "attachment"};
            case 4: return {"avatar", "flag"};
            default: return {"avatar", "unknown"};
            }
        case 14: return {"avatar", "devil's spirit"};
        default: return {"equipment", "unknown"};
        }
    }
    if (t[1] == 2) {
        if (t[2] == 1) return {"pet", t[3] == 1 ? "attack pet" : t[3] == 2 ? "pick pet" : "unknown"};
        if (t[2] == 2) return {"transform", "monster mask"};
        if (t[2] == 3) return {"mall", "magic cube"};
        return {"pet", "unknown"};
    }
    if (t[1] != 3) return {};
    switch (t[2]) {
    case 1:
        switch (t[3]) {
        case 1: return {"potion", "hp"};
        case 2: return {"potion", "mp"};
        case 3: return {"potion", "hp and mp"};
        case 4: return {"pet", "hp potion"};
        case 6: return {"pet", "revival"};
        case 8: return {"potion", "berserk"};
        case 9: return {"pet", "hgp potion"};
        case 10: return {"fortress", "repair kit"};
        default: return {"potion", "unknown"};
        }
    case 2:
        switch (t[3]) {
        case 1: return {"pill", "purification"};
        case 6: return {"pill", "universal"};
        case 7: return {"pet", "universal pill"};
        case 8: return {"pill", "pill bag"};
        default: return {"pill", "unknown"};
        }
    case 3:
        switch (t[3]) {
        case 1: return {"scroll", row.paramTexts[2] == "THIEFDEN" ? "thieves' town return" : "return"};
        case 2: return {"cos", "ride or transport scroll"};
        case 3: return {"scroll", "reverse return"};
        case 4: return {"stall", "decoration"};
        case 5: return {"scroll", "global chat"};
        case 6: return {"fortress", "tablet"};
        case 7: return {"fortress", "guard"};
        case 8: return {"fortress", "manual"};
        case 9: return {"fortress", "flag"};
        case 10: return {"scroll", "berserk time"};
        case 11: return {"fortress", "scroll"};
        case 12: return {"scroll", "skill points"};
        case 14: return {"scroll", "plus"};
        default: return {"scroll", "unknown"};
        }
    case 4: return {"ammunition", t[3] == 1 ? "arrows" : t[3] == 2 ? "bolts" : "unknown"};
    case 5: return {"money", t[3] == 0 ? "gold" : "coin"};
    case 6: return t[3] == 2 ? CatalogueKind{"fortress", "shock bomb"} : CatalogueKind{"event", "firework"};
    case 7: return {"event", "campfire"};
    case 8:
        if (t[3] == 1) return {"trade", "specialty good"};
        if (t[3] == 2) return {"trade", "bargain good"};
        return {"event", "food"};
    case 9: return {"quest", "quest item"};
    case 10:
        switch (t[3]) {
        case 1: return {"alchemy", "elixir"};
        case 2: return {"alchemy", "lucky powder"};
        case 4: return {"alchemy", "advanced elixir"};
        default: return {"alchemy", "unknown"};
        }
    case 11:
        switch (t[3]) {
        case 1: return {"alchemy", "magic stone"};
        case 2: return {"alchemy", "attribute stone"};
        case 3: return {"alchemy", "tablet"};
        case 4: return {"alchemy", "material"};
        case 5: return {"alchemy", "element"};
        case 6: return {"alchemy", "rondo"};
        case 7: return {"alchemy", "special stone"};
        case 8: return {"alchemy", "socket stone"};
        case 9: return {"alchemy", "socket eraser"};
        case 10: return {"alchemy", "socket maker"};
        default: return {"alchemy", "unknown"};
        }
    case 12:
        switch (t[3]) {
        case 1: return {"guild", "soldier scroll"};
        case 2: return {"guild", "crest"};
        case 3: return {"guild", "recall"};
        case 4: return {"guild", "arena ticket"};
        case 5: return {"forgotten world", "talisman"};
        case 6: return {"mall", "rename"};
        case 7: return {"forgotten world", "dimension hole"};
        default: return {"guild", "unknown"};
        }
    case 13:
        switch (t[3]) {
        case 0: return {"mall", "skill restoration"};
        case 1:
        case 2:
        case 3: return {"mall", "buff scroll"};
        case 4: return {"mall", "time ticket"};
        case 5: return {"mall", "skill time ticket"};
        case 6: return {"mall", "resurrection scroll"};
        case 7: return {"mall", "repair hammer"};
        case 8: return {"mall", "gender switch"};
        case 9: return {"mall", "skin change"};
        case 10: return {"mall", "storage ticket"};
        case 11: return {"mall", "reinforce scroll"};
        case 12: return {"pet", "clock"};
        case 13: return {"mall", "stat reset"};
        case 14: return {"mall", "premium"};
        case 15: return {"pet", "pet service"};
        case 16: return {"mall", "spirit extension"};
        case 17: return {"mall", "expansion"};
        case 18: return {"mall", "indulgence"};
        case 19: return {"mall", "open market ticket"};
        default: return {"mall", "unknown"};
        }
    case 14: return {"magic pop", t[3] == 1 ? "card" : t[3] == 2 ? "coupon" : "unknown"};
    case 15: return {"event", t[3] == 1 ? "monster box" : t[3] == 2 ? "summon scroll" : "unknown"};
    case 16:
        switch (t[3]) {
        case 2: return {"pet", "skill removal"};
        case 3: return {"pet", "name removal"};
        case 4: return {"pet", "level move"};
        default: return {"pet", "unknown"};
        }
    default: return {"other", "unknown"};
    }
}

const char* ItemSourceOf(const assets::ItemRow& row) {
    const std::string& c = row.codename;
    if (c.starts_with("ITEM_EVENT_RENT_")) return "rental";
    if (row.cashItem || c.starts_with("ITEM_MALL_") || c.starts_with("ITEM_PRE_MALL_")) return "mall";
    if (c.starts_with("ITEM_EVENT_")) return "event";
    if (c.starts_with("ITEM_ETC_E") && c.size() > 11 && std::isdigit(static_cast<unsigned char>(c[10])) != 0) return "event";
    if (c.starts_with("ITEM_QNO_") || c.starts_with("ITEM_QSP_") || c.starts_with("ITEM_QCX_") || c.starts_with("ITEM_QTUTORIAL_")) return "quest";
    if (c.starts_with("ITEM_GM_")) return "gm";
    return "game";
}

const char* RequirementName(int type) {
    switch (type) {
    case 1: return "level";
    case 2: return "trader level";
    case 3: return "thief level";
    case 4: return "hunter level";
    default: return nullptr;
    }
}

std::vector<ItemCatalogueEntry> BuildItemCatalogue(const assets::ItemTable& items, const ItemRouteData& data) {
    std::vector<ItemCatalogueEntry> out;
    out.reserve(items.size());
    for (const auto& [code, row] : items.rows()) {
        if (!row.inService) continue;
        ItemCatalogueEntry e;
        e.row = &row;
        e.kind = CatalogueKindOf(row);
        e.routing = ItemRouteOf(row, data);
        e.source = ItemSourceOf(row);
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const ItemCatalogueEntry& a, const ItemCatalogueEntry& b) {
        return std::tie(a.row->typeId, a.row->refObjId) < std::tie(b.row->typeId, b.row->refObjId);
    });
    return out;
}

} // namespace sr::game
