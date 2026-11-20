#include "sr/server/ItemLab.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <functional>
#include <span>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"
#include "sr/game/Alchemy.h"
#include "sr/game/Cos.h"
#include "sr/game/Guild.h"
#include "sr/game/GuildRecall.h"
#include "sr/game/ItemCatalogue.h"
#include "sr/game/Items.h"
#include "sr/game/MagicPop.h"
#include "sr/game/Mall.h"
#include "sr/game/MallServices.h"
#include "sr/game/Potions.h"
#include "sr/game/SkillText.h"
#include "sr/game/States.h"
#include "sr/game/TimedServices.h"
#include "sr/server/Database.h"

namespace sr::server {

namespace {

using json = nlohmann::json;

constexpr const char* kTextData = "server_dep\\silkroad\\textdata\\";

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string TypeIds(const assets::ItemRow& row) {
    return std::format("{}/{}/{}/{}", row.typeId[0], row.typeId[1], row.typeId[2], row.typeId[3]);
}

// A float as the data has it: whole numbers as integers, others to three places.
json Number(double v) {
    if (std::abs(v - std::round(v)) < 1e-6) return std::int64_t(std::llround(v));
    return std::round(v * 1000.0) / 1000.0;
}

// The white stats' columns (cols 63..117, _RefObjItem's names), each pair low / high and its plus where it has one.
struct WhiteColumn {
    const char* name;  // ours
    const char* label; // the tooltip's word
    int low, high, perPlus;
};
constexpr WhiteColumn kWhite[] = {
    {"durability", "Durability", 63, 64, 0},
    {"phyDefence", "Phy. defence", 65, 66, 67},
    {"parry", "Parry rate", 68, 69, 70},
    {"phyAbsorption", "Phy. absorption", 71, 72, 73},
    {"blocking", "Blocking rate", 74, 75, 0},
    {"magDefence", "Mag. defence", 76, 77, 78},
    {"magAbsorption", "Mag. absorption", 79, 80, 81},
    {"phyReinforce", "Phy. reinforce (tenths of %)", 82, 83, 0},
    {"magReinforce", "Mag. reinforce (tenths of %)", 84, 85, 0},
    {"phyAttackMin", "Phy. attack (min)", 95, 96, 99},
    {"phyAttackMax", "Phy. attack (max)", 97, 98, 99},
    {"magAttackMin", "Mag. attack (min)", 100, 101, 104},
    {"magAttackMax", "Mag. attack (max)", 102, 103, 104},
    {"phyReinforceMin", "Phy. reinforce min (tenths of %)", 105, 106, 0},
    {"phyReinforceMax", "Phy. reinforce max (tenths of %)", 107, 108, 0},
    {"magReinforceMin", "Mag. reinforce min (tenths of %)", 109, 110, 0},
    {"magReinforceMax", "Mag. reinforce max (tenths of %)", 111, 112, 0},
    {"hitRate", "Attack rating", 113, 114, 115},
    {"critical", "Critical", 116, 117, 0},
};

json StatsShown(const game::ItemStats& s) {
    json out = json::object();
    const auto put = [&](const char* key, const game::ItemStat& st, bool range) {
        if (!st.shown) return;
        out[key] = range ? json{Number(st.low), Number(st.high)} : Number(st.low);
    };
    put("phyAttack", s.phyAttack, true);
    put("magAttack", s.magAttack, true);
    put("phyDefence", s.phyDefence, false);
    put("magDefence", s.magDefence, false);
    put("phyAbsorption", s.phyAbsorption, false);
    put("magAbsorption", s.magAbsorption, false);
    if (s.durability.shown) out["durability"] = Number(s.durability.high);
    put("attackRating", s.attackRating, false);
    put("parry", s.parry, false);
    put("critical", s.critical, false);
    put("blocking", s.blocking, false);
    put("phyReinforce", s.phyReinforce, s.phyReinforce.low != s.phyReinforce.high);
    put("magReinforce", s.magReinforce, s.magReinforce.low != s.magReinforce.high);
    if (s.range > 0) out["range"] = Number(s.range);
    return out;
}

// What the original's equipment holds: the columns as written, and the tooltip's numbers of a new item of the worst
// and of the best white stats (variance 0 and 31 in every slot) at +0, and each plus's addition.
json Stats(const assets::ItemRow& row) {
    json columns = json::array();
    for (const WhiteColumn& c : kWhite) {
        const float lo = row.Stat(c.low), hi = row.Stat(c.high), inc = c.perPlus > 0 ? row.Stat(c.perPlus) : 0.0f;
        if (lo == 0.0f && hi == 0.0f && inc == 0.0f) continue;
        json j = {{"name", c.name}, {"label", c.label}, {"columns", std::format("{}..{}", c.low, c.high)}, {"low", Number(lo)}, {"high", Number(hi)}};
        if (c.perPlus > 0) j["perPlus"] = Number(inc);
        columns.push_back(std::move(j));
    }
    json weapon = json::object();
    if (game::KindOf(row) == game::ItemKind::Weapon) {
        weapon["range"] = Number(row.Stat(94) / 10.0);
        weapon["twoHanded"] = game::TwoHanded(row);
        weapon["speedClass"] = Number(row.Stat(92));
        weapon["quivered"] = row.Stat(86) != 0.0f;
        if (const int ammo = game::AmmunitionFor(row); ammo != 0) weapon["shoots"] = ammo == 1 ? "arrows" : "bolts";
    }
    game::ItemInstance worst, best;
    worst.refId = best.refId = row.refObjId;
    best.variance = 0;
    for (int i = 0; i < 12; ++i) best.variance |= std::uint64_t(31) << (5 * i);
    worst.durability = best.durability = game::NewDurability(row);
    json out = {{"columns", std::move(columns)}, {"worst", StatsShown(game::ComputeStats(row, worst, nullptr))}, {"best", StatsShown(game::ComputeStats(row, best, nullptr))}};
    if (!weapon.empty()) out["weapon"] = std::move(weapon);
    return out;
}

// Param1..20 and the text beside each, where either holds something (the texts lost to '?' in the client's files, the
// Korean labels, left out).
json Params(const assets::ItemRow& row) {
    json out = json::array();
    for (std::size_t i = 0; i < row.params.size(); ++i) {
        std::string text = Trim(row.paramTexts[i]);
        if (text.find('?') != std::string::npos) text.clear();
        if ((row.params[i] == -1 || row.params[i] == 0) && text.empty()) continue;
        json p = {{"n", int(i) + 1}, {"value", row.params[i]}};
        if (!text.empty()) p["text"] = text;
        out.push_back(std::move(p));
    }
    return out;
}

const char* CosKindName(game::CosKind k) {
    switch (k) {
    case game::CosKind::Ride: return "ride";
    case game::CosKind::Transport: return "transport";
    case game::CosKind::AttackPet: return "attack pet";
    case game::CosKind::GrabPet: return "pick pet";
    case game::CosKind::Soldier: return "soldier";
    case game::CosKind::None: return "none";
    }
    return "?";
}

const char* AlchemyKindName(game::alchemy::Kind k) {
    using game::alchemy::Kind;
    switch (k) {
    case Kind::None: return "none";
    case Kind::Equipment: return "equipment";
    case Kind::Elixir: return "elixir";
    case Kind::LuckyPowder: return "lucky powder";
    case Kind::AdvancedElixir: return "advanced elixir";
    case Kind::MagicStone: return "magic stone";
    case Kind::AttrStone: return "attribute stone";
    case Kind::Tablet: return "tablet";
    case Kind::ByProduct: return "material";
    case Kind::Element: return "element";
    case Kind::Rondo: return "rondo";
    }
    return "?";
}

const char* PlaceName(int place) {
    switch (place) {
    case game::slot::kHead: return "head";
    case game::slot::kChest: return "chest";
    case game::slot::kShoulders: return "shoulders";
    case game::slot::kHands: return "hands";
    case game::slot::kLegs: return "legs";
    case game::slot::kFeet: return "feet";
    case game::slot::kWeapon: return "weapon";
    case game::slot::kShield: return "shield";
    case game::slot::kJobSuit: return "job suit";
    case game::slot::kEarring: return "earring";
    case game::slot::kNecklace: return "necklace";
    case game::slot::kRingLeft:
    case game::slot::kRingRight: return "ring";
    default: return "none";
    }
}

class Maker {
public:
    explicit Maker(const ItemLabData& d) : d_(d) {
        for (const assets::MasteryRow& m : d.skills.masteries()) masteries_[m.id] = &m;
    }

