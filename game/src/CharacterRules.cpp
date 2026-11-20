#include "sr/game/CharacterRules.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>

#include "sr/assets/Dress.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Text.h"

namespace sr::game {

namespace {

// UTF-8 -> code points; false on malformed input.
bool Decode(std::string_view text, std::u32string& out) {
    out.clear();
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        int extra = 0;
        char32_t cp = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            return false;
        }
        if (i + std::size_t(extra) >= text.size()) return false; // a sequence cut short
        for (int k = 1; k <= extra; ++k) {
            const unsigned char d = static_cast<unsigned char>(text[i + std::size_t(k)]);
            if ((d & 0xC0) != 0x80) return false;
            cp = cp << 6 | (d & 0x3F);
        }
        out.push_back(cp);
        i += std::size_t(extra) + 1;
    }
    return true;
}

// The original lowers A-Z only.
std::u32string LowerAscii(std::u32string s) {
    for (char32_t& c : s) {
        if (c >= U'A' && c <= U'Z') c += 32;
    }
    return s;
}

bool ParseHex(std::string_view text, char32_t& out) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    std::uint32_t v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v, 16);
    if (ec != std::errc() || end != text.data() + text.size()) return false;
    out = char32_t(v);
    return true;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> cols;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        cols.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) return cols;
        start = tab + 1;
    }
}

} // namespace

void NameRules::Forbid(std::string_view word, bool anywhere) {
    std::u32string w;
    if (!Decode(word, w) || w.empty()) return;
    w = LowerAscii(std::move(w));
    if (anywhere) {
        anywhere_.push_back(std::move(w));
    } else {
        whole_.insert(std::move(w));
    }
}

void NameRules::ForbidInChat(std::string_view word, bool anywhere) {
    std::u32string w;
    if (!Decode(word, w) || w.empty()) return;
    w = LowerAscii(std::move(w));
    if (anywhere) {
        chatAnywhere_.push_back(std::move(w));
    } else {
        chatWhole_.insert(std::move(w));
    }
}

bool NameRules::Parse(std::string_view text, int group, std::string& error) {
    allowed_.clear();
    anywhere_.clear();
    whole_.clear();
    chatAnywhere_.clear();
    chatWhole_.clear();
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        const auto cols = SplitTabs(line);
        if (line.starts_with('#')) {
            // Six columns per locale group: "#ALLOW_ID_TABLE", first, last, three empty.
            const std::size_t at = std::size_t(group) * 6;
            if (cols.size() > at + 2 && cols[at] == "#ALLOW_ID_TABLE") {
                char32_t first = 0, last = 0;
                if (ParseHex(cols[at + 1], first) && ParseHex(cols[at + 2], last)) allowed_.emplace_back(first, last);
            }
            continue;
        }
        // The group's word, then its four flags: the first for names, the second for chat.
        const std::size_t at = std::size_t(group) * 6;
        if (cols.size() < at + 3 || cols[at].empty()) continue;
        const std::string_view word = cols[at], names = cols[at + 1], chat = cols[at + 2];
        if (names == "1" || names == "2") Forbid(word, names == "2");
        if (chat == "1" || chat == "2") ForbidInChat(word, chat == "2");
    }
    if (allowed_.empty()) {
        error = std::format("abusefilter.txt: no allowed characters for group {}", group);
        return false;
    }
    return true;
}

NameCheck NameRules::Check(std::string_view utf8Name) const {
    std::u32string name;
    if (!Decode(utf8Name, name)) return NameCheck::BadCharacter;
    if (name.size() < kMinLength || name.size() > kMaxLength) return NameCheck::BadLength;
    for (char32_t c : name) {
        bool ok = false;
        for (const auto& [first, last] : allowed_) ok |= c >= first && c <= last;
        if (!ok) return NameCheck::BadCharacter;
    }
    // A word anywhere in it, then the whole name.
    const std::u32string lower = LowerAscii(name);
    for (const std::u32string& word : anywhere_) {
        if (lower.find(word) != std::u32string::npos) return NameCheck::Forbidden;
    }
    if (whole_.contains(lower)) return NameCheck::Forbidden;
    return NameCheck::Ok;
}

bool NameRules::ChatAllowed(std::string_view utf8Line) const {
    std::u32string line;
    if (!Decode(utf8Line, line)) return false;
    line = LowerAscii(std::move(line));
    // Each word between spaces (punctuation stays in it), then a word anywhere, spaces and all.
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] == U' ') {
            ++i;
            continue;
        }
        std::size_t end = line.find(U' ', i);
        if (end == std::u32string::npos) end = line.size();
        if (chatWhole_.contains(line.substr(i, end - i))) return false;
        i = end;
    }
    for (const std::u32string& word : chatAnywhere_) {
        if (line.find(word) != std::u32string::npos) return false;
    }
    return true;
}

