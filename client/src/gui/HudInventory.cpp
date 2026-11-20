// The inventory and the equipment: the item tooltip, the inventory's
// places and gold, the equipment's worn places around the turning character.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "gui/Hud.h"
#include "gui/HudWindows.h"
#include "sr/game/MagicPop.h"
#include "sr/game/Alchemy.h"
#include "sr/game/Cos.h"
#include "sr/game/CosProtocol.h"

namespace sr::client {

namespace {

namespace proto = game::proto;
using game::ItemKind;

// The tooltip's colours (the original's ARGB).
const ui::Color kKindBeige = ui::Argb(0xFFEFDAA4), kWhite = ui::Argb(0xFFFFFFFF), kRed = ui::Argb(0xFFFF4A4A),
                kBlue = ui::Argb(0xFF00EAFF), kRareGold = ui::Argb(0xFFFFD953), kRepairedGreen = ui::Argb(0xFF6CE675),
                kMagicName = ui::Argb(0xFF72BFFF);
constexpr int kShadow = ui::kTextShadow, kBoldShadow = ui::kTextShadow | ui::kTextBold;

std::string Printf(const char* format, double a, double b = 0, int c = 0) {
    char buffer[128];
    std::snprintf(buffer, sizeof buffer, format, a, b, c);
    return buffer;
}

} // namespace

int InventoryPlaceOf(const ui::Widget* slot) {
    if (slot == nullptr) return -1;
    for (const ui::Widget* w = slot->parent(); w != nullptr; w = w->parent()) {
        if (dynamic_cast<const InventoryWindow*>(w) != nullptr) return slot->id() >= 100 ? 13 + slot->id() - 100 : -1;
        if (dynamic_cast<const EquipmentWindow*>(w) != nullptr) return slot->id() >= 100 && slot->id() <= 112 ? slot->id() - 100 : -1;
    }
    return -1;
}

int AvatarPlaceOf(const ui::Widget* slot) {
    if (slot == nullptr) return -1;
    for (const ui::Widget* w = slot->parent(); w != nullptr; w = w->parent()) {
        if (const auto* equipment = dynamic_cast<const EquipmentWindow*>(w)) return equipment->AvatarPlaceShown(slot);
    }
    return -1;
}

namespace {

int PlaceOf(const ui::Widget* slot) { return InventoryPlaceOf(slot); }

void RequestMove(HudContext& context, int from, int to, int quantity = 0) {
    if (context.hud == nullptr || from < 0 || to < 0 || from == to) return;
    if (AlchemyShown(context)) return; // the alchemy window holds the bag
    HudRequest r{HudRequest::Kind::MoveItem, from};
    r.extra = to | (quantity << 8);
    context.hud->Request(r);
}

// The text the server's refusal shows (the original's item error texts).
const char* RefusalKey(proto::ItemStatus status) {
    switch (status) {
    case proto::ItemStatus::WrongSlot: return "UIIT_MSG_STRGERR_EQUIPITEM";
    case proto::ItemStatus::FortressPosition: return "UIIT_STT_FORT_ITEMUSE_ERROR_POSITION";
    case proto::ItemStatus::Level: return "UIIT_MSG_STRGERR_HIGHER_LEVEL_REQUIRED";
    case proto::ItemStatus::Race: return "UIIT_MSG_STRGERR_COUNTRY_MISMATCH";
    case proto::ItemStatus::Sex: return "UIIT_MSG_STRGERR_GENDER_MISMATCH";
    case proto::ItemStatus::Broken: return "UIIT_MSG_STRGERR_CANT_EQUIP_RAZED_ITEM";
    case proto::ItemStatus::BagFull: return "UIIT_MSG_STRGERR_INVENTORY_FULL";
    case proto::ItemStatus::MixedArmour: return "UIIT_MSG_STRGERR_CANT_MIX_EXCLUSIVE_ARMOR_TYPE";
    case proto::ItemStatus::NotEnoughGold: return "UIIT_MSG_INTERACTION_FAIL_NOT_ENOUGH_MONEY";
    case proto::ItemStatus::NotForSale: return "UIIT_MSG_STRGERR_DEALER_DONT_TREAT_THIS_ITEM";
    case proto::ItemStatus::GuildBuyDenied: return "UIIT_MSG_GUILD_STORE_BUY_LIMIT";
    case proto::ItemStatus::GuildPointsShort: return "UIIT_MSG_GUILD_LACK_GP";
    // The shops' other prices (codes 0x18D4, 0x184A, 0x184D; their texts by their names: the error table's entries
    // for them were not read).
    case proto::ItemStatus::HonorShort: return "UIIT_MSG_TC_LACK_HONOR_POINT";
    case proto::ItemStatus::TokensShort: return "UIIT_MSG_NOT_ENOUGH_COIN";
    case proto::ItemStatus::CoinsShort: return "UIIT_MGS_ARENA_NOT_ENOUGH_COIN";
    case proto::ItemStatus::TooFar: return "UIIT_MSG_INTERACTION_FAIL_TOO_FAR";
    case proto::ItemStatus::NotYours: return "UIIT_MSG_ITEM_GET_FAIL"; // "Cannot loot the item"
    case proto::ItemStatus::Exchanging: return "UIIT_MSG_STRGERR_CANT_USEITEM_WHILE_INTERACT"; // 0x1835
    case proto::ItemStatus::PlaceTaken: return "UIIT_MSG_STRGERR_EQUIPITEM";                    // an avatar of that kind worn (0x180E)
    case proto::ItemStatus::NeedsDress: return "UIIT_STT_AVATAR_MAIL_ONLY_WEAR";
    case proto::ItemStatus::NoItem:
    case proto::ItemStatus::BadSlot: return "UIIT_MSG_STRGERR_CANT_BE_STORED";
    default: return nullptr;
    }
}

// A job suit's refusal by the original's code (its error table's group 1);
// null: no text (the change's pending 0x189B among them). `banner`: also in the warning banner.
const char* JobSuitKey(std::uint16_t code, bool* banner = nullptr) {
    const auto with = [&](const char* key, bool shown) {
        if (banner != nullptr) *banner = shown;
        return key;
    };
    switch (code) {
    case 0x18CD: return with("UIIT_MSG_WARENETWORK_ERROR_TRIANGLE", false);
    case 0x18CA: return with("UIIT_MSG_JOBITEM_WEAR_ERR_RIDE", false);
    case 0x1847: return with("UIIT_MSG_STRGERR_WAIT_FOR_JOBCHANGE_COMPLETE", true);
    case 0x18A3: return with("UIIT_MSG_JOBITEM_WEAR_ERR_WEARLIMIT", false);
    case 0x189E: return with("UIIT_MSG_JOBITEM_WEAR_ERR_NOJOB", false);
    case 0x189F: return with("UIIT_MSG_JOBITEM_WEAR_ERR_NOALIAS", false);
    case 0x18A2: return with("UIIT_MSG_JOBITEM_WEAR_ERR_NOPOSITION", false);
    case 0x18A0: return with("UIIT_MSG_JOBITEM_WEAR_ERR_PARTY", false);
    case 0x1816: return with("UIIT_MSG_STRGERR_GENDER_MISMATCH", true);
    case 0x182F: return with("UIIT_MSG_STRGERR_COUNTRY_MISMATCH", true);
    case 0x1810: return with("UIIT_MSG_STRGERR_HIGHER_LEVEL_REQUIRED", true);
    case 0x1846: return with("UIIT_MSG_STRGERR_CANT_SWAP_JOBITEM", true);
    case 0x1848: return with("UIIT_MSG_STRGERR_CANT_DETACH_JOBITEM_IN_BATTLESTATE", true);
    case 0x185A: return with("UIIT_MSG_STRGERR_CANT_HAVE_CART", false);
    // Specialty goods on the ground.
    case 0x1861: return with("UIIT_MSG_STRGERR_DONT_HAVE_ANY_TRADECART_TO_PICK_SPECIALTY", false);
    case 0x1863:
    case 0x1864: return with("UIIT_MSG_STRGERR_YOU_ARENT_BUYER_OF_THIS_SPECIALTY", false); // 0x1864's own text not found (inferred)
    case 0x186F: return with("UIIT_MSG_STRGERR_ROBBER_CANT_PICK_HIS_ITEM", false);
    case 0x1807: return with("UIIT_MSG_STRGERR_INVENTORY_FULL", false);
    default: return nullptr;
    }
}

} // namespace

// An item's slot: its icon, a stackable's count (shown even at 1), red when it cannot be used.
void ShowItem(ui::Slot& slot, const game::ItemInstance* item, const HudContext& context) {
    const assets::ItemRow* row = item != nullptr && context.items != nullptr ? context.items->Find(item->refId) : nullptr;
    if (row == nullptr) {
        slot.icon.clear();
        slot.count = 0;
        slot.usable = true;
        slot.rare = false;
        slot.petOut = false;
        slot.veil = 0;
        return;
    }
    slot.icon = row->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row->iconPath;
    slot.rare = row->Rare();
    // A pet's item whose pet is out: its edge runs (CosPetItems); whose pet is dead (an attack pet killed, a grab
    // pet's rental over: state 4) the blue veil (0x80004B7E).
    slot.petOut = false;
    slot.veil = 0;
    if (game::IsCosSummoner(*row) && item->durability != 0) {
        const auto pet = context.petItems.find(std::int64_t(item->durability));
        slot.petOut = pet != context.petItems.end() && pet->second.state == std::uint8_t(game::CosState::Summoned);
        if (pet != context.petItems.end() && pet->second.state == std::uint8_t(game::CosState::Dead)) slot.veil = ui::Argb(0x80004B7E);
    }
    // The tooltip's sign by the item's kind (inferred: the variants exist beside the plain one).
    slot.tooltipSign = row->Rare()               ? "interface\\ifcommon\\com_itemsign_rare.ddj"
                       : !item->options.empty() && !game::pop::IsCoupon(*row) ? "interface\\ifcommon\\com_itemsign_magic.ddj"
                       : item->plus > 0         ? "interface\\ifcommon\\com_itemsign_plus.ddj"
                                                : "interface\\ifcommon\\com_itemsign.ddj";
    slot.countAlways = game::KindOf(*row) == ItemKind::Expendable;
    slot.count = slot.countAlways ? item->count : 0;
    const int place = game::EquipPlaceFor(*row, false, false);
    const game::EquipCheck check =
        place < 0 ? game::EquipCheck::Ok : game::CanEquip(*row, place == game::slot::kRingLeft ? place : place, context.wearer, *item);
    slot.usable = check == game::EquipCheck::Ok || check == game::EquipCheck::WrongSlot;
    // Clothes beside worn armour, or armour beside worn clothes (bit 0x80: each of places 0..5 worn,
    // its own place too): red, as the move would be refused (UIIT_MSG_STRGERR_CANT_MIX_EXCLUSIVE_ARMOR_TYPE).
    if (const int family = game::ArmourFamily(*row); family != 0 && slot.usable && context.state != nullptr && context.items != nullptr) {
        for (int worn = game::slot::kHead; worn <= game::slot::kFeet; ++worn) {
            const game::ItemInstance* on = context.state->inventory.At(std::uint8_t(worn));
            if (on == nullptr || on == item) continue;
            const assets::ItemRow* onRow = context.items->Find(on->refId);
            const int other = onRow != nullptr ? game::ArmourFamily(*onRow) : 0;
            if (other != 0 && other != family) {
                slot.usable = false;
                break;
            }
        }
    }
    // What is not worn (a potion, a scroll) by its level alone.
    for (std::size_t i = 0; place < 0 && i < 4; ++i) {
        if (row->requirementTypes[i] == 1 && context.wearer.level < row->requirementLevels[i]) slot.usable = false;
    }
}

// A description: plain text as it is; SML2 markup ("<sml2>...</sml2>") cut at its line breaks, each
// line in the colour its text starts in, bold inside <strong> (the original lays the markup out as
// rich text in the bubble; ours keeps its lines and colours).
void AddTooltipDescription(std::vector<ui::TooltipLine>& out, std::string_view text) {
    if (!text.starts_with("<sml2>")) {
        out.push_back({std::string(text), kWhite, kShadow});
        return;
    }
    std::vector<ui::Color> colours{kWhite};
    int bold = 0;
    std::string line;
    ui::Color lineColour = kWhite;
    int lineStyle = kShadow;
    const auto finish = [&] {
        out.push_back({std::exchange(line, {}), lineColour, lineStyle});
        lineColour = colours.back();
        lineStyle = bold > 0 ? kBoldShadow : kShadow;
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '<') {
            if (line.empty()) {
                lineColour = colours.back();
                lineStyle = bold > 0 ? kBoldShadow : kShadow;
            }
            line += text[i++];
            continue;
        }
        const std::size_t end = text.find('>', i);
        if (end == std::string_view::npos) break;
        const std::string_view tag = text.substr(i + 1, end - i - 1);
        i = end + 1;
        if (tag == "br" || tag == "br/" || tag == "/p") {
            finish();
        } else if (tag == "strong" || tag == "b") {
            ++bold;
        } else if ((tag == "/strong" || tag == "/b") && bold > 0) {
            --bold;
        } else if (tag.starts_with("font")) {
            ui::Color c = colours.back();
            if (const std::size_t q = tag.find("color=\""); q != std::string_view::npos) {
                int v[4] = {255, 255, 255, 255};
                std::sscanf(std::string(tag.substr(q + 7)).c_str(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]);
                c = ui::Rgba(std::uint8_t(v[1]), std::uint8_t(v[2]), std::uint8_t(v[3]), std::uint8_t(v[0]));
            }
            colours.push_back(c);
        } else if (tag == "/font" && colours.size() > 1) {
            colours.pop_back();
        }
    }
    if (!line.empty()) finish();
}

