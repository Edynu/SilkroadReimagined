// A magic option's text as the 1.188 client writes it: the item tooltip's blue lines (each option in id order; the
// avatars' second line from its own table) and the names the alchemy stone's result lines use. The client goes by the
// option's name
// (magicoption.txt col 2), each with its own text keys and format; a name it does not know gets no line.

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "gui/HudWindows.h"
#include "sr/assets/GameData.h"
#include "sr/game/Alchemy.h"

namespace sr::client {

namespace {

// The row of the option at the item's degree (a positive row of another level is looked up again at the item's; none:
// no line).
const assets::MagicOptionRow* RowAtDegree(const std::vector<assets::MagicOptionRow>& options, const assets::MagicOptionRow& row, int degree) {
    if (row.degree == degree) return &row;
    return game::alchemy::OptionRow(options, row.optionName, degree);
}

// "(+X%)": the value's place between the row's first and last values (the non-zero halves of Param2..4, high
// first, up to the first zero), truncated, at least 0.
int PlaceInRange(const assets::MagicOptionRow& row, std::uint32_t value) {
    std::vector<std::uint32_t> halves;
    for (const std::uint32_t word : row.rangeWords) {
        bool stop = false;
        for (const std::uint32_t half : {word >> 16, word & 0xFFFF}) {
            if (half == 0) {
                stop = true;
                break;
            }
            halves.push_back(half);
        }
        if (stop) break;
    }
    if (halves.size() < 2 || halves.back() == halves.front()) return 0;
    const double x = (double(value) - double(halves.front())) / (double(halves.back()) - double(halves.front())) * 100.0;
    return std::max(0, int(std::trunc(x)));
}

// The avatars' table: option -> its stat's key and the word after it.
struct AvatarName {
    const char* option;
    const char* stat;
    const char* suffix;
};
constexpr AvatarName kAvatarNames[] = {
    {"AVATAR_STR", "PARAM_STR", "PARAM_INCREASE"},           {"AVATAR_STR_2", "PARAM_STR", "PARAM_INCREASE"},
    {"AVATAR_STR_3", "PARAM_STR", "PARAM_INCREASE"},         {"AVATAR_STR_4", "PARAM_STR", "PARAM_INCREASE"},
    {"AVATAR_INT", "PARAM_INT", "PARAM_INCREASE"},           {"AVATAR_INT_2", "PARAM_INT", "PARAM_INCREASE"},
    {"AVATAR_INT_3", "PARAM_INT", "PARAM_INCREASE"},         {"AVATAR_INT_4", "PARAM_INT", "PARAM_INCREASE"},
    {"AVATAR_ER", "PARAM_ER", "PARAM_INCREASE"},             {"AVATAR_MP", "PARAM_MP", "PARAM_INCREASE"},
    {"AVATAR_HP", "PARAM_HP", "PARAM_INCREASE"},             {"AVATAR_HR", "PARAM_HR", "PARAM_INCREASE"},
    {"AVATAR_DRUA", "PARAM_AVATAR_DRUA", "PARAM_INCREASE"},  {"AVATAR_DARA", "PARAM_AVATAR_DARA", "PARAM_INCREASE"},
    {"AVATAR_MDIA", "PARAM_AVATAR_MDIA", "UIIT_STT_PROBABILITY"},     {"AVATAR_MDIA_2", "PARAM_AVATAR_MDIA", "UIIT_STT_PROBABILITY"},
    {"AVATAR_MDIA_3", "PARAM_AVATAR_MDIA", "UIIT_STT_PROBABILITY"},   {"AVATAR_MDIA_4", "PARAM_AVATAR_MDIA", "UIIT_STT_PROBABILITY"},
    {"AVATAR_HPRG", "PARAM_AVATAR_HPRG", "PARAM_INCREASE"},  {"AVATAR_MPRG", "PARAM_AVATAR_MPRG", "PARAM_INCREASE"},
    {"AVATAR_LUCK", "PARAM_LUCK", "PARAM_INCREASE"},         {"AVATAR_LUCK_2", "PARAM_LUCK", "PARAM_INCREASE"},
    {"AVATAR_LUCK_3", "PARAM_LUCK", "PARAM_INCREASE"},       {"AVATAR_LUCK_4", "PARAM_LUCK", "PARAM_INCREASE"},
    {"APE", "PARAM_ATTR_ASSIMILATION", "UIIT_STT_COUNT"},    {"REINFORCE_ITEM_SET", "PARAM_REINFORCE_ITEM_SET", "PARAM_REINFORCE"},
    {"RESIST_ALL_SET", "PARAM_RESIST_ALL_SET", "PARAM_INCREASE"},
};

std::string Bare(std::string_view name) { return std::string(name.starts_with("MATTR_") ? name.substr(6) : name); }
bool Is(const std::string& n, std::initializer_list<const char*> names) {
    for (const char* x : names)
        if (n == x) return true;
    return false;
}

// The resists' names (the tooltip's "name and duration" part): frostbite with freezing before it.
std::string ResistName(const HudContext& c, const std::string& n) {
    if (n == "RESIST_FROSTBITE") return c.Text("PARAM_FZ") + "," + c.Text("PARAM_FB") + c.Text("PARAM_HOUR");
    const char* key = n == "RESIST_ESHOCK" ? "PARAM_ES" : n == "RESIST_BURN" ? "PARAM_BU" : n == "RESIST_POISON" ? "PARAM_PS" : "PARAM_ZB";
    return c.Text(key) + c.Text("PARAM_HOUR");
}
const char* ChanceResistKey(const std::string& n) {
    return n == "RESIST_STUN" ? "PARAM_STUN" : n == "RESIST_CSMP" ? "PARAM_CURSIE_MP" : n == "RESIST_DISEASE" ? "PARAM_DISEASE" : n == "RESIST_SLEEP" ? "PARAM_SLEEP" : "PARAM_FEAR";
}

} // namespace

MagicOptionText MagicOptionLine(const HudContext& c, const assets::ItemRow& item, const game::ItemInstance::Option& o) {
    MagicOptionText out;
    if (c.magicOptions == nullptr) return out;
    const assets::MagicOptionRow* own = game::FindOption(*c.magicOptions, o.id);
    if (own == nullptr) return out;
    const std::string n = Bare(own->optionName);
    // '+' first, then '-' ('@' is never looked at).
    out.negative = own->paramName.find('+') == std::string::npos && own->paramName.find('-') != std::string::npos;
    const int tid3 = item.typeId[2];
    // Avatars (TID3 13): the table's line alone for their own options.
    if (tid3 == 13) {
        for (const AvatarName& a : kAvatarNames) {
            if (n != a.option) continue;
            const std::string stat = c.Text(a.stat), suffix = c.Text(a.suffix);
            const bool plain = std::string_view(a.stat).find("STR") != std::string_view::npos || std::string_view(a.stat).find("INT") != std::string_view::npos ||
                               std::string_view(a.stat) == "PARAM_HP" || std::string_view(a.stat) == "PARAM_MP";
            out.text = plain ? std::format("{} {} {}", stat, o.value, suffix) : std::format("{} {}% {}", stat, o.value, suffix);
            return out;
        }
    }
    const std::string inc = c.Text("PARAM_INCREASE"), dec = c.Text("PARAM_DECREASE"), count = c.Text("UIIT_STT_COUNT");
    const std::uint32_t v = o.value;
    if (out.negative) {
        if (n == "DEC_MAXDUR" && tid3 != 5 && tid3 != 12) out.text = std::format("{} {}% {}", c.Text("PARAM_MAX_DURABILITY"), v, dec);
        // Repair invalid shows its row's own Param3 (400), not the item's value.
        if (n == "NOT_REPARABLE") {
            const assets::MagicOptionRow* row = game::alchemy::OptionRow(*c.magicOptions, own->optionName, own->degree);
            (void)row;
            out.text = std::format("{} ({} {}% {})", c.Text("PARAM_NOT_REPAIRABLE"), c.Text("PARAM_MAX_DURABILITY"), 400, inc);
        }
        return out;
    }
    const assets::MagicOptionRow* row = RowAtDegree(*c.magicOptions, *own, game::alchemy::Degree(item));
    if (row == nullptr) return out; // no row at the item's degree: no line
    const bool cut = (tid3 == 7 || tid3 == 13 || tid3 == 14) && n != "NASRUN_UMDU";
    const std::string x = cut ? std::string() : std::format(" (+{}%)", PlaceInRange(*row, v));
    const std::uint32_t shown = v == 0 ? std::uint32_t(row->degree) : v;
    if (Is(n, {"STR", "STR_SET", "INT", "INT_SET"})) {
        out.text = std::format("{} {} {}{}", c.Text(n.starts_with("STR") ? "PARAM_STR" : "PARAM_INT"), shown, inc, x);
    } else if (Is(n, {"DUR", "DUR_SET", "HR", "ER", "ER_SET"})) {
        out.text = std::format("{} {}% {}{}", c.Text(n.starts_with("DUR") ? "PARAM_DUR" : n == "HR" ? "PARAM_HR" : "PARAM_ER"), v, inc, x);
    } else if (n == "EVADE_BLOCK") {
        out.text = std::format("{} {}{}", c.Text("PARAM_IGNORE_BLOCKING"), v, x);
    } else if (n == "EVADE_CRITICAL") {
        out.text = std::format("{} {}{}", c.Text("PARAM_EVADE_CRITICAL"), v, x);
    } else if (Is(n, {"HP", "HP_SET", "MP", "MP_SET"})) {
        out.text = std::format("{} {} {}{}", c.Text(n.starts_with("HP") ? "PARAM_HP" : "PARAM_MP"), v, inc, x);
    } else if (Is(n, {"CRITICAL", "BLOCKRATE", "NASRUN_BLOCKRATE"})) {
        out.text = std::format("{} {} {}{}", c.Text(n == "CRITICAL" ? "PARAM_CRITICAL" : "PARAM_BLOCKING"), v, inc, x);
    } else if (Is(n, {"RESIST_FROSTBITE", "RESIST_ESHOCK", "RESIST_BURN", "RESIST_POISON", "RESIST_ZOMBIE"})) {
        out.text = std::format("{} {}% {}{}", ResistName(c, n), v, dec, x);
    } else if (Is(n, {"RESIST_STUN", "RESIST_CSMP", "RESIST_DISEASE", "RESIST_SLEEP", "RESIST_FEAR"})) {
        out.text = std::format("{}{} {}% {}{}", c.Text(ChanceResistKey(n)), c.Text("UIIT_STT_PROBABILITY"), v, dec, x);
    } else if (n == "REGENHPMP") {
        out.text = std::format("{}/{} {}% {}{}", c.Text("PARAM_HEAL_HP"), c.Text("PARAM_HEAL_MP"), v, inc, x);
    } else if (Is(n, {"ATHANASIA", "SOLID", "LUCK", "LUCK_SET"})) {
        out.text = std::format("{}({}{})", c.Text(n == "ATHANASIA" ? "PARAM_ATHANASIA" : n == "SOLID" ? "PARAM_SOLID" : "PARAM_LUCK"), v, count);
    } else if (n == "ASTRAL") {
        out.text = std::format("{} {} {}", c.Text("PARAM_ASTRAL"), v, count);
    } else if (n == "APE") {
        out.text = std::format("{}: {} {}", c.Text("PARAM_ATTR_ASSIMILATION"), v, count);
    } else if (n == "REPAIR") {
        out.text = std::format("{} ({}{})", c.Text("PARAM_REPAIR"), v > 1 ? v - 1 : 0, count);
    } else if (n == "REINFORCE_ITEM") {
        // +1 or +2 for a time: seconds, or minutes and seconds from a minute.
        const std::uint32_t plus = row->rangeWords[1] != 0 ? row->rangeWords[1] : 1;
        out.text = v >= 60000 ? std::format("+{} {}({}{} {}{})", plus, c.Text("PARAM_REINFORCE"), v / 60000, c.Text("PARAM_MINUTE"), (v % 60000) / 1000,
                                            c.Text("PARAM_SECOND"))
                              : std::format("+{} {}({}{})", plus, c.Text("PARAM_REINFORCE"), v / 1000, c.Text("PARAM_SECOND"));
    } else if (Is(n, {"STR_3JOB", "INT_3JOB"})) {
        out.text = std::format("{} {} {}", c.Text(n.starts_with("STR") ? "PARAM_STR" : "PARAM_INT"), row->rangeWords[0] >> 16, inc);
    } else if (Is(n, {"STR_AVATAR", "INT_AVATAR"})) {
        out.text = std::format("{} {} {}", c.Text(n.starts_with("STR") ? "PARAM_STR" : "PARAM_INT"), shown, inc);
    } else if (Is(n, {"NASRUN_HPNA", "NASRUN_MPNA"})) {
        out.text = std::format("{}{} {}% {}", c.Text("PARAM_MAX"), c.Text(n == "NASRUN_HPNA" ? "PARAM_HP" : "PARAM_MP"), v, inc);
    } else if (n == "NASRUN_UMDU") {
        out.text = std::format("{}% {}{}", v, c.Text("PARAM_NASRUN_UMDU"), x);
    }
    return out;
}

MagicOptionNames MagicOptionAlchemyNames(const HudContext& c, std::string_view optionName, std::uint32_t value) {
    MagicOptionNames out;
    const std::string n = Bare(optionName);
    const std::string inc = c.Text("PARAM_INCREASE"), dec = c.Text("PARAM_DECREASE");
    const auto with = [](const std::string& key, const std::string& word) { return key + " " + word; };
    if (Is(n, {"STR", "INT", "HP", "MP"})) {
        const std::string key = c.Text(n == "STR" ? "PARAM_STR" : n == "INT" ? "PARAM_INT" : n == "HP" ? "PARAM_HP" : "PARAM_MP");
        out.append = std::format("{} {} {}", key, value, inc);
        out.change = key;
    } else if (Is(n, {"CRITICAL", "BLOCKRATE"})) {
        const std::string key = c.Text(n == "CRITICAL" ? "PARAM_CRITICAL" : "PARAM_BLOCKING");
        out.append = std::format("{} {} {}", key, value, inc);
        out.change = with(key, inc);
    } else if (Is(n, {"DUR", "HR", "ER"})) {
        const std::string key = c.Text(n == "DUR" ? "PARAM_DUR" : n == "HR" ? "PARAM_HR" : "PARAM_ER");
        out.append = std::format("{} {}% {}", key, value, inc);
        out.change = with(key, inc);
        out.percent = true;
    } else if (Is(n, {"EVADE_BLOCK", "EVADE_CRITICAL"})) {
        const std::string key = c.Text(n == "EVADE_BLOCK" ? "PARAM_IGNORE_BLOCKING" : "PARAM_EVADE_CRITICAL");
        out.append = std::format("{} {}", key, value);
        out.change = key;
    } else if (Is(n, {"RESIST_FROSTBITE", "RESIST_ESHOCK", "RESIST_BURN", "RESIST_POISON", "RESIST_ZOMBIE"})) {
        out.append = std::format("{} {}% {}", ResistName(c, n), value, dec);
        out.change = with(ResistName(c, n), dec);
        out.percent = true;
    } else if (Is(n, {"RESIST_STUN", "RESIST_CSMP", "RESIST_DISEASE", "RESIST_SLEEP", "RESIST_FEAR"})) {
        const std::string key = c.Text(ChanceResistKey(n)), chance = c.Text("UIIT_STT_PROBABILITY");
        out.append = std::format("{} {} {} % {}", key, chance, value, dec);
        out.change = key + " " + chance + " " + dec;
        out.percent = true;
    } else if (n == "REGENHPMP") {
        out.append = std::format("{}/{} {} % {}", c.Text("PARAM_HEAL_HP"), c.Text("PARAM_HEAL_MP"), value, inc);
        out.change = std::format("{}/{} {}", c.Text("PARAM_HEAL_HP"), c.Text("PARAM_HEAL_MP"), inc);
        out.percent = true;
    } else if (Is(n, {"ATHANASIA", "SOLID", "LUCK", "ASTRAL", "REPAIR", "APE", "NASRUN_HPNA", "NASRUN_MPNA"})) {
        const char* key = n == "ATHANASIA" ? "PARAM_ATHANASIA" : n == "SOLID" ? "PARAM_SOLID" : n == "LUCK" ? "PARAM_LUCK" : n == "ASTRAL" ? "PARAM_ASTRAL"
                          : n == "REPAIR" ? "PARAM_REPAIR" : n == "APE" ? "PARAM_ATTR_ASSIMILATION" : n == "NASRUN_HPNA" ? "PARAM_NASRUN_HPNA" : "PARAM_NASRUN_MPNA";
        out.append = c.Text(key); // the key alone, no number
        out.change = c.Text(key);
        out.percent = n == "NASRUN_HPNA" || n == "NASRUN_MPNA";
    } else if (n == "NASRUN_UMDU") {
        out.append = std::format("{}% {}", value, c.Text("PARAM_NASRUN_UMDU"));
        out.change = c.Text("PARAM_NASRUN_UMDU");
        out.percent = true;
    } else {
        out.append = out.change = c.Text(std::string(optionName));
    }
    return out;
}

} // namespace sr::client