bool CreationRules::Load(const assets::CharacterTable& characters, const assets::ItemTable& items, std::string& error) {
    figures_.clear();
    armour_.clear();
    weapons_.clear();
    struct Set {
        const char* prefix;
        Race race;
        bool male;
        std::array<const char*, 13> figures;
    };
    // The figures of the create screens (their UIO_NEWCHAR_* texts), per race and sex.
    static const Set kSets[] = {
        {"CHAR_CH_MAN_", Race::China, true,
         {"NOBLEBOY", "SCHOLAR", "PERFORMER", "MERCHANT", "WARRIOR", "MONK", "ADVENTURER", "FIGHTER", "NECROMANCER", "PRIEST", "BOGY",
          "MONKEY", "TATTOO"}},
        {"CHAR_CH_WOMAN_", Race::China, false,
         {"NOBLEGIRL", "SCHOLAR", "KISAENG", "MERCHANT", "ASSASSIN", "WARRIOR", "ADVENTURER", "FIGHTER", "NECROMENCERW", "NECROMENCERB",
          "FOX", "BOGY", "KANGSI"}},
        {"CHAR_EU_MAN_", Race::Europe, true,
         {"NOBLE", "EXORCIST", "NECROMENCER", "MERCHANT", "PRIEST", "KNIGHT", "WARRIOR", "GLADIATOR", "BARBARIAN", "ADVENTURER", "ANGEL",
          "DEVIL", "WEREWOLF"}},
        {"CHAR_EU_WOMAN_", Race::Europe, false,
         {"NOBLE", "WITCH", "SUMMONER", "MERCHANT", "ORACLE", "CRUSADER", "AMAZONESS", "KNIGHT", "ADVENTURER", "GLADIATOR", "ANGEL", "DEVIL",
          "SUCCUBUS"}},
    };
    for (const Set& set : kSets) {
        for (const char* figure : set.figures) {
            const std::string codename = std::string(set.prefix) + figure;
            const assets::ObjectRow* row = characters.Find(codename);
            if (!row) {
                error = "characterdata has no " + codename;
                return false;
            }
            figures_[row->refObjId] = {set.race, set.male};
        }
    }
    const auto item = [&](const std::string& codename, std::uint32_t& id) {
        const assets::ItemRow* row = items.Find(codename);
        if (!row) {
            error = "itemdata has no " + codename;
            return false;
        }
        id = row->refObjId;
        return true;
    };
    for (const char* race : {"CH", "EU"}) {
        const Race r = race[0] == 'C' ? Race::China : Race::Europe;
        for (const char* sex : {"M", "W"}) {
            const std::pair<const char*, ArmourKind> kinds[] = {
                {"HEAVY", ArmourKind::Heavy}, {"LIGHT", ArmourKind::Light}, {"CLOTHES", ArmourKind::Clothes}};
            for (const auto& [kind, k] : kinds) {
                const std::string base = std::format("ITEM_{}_{}_{}_01_", race, sex, kind);
                std::uint32_t chest = 0, legs = 0, shoes = 0;
                if (!item(base + "BA_A_DEF", chest) || !item(base + "LA_A_DEF", legs) || !item(base + "FA_A_DEF", shoes)) return false;
                armour_.push_back({r, sex[0] == 'M', chest, legs, shoes, k});
            }
        }
    }
    std::uint32_t chinaShield = 0, europeShield = 0;
    if (!item("ITEM_CH_SHIELD_01_A_DEF", chinaShield) || !item("ITEM_EU_SHIELD_01_A_DEF", europeShield)) return false;
    // The armour a European weapon allows, by its class (the fourth type id).
    const auto europeKinds = [](int weaponClass) -> std::uint8_t {
        constexpr std::uint8_t heavy = 1u << int(ArmourKind::Heavy), light = 1u << int(ArmourKind::Light),
                               clothes = 1u << int(ArmourKind::Clothes);
        switch (weaponClass) {
        case 7: case 8: case 9: return heavy | light;     // sword, two-hand sword, axe
        case 10: case 11: case 14: return clothes;        // dark staff, two-hand staff, harp
        case 12: case 13: return light;                   // crossbow, dagger
        case 15: return light | clothes;                  // cleric staff
        default: return heavy | light | clothes;
        }
    };
    static const char* kChina[] = {"BLADE", "BOW", "SPEAR", "SWORD", "TBLADE"};
    static const char* kEurope[] = {"AXE", "CROSSBOW", "DAGGER", "DARKSTAFF", "HARP", "STAFF", "SWORD", "TSTAFF", "TSWORD"};
    for (const char* w : kChina) {
        const assets::ItemRow* row = items.Find(std::format("ITEM_CH_{}_01_A_DEF", w));
        if (!row) {
            error = std::format("itemdata has no ITEM_CH_{}_01_A_DEF", w);
            return false;
        }
        weapons_[row->refObjId] = {Race::China, 0, assets::OneHanded(*row) ? chinaShield : 0};
    }
    for (const char* w : kEurope) {
        const assets::ItemRow* row = items.Find(std::format("ITEM_EU_{}_01_A_DEF", w));
        if (!row) {
            error = std::format("itemdata has no ITEM_EU_{}_01_A_DEF", w);
            return false;
        }
        weapons_[row->refObjId] = {Race::Europe, europeKinds(row->typeId[3]), assets::OneHanded(*row) ? europeShield : 0};
    }
    return true;
}