namespace {

// The requirement lines, white when met, red when not: the level, a mastery
// ("Mastery: Bicheon Mastery level 20"; white: the masteries are not in the player's state yet),
// the sex, the race.
std::vector<ui::TooltipLine> Requirements(const assets::ItemRow& row, const HudContext& c) {
    std::vector<ui::TooltipLine> needs;
    for (std::size_t i = 0; i < 4; ++i) {
        const int type = row.requirementTypes[i], value = row.requirementLevels[i];
        if (type == 1 && value > 0) {
            needs.push_back({c.Text("PARAM_REQ_LV") + " " + std::to_string(value), c.wearer.level >= value ? kWhite : kRed, kShadow});
        } else if (type > 1 && value > 0 && c.skills != nullptr) {
            if (const assets::MasteryRow* m = c.skills->Mastery(std::uint32_t(type))) {
                needs.push_back({c.Text("PARAM_MASTERY") + ": " + c.Text(m->nameKey) + " " + c.Text("PARAM_MASTERY_LEVEL") + " " + std::to_string(value),
                                 kWhite, kShadow});
            }
        }
    }
    if (row.sex == 0 || row.sex == 1) {
        needs.push_back({c.Text(row.sex == 1 ? "UIO_NEWCHAR_CTL_MALE" : "UIO_NEWCHAR_CTL_FEMALE"), (row.sex == 1) == c.wearer.male ? kWhite : kRed, kShadow});
    }
    if (row.country == 0 || row.country == 1) {
        needs.push_back({c.Text(row.country == 0 ? "UIO_NEWCHAR_CTL_CHINESE" : "UIO_NEWCHAR_CTL_EUROPEAN"), row.country == c.wearer.country ? kWhite : kRed,
                         kShadow});
    }
    return needs;
}

// "Reuse time 30 Second" from a wait in ms. The original prints "%s %d%s" with the texts as they are
// ("Reuse time 30Second"); ours puts a space before the unit.
std::string Seconds(const HudContext& c, const char* key, int ms) {
    return c.Text(key) + " " + std::to_string(ms / 1000) + " " + c.Text("PARAM_SECOND");
}

// What an expendable does, by its type (potions and cures, scrolls, the speed scrolls' skill line). Spaces between
// the texts
// the original glues together ("HP recovery[120]") are ours.
std::vector<ui::TooltipLine> ExpendableLines(const assets::ItemRow& row, const HudContext& c) {
    std::vector<ui::TooltipLine> out;
    const auto line = [&](std::string text, ui::Color colour = kWhite) { out.push_back({std::move(text), colour, kShadow}); };
    const auto has = [&](const char* key) { return !c.hasText || c.hasText(key); };
    const int type3 = row.typeId[2], type4 = row.typeId[3];
    const auto& p = row.params;
    if (type3 == 1) { // potions
        if (type4 == 9) {
            if (p[0] > 0) line(c.Text("PARAM_HEAL_HGP") + " [" + std::to_string(p[0]) + "%]");
        } else if (type4 == 10) {
            if (row.cooldownMs > 0) line(Seconds(c, "UIIT_STT_ITEM_FORMAT_COOLTIME", row.cooldownMs));
        } else if (type4 != 6 && type4 != 8) {
            if (p[0] > 0) line(c.Text("PARAM_HEAL_HP") + " [" + std::to_string(p[0]) + "]");
            if (p[1] > 0) line(c.Text("PARAM_HEAL_HP") + " [" + std::to_string(p[1]) + "]%");
            if (p[2] > 0) line(c.Text("PARAM_HEAL_MP") + " [" + std::to_string(p[2]) + "]");
            if (p[3] > 0) line(c.Text("PARAM_HEAL_MP") + " [" + std::to_string(p[3]) + "]%");
        }
    } else if (type3 == 2) { // cures
        if (type4 == 1) {
            // Param1 holds the states it cures as bits, Param2 the level: one line per series.
            const std::pair<std::uint32_t, const char*> kSeries[] = {
                {0x1E2800u, "PARAM_WEAKLY"}, {0x1C0u, "PARAM_RESTRICTION"}, {0x1E18600u, "PARAM_CURSING"}};
            for (const auto& [bits, key] : kSeries) {
                if ((std::uint32_t(p[0]) & bits) != 0) {
                    line(c.Text(key) + " " + c.Text("PARAM_CURE") + " " + std::to_string(p[1]) + " " + c.Text("UIIT_STT_GRADE"));
                }
            }
        } else if (p[0] > 0 && p[1] > 0 && p[2] > 0 && p[3] > 0 && p[4] > 0 && p[5] > 0) {
            line(c.Text("PARAM_CURE_STATE") + " " + std::to_string(p[0]));
        } else {
            constexpr const char* kStates[] = {"PARAM_CURE_FROZEN_LV", "PARAM_CURE_FROSTBITE_LV", "PARAM_CURE_BURN_LV",
                                               "PARAM_CURE_ESHOCK_LV", "PARAM_CURE_POISON_LV",   "PARAM_CURE_ZOMBIE_LV"};
            // v1.150 has no English for these (ours: the line is left out instead of its key).
            for (std::size_t i = 0; i < 6; ++i) {
                if (p[i] > 0 && has(kStates[i])) line(c.Text(kStates[i]) + " " + std::to_string(p[i]));
            }
        }
    } else if (type3 == 3) { // scrolls: the return scroll's wait (summon scrolls come with the pets)
        if (type4 == 1 && p[0] > 0) line(Seconds(c, "PARAM_RECALL_TIME", p[0]));
    } else if (type3 == 11 && type4 == 8) {
        // A socket stone (its tooltip): the part it goes on (its Param2: 2 the chest,
        // 1 the weapon), each level's skill as "Lv%d : <what it does>", then that the level is rolled. The part's keys
        // are ours (the tooltip's "Mounting part"; "Chest" and "Weapon" from the client's texts).
        line(c.Text("UIIT_STT_ARMOR_POSITION") + " : " + c.Text(p[1] == 2 ? "UIIT_STT_ARMOR_POSITION_BREAST" : "SN_TAB_WEAPON"), kKindBeige);
        if (c.magicOptGroups != nullptr && c.skills != nullptr) {
            for (const assets::MagicOptGroupRow* g : game::alchemy::StoneSkills(*c.magicOptGroups, row)) {
                const assets::SkillRow* skill = c.skills->Find(g->optId);
                if (skill == nullptr) continue;
                std::string effects;
                for (const std::string& e : SkillEffectTexts(*skill, c)) {
                    if (e.empty()) continue;
                    effects += (effects.empty() ? "" : ", ") + e;
                }
                line(c.Text("UIIT_STT_LEVEL_LV") + std::to_string(g->optLevel) + " : " + effects);
            }
        }
        line(c.Text("UIIT_STT_SOCKET_SELECT_LEVEL"));
    } else if (type3 == 13 && type4 >= 1 && type4 <= 3 && c.skills != nullptr) {
        // The skill the scroll casts (Param1's text names it), its effect in the kind colour (the
        // original's skill text builder; ours: the moving speed only, the speed scrolls').
        if (const assets::SkillRow* skill = c.skills->Find(std::string_view(row.paramTexts[0])); skill && skill->speedPercent > 0) {
            line(c.Text("PARAM_HSTE") + " " + std::to_string(skill->speedPercent) + "% " + c.Text("PARAM_INCREASE"), kKindBeige);
        }
    }
    return out;
}
} // namespace

// --- The tooltip ---------------------------------------------------------------------------------

namespace {
std::vector<ui::TooltipLine> ItemTooltipBody(const assets::ItemRow& row, const game::ItemInstance& item, const HudContext& c);
} // namespace

std::vector<ui::TooltipLine> ItemTooltip(const assets::ItemRow& row, const game::ItemInstance& item, const HudContext& c) {
    // A Magic POP coupon's params are its prize, no options: its body without them, then "Can be changed to
    // [%s][%d].".
    if (game::pop::IsCoupon(row)) {
        game::ItemInstance plain = item;
        plain.options.clear();
        std::vector<ui::TooltipLine> out = ItemTooltipBody(row, plain, c);
        for (const std::string& line : MagicPopCouponLine(c, row, item)) out.push_back({line, kWhite, kShadow});
        return out;
    }
    std::vector<ui::TooltipLine> out = ItemTooltipBody(row, item, c);
    // A pet's summoning item: its pet's lines after the item's.
    if (game::IsCosSummoner(row)) {
        const auto pet = item.durability != 0 ? c.petItems.find(std::int64_t(item.durability)) : c.petItems.end();
        const bool known = pet != c.petItems.end();
        const bool dead = known && pet->second.state == std::uint8_t(game::CosState::Dead);
        out.push_back({{}, kWhite, kShadow});
        if (game::PetOfItem(row) == game::CosKind::GrabPet) {
            // A grab pet's rental: from its first summon on; then the time left; over once it is dead.
            if (!known && item.durability == 0) {
                out.push_back({c.Text("UIIT_STT_COSNEWUI_ITEM_BEFORESUM"), kWhite, kShadow});
            } else if (dead) {
                out.push_back({c.Text("UIIT_STT_COSNEWUI_ITEM_EXPIRERENT"), kRed, kShadow});
            } else if (known) {
                const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                const std::int64_t left = std::max<std::int64_t>(0, pet->second.rentUntil - std::int64_t(now));
                out.push_back({c.Text("UIIT_STT_COSNEWUI_RENTTIME"), kKindBeige, kShadow});
                out.push_back({std::to_string(left / 86400) + c.Text("PARAM_DAY") + " " + std::to_string(left % 86400 / 3600) + c.Text("PARAM_HOUR") + " " +
                                   std::to_string(left % 3600 / 60) + c.Text("PARAM_MINUTE"),
                               kWhite, kShadow});
            }
        } else if (known) {
            // An attack pet's: its name ("No name" when it has none), its level (red above the player's), its
            // condition (Normal, or Dead in red).
            out.push_back({c.Text("UIIT_STT_COSNEWUI_TOOLTIP_PETINFO"), kKindBeige, kShadow});
            out.push_back({c.Text("UIIT_STT_COSNEWUI_TOOLTIP_PETNAME") + " : " + (pet->second.name.empty() ? c.Text("UIIT_STT_COSNEWUI_TITLE") : pet->second.name),
                           kWhite, kShadow});
            out.push_back({c.Text("UIIT_STT_COSNEWUI_TOOLTIP_PETLEVEL") + " : " + std::to_string(pet->second.level),
                           pet->second.level > c.wearer.level ? kRed : kWhite, kShadow});
            out.push_back({c.Text("UIIT_STT_COSNEWUI_TOOLTIP_PETSTATE") + " : " +
                               c.Text(dead ? "UIIT_STT_COSNEWUI_TOOLTIP_PETSTATE_DEAD" : "UIIT_STT_COSNEWUI_TOOLTIP_PETSTATE_NORMAL"),
                           dead ? kRed : kWhite, kShadow});
        }
        if (out.back().text.empty()) out.pop_back(); // nothing known of it yet
    }
    // While a store is open: what the merchant pays for it, "Price %d".
    if (c.shopPrices) {
        out.push_back({{}, kWhite, kShadow});
        out.push_back({c.Text("UIIT_STT_PRICE") + " " + std::to_string(row.sellPrice * std::max<std::uint64_t>(1, item.count)), kWhite, kShadow});
    }
    return out;
}