    std::string Text(const std::string& key) const {
        const auto it = d_.text.find(key);
        return it != d_.text.end() ? it->second : std::string();
    }

    json Skill(const std::string& code) const {
        const assets::SkillRow* s = d_.skills.Find(std::string_view(code));
        if (s == nullptr) return {{"code", code}, {"found", false}};
        const game::SkillTextLookup lookup = [&](const std::string& key) {
            const auto it = d_.text.find(key);
            return it != d_.text.end() ? it->second : key;
        };
        json j = {{"code", code}, {"found", true}, {"id", s->id}, {"name", Text(s->nameKey)}, {"effects", game::SkillEffectLines(*s, &d_.skills, lookup)}};
        if (s->hasDuration) j["durationMs"] = s->buffDurationMs;
        if (!s->iconPath.empty()) j["icon"] = s->iconPath;
        return j;
    }

    json Requirements(const assets::ItemRow& row) const {
        json out = json::array();
        for (std::size_t i = 0; i < row.requirementTypes.size(); ++i) {
            const int type = row.requirementTypes[i];
            if (type == 0 || type == -1) continue;
            std::string name;
            if (const char* n = game::RequirementName(type)) {
                name = n;
            } else if (const auto m = masteries_.find(std::uint32_t(type)); m != masteries_.end()) {
                name = "mastery " + Text(m->second->nameKey);
            } else {
                name = std::format("type {}", type);
            }
            out.push_back({{"type", type}, {"name", name}, {"level", row.requirementLevels[i]}});
        }
        if (row.requiredStr > 0) out.push_back({{"type", "str"}, {"name", "STR"}, {"level", row.requiredStr}});
        if (row.requiredInt > 0) out.push_back({{"type", "int"}, {"name", "INT"}, {"level", row.requiredInt}});
        return out;
    }