CreationCheck CreationRules::Check(std::uint32_t model, std::uint32_t chest, std::uint32_t legs, std::uint32_t shoes,
                                   std::uint32_t weapon) const {
    const auto figure = figures_.find(model);
    if (figure == figures_.end()) return CreationCheck::BadModel;
    const auto w = weapons_.find(weapon);
    if (w == weapons_.end() || w->second.race != figure->second.race) return CreationCheck::BadWeapon;
    // The European screen offers no armour at all (0, 0, 0); China's always one.
    if (chest == 0 && legs == 0 && shoes == 0) return figure->second.race == Race::Europe ? CreationCheck::Ok : CreationCheck::BadArmour;
    for (const ArmourSet& set : armour_) {
        if (set.race == figure->second.race && set.male == figure->second.male && set.chest == chest && set.legs == legs &&
            set.shoes == shoes) {
            return Allows(weapon, set.kind) ? CreationCheck::Ok : CreationCheck::BadArmour;
        }
    }
    return CreationCheck::BadArmour;
}

std::uint32_t CreationRules::ShieldFor(std::uint32_t weapon) const {
    const auto w = weapons_.find(weapon);
    return w == weapons_.end() ? 0 : w->second.shield;
}

bool CreationRules::Allows(std::uint32_t weapon, ArmourKind kind) const {
    const auto w = weapons_.find(weapon);
    if (w == weapons_.end()) return false;
    return w->second.kinds == 0 || (w->second.kinds & (1u << int(kind))) != 0;
}

std::vector<std::uint32_t> CreationRules::FiguresOf(Race race, bool male) const {
    std::vector<std::uint32_t> out;
    for (const auto& [id, f] : figures_) {
        if (f.race == race && f.male == male) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<CreationRules::Armour> CreationRules::ArmourOf(Race race, bool male) const {
    std::vector<Armour> out;
    for (const ArmourSet& s : armour_) {
        if (s.race == race && s.male == male) out.push_back({s.chest, s.legs, s.shoes, s.kind});
    }
    std::sort(out.begin(), out.end(), [](const Armour& a, const Armour& b) { return a.chest < b.chest; });
    return out;
}

std::vector<std::uint32_t> CreationRules::WeaponsOf(Race race) const {
    std::vector<std::uint32_t> out;
    for (const auto& [id, r] : weapons_) {
        if (r.race == race) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool CharacterRules::Load(const assets::FileSystem& fs, std::string& error) {
    assets::CharacterTable characters;
    auto items = std::make_shared<assets::ItemTable>();
    if (!characters.Load(fs, error) || !items->Load(fs, error)) return false;
    if (!Load(fs, characters, *items, error)) return false;
    this->items = std::move(items);
    return true;
}

bool CharacterRules::Load(const assets::FileSystem& fs, const assets::CharacterTable& characters, const assets::ItemTable& items,
                          std::string& error) {
    if (!creation.Load(characters, items, error)) return false;
    const auto filter = fs.Read("server_dep\\silkroad\\textdata\\abusefilter.txt");
    if (!filter) {
        error = "abusefilter.txt is missing";
        return false;
    }
    return names.Parse(core::DecodeTextFile(filter->Bytes()), 4, error);
}

} // namespace sr::game