namespace {
std::vector<ui::TooltipLine> ItemTooltipBody(const assets::ItemRow& row, const game::ItemInstance& item, const HudContext& c) {
    std::vector<ui::TooltipLine> out;
    const auto line = [&](std::string text, ui::Color colour = kWhite, int style = kShadow) { out.push_back({std::move(text), colour, style}); };
    const auto blank = [&] { out.push_back({{}, 0, kShadow}); };
    const ItemKind kind = game::KindOf(row);
    const game::ItemStats s = game::ComputeStats(row, item, c.magicOptions);
    const bool rare = row.Rare();
    bool repaired = false;
    if (c.magicOptions != nullptr) {
        for (const auto& o : item.options) {
            if (const auto* m = game::FindOption(*c.magicOptions, o.id); m && m->optionName == "MATTR_REPAIR") repaired = true;
        }
    }
    // The name (with its plus) and its colour: rare gold (green when repaired), magic blue, else white.
    const ui::Color nameColour = rare ? (repaired ? kRepairedGreen : kRareGold) : !item.options.empty() ? kMagicName : kWhite;
    std::string name = c.Text(row.nameKey);
    if (game::TotalPlus(item) > 0) name += " (+" + std::to_string(game::TotalPlus(item)) + ")"; // with an advanced elixir's
    line(name, nameColour, kBoldShadow);
    blank();
    const auto description = [&] {
        if (!row.descKey.empty() && c.hasText && c.hasText(row.descKey) && !c.Text(row.descKey).empty()) {
            AddTooltipDescription(out, c.Text(row.descKey));
            blank();
        }
    };
    // Expendables: the description, what it does, the requirements, then how
    // many (the stack, and the most one may carry).
    if (kind == ItemKind::Expendable) {
        description();
        if (auto lines = ExpendableLines(row, c); !lines.empty()) {
            out.insert(out.end(), lines.begin(), lines.end());
            blank();
        }
        if (auto needs = Requirements(row, c); !needs.empty()) {
            out.insert(out.end(), needs.begin(), needs.end());
            blank();
        }
        line(c.Text("UIIT_STT_AMOUNT") + " " + std::to_string(item.count));
        if (row.maxPossess >= 0) line(c.Text("PARAM_MAX_CONTAIN") + " " + std::to_string(row.maxPossess));
        return out;
    }
    // Avatars and job suits: the description, an avatar's sort, the
    // most blues, the requirements, the blues (their text without the "(+X%)").
    if (kind == ItemKind::Avatar || kind == ItemKind::JobSuit) {
        description();
        if (kind == ItemKind::Avatar && row.typeId[2] == 13) { // not the Devil's Spirit (3/1/14): its TID4 names no sort
            constexpr const char* kSorts[] = {nullptr, "UIIT_STT_SILKMALL_HAT", "UIIT_CTL_DRESS", "UIIT_CTL_WK_AVATAR_ATTACH", "UIIT_CTL_WK_ETC"};
            const int sort = row.typeId[3];
            if (sort >= 1 && sort <= 4) line(c.Text("UIIT_STT_ITEM_TYPE") + ": " + c.Text(kSorts[sort]), kKindBeige);
        }
        line(c.Text("UIIT_STT_AVATAR_MAGICOPTION_MAXCOUNT") + ": " + std::to_string(row.maxMagicOptions) + c.Text("UIIT_STT_UNIT"), kKindBeige);
        if (auto needs = Requirements(row, c); !needs.empty()) {
            blank();
            out.insert(out.end(), needs.begin(), needs.end());
        }
        if (!item.options.empty() && c.magicOptions != nullptr) {
            blank();
            // In option-id order, each as 1.188 writes it (MagicOptionLine).
            std::vector<game::ItemInstance::Option> sorted = item.options;
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            for (const auto& o : sorted) {
                const MagicOptionText t = MagicOptionLine(c, row, o);
                if (!t.text.empty()) line(t.text, t.negative ? kRed : kBlue, kBoldShadow);
            }
        }
        return out;
    }
    // The seal of a rare item after the blank line, above the kind, in the name's colour (as the
    // original's screenshots show it; the v1.150 client put it after the
    // requirements: the user's call, 2026-10-12).
    if (rare && s.seal >= 0) {
        constexpr const char* kSeals[] = {"PARAM_RARE_FIRST", "PARAM_RARE_SECOND", "PARAM_RARE_THIRD"};
        line(c.Text(kSeals[s.seal]), nameColour, kBoldShadow);
    }
    const bool equipment = kind == ItemKind::Weapon || kind == ItemKind::Shield || kind == ItemKind::Armour || kind == ItemKind::Accessory;
    if (equipment) {
        const int part = row.typeId[3];
        if (kind == ItemKind::Weapon) {
            constexpr const char* kWeapons[] = {nullptr, nullptr, "PARAM_WEAPON_SWORD", "PARAM_WEAPON_BLADE", "PARAM_WEAPON_SPEAR",
                                                "PARAM_WEAPON_TBLADE", "PARAM_WEAPON_BOW", "UIO_NEWCHAR_STT_EU_ONEHANDSWORD",
                                                "UIO_NEWCHAR_STT_EU_TWOHANDSWORD", "UIO_NEWCHAR_STT_EU_DUELAXE", "UIO_NEWCHAR_STT_EU_DARKSTAFF",
                                                "UIO_NEWCHAR_STT_EU_TWOHANDSTAFF", "UIO_NEWCHAR_STT_EU_CROSSBOW", "UIO_NEWCHAR_STT_EU_DAGGER",
                                                "UIO_NEWCHAR_STT_EU_HARP", "UIO_NEWCHAR_STT_EU_ONEHANDSTAFF", "UIIT_STT_FORT_ETC_SIEGEWEAPON"};
            if (part >= 2 && part <= 16) line(c.Text("UIIT_STT_WEAPON_TYPE") + ": " + c.Text(kWeapons[part]), kKindBeige);
        } else if (kind == ItemKind::Shield) {
            line(c.Text("UIIT_STT_SHIELD"), kKindBeige);
        } else if (kind == ItemKind::Armour) {
            const int cls = row.typeId[2];
            const char* type = cls == 1   ? "UIO_NEWCHAR_STT_CLOTHES"
                               : cls == 2 ? "UIO_NEWCHAR_STT_LIGHT_ARMOR"
                               : cls == 3 ? "UIO_NEWCHAR_STT_HEAVY_ARMOR"
                               : cls == 9 ? "UIO_NEWCHAR_STT_EU_ROBE"
                               : cls == 10 ? "UIO_NEWCHAR_STT_EU_LIGHT_ARMOR"
                                           : "UIO_NEWCHAR_STT_EU_HEAVY_ARMOR";
            constexpr const char* kParts[] = {nullptr,
                                              "UIIT_STT_ARMOR_POSITION_HEAD",
                                              "UIIT_STT_ARMOR_POSITION_SHOULDER",
                                              "UIIT_STT_ARMOR_POSITION_BREAST",
                                              "UIIT_STT_ARMOR_POSITION_LEG",
                                              "UIIT_STT_ARMOR_POSITION_HAND",
                                              "UIIT_STT_ARMOR_POSITION_FOOT"};
            line(c.Text("UIIT_STT_ARMOR_TYPE") + ": " + c.Text(type), kKindBeige);
            if (part >= 1 && part <= 6) line(c.Text("UIIT_STT_ARMOR_POSITION") + ": " + c.Text(kParts[part]), kKindBeige);
        } else {
            constexpr const char* kAccessories[] = {nullptr, "UIIT_STT_ACCESSARY_EARRING", "UIIT_STT_ACCESSARY_NECKLACE", "UIIT_STT_ACCESSARY_RING"};
            if (part >= 1 && part <= 3) line(c.Text("UIIT_STT_ACCESSARY_TYPE") + ": " + c.Text(kAccessories[part]), kKindBeige);
        }
        if (s.degree > 0) line(FormatText(c.Text("UIIT_TOOLTIP_EQUIPMENT_CLASS"), {std::to_string(s.degree)}), kKindBeige);
        line(c.Text("UIIT_STT_AVATAR_MAGICOPTION_MAXCOUNT") + ": " + std::to_string(row.maxMagicOptions) + c.Text("UIIT_STT_UNIT"), kKindBeige);
        blank();
    }
    // The description, when it has one.
    description();
    if (equipment) {
        // The white stats, each when the item has it.
        const auto range = [&](const char* key, const game::ItemStat& st) {
            if (st.shown) line(c.Text(key) + Printf(" %.0f ~ %.0f", st.low, st.high) + " (+" + std::to_string(st.percent) + "%)");
        };
        const auto real = [&](const char* key, const game::ItemStat& st) {
            if (st.shown) line(c.Text(key) + Printf(" %.1f", st.low) + " (+" + std::to_string(st.percent) + "%)");
        };
        const auto whole = [&](const char* key, const game::ItemStat& st) {
            if (st.shown) line(c.Text(key) + Printf(" %.0f", st.low) + " (+" + std::to_string(st.percent) + "%)");
        };
        range("PARAM_PA", s.phyAttack);
        range("PARAM_MA", s.magAttack);
        real("PARAM_PD", s.phyDefence);
        real("PARAM_MD", s.magDefence);
        real("PARAM_PR", s.phyAbsorption);
        real("PARAM_MR", s.magAbsorption);
        if (s.durability.shown) {
            line(c.Text("PARAM_DUR") + Printf(" %.0f/%.0f", s.durability.low, s.durability.high) + " (+" + std::to_string(s.durability.percent) + "%)",
                 s.durability.low <= 0 ? kRed : kWhite);
        }
        if (s.range > 0) line(c.Text("PARAM_RANGE") + Printf(" %.1f m", s.range));
        whole("PARAM_HR", s.attackRating);
        whole("PARAM_ER", s.parry);
        whole("PARAM_CRITICAL", s.critical);
        whole("PARAM_BLOCKING", s.blocking);
        for (const auto& [key, st] : {std::pair{"PARAM_PHYSICAL_SPECIALIZE", s.phyReinforce}, std::pair{"PARAM_MAGICAL_SPECIALIZE", s.magReinforce}}) {
            if (!st.shown) continue;
            if (kind == ItemKind::Weapon) {
                line(c.Text(key) + Printf(" %.1f %% ~ %.1f %%", st.low, st.high) + " (+" + std::to_string(st.percent) + "%)");
            } else {
                line(c.Text(key) + Printf(" %.1f %%", st.low) + " (+" + std::to_string(st.percent) + "%)");
            }
        }
    }
    // The requirements, red when not met.
    std::vector<ui::TooltipLine> needs = Requirements(row, c);
    if (!needs.empty()) {
        blank();
        out.insert(out.end(), needs.begin(), needs.end());
    }
    // The blues, by option id; negative ones in red.
    if (!item.options.empty() && c.magicOptions != nullptr) {
        blank();
        std::vector<game::ItemInstance::Option> sorted = item.options;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return (a.id & 0xFFFF) < (b.id & 0xFFFF); });
        // Each as 1.188 writes it (MagicOptionLine): its keys and format, "(+X%)" by the row at the
        // item's degree, red for a penalty; an option with no row at the item's degree gets no line.
        for (const auto& o : sorted) {
            const MagicOptionText t = MagicOptionLine(c, row, o);
            if (!t.text.empty()) line(t.text, t.negative ? kRed : kBlue, kBoldShadow);
        }
    }
    // The advanced elixir's line on every weapon, armour, shield and accessory: free to use, grey;
    // in effect with its plus, white.
    if (const ItemKind kind = game::KindOf(row); kind == ItemKind::Weapon || kind == ItemKind::Armour || kind == ItemKind::Shield || kind == ItemKind::Accessory) {
        if (item.advanced == 0) line(c.Text("UIIT_STT_SOCKET_TIP_UPPER_REINFOREC_UNUSE"), ui::Argb(0xFFC8C8C8));
        else line(FormatText(c.Text("UIIT_STT_SOCKET_TIP_UPPER_REINFOREC_USED"), {std::to_string(item.advanced)}), kWhite);
    }
    // Its sockets (writes them in the tooltip's second box, ours after the other
    // lines): each made one "Empty slot" or "<stone> Lv<level>", white bold, then where to see their skills.
    if (game::alchemy::MadeSockets(item) > 0) {
        blank();
        for (const game::ItemSocket& socket : item.sockets) {
            if (!socket.made) continue;
            const assets::ItemRow* stone = socket.Filled() && c.items != nullptr ? c.items->Find(socket.stone) : nullptr;
            if (stone != nullptr) {
                line(c.Text(stone->nameKey) + " " + c.Text("UIIT_STT_LEVEL_LV") + std::to_string(socket.level), kWhite, kBoldShadow);
            } else {
                line(c.Text("UIIT_STT_SOCKET_EMPTY_SLOT"), kWhite, kBoldShadow);
            }
        }
        line(c.Text("UIIT_STT_SOCKET_HELP_TIP"), kWhite);
    }
    return out;
}

} // namespace

std::string GoldText(std::uint64_t gold) {
    std::string digits = std::to_string(gold), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

ui::Color GoldColor(std::uint64_t gold) {
    // Observed:.
    constexpr std::pair<std::uint64_t, std::uint32_t> kSteps[] = {
        {10'000ull, 0xFFFFFFFF},          {100'000ull, 0xFFFFFA85},        {1'000'000ull, 0xFFFFD348},
        {10'000'000ull, 0xFFFFAD5C},      {100'000'000ull, 0xFFFF9AA1},    {1'000'000'000ull, 0xFFEBA1FF},
        {10'000'000'000ull, 0xFFB8BBFF},  {100'000'000'000ull, 0xFF95DEFF}};
    for (const auto& [below, colour] : kSteps) {
        if (gold < below) return ui::Argb(colour);
    }
    return ui::Argb(0xFF8BFFE5);
}

// --- The inventory -------------------------------------------------------------------------------

void InventoryWindow::Created(ui::Interface& ui) {
    ui_ = &ui;
    // An item let go over the world (no window under the mouse): from the bag, dropped after asking;
    // a worn one is refused. The main window (both pages' home) hears of it.
    if (ui::Widget* home = Root(); home != nullptr && home != this) {
        auto previous = home->onPickEnd;
        home->onPickEnd = [this, previous](ui::Widget* source, ui::Widget* taker) {
            if (previous) previous(source, taker);
            if (taker != nullptr || ui_ == nullptr || ui_->under() != nullptr || context_.state == nullptr || context_.hud == nullptr) return;
            if (AvatarPlaceOf(source) >= 0) { // an avatar worn: as a worn item (inferred)
                context_.state->Post(game::GameMessage::Kind::General, context_.Text("UIIT_MSG_STRGERR_CANT_DROP_EQUIPED_ITEM_DIRECTLY"), context_.time);
                return;
            }
            const int place = PlaceOf(source);
            if (place < 0 || context_.state->inventory.At(std::uint8_t(place)) == nullptr) return;
            if (place < game::slot::kWorn) {
                context_.state->Post(game::GameMessage::Kind::General, context_.Text("UIIT_MSG_STRGERR_CANT_DROP_EQUIPED_ITEM_DIRECTLY"), context_.time);
                return;
            }
            context_.hud->Request({HudRequest::Kind::DropItem, place});
        };
    }
    // The gold right-aligned in its strip.
    if (auto* gold = ui::As<ui::Static>(Child(10))) gold->textStyle |= ui::kTextShadow;
    // An item dropped on a place moves there; one dropped on the window's background goes nowhere.
    onDrop = [this](ui::Widget* source, ui::Widget* target) {
        const int from = PlaceOf(source), to = PlaceOf(target);
        if (to < 13 || dynamic_cast<ui::Slot*>(target) == nullptr) return false;
        if (context_.hud != nullptr && context_.items != nullptr && context_.state != nullptr) {
            // A good of the store: the buy box (kind 15; the place does not matter).
            int tab = 0;
            if (const assets::ShopGood* good = StoreWindow::GoodOf(source, &tab)) {
                if (const assets::ItemRow* row = context_.items->Find(std::string_view(good->itemCodename))) {
                    const bool stacks = game::MaxStack(*row) > 1;
                    DealBox::Deal deal;
                    deal.kind = DealBox::Deal::Kind::Buy;
                    deal.icon = row->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row->iconPath;
                    deal.name = context_.Text(row->nameKey);
                    deal.unitPrice = context_.TaxedBuy(good->price); // with the fortress tax
                    deal.count = deal.most = stacks ? game::MaxStack(*row) : 1;
                    if (!stacks) deal.most = 5;
                    deal.tab = tab;
                    deal.slot = good->slot;
                    context_.hud->Deal(deal);
                }
                return true;
            }
            // A sold item: the buy-back box.
            if (const int sold = StoreWindow::SoldOf(source); sold >= 0) {
                if (std::size_t(sold) < context_.state->buyback.size()) {
                    const proto::BuybackRecord& r = context_.state->buyback[std::size_t(sold)];
                    if (const assets::ItemRow* row = context_.items->Find(r.item.item)) {
                        DealBox::Deal deal;
                        deal.kind = DealBox::Deal::Kind::Buyback;
                        deal.icon = row->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row->iconPath;
                        deal.name = context_.Text(row->nameKey);
                        deal.unitPrice = r.price;
                        deal.count = deal.most = std::max<int>(1, r.item.quantity);
                        deal.index = sold;
                        context_.hud->Deal(deal);
                    }
                }
                return true;
            }
        }
        // An avatar taken to the bag: off, into the bag's first free place whatever place it was let go on (the original's
        // server picks it).
        if (const int avatar = AvatarPlaceOf(source); avatar >= 0) {
            if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::TakeOffAvatar, avatar});
            return true;
        }
        if (const int stored = StorageWindow::PlaceOf(source); stored >= 0) {
            // Out of the storage into this place.
            if (context_.hud != nullptr) context_.hud->Request({HudRequest::Kind::StorageMove, stored | to << 8 | 1 << 16});
            return true;
        }
        if (const int pet = CosBagPlaceOf(source); pet >= 0) {
            // Out of the picked pet's bag into this place (HudCos.cpp).
            if (context_.cosRequest) {
                context_.cosRequest(game::proto::CosBagMove::kId,
                                    game::proto::Encode(game::proto::CosBagMove{context_.cosSelected, std::uint8_t(pet), std::uint8_t(to), true, false}));
            }
            return true;
        }
        if (from < 0) return false;
        // A Clock of Reincarnation or a Grass of Life dropped on a pet's item: used on its pet (the 1.188 client's
        // target slot 0x46; the server checks the pet).
        if (context_.items != nullptr && context_.state != nullptr && context_.cosRequest && from >= 13) {
            const game::Inventory& inv = context_.state->inventory;
            const game::ItemInstance* tool = inv.At(std::uint8_t(from));
            const game::ItemInstance* pet = inv.At(std::uint8_t(to));
            const assets::ItemRow* toolRow = tool != nullptr ? context_.items->Find(tool->refId) : nullptr;
            const assets::ItemRow* petRow = pet != nullptr ? context_.items->Find(pet->refId) : nullptr;
            if (toolRow != nullptr && petRow != nullptr && game::PetTreatmentOf(*toolRow) != game::PetTreatment::None && game::IsCosSummoner(*petRow)) {
                context_.cosRequest(game::proto::CosUseOnPet::kId, game::proto::Encode(game::proto::CosUseOnPet{std::uint8_t(from), std::uint8_t(to)}));
                return true;
            }
        }
        // An elixir dropped on a magic cube of its kind: into the cube (HudServices.cpp).
        if (from >= 13 && CubeDrop(context_, from, to)) return true;
        RequestMove(context_, from, to);
        return true;
    };
    Build(ui, 32);
}