    // What using it does by its data, by the path it takes (and by its kind for the ones never used).
    json Use(const assets::ItemRow& row, const game::ItemCatalogueEntry& e) const {
        using game::ItemRoute;
        json u = json::object();
        const auto& p = row.params;
        switch (e.routing.route) {
        case ItemRoute::Equip:
        case ItemRoute::JobSuit:
            u["place"] = PlaceName(game::EquipPlaceFor(row, false, false));
            if (const int sockets = game::alchemy::SocketCount(row); sockets > 0) u["sockets"] = sockets;
            u["mostMagicOptions"] = row.maxMagicOptions;
            if (row.setId != 0) u["set"] = row.setId;
            u["repair"] = {{"canRepair", row.canRepair}, {"costRepair", row.costRepair}, {"canRevive", row.canRevive}, {"costRevive", row.costRevive}};
            if (e.routing.route == ItemRoute::JobSuit) u["job"] = row.typeId[3] == 1 ? "trader" : row.typeId[3] == 2 ? "thief" : "hunter";
            break;
        case ItemRoute::Avatar: {
            static const char* const kPlaces[] = {"hat", "dress", "attachment", "flag", "devil's spirit"};
            const int place = game::AvatarPlaceFor(row);
            u["place"] = place >= 0 && place < 5 ? kPlaces[place] : "none";
            if (row.childItems > 0) u["takesAttachment"] = true;
            if (game::IsDevilSpirit(row)) u["awakeningSeconds"] = p[0];
            if (row.maxMagicOptions > 0) u["mostMagicOptions"] = row.maxMagicOptions;
            break;
        }
        case ItemRoute::Ammunition:
            u["for"] = row.typeId[3] == 1 ? "bow" : "crossbow";
            u["stack"] = {{"data", row.maxStack}, {"ours", game::MaxStack(row)}};
            break;
        case ItemRoute::Recovery:
            if (game::IsPotion(row)) {
                u["hp"] = std::max(0, p[0]);
                u["hpPercent"] = std::max(0, p[1]);
                u["mp"] = std::max(0, p[2]);
                u["mpPercent"] = std::max(0, p[3]);
                u["flat"] = p[0] > 0 || p[2] > 0;
                u["formula"] = "flat: Param1 HP and Param3 MP x 1.02^(level - 1) x (1 + STR or INT / 416), in five steps a second apart; "
                               "else Param2 % of the most HP and Param4 % of the most MP at once";
            } else {
                const game::CureSpec c = game::PillCureOf(row);
                u["cure"] = {{"elementMask", c.elementMask}, {"elementLevel", c.elementLevel}, {"curseMask", c.curseMask}, {"curseChance", c.curseChance},
                             {"curseLevel", c.curseLevel}, {"most", c.most}};
            }
            [[fallthrough]];
        case ItemRoute::PetRecovery: {
            if (e.routing.route == ItemRoute::PetRecovery) {
                u["hp"] = std::max(0, p[0]);
                u["hpPercent"] = std::max(0, p[1]);
            }
            const game::ReuseLock china = game::ReuseLockOf(row, false), europe = game::ReuseLockOf(row, true);
            u["reuse"] = {{"china", {{"bit", china.bit}, {"ms", china.ms}}}, {"europe", {{"bit", europe.bit}, {"ms", europe.ms}}}};
            break;
        }
        case ItemRoute::PetFood: u["hgp"] = {{"percent", p[0]}, {"points", game::PetHgpPotionAmount(row)}}; break;
        case ItemRoute::PetSummon:
            u["pet"] = game::PetCodename(row);
            u["kind"] = CosKindName(game::PetOfItem(row));
            if (const int minutes = game::PetRentalMinutes(row); minutes > 0) u["rentalMinutes"] = minutes;
            break;
        case ItemRoute::CosSummon: {
            u["summons"] = Trim(row.paramTexts[0]);
            json steps = json::array();
            for (int level = 1; level <= 140; ++level) {
                const auto code = game::CosOfScroll(row, level);
                if (!code || (!steps.empty() && steps.back()["cos"] == *code)) continue;
                const assets::ObjectRow* cos = d_.characters.Find(std::string_view(*code));
                steps.push_back({{"from", level}, {"cos", *code}, {"kind", cos != nullptr ? CosKindName(game::CosKindOf(*cos)) : "not found"}});
            }
            u["byLevel"] = std::move(steps);
            break;
        }
        case ItemRoute::PetTreatment:
            u["treatment"] = game::PetTreatmentOf(row) == game::PetTreatment::Revival ? "revival (a dead attack pet)" : "extension (a pick pet's time)";
            break;
        case ItemRoute::BuffScroll: u["skill"] = Skill(row.paramTexts[0]); break;
        case ItemRoute::MonsterMask: u["skill"] = Skill(Trim(row.paramTexts[0])); break;
        case ItemRoute::ReturnScroll:
            u["castMs"] = std::max(100, p[0]);
            u["to"] = "the return point (the race's start town until a gate designates one)";
            break;
        case ItemRoute::ThiefDen:
            u["castMs"] = std::max(100, p[0]);
            u["to"] = "the thieves' town (" + Trim(row.paramTexts[3]) + ")";
            u["only"] = "a thief";
            break;
        case ItemRoute::ReverseReturn:
            u["castMs"] = std::max(100, p[0]);
            u["ways"] = json::array({"1: the recall point (where a return scroll was last read)", "2: the place of death", "3: a point on the map (its level range)"});
            break;
        case ItemRoute::TimeTicket: {
            const game::TimedService s = game::TimedService::Begin(row, game::TimedCategoryOf(row), 0);
            u["category"] = s.category;
            u["days"] = s.days;
            u["bonusSecondsADay"] = s.dailyBonus;
            u["expPercent"] = s.expPercent;
            u["skillPercent"] = s.skillPercent;
            break;
        }
        case ItemRoute::Premium: {
            u["seconds"] = p[0];
            json parts = json::array();
            for (const game::PremiumPart& part : game::PremiumPartsOf(row)) {
                parts.push_back({{"tag", part.tag}, {"target", part.target}, {"period", part.period}, {"count", part.count}, {"args", part.args}});
            }
            u["parts"] = std::move(parts);
            break;
        }
        case ItemRoute::Expansion:
            u["places"] = p[0];
            u["of"] = p[1] == 0 ? "bag" : p[1] == 1 ? "storage" : "?";
            break;
        case ItemRoute::Resurrection:
            u["hpAndMp"] = p[0];
            u["percent"] = p[1];
            u["experienceBackPercent"] = p[2];
            break;
        case ItemRoute::SpiritExtension:
        case ItemRoute::PetGrowth: u["seconds"] = p[0]; break;
        case ItemRoute::PetItem: {
            const game::PetItemJobs jobs = game::PetItemJobsOf(row);
            u["pet"] = jobs.grabPet ? "grab" : "attack";
            u["skill"] = jobs.skill + (jobs.skillByLevel ? "_NNN" : "");
            u["bagPlaces"] = jobs.bagPlaces;
            u["seconds"] = p[0];
            break;
        }
        case ItemRoute::GenderSwitch: u["mostDegree"] = p[0]; break;
        case ItemRoute::SkillWithdraw:
            u["mode"] = game::WithdrawModeOf(row) == game::WithdrawMode::Quest ? "quest (80 % of the SP, gold by the level)" : "mall (all of the SP, no gold)";
            break;
        case ItemRoute::MagicCube:
            u["places"] = game::CubePlaces(row);
            u["keeps"] = p[9] == 1 ? "weapon elixirs" : p[9] == 2 ? "armour elixirs" : p[9] == 4 ? "accessory elixirs" : p[9] == 8 ? "shield elixirs" : "?";
            break;
        case ItemRoute::Hole:
            u["world"] = Trim(row.paramTexts[1]);
            u["grade"] = p[0];
            u["levels"] = {p[2], p[3]};
            u["theme"] = Trim(row.paramTexts[8]);
            u["waitMs"] = row.cooldownMs;
            break;
        case ItemRoute::Talisman:
            if (const assets::CollectionItemRow* t = d_.book.Item(row.codename)) {
                const assets::CollectionThemeRow* theme = d_.book.Theme(t->theme);
                u["theme"] = {{"code", t->theme}, {"name", theme != nullptr ? Text(theme->nameKey) : std::string()}};
                u["slot"] = t->slot;
                u["story"] = Text(t->storyKey);
            }
            break;
        case ItemRoute::GuildRecall: {
            const game::GuildRecallItem r = game::GuildRecallOf(row);
            u["waitSeconds"] = r.waitSeconds;
            u["answerSeconds"] = r.answerSeconds;
            u["guildLevel"] = r.guildLevel;
            break;
        }
        case ItemRoute::GuildCrest:
            if (const auto c = game::CrestItemOf(row.codename)) {
                u["of"] = c->kind == game::proto::kCrestGuild ? "guild" : "union";
                u["deletes"] = c->remove;
            }
            break;
        case ItemRoute::Alchemy:
            u["kind"] = AlchemyKindName(game::alchemy::KindOf(row));
            u["degree"] = game::alchemy::Degree(row);
            if (const std::string option = Trim(row.paramTexts[0]); option.starts_with("MATTR_") || option.starts_with("NATTR_")) {
                u["option"] = option;
                json rows = json::array();
                for (const assets::MagicOptionRow& o : d_.options) {
                    if (o.optionName != option || o.degree != game::alchemy::Degree(row)) continue;
                    rows.push_back({{"id", o.paramId}, {"degree", o.degree}, {"param", o.paramName}, {"on", o.availGroups}});
                }
                if (!rows.empty()) u["optionRows"] = std::move(rows);
            }
            break;
        case ItemRoute::Socket:
            if (game::alchemy::IsSocketStone(row)) {
                u["for"] = p[1] == 1 ? "weapon" : p[1] == 2 ? "armour (chest)" : "?";
                u["grade"] = row.itemClass;
                if (game::alchemy::SocketStoneDisabled(row)) u["disabled"] = "ours lets it into no socket (its 'bldr' never acts in the original)";
                json skills = json::array();
                for (const assets::MagicOptGroupRow* g : game::alchemy::StoneSkills(d_.optionGroups, row)) {
                    const assets::SkillRow* s = d_.skills.Find(g->optId);
                    skills.push_back({{"level", g->optLevel}, {"skill", s != nullptr ? s->codename : std::to_string(g->optId)}, {"name", s != nullptr ? Text(s->nameKey) : ""}});
                }
                u["skills"] = std::move(skills);
            } else {
                u["tool"] = game::alchemy::IsSocketMaker(row) ? "makes a socket" : "erases a socket's stone";
            }
            break;
        case ItemRoute::MagicPop:
            if (game::pop::IsCoupon(row)) u["won"] = game::pop::CouponWon(row);
            else u["card"] = true;
            break;
        case ItemRoute::TradeGood: u["bargain"] = row.typeId[3] == 2; break;
        default: break;
        }
        // Skills its texts name (a buff scroll's, a pet's, a package's parts, ...).
        json linked = json::array();
        for (const std::string& raw : row.paramTexts) {
            const std::string text = Trim(raw);
            for (std::size_t at = text.find("SKILL_"); at != std::string::npos; at = text.find("SKILL_", at + 1)) {
                std::size_t end = at;
                while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_')) ++end;
                const std::string code = text.substr(at, end - at);
                if (d_.skills.Find(std::string_view(code)) != nullptr && std::none_of(linked.begin(), linked.end(), [&](const json& j) { return j["code"] == code; })) {
                    linked.push_back(Skill(code));
                }
                at = end;
                if (at >= text.size()) break;
            }
        }
        if (!linked.empty()) u["linkedSkills"] = std::move(linked);
        if (row.cooldownMs > 0) u["cooldown"] = {{"ms", row.cooldownMs}, {"group", row.cooldownGroup}};
        return u;
    }