namespace {
// A tab's art: the first a full tab, the extensions short ones (1.188).
const char* TabArt(int tab, bool on) {
    if (tab == 0) return on ? "interface\\ifcommon\\com_tab_on.ddj" : "interface\\ifcommon\\com_tab_off.ddj";
    return on ? "interface\\ifcommon\\com_short_tab_on.ddj" : "interface\\ifcommon\\com_short_tab_off.ddj";
}
} // namespace

void InventoryWindow::Build(ui::Interface& ui, int places) {
    // Made in code: tabs of 32 places, three at most; the places beyond the size locked.
    places_ = places;
    for (ui::Slot* s : slots_) s->visible = false;
    for (ui::SelectableArea* t : tabs_) t->visible = false;
    const int tabs = std::clamp((std::max(places, 1) - 1) / 32 + 1, 1, 3);
    while (int(tabs_.size()) < tabs) {
        const int t = int(tabs_.size());
        assets::UiLayoutNode node;
        node.type = "CIFSelectableArea";
        node.id = 80 + t;
        // 1.188's places: the first a full tab, the extensions short ones.
        constexpr float kX[] = {4.0f, 64.0f, 118.0f}, kW[] = {60.0f, 56.0f, 57.0f};
        node.rect = {kX[t], -23.0f, kW[t], 24.0f};
        node.ddj = TabArt(t, false);
        node.hAlign = 1;
        node.vAlign = 1;
        auto tab = ui.Make(node);
        tab->SetText(context_.Text(t == 0 ? "UIIT_CTL_BELOINGING" : t == 1 ? "UIIT_STT_INVENTORY_EXTENSION_TEB" : "UIIT_STT_INVENTORY_EXTENSION_TEB2"));
        tabs_.push_back(ui::As<ui::SelectableArea>(Add(std::move(tab))));
    }
    while (int(slots_.size()) < tabs * 32) {
        const int k = int(slots_.size());
        assets::UiLayoutNode node;
        node.type = "CIFSlotWithHelp";
        node.id = 100 + k;
        node.rect = {18.0f + 36.0f * float(k % 4), 13.0f + 36.0f * float((k % 32) / 4), 32.0f, 32.0f};
        auto made = ui.Make(node);
        auto* slot = ui::As<ui::Slot>(Add(std::move(made)));
        slot->tooltipLines = [this, k] {
            const game::ItemInstance* item = context_.state ? context_.state->inventory.At(std::uint8_t(13 + k)) : nullptr;
            const assets::ItemRow* row = item && context_.items ? context_.items->Find(item->refId) : nullptr;
            std::vector<ui::TooltipLine> lines = row ? ItemTooltip(*row, *item, context_) : std::vector<ui::TooltipLine>{};
            // A magic cube's elixirs (ours: the original's tooltip for them is not traced).
            for (const std::string& line : CubeLines(context_, 13 + k)) lines.push_back({line, kWhite, kShadow});
            return lines;
        };
        slots_.push_back(slot);
    }
    for (int t = 0; t < tabs; ++t) tabs_[std::size_t(t)]->visible = true;
    SelectTab(std::min(tab_, tabs - 1));
}

void InventoryWindow::SelectTab(int tab) {
    tab_ = tab;
    for (std::size_t t = 0; t < tabs_.size(); ++t) {
        tabs_[t]->selected = int(t) == tab;
        tabs_[t]->texture = TabArt(int(t), int(t) == tab);
    }
    for (std::size_t k = 0; k < slots_.size(); ++k) slots_[k]->visible = int(k) / 32 == tab && int(k) < (int(tabs_.size()) * 32);
}

ui::Slot* InventoryWindow::SlotOf(int place) const {
    const int k = place - 13;
    return k >= 0 && k < int(slots_.size()) ? slots_[std::size_t(k)] : nullptr;
}

void InventoryWindow::Refresh() {
    const game::Inventory& inv = context_.state->inventory;
    for (std::size_t k = 0; k < slots_.size(); ++k) {
        ui::Slot& slot = *slots_[k];
        if (int(k) >= places_) {
            // Locked: beyond the inventory's size.
            slot.icon = "interface\\pet\\pt_block.ddj";
            slot.count = 0;
            slot.enabled = false;
            slot.pickable = false;
            continue;
        }
        slot.enabled = true;
        slot.pickable = !ServicePicking(context_); // a pick mode takes the click (HudServices.cpp)
        ShowItem(slot, inv.At(std::uint8_t(13 + k)), context_);
        // Offered in an exchange: held there, under a white veil, until it ends.
        slot.ghost = context_.exchange != nullptr && context_.exchange->Offered(int(13 + k));
        if (slot.ghost) slot.pickable = false;
    }
    if (auto* gold = ui::As<ui::Static>(Child(10))) {
        gold->SetText(GoldText(inv.gold));
        gold->textColor = GoldColor(inv.gold);
    }
}

void InventoryWindow::Update(ui::Frame&) {
    game::PlayerState* state = context_.state;
    if (state == nullptr || ui_ == nullptr) return;
    const int places = std::max(0, int(state->inventory.size) - 13);
    if (places != places_) {
        Build(*ui_, places);
        seen_ = ~0ull;
    }
    const std::uint64_t exchange = context_.exchange != nullptr ? context_.exchange->Revision() : 0;
    if (ServicePicking(context_) != picking_) {
        picking_ = ServicePicking(context_);
        seen_ = ~0ull;
    }
    if (state->inventoryVersion != seen_ || exchange != exchangeSeen_) {
        seen_ = state->inventoryVersion;
        exchangeSeen_ = exchange;
        Refresh();
    }
    // The waits of the items (a potion's clock), as on the quick slots; no time written here (the
    // underbar's slots only).
    for (std::size_t k = 0; k < slots_.size() && int(k) < places_; ++k) {
        const game::ItemInstance* item = state->inventory.At(std::uint8_t(13 + k));
        const HudContext::Cooling c = item != nullptr && context_.itemCooling ? context_.itemCooling(item->refId) : HudContext::Cooling{};
        slots_[k]->cooling = c.cooling;
        slots_[k]->charge = c.charge;
    }
    // The server's refusals, as system lines.
    for (std::size_t k = 0; k < state->itemRefusals.size(); ++k) {
        const proto::ItemStatus status = state->itemRefusals[k];
        const std::uint16_t code = k < state->itemRefusalCodes.size() ? state->itemRefusalCodes[k] : 0;
        bool banner = false;
        const char* key = status == proto::ItemStatus::JobSuit || status == proto::ItemStatus::JobChange ? JobSuitKey(code, &banner) : RefusalKey(status);
        if (key != nullptr && banner) state->Notify(game::Banner::Kind::Warning, context_.Text(key), context_.time);
        if (status == proto::ItemStatus::MixedArmour && context_.wearer.country == 1) key = "UIIT_MSG_STRGERR_EU_CANT_MIX_EXCLUSIVE_ARMOR_TYPE";
        if (key != nullptr) state->Post(game::GameMessage::Kind::General, context_.Text(key), context_.time);
    }
    state->itemRefusals.clear();
    state->itemRefusalCodes.clear();
}

bool InventoryWindow::OnNotice(const ui::Notice& notice) {
    // A pick mode (the store's Repair, the gender switch tool; HudServices.cpp): a click on an item is its; a right
    // click gives the mode up.
    if (ServicePicking(context_) && (notice.kind == ui::Notice::Kind::Clicked || notice.kind == ui::Notice::Kind::RightClicked)) {
        const int place = PlaceOf(notice.source);
        if (place >= game::slot::kWorn) {
            if (notice.kind == ui::Notice::Kind::RightClicked) context_.servicePick = 0;
            else ServicePick(context_, place);
            return true;
        }
    }
    if (notice.kind == ui::Notice::Kind::Clicked && notice.source != nullptr && notice.source->parent() == this && notice.control >= 80 &&
        notice.control < 80 + int(tabs_.size())) {
        SelectTab(notice.control - 80);
        return true;
    }
    if (notice.kind == ui::Notice::Kind::Clicked && notice.control == 11) {
        // The coin: into the storage while it is open; dropping gold comes with items on the ground (C10).
        // Nothing with a store open; into the storage while one is; else onto the ground.
        if (context_.state != nullptr && context_.hud != nullptr && context_.state->shopNpc == 0) {
            if (context_.state->guildStorageOpen) {
                context_.hud->StorageGold(true, true);
            } else if (context_.state->storageOpen) {
                context_.hud->StorageGold(true);
            } else {
                context_.hud->DropGoldBox();
            }
        }
        return true;
    }
    // Ctrl + click with a store open: sold at once, the whole stack.
    if (notice.kind == ui::Notice::Kind::Clicked && notice.value == 2 && context_.hud != nullptr && context_.state != nullptr &&
        context_.state->shopNpc != 0) {
        const int place = PlaceOf(notice.source);
        if (place >= game::slot::kWorn && context_.state->inventory.At(std::uint8_t(place)) != nullptr) {
            context_.hud->Request({HudRequest::Kind::ShopSell, place});
        }
        return true;
    }
    // Shift + click on a stack of more than one: the split box; the part goes to the first empty
    // place.
    if (notice.kind == ui::Notice::Kind::Clicked && notice.value == 1 && context_.hud != nullptr && context_.state != nullptr &&
        context_.items != nullptr) {
        const int place = PlaceOf(notice.source);
        const game::ItemInstance* item = place >= game::slot::kWorn ? context_.state->inventory.At(std::uint8_t(place)) : nullptr;
        const assets::ItemRow* row = item != nullptr ? context_.items->Find(item->refId) : nullptr;
        if (row != nullptr && game::KindOf(*row) == ItemKind::Expendable && item->count > 1) {
            const std::string icon = row->iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row->iconPath;
            HudContext* c = &context_;
            context_.hud->Divide(icon, context_.Text(row->nameKey), item->count, [c, place](int number) {
                const int free = c->state != nullptr ? c->state->inventory.FreeBagPlace() : -1;
                if (free >= 0) RequestMove(*c, place, free, number);
            });
        }
        return true;
    }
    // A right click on an item uses it (the server says what may be used); while the alchemy window shows, it goes
    // into that window.
    if (notice.kind == ui::Notice::Kind::RightClicked && context_.hud != nullptr) {
        const int place = PlaceOf(notice.source);
        if (place >= game::slot::kWorn && AlchemyShown(context_)) {
            AlchemyPut(context_, place);
            return true;
        }
        // The Open Market's Register page takes it (its price asked).
        if (place >= game::slot::kWorn && OpenMarketTakesItems(context_)) {
            OpenMarketPut(context_, place);
            return true;
        }
        if (place >= game::slot::kWorn && context_.state != nullptr && context_.state->inventory.At(std::uint8_t(place)) != nullptr) {
            context_.hud->Request({HudRequest::Kind::UseItem, place});
            return true;
        }
    }
    return false;
}

// --- The storage ---------------------------------------------------------------------------------

int StorageWindow::PlaceOf(const ui::Widget* slot) {
    const auto* window = slot != nullptr ? dynamic_cast<const StorageWindow*>(slot->parent()) : nullptr;
    if (window == nullptr) return -1;
    if (slot == window->proxy_) return window->proxyPlace_;
    if (slot->id() < 100 || slot->id() > 129) return -1;
    return window->page_ * 30 + slot->id() - 100;
}

void StorageWindow::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    guild_ = id() == hud::kGuildStorage;
    for (int id = 100; id <= 129; ++id) {
        auto* slot = ui::As<ui::Slot>(Child(id));
        if (slot == nullptr) continue;
        slot->pickable = false; // a click picks up the proxy (OnNotice)
        slot->tooltipLines = [this, id] {
            const game::ItemInstance* item = context_.state ? Store().At(std::uint8_t(page_ * 30 + id - 100)) : nullptr;
            const assets::ItemRow* row = item && context_.items ? context_.items->Find(item->refId) : nullptr;
            return row ? ItemTooltip(*row, *item, context_) : std::vector<ui::TooltipLine>{};
        };
    }
    if (auto* gold = ui::As<ui::Static>(Child(10))) gold->textStyle |= ui::kTextShadow;
    assets::UiLayoutNode held;
    held.type = "CIFSlotWithHelp";
    held.id = 199;
    held.rect = {0.0f, 0.0f, 32.0f, 32.0f};
    proxy_ = ui::As<ui::Slot>(Add(ui.Make(held)));
    if (proxy_ != nullptr) proxy_->visible = false;
    // Dropped on a place: from the bag into it, or from another place of the storage.
    onDrop = [this](ui::Widget* source, ui::Widget* target) {
        const int to = PlaceOf(target);
        if (to < 0 || context_.hud == nullptr) return false;
        const int guild = guild_ ? 1 << 18 : 0;
        if (const int stored = PlaceOf(source); stored >= 0) {
            if (stored != to) context_.hud->Request({HudRequest::Kind::StorageMove, stored | to << 8 | 1 << 16 | 1 << 17 | guild});
            return true;
        }
        if (const int place = InventoryPlaceOf(source); place >= game::slot::kWorn) {
            context_.hud->Request({HudRequest::Kind::StorageMove, place | to << 8 | 1 << 17 | guild});
            return true;
        }
        return false;
    };
}

void StorageWindow::Refresh() {
    const game::Storage& s = Store();
    for (int id = 100; id <= 129; ++id) {
        auto* slot = ui::As<ui::Slot>(Child(id));
        if (slot == nullptr) continue;
        const int place = page_ * 30 + id - 100;
        slot->enabled = place < s.size;
        ShowItem(*slot, s.At(std::uint8_t(place)), context_);
    }
    if (auto* gold = ui::As<ui::Static>(Child(10))) {
        gold->SetText(GoldText(s.gold));
        gold->textColor = GoldColor(s.gold);
    }
}

void StorageWindow::Update(ui::Frame&) {
    if (context_.state == nullptr) return;
    // The page from the spinner (1 .. size / 30).
    if (auto* spin = ui::As<ui::Spin>(Child(13))) {
        spin->max = std::max(0, (int(Store().size) + 29) / 30 - 1);
        spin->value = std::clamp(spin->value, 0, spin->max);
        if (spin->value != page_) {
            page_ = spin->value;
            seen_ = ~0ull;
        }
    }
    if (context_.state->inventoryVersion != seen_) {
        seen_ = context_.state->inventoryVersion;
        Refresh();
    }
}

// --- The storage's gold box ------------------------------------------------------------------------

ui::Widget* GoldBox::Named(std::string_view name) const {
    for (const auto& c : children()) {
        if (c->node().name == name) return c.get();
    }
    return nullptr;
}

void GoldBox::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    // The money box (ifmessagebox.txt's MsgBoxStoreMoney; in the notes): the
    // coin in the icon well (icon\mini_gold_icon.ddj), the purpose's label on the board ("Deposit"
    // into the storage, "Taken" out of it), the amount (its edit, made hidden by the layout's style,
    // shown) and "Gold", Confirm and Cancel; the price, the count and the second board stay hidden.
    if (const assets::UiLayout* layout = ui.Layout("resinfo\\ifmessagebox.txt")) {
        // The box's own background first (its Create section's tile, com_bg_tile_b.ddj, sized to
        // this box), then the money parts over it.
        for (const auto& section : layout->sections) {
            if (section.name != "Create") continue;
            for (auto it = section.nodes.rbegin(); it != section.nodes.rend(); ++it) {
                assets::UiLayoutNode n = *it;
                n.rect = {16.0f, 40.0f, rect.w - 32.0f, rect.h - 56.0f};
                Add(ui.Make(n));
            }
        }
        for (const auto& section : layout->sections) {
            if (section.name != "MsgBoxStoreMoney") continue;
            for (auto it = section.nodes.rbegin(); it != section.nodes.rend(); ++it) Add(ui.Make(*it));
        }
    }
    for (const char* hidden : {"GDR_MBS_STATIC_PRICE", "GDR_MBS_EDIT_AMOUNT", "GDR_MBS_STATIC_AMOUNT", "GDR_MBS_STATIC_PRICET1", "GDR_MBS_STATIC_NAME2"}) {
        if (ui::Widget* w = Named(hidden)) w->visible = false;
    }
    const bool deposit = id() == 1;
    // The purpose's label: into the storage, out of it, on the ground ("Amount", purpose 9).
    if (ui::Widget* w = Named("GDR_MBS_STATIC_NAME1")) {
        w->SetText(context_.Text(id() == 3 || id() == 6 ? "UIIT_STT_AMOUNT_MONEY" : deposit ? "UIIT_STT_DEPOSITMONEY" : "UIIT_STT_WITHDRAWMONEY"));
    }
    if (ui::Widget* w = Named("GDR_MBS_BTN_OK")) w->SetText(context_.Text("UIIT_CTL_CONFIRM"));
    if (ui::Widget* w = Named("GDR_MBS_BTN_CANCEL")) w->SetText(context_.Text("UIIT_CTL_CANCEL"));
    if (auto* icon = ui::As<ui::Static>(Named("GDR_MBS_STATIC_ICON"))) icon->texture = "icon\\mini_gold_icon.ddj";
    edit_ = ui::As<ui::Edit>(Named("GDR_MBS_EDIT_PRICE"));
    if (edit_ != nullptr) {
        edit_->visible = true;
        edit_->numeric = true;
        edit_->limit = 12;
        edit_->passKeys = true; // Enter: Confirm; Escape: Cancel
        edit_->SetValue("0");
    }
}

void GoldBox::Update(ui::Frame& frame) {
    if (edit_ != nullptr && !focused_) {
        focused_ = true;
        edit_->Focus(frame);
    }
    if (edit_ != nullptr && context_.state != nullptr) {
        // The first digit replaces the 0 (ours); no more than there is to move (the original's
        // maximum: the gold the side holds).
        std::string typed = edit_->text();
        if (typed.size() > 1 && typed.front() == '0') typed.erase(0, 1);
        const std::uint64_t most = id() == 2 ? context_.state->storage.gold : id() == 5 ? context_.state->guildStorage.gold : context_.state->inventory.gold;
        if (std::strtoull(typed.c_str(), nullptr, 10) > most) typed = std::to_string(most);
        if (typed != edit_->text()) edit_->SetValue(typed);
    }
    // Its storage put away: gone (1, 2 the account's; 4, 5 the guild's; 3 the ground's stays); 6 an
    // exchange's, gone with it.
    const bool open = id() == 6 ? context_.exchange != nullptr && context_.exchange->Open()
                                : context_.state != nullptr && (id() >= 4 ? context_.state->guildStorageOpen : context_.state->storageOpen);
    if (id() != 3 && !open && ui_ != nullptr) ui_->Close(this);
}

bool GoldBox::OnNotice(const ui::Notice& notice) {
    const bool mine = notice.source != nullptr && notice.source->parent() == this;
    const auto named = [&](const char* name) { return notice.source != nullptr && notice.source->node().name == name; };
    const bool clicked = notice.kind == ui::Notice::Kind::Clicked;
    const bool confirm = mine && ((clicked && named("GDR_MBS_BTN_OK")) || notice.kind == ui::Notice::Kind::Submitted);
    const bool cancel = mine && ((clicked && (named("GDR_MBS_BTN_CANCEL") || notice.control == 2)) || notice.kind == ui::Notice::Kind::Cancelled);
    if (!confirm && !cancel) return ui::MainFrame::OnNotice(notice);
    const std::uint64_t amount = edit_ != nullptr ? std::strtoull(edit_->text().c_str(), nullptr, 10) : 0;
    // The box's id says which way (Hud::StorageGold): 1 into the storage, 2 out of it (4, 5 the guild's).
    if (confirm && id() == 6 && context_.exchangeRequest) {
        // The gold offered in the exchange (0: none).
        context_.exchangeRequest(game::proto::ExchangeGold::kId, game::proto::Encode(game::proto::ExchangeGold{amount}));
    } else if (confirm && amount > 0 && context_.hud != nullptr) {
        if (id() == 3) {
            context_.hud->Request({HudRequest::Kind::DropGold, 0, std::to_string(amount)});
        } else {
            context_.hud->Request({HudRequest::Kind::StorageGold, (id() == 1 || id() == 4 ? 1 : 0) | (id() >= 4 ? 2 : 0), std::to_string(amount)});
        }
    }
    if (ui_ != nullptr) ui_->Close(this);
    return true;
}

bool StorageWindow::OnNotice(const ui::Notice& notice) {
    // The gold button: the gold box.
    if (notice.kind == ui::Notice::Kind::Clicked && notice.control == 11 && notice.source != nullptr && notice.source->parent() == this) {
        if (context_.hud != nullptr) context_.hud->StorageGold(false, guild_); // taken out of the storage
        return true;
    }
    // A click on an item: the proxy picked up in its stead.
    if (notice.kind == ui::Notice::Kind::Clicked && notice.value == 0 && notice.source != nullptr && notice.source->parent() == this &&
        notice.control >= 100 && notice.control <= 129 && proxy_ != nullptr && context_.hud != nullptr && context_.hud->interface() != nullptr) {
        const auto* slot = ui::As<ui::Slot>(notice.source);
        if (slot != nullptr && !slot->icon.empty()) {
            proxy_->icon = slot->icon;
            proxy_->count = slot->count;
            proxy_->countAlways = slot->countAlways;
            proxy_->rare = slot->rare;
            proxyPlace_ = page_ * 30 + notice.control - 100;
            context_.hud->interface()->Pick(proxy_);
        }
        return true;
    }
    // Closed: the storage is put away (opened again at a keeper; the guild's freed for the others).
    if (notice.kind == ui::Notice::Kind::Clicked && notice.control == 2 && notice.source != nullptr && notice.source->parent() == this &&
        context_.state != nullptr) {
        (guild_ ? context_.state->guildStorageOpen : context_.state->storageOpen) = false;
    }
    return ui::MainFrame::OnNotice(notice);
}

// --- The store (C7i) ---------------------------------------------------------------------------

namespace {

const StoreWindow* StoreOf(const ui::Widget* slot) { return slot != nullptr ? dynamic_cast<const StoreWindow*>(slot->parent()) : nullptr; }

std::string IconOf(const assets::ItemRow& row) { return row.iconPath.empty() ? std::string("icon\\icon_default.ddj") : "icon\\" + row.iconPath; }

} // namespace

game::ItemInstance StoreWindow::ItemOf(const assets::ShopGood& good, const assets::ItemTable& items) {
    game::ItemInstance item;
    if (const assets::ItemRow* row = items.Find(std::string_view(good.itemCodename))) {
        item.refId = row->refObjId;
        item.plus = std::uint8_t(good.plus);
        item.variance = good.variance;
        if (game::MaxStack(*row) <= 1) item.durability = good.data > 0 ? std::uint32_t(good.data) : game::NewDurability(*row);
    }
    return item;
}

const assets::ShopGood* StoreWindow::GoodOf(const ui::Widget* slot, int* tabIndex) {
    const StoreWindow* store = StoreOf(slot);
    if (store == nullptr || store->group_ == nullptr || slot->id() < 100 || slot->id() > 129) return nullptr;
    if (store->tab_ >= int(store->group_->tabs.size())) return nullptr;
    const assets::ShopTab& tab = store->group_->tabs[std::size_t(store->tab_)];
    const std::size_t k = std::size_t(store->page_ * 30 + slot->id() - 100);
    if (k >= tab.goods.size()) return nullptr;
    if (tabIndex != nullptr) *tabIndex = tab.index;
    return &tab.goods[k];
}

int StoreWindow::SoldOf(const ui::Widget* slot) {
    return StoreOf(slot) != nullptr && slot->id() >= 50 && slot->id() <= 54 ? slot->id() - 50 : -1;
}