    LabItemRow Row(const game::ItemCatalogueEntry& e) const {
        const assets::ItemRow& row = *e.row;
        LabItemRow r;
        r.code = row.codename;
        r.id = row.refObjId;
        r.name = Text(row.nameKey);
        if (r.name.empty()) r.name = row.codename;
        r.typeIds = TypeIds(row);
        r.group = e.kind.group;
        r.kind = e.kind.kind;
        r.route = game::ItemRouteName(e.routing.route);
        r.state = game::ItemStateName(game::ItemStateOf(e.routing));
        r.source = e.source;
        r.race = row.country;
        r.sex = row.sex;
        for (std::size_t i = 0; i < row.requirementTypes.size(); ++i) {
            if (row.requirementTypes[i] == 1) r.level = std::max(r.level, row.requirementLevels[i]);
        }
        const bool equipment = row.typeId[0] == 3 && row.typeId[1] == 1;
        r.degree = equipment ? (row.itemClass > 0 ? (row.itemClass - 1) / 3 + 1 : 0) : game::alchemy::Degree(row);
        r.rarity = row.rarity;
        r.usable = row.canUse;
        r.icon = row.iconPath;

        json rarity = {{"value", row.rarity}};
        if (equipment && row.Rare() && row.itemClass > 0) {
            static const char* const kSeals[] = {"Seal of Star", "Seal of Moon", "Seal of Sun"};
            rarity["seal"] = kSeals[(row.itemClass - 1) % 3];
        }
        rarity["name"] = row.rarity == 0 ? "normal" : row.rarity == 2 ? "rare" : row.rarity == 3 ? "set" : row.rarity == 6 ? "rare set" : row.rarity == 7 ? "legend"
                                                                                                                                       : std::format("rarity {}", row.rarity);
        json j = {
            {"code", row.codename},
            {"id", row.refObjId},
            {"name", r.name},
            {"description", Text(row.descKey)},
            {"nameKey", row.nameKey},
            {"descKey", row.descKey},
            {"icon", row.iconPath},
            {"model", row.modelPath},
            {"dropModel", row.dropModelPath},
            {"linked", row.linkedCodename == "xxx" ? std::string() : row.linkedCodename},
            {"typeIds", row.typeId},
            {"group", r.group},
            {"kind", r.kind},
            {"source", r.source},
            {"race", row.country == 0 ? "China" : row.country == 1 ? "Europe" : row.country == 3 ? "both" : std::format("country {}", row.country)},
            {"sex", row.sex == 0 ? "woman" : row.sex == 1 ? "man" : "either"},
            {"degree", r.degree},
            {"itemClass", row.itemClass},
            {"rarity", std::move(rarity)},
            {"requirements", Requirements(row)},
            {"economy",
             {{"price", row.price},
              {"sellPrice", row.sellPrice},
              {"stack", row.maxStack},
              {"stackOurs", game::MaxStack(row)},
              {"mostCarried", row.maxPossess},
              {"keepingFee", row.keepingFee},
              {"costBorrow", row.costBorrow},
              {"cashItem", row.cashItem}}},
            {"flags",
             {{"canTrade", row.canTrade}, {"canSell", row.canSell}, {"canBuy", row.canBuy}, {"canBorrow", row.canBorrow}, {"canDrop", row.canDrop},
              {"canPick", row.canPick}, {"canRepair", row.canRepair}, {"canRevive", row.canRevive}, {"canUse", row.canUse}, {"canThrow", row.canThrow},
              {"decayMs", row.decayMs}}},
            {"route",
             {{"name", r.route},
              {"text", game::ItemRouteText(e.routing.route)},
              {"handler", game::ItemRouteHandler(e.routing.route)},
              {"why", e.routing.why},
              {"state", r.state}}},
            {"note", game::ItemRouteNote(e.routing.route)},
            {"use", Use(row, e)},
            {"params", Params(row)},
        };
        if (equipment && game::KindOf(row) != game::ItemKind::Avatar && game::KindOf(row) != game::ItemKind::JobSuit) j["stats"] = Stats(row);
        r.data = j.dump(-1, ' ', false, json::error_handler_t::replace);
        return r;
    }

private:
    const ItemLabData& d_;
    std::unordered_map<std::uint32_t, const assets::MasteryRow*> masteries_;
};

} // namespace

bool LoadItemLabData(const assets::FileSystem& files, ItemLabData& out, std::string& error) {
    if (!out.items.Load(files, error)) return false;
    if (!out.skills.Load(files, error)) return false;
    if (!out.characters.Load(files, error)) return false;
    if (!assets::LoadTextCatalog(files, std::string(kTextData) + "textuisystem.txt", out.text, error)) return false;
    if (!assets::LoadTextCatalog(files, std::string(kTextData) + "textdataname.txt", out.text, error)) return false;
    if (const auto file = files.Read(std::string(kTextData) + "magicoption.txt")) {
        if (!assets::DecodeMagicOptions(core::DecodeTextFile(file->Bytes()), out.options, error)) return false;
    }
    if (const auto file = files.Read(std::string(kTextData) + "refmagicoptgroup.txt")) {
        if (!assets::DecodeMagicOptGroups(core::DecodeTextFile(file->Bytes()), out.optionGroups, error)) return false;
    }
    if (!out.book.Load(files, error)) return false;
    for (const assets::CollectionThemeRow& theme : out.book.Themes()) {
        for (int slot = 1; slot <= 32; ++slot) {
            if (const assets::CollectionItemRow* item = out.book.Item(theme.codename, slot)) out.talismans.insert(item->item);
        }
    }
    return true;
}

std::vector<LabItemRow> MakeLabItemRows(const ItemLabData& data) {
    const game::ItemRouteData routeData{&data.skills, &data.characters, &data.talismans};
    const Maker maker(data);
    std::vector<LabItemRow> out;
    for (const game::ItemCatalogueEntry& e : game::BuildItemCatalogue(data.items, routeData)) out.push_back(maker.Row(e));
    return out;
}

bool SaveLabItems(Database& db, const std::vector<LabItemRow>& rows, const std::string& build, std::string& error) {
    return db.Transaction(
        [&](Database& d) {
            if (!d.Query("CREATE TEMP TABLE lab_items_seen (code varchar(128) PRIMARY KEY) ON COMMIT DROP").Ok()) return false;
            for (const LabItemRow& r : rows) {
                const QueryResult q = d.Query(
                    "INSERT INTO sro_vt_account.sr_lab_items (code, item_id, name, type_ids, item_group, kind, route, source, race, sex, level, degree, rarity, "
                    "usable, icon, data, build, updated_at) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16::jsonb, $17, now()) "
                    "ON CONFLICT (code) DO UPDATE SET item_id = EXCLUDED.item_id, name = EXCLUDED.name, type_ids = EXCLUDED.type_ids, "
                    "item_group = EXCLUDED.item_group, kind = EXCLUDED.kind, route = EXCLUDED.route, source = EXCLUDED.source, race = EXCLUDED.race, "
                    "sex = EXCLUDED.sex, level = EXCLUDED.level, degree = EXCLUDED.degree, rarity = EXCLUDED.rarity, usable = EXCLUDED.usable, "
                    "icon = EXCLUDED.icon, data = EXCLUDED.data, build = EXCLUDED.build, updated_at = now()",
                    {r.code, std::int64_t(r.id), r.name, r.typeIds, r.group, r.kind, r.route, r.source, r.race, r.sex, r.level, r.degree, r.rarity, r.usable,
                     r.icon, r.data, build});
                if (!q.Ok()) {
                    error = r.code + ": " + q.Error();
                    return false;
                }
                if (!d.Query("INSERT INTO lab_items_seen (code) VALUES ($1) ON CONFLICT DO NOTHING", {r.code}).Ok()) return false;
            }
            return d.Query("DELETE FROM sro_vt_account.sr_lab_items s WHERE NOT EXISTS (SELECT 1 FROM lab_items_seen n WHERE n.code = s.code)").Ok();
        },
        error);
}

} // namespace sr::server