void StoreWindow::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    // Four tabs made in code: 56 x 24 at x 16 + 56 i, y 38.
    for (int t = 0; t < 4; ++t) {
        assets::UiLayoutNode node;
        node.type = "CIFSelectableArea";
        node.id = 200 + t;
        node.rect = {16.0f + 56.0f * float(t), 38.0f, 56.0f, 24.0f};
        node.ddj = "interface\\ifcommon\\com_short_tab_off.ddj";
        node.hAlign = 1;
        node.vAlign = 1;
        Add(ui.Make(node))->visible = false;
    }
    for (int id : {11, 12}) {
        if (ui::Widget* w = Child(id)) w->visible = false; // shown at an NPC that repairs (Update)
    }
    // The goods: their tooltip with the package's price ("Price: 890Gold").
    for (int id = 100; id <= 129; ++id) {
        auto* slot = ui::As<ui::Slot>(Child(id));
        if (slot == nullptr) continue;
        slot->pickable = true;
        slot->tooltipLines = [this, slot] {
            const assets::ShopGood* good = GoodOf(slot);
            const assets::ItemRow* row = good != nullptr && context_.items != nullptr ? context_.items->Find(std::string_view(good->itemCodename)) : nullptr;
            if (row == nullptr) return std::vector<ui::TooltipLine>{};
            HudContext plain = context_;
            plain.shopPrices = false;
            std::vector<ui::TooltipLine> lines = ItemTooltip(*row, ItemOf(*good, *context_.items), plain);
            lines.push_back({{}, kWhite, kShadow});
            // With the fortress tax of the NPC's town.
            lines.push_back({context_.Text("UIIT_STT_PRICE") + " : " + std::to_string(context_.TaxedBuy(good->price)) + context_.Text("UIIT_STT_GOLD"), kWhite, kShadow});
            // A guild's good: its guild points too.
            if (good->guildPoints != 0) {
                lines.push_back({FormatText("%s : %d%s", {context_.Text("UIIT_STT_GUILD_POINT"), std::to_string(good->guildPoints), context_.Text("UIIT_STT_GUILD_POINT_GP")}), kWhite, kShadow});
            }
            // Its honour points and tokens, each its line (.. "%s: %I64d%s", the honour's with
            // UIIT_STT_SILKMALL_P_POINT, a token's name with UIIT_STT_UNIT).
            if (good->honorPoints != 0) {
                lines.push_back({FormatText("%s : %d%s", {context_.Text("UIIT_STT_TC_HONOR_POINT"), std::to_string(good->honorPoints), context_.Text("UIIT_STT_SILKMALL_P_POINT")}),
                                 kWhite, kShadow});
            }
            for (std::size_t k = 0; k < good->tokens.size(); ++k) {
                if (good->tokens[k] == 0) continue;
                const std::string name = context_.Text(std::string("SN_") + assets::kShopTokens[k]);
                lines.push_back({FormatText("%s : %d%s", {name, std::to_string(good->tokens[k]), context_.Text("UIIT_STT_UNIT")}), kWhite, kShadow});
            }
            return lines;
        };
    }
    // What was sold: its tooltip with what buying it back costs.
    for (int id = 50; id <= 54; ++id) {
        auto* slot = ui::As<ui::Slot>(Child(id));
        if (slot == nullptr) continue;
        slot->pickable = true;
        slot->tooltipLines = [this, id] {
            if (context_.state == nullptr || std::size_t(id - 50) >= context_.state->buyback.size()) return std::vector<ui::TooltipLine>{};
            const proto::BuybackRecord& r = context_.state->buyback[std::size_t(id - 50)];
            const game::ItemInstance item = game::Inventory::FromRecord(r.item);
            const assets::ItemRow* row = context_.items != nullptr ? context_.items->Find(item.refId) : nullptr;
            if (row == nullptr) return std::vector<ui::TooltipLine>{};
            HudContext plain = context_;
            plain.shopPrices = false;
            std::vector<ui::TooltipLine> lines = ItemTooltip(*row, item, plain);
            lines.push_back({{}, kWhite, kShadow});
            lines.push_back({context_.Text("UIIT_STT_PRICE") + " " + std::to_string(r.price), kWhite, kShadow});
            return lines;
        };
    }
    // An item of the bag dropped anywhere on the store: the sell box.
    onDrop = [this](ui::Widget* source, ui::Widget*) {
        // A good of the transport's bag: the sale's window at a specialty NPC,
        // else the pets' "cannot trade here".
        if (const int cart = CosBagPlaceOf(source); cart >= 0) {
            const HudContext::CosShown* cos = nullptr;
            for (const HudContext::CosShown& s : context_.coses) {
                if (s.entity == context_.cosSelected) cos = &s;
            }
            const auto it = cos != nullptr ? cos->bag.find(std::uint8_t(cart)) : decltype(cos->bag.end()){};
            const bool transport = cos != nullptr && cos->kind == std::uint8_t(game::CosKind::Transport) && it != cos->bag.end();
            if (!transport || context_.tradeNpc == 0 || !SellFromCart(context_, it->second, cart)) {
                if (context_.state != nullptr) {
                    const std::string text = context_.Text("UIIT_MSG_COSPETERR_CANT_TRADE_SHOP");
                    context_.state->Post(game::GameMessage::Kind::General, text, context_.time);
                    context_.state->Notify(game::Banner::Kind::Warning, text, context_.time);
                }
            }
            return true;
        }
        const int place = InventoryPlaceOf(source);
        if (place < 0 || context_.state == nullptr || context_.items == nullptr || context_.hud == nullptr) return false;
        const game::ItemInstance* item = context_.state->inventory.At(std::uint8_t(place));
        const assets::ItemRow* row = item != nullptr ? context_.items->Find(item->refId) : nullptr;
        if (row == nullptr) return true;
        if (place < game::slot::kWorn || !row->canSell) {
            context_.state->itemRefusals.push_back(proto::ItemStatus::NotForSale);
            return true;
        }
        DealBox::Deal deal;
        deal.kind = DealBox::Deal::Kind::Sell;
        deal.icon = IconOf(*row);
        deal.name = context_.Text(row->nameKey);
        deal.unitPrice = row->sellPrice;
        deal.count = deal.most = std::max<int>(1, item->count);
        deal.place = place;
        context_.hud->Deal(deal);
        return true;
    };
}

void StoreWindow::Open(std::uint32_t npc, const assets::NpcShop& shop, int group, const std::string& title) {
    npc_ = npc;
    group_ = group >= 0 && std::size_t(group) < shop.groups.size() ? &shop.groups[std::size_t(group)] : nullptr;
    if (ui::Widget* t = Child(0)) t->SetText(title);
    for (int t = 0; t < 4; ++t) {
        ui::Widget* tab = Child(200 + t);
        if (tab == nullptr) continue;
        tab->visible = group_ != nullptr && std::size_t(t) < group_->tabs.size();
        if (tab->visible) tab->SetText(context_.Text(group_->tabs[std::size_t(t)].labelKey));
    }
    SelectTab(0);
}

void StoreWindow::SelectTab(int tab) {
    // A new tab starts on its first page.
    tab_ = tab;
    page_ = 0;
    for (int t = 0; t < 4; ++t) {
        if (auto* area = ui::As<ui::SelectableArea>(Child(200 + t))) {
            area->selected = t == tab;
            area->texture = t == tab ? "interface\\ifcommon\\com_short_tab_on.ddj" : "interface\\ifcommon\\com_short_tab_off.ddj";
        }
    }
    if (auto* spin = ui::As<ui::Spin>(Child(14))) spin->value = 0;
    seen_ = ~0ull;
}

void StoreWindow::Refresh() {
    const std::size_t goods = group_ != nullptr && tab_ < int(group_->tabs.size()) ? group_->tabs[std::size_t(tab_)].goods.size() : 0;
    if (auto* spin = ui::As<ui::Spin>(Child(14))) spin->max = int(goods == 0 ? 0 : (goods - 1) / 30);
    for (int id = 100; id <= 129; ++id) {
        auto* slot = ui::As<ui::Slot>(Child(id));
        if (slot == nullptr) continue;
        const assets::ShopGood* good = GoodOf(slot);
        if (good == nullptr || context_.items == nullptr) {
            ShowItem(*slot, nullptr, context_);
            continue;
        }
        const game::ItemInstance item = ItemOf(*good, *context_.items);
        ShowItem(*slot, &item, context_);
        slot->count = 0;
        slot->countAlways = false;
    }
    // The sold ones, oldest left.
    for (int k = 0; k < 5; ++k) {
        auto* slot = ui::As<ui::Slot>(Child(50 + k));
        if (slot == nullptr) continue;
        if (context_.state == nullptr || std::size_t(k) >= context_.state->buyback.size()) {
            ShowItem(*slot, nullptr, context_);
            continue;
        }
        const game::ItemInstance item = game::Inventory::FromRecord(context_.state->buyback[std::size_t(k)].item);
        ShowItem(*slot, &item, context_);
    }
}

void StoreWindow::Update(ui::Frame&) {
    if (context_.state == nullptr) return;
    // Repair and Repair all at the smiths and armour merchants (NPC function 4).
    const bool repairs = context_.npcServicesOf == npc_ && (context_.npcServices & proto::kNpcRepair) != 0;
    for (int id : {11, 12}) {
        if (ui::Widget* w = Child(id)) w->visible = repairs;
    }
    if (auto* spin = ui::As<ui::Spin>(Child(14)); spin != nullptr && spin->value != page_) {
        page_ = spin->value;
        seen_ = ~0ull;
    }
    if (context_.state->inventoryVersion != seen_) {
        seen_ = context_.state->inventoryVersion;
        Refresh();
    }
}

bool StoreWindow::OnNotice(const ui::Notice& notice) {
    const bool mine = notice.source != nullptr && notice.source->parent() == this;
    if (mine && notice.kind == ui::Notice::Kind::Clicked && notice.control >= 200 && notice.control <= 203) {
        SelectTab(notice.control - 200);
        return true;
    }
    // Ctrl + click on a good: bought at once, a whole stack or one.
    if (mine && notice.kind == ui::Notice::Kind::Clicked && notice.value == 2 && context_.hud != nullptr && context_.items != nullptr) {
        int tab = 0;
        if (const assets::ShopGood* good = GoodOf(notice.source, &tab)) {
            const assets::ItemRow* row = context_.items->Find(std::string_view(good->itemCodename));
            HudRequest r{HudRequest::Kind::ShopBuy, tab | good->slot << 8};
            r.extra = row != nullptr ? game::MaxStack(*row) : 1;
            context_.hud->Request(r);
        }
        return true;
    }
    // Repair: the next item clicked (worn or in the bag) is mended (the original's cursor 0x96); Repair
    // all: its box with the price (kind 0x0C), then everything.
    if (mine && notice.kind == ui::Notice::Kind::Clicked && notice.control == 11) {
        context_.servicePick = 1;
        context_.servicePickNpc = npc_;
        return true;
    }
    if (mine && notice.kind == ui::Notice::Kind::Clicked && notice.control == 12) {
        AskRepairAll(context_, npc_);
        return true;
    }
    // Closed: the store is put away.
    if (mine && notice.kind == ui::Notice::Kind::Clicked && notice.control == 2 && context_.state != nullptr) {
        context_.state->shopNpc = 0;
        if (context_.servicePick == 1) context_.servicePick = 0;
    }
    return ui::MainFrame::OnNotice(notice);
}

// --- The store's boxes ----------------------------------------------------------------------------

ui::Widget* DealBox::Named(std::string_view name) const {
    for (const auto& c : children()) {
        if (c->node().name == name) return c.get();
    }
    return nullptr;
}

void DealBox::Created(ui::Interface& ui) {
    ui::MainFrame::Created(ui);
    ui_ = &ui;
    const auto add = [&](const char* layout, const char* section, float dx, float dy) {
        if (const assets::UiLayout* l = ui.Layout(layout)) {
            for (const auto& s : l->sections) {
                if (s.name != section) continue;
                for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it) {
                    assets::UiLayoutNode n = *it;
                    n.rect.x += dx;
                    n.rect.y += dy;
                    Add(ui.Make(n));
                }
            }
        }
    };
    const auto place = [&](const char* name, ui::Rect r) {
        if (ui::Widget* w = Named(name)) w->rect = r;
    };
    if (node().subSection == "Buy") {
        // ConfirmBuy (ifitemmallconfirmbuy.txt) and its gold row (ifitemmallconfirmslot.txt) at y 100;
        // the buttons placed in code (their y ours, 40 above the bottom).
        add("resinfo\\ifitemmallconfirmbuy.txt", "Create", 0.0f, 0.0f);
        add("resinfo\\ifitemmallconfirmslot.txt", "Create", 0.0f, 100.0f);
        place("GDR_ITEMMALL_CONFIRM_BUY_BG", {16.0f, 40.0f, 295.0f, rect.h - 56.0f});
        place("GDR_ITEMMALL_CONFIRM_BTN_BUY", {82.0f, rect.h - 40.0f, 76.0f, 24.0f});
        place("GDR_ITEMMALL_CONFIRM_BTN_CANCEL", {170.0f, rect.h - 40.0f, 76.0f, 24.0f});
        if (ui::Widget* w = Named("GDR_ITEMMALL_CONFIRM_STA_1")) w->SetText(context_.Text("UIIT_CTL_WARENETWORK_RESULT_PRICE"));
        if (ui::Widget* w = Named("GDR_ITEMMALL_CONFIRM_STA_2")) w->SetText(context_.Text("UIIT_STT_GOLD"));
        count_ = ui::As<ui::Edit>(Named("GDR_ITEMMALL_CONFIRM_EDIT_COUNT"));
        price_ = Named("GDR_ITEMMALL_CONFIRM_EDIT");
        if (auto* name = ui::As<ui::Edit>(Named("GDR_ITEMMALL_CONFIRM_EDIT_ITEM_NAME"))) name->interactive = false;
    } else {
        // MsgBoxStoreConfirm (ifmessagebox.txt) over the box's background tile; buttons Sell (or
        // Repurchase) and Cancel at 82 / 170, 135.
        add("resinfo\\ifmessagebox.txt", "Create", 0.0f, 0.0f);
        if (ui::Widget* bg = Named("GDR_MSGBOX_BG")) bg->rect = {16.0f, 40.0f, rect.w - 32.0f, rect.h - 56.0f};
        add("resinfo\\ifmessagebox.txt", "MsgBoxStoreConfirm", 0.0f, 0.0f);
        place("GDR_MBS_CONFIRM_BTN_SELL", {82.0f, 135.0f, 76.0f, 24.0f});
        place("GDR_MBS_CONFIRM_BTN_CANCEL", {170.0f, 135.0f, 76.0f, 24.0f});
        count_ = ui::As<ui::Edit>(Named("GDR_MBS_CONFIRM_EDIT_COUNT"));
        price_ = Named("GDR_MBS_CONFIRM_EDIT_PRICE");
        if (auto* name = ui::As<ui::Edit>(Named("GDR_MBS_CONFIRM_EDIT_ITEM_NAME"))) name->interactive = false;
    }
    for (const auto& c : children()) c->visible = c->visible || c->node().name.starts_with("GDR_"); // the layouts' made-hidden parts
    if (auto* p = ui::As<ui::Edit>(price_)) p->interactive = false;
    if (auto* p = ui::As<ui::Edit>(price_); p != nullptr && node().subSection == "StallRegister") {
        // A stall's register: the price typed in, the count the stack's own (ours: a stack is
        // sold whole).
        p->interactive = true;
        p->numeric = true;
        p->limit = 10;
        p->passKeys = true;
    }
    if (count_ != nullptr) {
        count_->numeric = true;
        count_->limit = 5;
        count_->passKeys = true; // Enter: the deal; Escape: Cancel
    }
}

void DealBox::Set(const Deal& deal) {
    deal_ = deal;
    const bool buy = deal.kind == Deal::Kind::Buy;
    if (auto* icon = ui::As<ui::Static>(Named(buy ? "GDR_ITEMMALL_CONFIRM_BUY_ITEM_ICON" : "GDR_MBS_CONFIRM_BUY_ITEM_ICON"))) icon->texture = deal.icon;
    if (ui::Widget* name = Named(buy ? "GDR_ITEMMALL_CONFIRM_EDIT_ITEM_NAME" : "GDR_MBS_CONFIRM_EDIT_ITEM_NAME")) name->SetText(deal.name);
    if (deal.kind == Deal::Kind::StallRegister) {
        // "Confirmation window", Register and Cancel.
        if (ui::Widget* ok = Named("GDR_MBS_CONFIRM_BTN_SELL")) ok->SetText(context_.Text("UIIT_CTL_STALL_REGISTER"));
        for (const char* spin : {"GDR_MBS_CONFIRM_SPINBTN_UP", "GDR_MBS_CONFIRM_SPINBTN_DOWN"}) {
            if (ui::Widget* w = Named(spin)) w->visible = false;
        }
        if (count_ != nullptr) {
            count_->interactive = false;
            count_->SetValue(std::to_string(deal.count));
        }
        if (auto* p = ui::As<ui::Edit>(price_)) {
            p->SetValue(deal.unitPrice != 0 ? std::to_string(deal.unitPrice) : std::string());
            if (ui_ != nullptr) ui_->Focus(p);
        }
        return;
    }
    if (!buy) {
        // A sale's button says Sell, a buy-back's Repurchase; a buy-back's count is the entry's.
        if (ui::Widget* ok = Named("GDR_MBS_CONFIRM_BTN_SELL")) {
            ok->SetText(context_.Text(deal.kind == Deal::Kind::Sell ? "UIIT_STT_SELL" : "UIIT_STT_RE_BUY_OBJECT"));
        }
        if (deal.kind == Deal::Kind::Buyback) {
            for (const char* spin : {"GDR_MBS_CONFIRM_SPINBTN_UP", "GDR_MBS_CONFIRM_SPINBTN_DOWN"}) {
                if (ui::Widget* w = Named(spin)) w->visible = false;
            }
            if (count_ != nullptr) count_->interactive = false;
        }
    }
    if (count_ != nullptr) {
        count_->SetValue(std::to_string(deal.count));
        if (deal.kind != Deal::Kind::Buyback && ui_ != nullptr) ui_->Focus(count_);
    }
}

int DealBox::Count() const {
    const int typed = count_ != nullptr ? std::atoi(count_->text().c_str()) : deal_.count;
    return std::clamp(typed, 1, std::max(1, deal_.most));
}

void DealBox::Update(ui::Frame&) {
    if (deal_.kind == Deal::Kind::StallRegister) {
        // Open while the player's own stall is.
        if ((context_.stall == nullptr || !context_.stall->Mine()) && ui_ != nullptr) ui_->Close(this);
        return;
    }
    if (count_ != nullptr && deal_.kind != Deal::Kind::Buyback) {
        const int n = Count();
        if (count_->text() != std::to_string(n) && !count_->text().empty()) count_->SetValue(std::to_string(n));
    }
    // A buy: the count times the price; a sale: the price of one; a buy-back: what it costs.
    if (price_ != nullptr) {
        const std::uint64_t shown = deal_.kind == Deal::Kind::Buy ? deal_.unitPrice * std::uint64_t(Count()) : deal_.unitPrice;
        price_->SetText(GoldText(shown));
    }
    if (context_.state != nullptr && context_.state->shopNpc == 0 && ui_ != nullptr) ui_->Close(this);
}

bool DealBox::OnNotice(const ui::Notice& notice) {
    const bool mine = notice.source != nullptr && notice.source->parent() == this;
    if (!mine) return ui::MainFrame::OnNotice(notice);
    const std::string& name = notice.source->node().name;
    const bool clicked = notice.kind == ui::Notice::Kind::Clicked;
    // The spin buttons: one more or less, within the limits.
    if (clicked && (name == "GDR_ITEMMALL_CONFIRM_SPINBTN_UP" || name == "GDR_MBS_CONFIRM_SPINBTN_UP" || name == "GDR_ITEMMALL_CONFIRM_SPINBTN_DOWN" ||
                    name == "GDR_MBS_CONFIRM_SPINBTN_DOWN")) {
        const int step = name.ends_with("_UP") ? 1 : -1;
        if (count_ != nullptr) count_->SetValue(std::to_string(std::clamp(Count() + step, 1, std::max(1, deal_.most))));
        return true;
    }
    const bool confirm = (clicked && (name == "GDR_ITEMMALL_CONFIRM_BTN_BUY" || name == "GDR_MBS_CONFIRM_BTN_SELL")) ||
                         notice.kind == ui::Notice::Kind::Submitted;
    const bool cancel = (clicked && (name == "GDR_ITEMMALL_CONFIRM_BTN_CANCEL" || name == "GDR_MBS_CONFIRM_BTN_CANCEL" || notice.control == 2)) ||
                        notice.kind == ui::Notice::Kind::Cancelled;
    if (!confirm && !cancel) return ui::MainFrame::OnNotice(notice);
    if (confirm && context_.hud != nullptr) {
        switch (deal_.kind) {
        case Deal::Kind::Buy: {
            HudRequest r{HudRequest::Kind::ShopBuy, deal_.tab | deal_.slot << 8};
            r.extra = Count();
            context_.hud->Request(r);
            break;
        }
        case Deal::Kind::Sell: {
            HudRequest r{HudRequest::Kind::ShopSell, deal_.place};
            r.extra = Count();
            context_.hud->Request(r);
            break;
        }
        case Deal::Kind::Buyback: context_.hud->Request({HudRequest::Kind::ShopBuyback, deal_.index}); break;
        case Deal::Kind::StallRegister: {
            const std::uint64_t price = price_ != nullptr ? std::strtoull(price_->text().c_str(), nullptr, 10) : 0;
            if (context_.stallRequest) {
                context_.stallRequest(proto::StallPut::kId, proto::Encode(proto::StallPut{std::uint8_t(deal_.stallSlot), std::uint8_t(deal_.place), price}));
            }
            break;
        }
        }
    }
    if (ui_ != nullptr) ui_->Close(this);
    return true;
}

// --- The equipment ---------------------------------------------------------------------------------

namespace {

// The part pictures, drawn under every worn place.
struct Silhouette {
    const char* file;
    float x, y, size;
};
constexpr Silhouette kSilhouettes[13] = {
    {"equip_slot_helm", 1, 82, 40},       {"equip_slot_mail", 1, 125, 40},      {"equip_slot_shoulderguard", 137, 82, 40},
    {"equip_slot_gauntlet", 137, 125, 40}, {"equip_slot_pants", 1, 168, 40},     {"equip_slot_boots", 137, 168, 40},
    {"equip_slot_weapon", 0, 0, 56},      {"equip_slot_shield", 122, 0, 56},    {"equip_slot_specialdress", 137, 304, 40},
    {"equip_slot_earring", 1, 211, 40},   {"equip_slot_necklace", 137, 211, 40}, {"equip_slot_l_ring", 1, 254, 40},
    {"equip_slot_r_ring", 137, 254, 40}};
// The dress side's (1.188: textures, positions), 40 x 40 under slots 113..117:
// the hat, the dress, the attachment, the flag, the Devil's Spirit.
constexpr Silhouette kAvatarSilhouettes[5] = {{"equip_slot_helm", 1, 82, 40},
                                              {"equip_slot_cloth", 1, 125, 40},
                                              {"equip_slot_pandernt", 137, 125, 40},
                                              {"equip_slot_plag", 137, 82, 40},
                                              {"equip_slot_extraneous01", 1, 169, 40}};
// Slot 113 + k shows the avatar of kind k (takes 0x71 + k into its slots; picks one by type).
constexpr int kFirstAvatarSlot = 113;
// The avatar skill window (CIFEquipSkillSlotWnd 500, ifequipskillslotwnd.txt): the Devil's Spirit's skills.
constexpr int kAvatarSkills = 500;

} // namespace

int EquipmentWindow::AvatarPlaceShown(const ui::Widget* slot) const {
    for (std::size_t k = 0; k < avatarSlots_.size(); ++k) {
        if (avatarSlots_[k] != nullptr && avatarSlots_[k] == slot) return avatarShown_[k];
    }
    return -1;
}

void EquipmentWindow::Created(ui::Interface&) {
    // The frame and the background are drawn by the window, under the character.
    for (int id : {4, 5}) {
        if (Widget* w = Child(id)) w->visible = false;
    }
    for (int i = 0; i < 13; ++i) {
        auto* slot = ui::As<ui::Slot>(Child(100 + i));
        slots_[std::size_t(i)] = slot;
        if (slot == nullptr) continue;
        slot->tooltipLines = [this, i] {
            const game::ItemInstance* item = context_.state ? context_.state->inventory.At(std::uint8_t(i)) : nullptr;
            const assets::ItemRow* row = item && context_.items ? context_.items->Find(item->refId) : nullptr;
            return row ? ItemTooltip(*row, *item, context_) : std::vector<ui::TooltipLine>{};
        };
    }
    // The dress side's slots (the 1.188 layout's GDR_AVATAR_SLOT_01..05; 05 only with AVATAR_SPECIAL_ITEM defined).
    for (int k = 0; k < game::avatar::kPlaces; ++k) {
        auto* slot = ui::As<ui::Slot>(Child(kFirstAvatarSlot + k));
        avatarSlots_[std::size_t(k)] = slot;
        if (slot == nullptr) continue;
        slot->tooltipLines = [this, k] {
            const int place = avatarShown_[std::size_t(k)];
            const game::ItemInstance* item = context_.state && place >= 0 ? context_.state->inventory.Avatar(std::uint8_t(place)) : nullptr;
            const assets::ItemRow* row = item && context_.items ? context_.items->Find(item->refId) : nullptr;
            return row ? ItemTooltip(*row, *item, context_) : std::vector<ui::TooltipLine>{};
        };
    }
    if (auto* left = ui::As<ui::Button>(Child(10))) left->behaviour |= ui::Button::kRepeat;
    if (auto* right = ui::As<ui::Button>(Child(11))) right->behaviour |= ui::Button::kRepeat;
    // An inventory item dropped anywhere on the window goes to the place its type is worn in:
    // an avatar into its avatar place, asked of the server after the client's own two checks (its kind's
    // slot holds one, "Cannot equip the selected item"; an attachment with no dress, "Can only be equipped
    // when avatar dress is worn.").
    onDrop = [this](ui::Widget* source, ui::Widget*) {
        const int from = PlaceOf(source);
        if (from < 13 || context_.state == nullptr || context_.items == nullptr) return false; // only from the inventory
        const game::ItemInstance* item = context_.state->inventory.At(std::uint8_t(from));
        const assets::ItemRow* row = item ? context_.items->Find(item->refId) : nullptr;
        if (row == nullptr) return false;
        const game::Inventory& inv = context_.state->inventory;
        if (game::AvatarPlaceFor(*row) >= 0) {
            const proto::ItemStatus check = inv.CheckWearAvatar(*context_.items, context_.wearer, std::uint8_t(from));
            if (check == proto::ItemStatus::PlaceTaken || check == proto::ItemStatus::NeedsDress) {
                context_.state->Post(game::GameMessage::Kind::General, context_.Text(RefusalKey(check)), context_.time);
            } else if (context_.hud != nullptr) {
                context_.hud->Request({HudRequest::Kind::WearAvatar, from});
            }
            return true;
        }
        const int to = game::EquipPlaceFor(*row, inv.At(game::slot::kRingLeft) != nullptr, inv.At(game::slot::kRingRight) != nullptr);
        if (to < 0) return false;
        RequestMove(context_, from, to);
        return true;
    };
    ShowView(false);
}

void EquipmentWindow::ShowView(bool avatars) {
    // As 1.188's the worn places, or the dress side (the five avatar slots and the job suit's), the button to
    // the other side. The avatar skill window stays hidden (ours: the Devil's Spirit's skills are not built; the
    // original shows it on the dress side while a Devil's Spirit with its skills is worn). Hiding slot 117 matters: it
    // stands on the legs' place (5, 172), in front of it.
    avatars_ = avatars;
    for (int i = 0; i < 13; ++i) {
        if (slots_[std::size_t(i)] != nullptr) slots_[std::size_t(i)]->visible = !avatars || i == game::slot::kJobSuit;
    }
    for (ui::Slot* s : avatarSlots_) {
        if (s != nullptr) s->visible = avatars;
    }
    if (EquipSkillWindow* skills = SkillWindow()) skills->Show({}, -1);
    else if (Widget* other = Child(kAvatarSkills)) other->visible = false;
    if (Widget* b = Child(13)) b->visible = !avatars;
    if (Widget* b = Child(14)) b->visible = avatars;
}

EquipSkillWindow* EquipmentWindow::SkillWindow() const { return dynamic_cast<EquipSkillWindow*>(Child(kAvatarSkills)); }

void EquipmentWindow::ShowSocketSkills(int place) {
    EquipSkillWindow* window = SkillWindow();
    if (window == nullptr || context_.state == nullptr) return;
    std::vector<std::uint32_t> skills;
    const game::ItemInstance* item = place >= 0 && place < game::slot::kWorn ? context_.state->inventory.At(std::uint8_t(place)) : nullptr;
    if (item != nullptr && !avatars_) game::alchemy::AddSocketSkills(*item, skills);
    window->Show(skills, skills.empty() ? -1 : place);
}

void EquipmentWindow::Refresh() {
    const game::Inventory& inv = context_.state->inventory;
    // The sockets' skill window follows its item: gone or changed, shown again or hidden.
    if (EquipSkillWindow* skills = SkillWindow(); skills != nullptr && skills->place() >= 0) ShowSocketSkills(skills->place());
    for (int i = 0; i < 13; ++i) {
        if (slots_[std::size_t(i)] == nullptr) continue;
        ui::Slot& slot = *slots_[std::size_t(i)];
        slot.ghost = false;
        slot.pickable = context_.servicePick != 1; // the store's Repair takes the click (HudServices.cpp)
        ShowItem(slot, inv.At(std::uint8_t(i)), context_);
    }
    // A two-handed weapon (not a bow or crossbow) shows a pale copy in the empty shield place.
    const game::ItemInstance* weapon = inv.At(game::slot::kWeapon);
    const assets::ItemRow* row = weapon && context_.items ? context_.items->Find(weapon->refId) : nullptr;
    if (row && game::TwoHanded(*row) && game::AmmunitionFor(*row) == 0 && inv.At(game::slot::kShield) == nullptr && slots_[7]) {
        ShowItem(*slots_[7], weapon, context_);
        slots_[7]->ghost = true;
        slots_[7]->pickable = false;
    }
    // The avatars, each in the slot of its kind (picks the slot by the item's type).
    avatarShown_.fill(-1);
    const assets::ItemRow* dress = nullptr;
    for (const auto& [place, item] : inv.avatars) {
        const assets::ItemRow* r = context_.items ? context_.items->Find(item.refId) : nullptr;
        const int kind = r != nullptr ? game::AvatarPlaceFor(*r) : -1;
        const int shown = kind >= 0 ? kind : place;
        if (shown >= 0 && shown < game::avatar::kPlaces && avatarShown_[std::size_t(shown)] < 0) avatarShown_[std::size_t(shown)] = place;
        if (kind == game::avatar::kDress) dress = r;
    }
    for (int k = 0; k < game::avatar::kPlaces; ++k) {
        ui::Slot* slot = avatarSlots_[std::size_t(k)];
        if (slot == nullptr) continue;
        const int place = avatarShown_[std::size_t(k)];
        ShowItem(*slot, place >= 0 ? inv.Avatar(std::uint8_t(place)) : nullptr, context_);
        slot->pickable = place >= 0;
        slot->enabled = true;
        slot->ghost = false;
    }
    // A dress that takes no attachment (ChildItemCount 0): the attachment's slot disabled under a white veil
    // (the slot's enable off and its effect\icon\int_effect.ddj, white at alpha 115, over it); again
    // once the dress comes off.
    if (ui::Slot* attachment = avatarSlots_[game::avatar::kAttachment]; attachment != nullptr && dress != nullptr && dress->childItems <= 0) {
        attachment->enabled = false;
        attachment->ghost = true;
    }
}

void EquipmentWindow::Update(ui::Frame& frame) {
    frameMs_ = frame.dt * 1000.0f;
    if ((context_.servicePick == 1) != repairing_) {
        repairing_ = context_.servicePick == 1;
        seen_ = ~0ull;
    }
    if (context_.state != nullptr && context_.state->inventoryVersion != seen_) {
        seen_ = context_.state->inventoryVersion;
        Refresh();
    }
    // The character's view: the window inset (2, 15, w - 2, h - 37), only when all of it is on the
    // screen.
    const ui::Rect r = ScreenRect();
    const ui::Rect view{r.x + 2.0f, r.y + 15.0f, r.w - 2.0f, r.h - 37.0f};
    const ui::Vec2 screen = frame.interface.screen();
    context_.characterView.shown = ShownOnScreen() && view.x > 0 && view.y > 0 && r.x + r.w < screen.x && view.y + view.h + 15.0f < screen.y;
    context_.characterView.rect = view;
    context_.characterView.yaw = yaw_;
    // The buttons' help texts.
    for (const auto& [id, key] : {std::pair{13, "UIIT_STT_AVATAR_VIEW_AVATARSLOT"}, std::pair{14, "UIIT_STT_AVATAR_VIEW_EQUIPSLOT"}}) {
        if (Widget* b = Child(id); b != nullptr && b->visible && frame.pointer.hovered == b) frame.interface.ShowTooltip(context_.Text(key), b->ScreenRect());
    }
}

void EquipmentWindow::Draw(ui::Frame& frame) {
    Widget::Draw(frame);
    // The frame and the stone tile, then the character over them, then the part pictures; the slots
    // (icons) and buttons are the children, drawn after.
    for (int id : {4, 5}) {
        if (Widget* w = Child(id)) w->Draw(frame);
    }
    const ui::Rect r = ScreenRect();
    if (context_.characterView.shown) {
        const ui::Vec2 screen = frame.interface.screen();
        frame.paint.SceneOverlay(context_.characterView.rect, {screen.x * frame.paint.scale(), screen.y * frame.paint.scale()});
    }
    const float opacity = Opacity();
    const auto picture = [&](const Silhouette& s) {
        frame.paint.Image(std::string("interface\\equipment\\") + s.file + ".ddj", {r.x + s.x, r.y + s.y, s.size, s.size}, {0, 0}, {1, 1},
                          ui::WithAlpha(ui::kWhite, opacity));
    };
    // The dress side: the job suit's picture and the five avatar ones.
    for (int i = 0; i < 13; ++i) {
        if (!avatars_ || i == game::slot::kJobSuit) picture(kSilhouettes[i]);
    }
    if (avatars_) {
        for (const Silhouette& s : kAvatarSilhouettes) picture(s);
    }
}

bool EquipmentWindow::OnNotice(const ui::Notice& notice) {
    // A right click on a worn item: its sockets' skills (1.188's window 500), else hidden.
    if (notice.kind == ui::Notice::Kind::RightClicked && notice.source != nullptr && notice.source->parent() == this) {
        for (int i = 0; i < 13; ++i) {
            if (slots_[std::size_t(i)] != notice.source) continue;
            const EquipSkillWindow* skills = SkillWindow();
            ShowSocketSkills(skills != nullptr && skills->place() == i ? -1 : i);
            return true;
        }
    }
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || notice.source->parent() != this) return false;
    // The store's Repair (HudServices.cpp): a worn item clicked is mended.
    if (context_.servicePick == 1) {
        for (int i = 0; i < 13; ++i) {
            if (slots_[std::size_t(i)] == notice.source) return ServicePick(context_, i);
        }
    }
    switch (notice.control) {
    case 10: yaw_ -= frameMs_ * 0.0015f; return true; // a step a click: the frame's ms x 0.0015
    case 11: yaw_ += frameMs_ * 0.0015f; return true;
    case 12: yaw_ = 0.0f; return true;
    case 13: ShowView(true); return true;
    case 14: ShowView(false); return true;
    default: return false;
    }
}

// --- The sockets' skill window (500) ------------------------------------------------------------------------------------

void EquipSkillWindow::Created(ui::Interface& ui) {
    // Its own layout's Create section; its CIFSkillSlots made as the slots ours draws (fills them).
    if (const assets::UiLayout* layout = ui.Layout("resinfo\\ifequipskillslotwnd.txt")) {
        for (const auto& section : layout->sections) {
            if (section.name != "Create") continue;
            for (auto it = section.nodes.rbegin(); it != section.nodes.rend(); ++it) {
                assets::UiLayoutNode n = *it;
                if (n.type == "CIFSkillSlot") n.type = "CIFSlotWithHelp";
                Add(ui.Make(n));
            }
        }
    }
    for (int k = 0; k < 4; ++k) {
        auto* slot = ui::As<ui::Slot>(Child(k + 1));
        slots_[std::size_t(k)] = slot;
        if (slot == nullptr) continue;
        slot->pickable = false;
        slot->tooltipSigned = false;
        slot->tooltipLines = [this, k] {
            const assets::SkillRow* row = context_.skills != nullptr && skills_[std::size_t(k)] != 0 ? context_.skills->Find(skills_[std::size_t(k)]) : nullptr;
            return row != nullptr ? SkillTooltip(*row, context_) : std::vector<ui::TooltipLine>{};
        };
    }
    visible = false;
}

void EquipSkillWindow::Show(const std::vector<std::uint32_t>& skills, int place) {
    skills_.fill(0);
    for (std::size_t k = 0; k < skills.size() && k < skills_.size(); ++k) skills_[k] = skills[k];
    place_ = place;
    visible = !skills.empty();
    for (std::size_t k = 0; k < slots_.size(); ++k) {
        ui::Slot* slot = slots_[k];
        if (slot == nullptr) continue;
        const assets::SkillRow* row = context_.skills != nullptr && skills_[k] != 0 ? context_.skills->Find(skills_[k]) : nullptr;
        slot->icon = row != nullptr && !row->iconPath.empty() ? "icon\\" + row->iconPath : std::string();
        // An active one is dragged to the quick slots; a passive one runs by itself, veiled (ours: the original's grey).
        slot->pickable = row != nullptr && row->activity != 0;
        slot->ghost = row != nullptr && row->activity == 0;
    }
}

void EquipSkillWindow::Update(ui::Frame& frame) {
    Widget::Update(frame);
    // The actives' waits, as their quick slots show them.
    for (std::size_t k = 0; k < slots_.size(); ++k) {
        if (slots_[k] == nullptr) continue;
        const HudContext::Cooling c = skills_[k] != 0 && context_.skillCooling ? context_.skillCooling(skills_[k]) : HudContext::Cooling{};
        slots_[k]->cooling = c.cooling;
        slots_[k]->charge = c.charge;
    }
}

namespace {
EquipmentWindow* FindEquipment(ui::Widget* w) {
    if (w == nullptr) return nullptr;
    if (auto* e = dynamic_cast<EquipmentWindow*>(w)) return e;
    for (const auto& c : w->children()) {
        if (EquipmentWindow* e = FindEquipment(c.get())) return e;
    }
    return nullptr;
}
} // namespace

namespace {
InventoryWindow* FindInventory(ui::Widget* w) {
    if (w == nullptr) return nullptr;
    if (auto* i = dynamic_cast<InventoryWindow*>(w)) return i;
    for (const auto& c : w->children()) {
        if (InventoryWindow* i = FindInventory(c.get())) return i;
    }
    return nullptr;
}
} // namespace

void FlashItemSlot(HudContext& c, int place, ui::Slot::Flash kind) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    ui::Widget* popup = ui != nullptr ? ui->Window(hud::kMainPopup) : nullptr;
    if (popup == nullptr) return;
    ui::Slot* slot = nullptr;
    if (place < 13) {
        if (EquipmentWindow* e = FindEquipment(popup)) slot = e->SlotOf(place);
    } else if (InventoryWindow* i = FindInventory(popup)) {
        slot = i->SlotOf(place);
    }
    if (slot != nullptr) slot->Play(kind);
}

bool ShowWornSocketSkills(HudContext& c, int place) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    if (ui == nullptr) return false;
    c.hud->OpenPage(hud::kPageEquipment);
    EquipmentWindow* equipment = FindEquipment(ui->Window(hud::kMainPopup));
    if (equipment == nullptr) return false;
    equipment->ShowSocketSkills(place);
    const EquipSkillWindow* skills = equipment->SkillWindow();
    return skills != nullptr && skills->visible;
}

std::uint32_t WornSocketSkill(HudContext& c, int index, bool dragged) {
    ui::Interface* ui = c.hud != nullptr ? c.hud->interface() : nullptr;
    EquipmentWindow* equipment = ui != nullptr ? FindEquipment(ui->Window(hud::kMainPopup)) : nullptr;
    EquipSkillWindow* skills = equipment != nullptr ? equipment->SkillWindow() : nullptr;
    if (skills == nullptr || !skills->visible || index < 0 || index >= 4) return 0;
    // Dragged: what a drop on a quick slot takes (DraggedSkill: an active's id, a passive's none).
    if (dragged) return DraggedSkill(skills->Child(index + 1));
    return skills->skills()[std::size_t(index)];
}

std::uint32_t EquipSkillWindow::SkillOf(const ui::Widget* w) const {
    for (std::size_t k = 0; k < slots_.size(); ++k) {
        if (w == slots_[k] && skills_[k] != 0 && context_.skills != nullptr) {
            const assets::SkillRow* row = context_.skills->Find(skills_[k]);
            return row != nullptr && row->activity != 0 ? row->id : 0;
        }
    }
    return 0;
}

} // namespace sr::client
