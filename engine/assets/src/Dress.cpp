#include "sr/assets/Dress.h"

#include <algorithm>
#include <cstdlib>
#include <format>

#include "sr/assets/FileSystem.h"
#include "sr/core/Text.h"
#include "sr/assets/GameData.h"
#include "sr/assets/ItemGlow.h"

namespace sr::assets {

namespace {

bool IsArmour(const ItemRow& row) {
    const int kind = row.typeId[2];
    return row.typeId[0] == 3 && row.typeId[1] == 1 && ((kind >= 1 && kind <= 3) || (kind >= 9 && kind <= 11));
}

// 1 clothes, 2 light, 3 heavy (item types 1/9, 2/10, 3/11); 0 for anything else.
int ArmourClass(const ItemRow& row) {
    if (!IsArmour(row)) return 0;
    const int kind = row.typeId[2];
    return kind > 8 ? kind - 8 : kind;
}

std::string WithSuffix(const std::string& path, const char* suffix) {
    const std::size_t dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(0, dot) + suffix + path.substr(dot);
}

// The volume tables (men, women): bone, factor at volume 0, factor at 4.
struct VolumeBone {
    const char* bone;
    float low, high;
};
constexpr VolumeBone kMale[] = {
    {"Bip01 Spine", 0.88f, 1.1f},     {"Bip01 Spine1", 0.95f, 1.11f}, {"Bip01 L UpperArm", 0.92f, 1.2f},
    {"Bip01 R UpperArm", 0.92f, 1.2f}, {"Bip01 L Thigh", 0.9f, 1.15f},  {"Bip01 R Thigh", 0.9f, 1.15f},
    {"Bip01 Pelvis", 0.9f, 1.0f},
};
constexpr VolumeBone kFemale[] = {
    {"Bip01 Spine", 0.95f, 1.1f},      {"Bip01 Spine1", 0.95f, 1.0f},  {"Bip01 L UpperArm", 0.95f, 1.15f},
    {"Bip01 R UpperArm", 0.95f, 1.15f}, {"Bip01 L Thigh", 0.9f, 1.15f}, {"Bip01 R Thigh", 0.9f, 1.15f},
    {"Bip01 Pelvis", 0.9f, 1.08f},     {"Bone01", 0.8f, 1.18f},
};

} // namespace

std::string DressedCharacter::Key() const {
    std::string key = body;
    for (const std::string& item : items) key += "|" + item;
    return key;
}

int EquipSlot(const ItemRow& row) {
    if (row.typeId[0] != 3 || row.typeId[1] != 1) return -1;
    if (IsArmour(row)) {
        // The fourth type id: 1 head, 2 shoulders, 3 chest, 4 legs, 5 hands, 6 feet.
        constexpr int kSlot[] = {-1, 0, 2, 1, 4, 3, 5};
        const int part = row.typeId[3];
        return part >= 1 && part <= 6 ? kSlot[part] : -1;
    }
    if (row.typeId[2] == 6) return 6;
    if (row.typeId[2] == 4 && (row.typeId[3] == 1 || row.typeId[3] == 2)) return 7;
    if (row.typeId[2] == 7) return 8; // the job suit (and the special dresses of place 8)
    return -1;
}

int AvatarSlot(const ItemRow& row) {
    if (row.typeId[0] != 3 || row.typeId[1] != 1) return -1;
    if (row.typeId[2] == 13 && row.typeId[3] >= 1 && row.typeId[3] <= 3) return row.typeId[3] - 1;
    if (row.typeId[2] == 14 && row.typeId[3] == 1) return 3;
    return -1;
}

namespace {

std::string_view TrimCell(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

long Number(std::string_view s) {
    const std::string text(s);
    return std::strtol(text.c_str(), nullptr, 0); // decimal, or "0x0700"
}

std::uint32_t PackedType(const ItemRow& row) {
    return std::uint32_t(row.typeId[0] << 2 | row.typeId[1] << 5 | row.typeId[2] << 7 | row.typeId[3] << 11);
}

} // namespace

bool VisualChangeTable::Load(const FileSystem& fs, std::string& error) {
    const auto file = fs.Read("server_dep\\silkroad\\textdata\\charactervisualchange.txt");
    if (!file) {
        error = "charactervisualchange.txt not found";
        return false;
    }
    return Parse(core::DecodeTextFile(file->Bytes()), error);
}

bool VisualChangeTable::Parse(std::string_view text, std::string& error) {
    byType_.clear();
    byAvatar_.clear();
    std::vector<std::string_view> c;
    for (std::size_t start = 0; start < text.size();) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = TrimCell(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.starts_with("//")) continue;
        c.clear();
        for (std::size_t from = 0; from <= line.size();) {
            const std::size_t tab = std::min(line.find('\t', from), line.size());
            c.push_back(TrimCell(line.substr(from, tab - from)));
            from = tab + 1;
        }
        if (c.size() < 17 || Number(c[0]) != 1 || (Number(c[3]) != 1 && Number(c[3]) != 2)) continue; // in service, the equipment's, the avatars'
        Row row;
        row.slots = std::uint16_t(Number(c[13]));
        row.overlap = std::uint16_t(Number(c[14]));
        row.group = int(Number(c[15]));
        row.priority = int(Number(c[16]));
        if (Number(c[3]) == 2) {
            byAvatar_[std::uint64_t(std::uint32_t(Number(c[5]))) << 32 | std::uint32_t(Number(c[6]))] = row;
        } else {
            byType_[std::uint32_t(Number(c[5]))] = row;
        }
    }
    if (byType_.empty()) {
        error = "charactervisualchange.txt: no equipment rows";
        return false;
    }
    return true;
}

const VisualChangeTable::Row* VisualChangeTable::ForItem(const ItemRow& row) const {
    if (AvatarSlot(row) >= 0) {
        const auto a = byAvatar_.find(std::uint64_t(PackedType(row)) << 32 | std::uint32_t(row.params[6]));
        if (a != byAvatar_.end()) return &a->second;
    }
    const auto it = byType_.find(PackedType(row));
    return it != byType_.end() ? &it->second : nullptr;
}

std::string StanceOf(const ItemRow* weapon) {
    if (weapon == nullptr || EquipSlot(*weapon) != 6) return "default";
    return StanceOfWeaponKind(weapon->typeId[3]);
}

std::string StanceOfWeaponKind(int kind) {
    switch (kind) {
    case 2: case 3: return "sword";
    case 4: case 5: return "spear";
    case 6: case 12: return "bow";
    case 7: return "onehand_sword";
    case 8: return "twohand_sword";
    case 9: return "dual_axe";
    case 10: case 15: return "onehand_staff";
    case 11: return "twohand_staff";
    case 13: return "dagger";
    case 14: return "harf";
    default: return "default";
    }
}

bool OneHanded(const ItemRow& weapon) { return weapon.Stat(93) == 0.0f; }

float HeightScale(std::uint8_t shape) {
    constexpr float kHeights[] = {0.94f, 0.97f, 1.0f, 1.03f, 1.06f};
    const int h = shape & 0xF;
    return kHeights[h <= 4 ? h : 0];
}

std::vector<std::pair<std::string, float>> VolumeScales(std::uint8_t shape, bool female) {
    const float v = std::clamp(static_cast<float>(shape >> 4), 0.0f, 4.0f);
    std::vector<std::pair<std::string, float>> out;
    const auto add = [&](const auto& table) {
        for (const VolumeBone& b : table) {
            out.emplace_back(b.bone, v >= 2.0f ? 1.0f + (b.high - 1.0f) * (v - 2.0f) * 0.5f : b.low + (1.0f - b.low) * v * 0.5f);
        }
    };
    if (female) {
        add(kFemale);
    } else {
        add(kMale);
    }
    return out;
}

namespace {
constexpr std::string_view kGlowMark = "*glow:";
} // namespace

std::string WithGlow(const std::string& itemPath, const EquipGlow& g) {
    return std::format("{}{}{};{};{};{};{},{},{};{},{},{};{};{}", itemPath, kGlowMark, g.texture, g.scroll[0], g.scroll[1], g.colorOp, g.from[0],
                       g.from[1], g.from[2], g.to[0], g.to[1], g.to[2], g.periodMs, g.alphaTest ? 1 : 0);
}

bool SplitGlow(std::string& path, EquipGlow& glow) {
    const std::size_t mark = path.find(kGlowMark);
    if (mark == std::string::npos) return false;
    const std::string spec = path.substr(mark + kGlowMark.size());
    std::vector<std::string> fields;
    for (std::size_t start = 0; start <= spec.size();) {
        const std::size_t semi = std::min(spec.find(';', start), spec.size());
        fields.push_back(spec.substr(start, semi - start));
        start = semi + 1;
    }
    if (fields.size() != 8) return false;
    EquipGlow g;
    g.texture = fields[0];
    const auto number = [](const std::string& s, float& out) {
        char* end = nullptr;
        out = std::strtof(s.c_str(), &end);
        return end != s.c_str();
    };
    const auto triple = [&](const std::string& s, std::array<float, 3>& out) {
        std::size_t at = 0;
        for (float& v : out) {
            const std::size_t comma = std::min(s.find(',', at), s.size());
            if (at > s.size() || !number(s.substr(at, comma - at), v)) return false;
            at = comma + 1;
        }
        return true;
    };
    float op = 0, period = 0, alpha = 0;
    if (!number(fields[1], g.scroll[0]) || !number(fields[2], g.scroll[1]) || !number(fields[3], op) || !triple(fields[4], g.from) ||
        !triple(fields[5], g.to) || !number(fields[6], period) || !number(fields[7], alpha)) {
        return false;
    }
    g.colorOp = static_cast<std::uint8_t>(op);
    g.periodMs = static_cast<std::uint32_t>(std::max(period, 0.0f));
    g.alphaTest = alpha != 0.0f;
    glow = std::move(g);
    path.resize(mark);
    return true;
}

bool Dress(const FileSystem& fs, const CharacterTable& characters, const ItemTable& items, std::uint32_t model,
           std::span<const std::uint32_t> worn, std::uint8_t shape, DressedCharacter& out, std::string& error,
           std::span<const std::uint8_t> plus, const EquipGlowTable* glows, const VisualChangeTable* visuals, bool garments) {
    out = DressedCharacter{};
    const ObjectRow* figure = characters.Find(model);
    if (figure == nullptr) {
        error = "figure " + std::to_string(model) + " is not in characterdata";
        return false;
    }
    out.body = CharacterTable::ResolveModel(fs, *figure);
    if (out.body.empty()) {
        error = figure->codename + ": no model";
        return false;
    }
    out.europe = figure->codename.starts_with("CHAR_EU_");
    out.female = figure->codename.find("_WOMAN_") != std::string::npos;
    out.height = HeightScale(shape);
    out.volume = VolumeScales(shape, out.female);

    // The worn items by slot (the last one of a slot wins).
    // Places 0..8 the body's; 9..12 the dress side's (AvatarSlot + 9), drawn over what their visual covers.
    constexpr int kSlots = 13;
    const ItemRow* slots[kSlots] = {};
    int slotPlus[kSlots] = {};
    for (std::size_t i = 0; i < worn.size(); ++i) {
        const ItemRow* row = items.Find(worn[i]);
        if (row == nullptr) continue;
        const int avatar = AvatarSlot(*row);
        const int slot = avatar >= 0 ? 9 + avatar : EquipSlot(*row);
        if (slot < 0) continue;
        slots[slot] = row;
        slotPlus[slot] = i < plus.size() ? plus[i] : 0;
    }
    // Default garments on an empty chest or legs: light under light or heavy armour, else clothes.
    int wornClass = 0;
    for (const ItemRow* row : slots) {
        if (row != nullptr && wornClass == 0) wornClass = ArmourClass(*row);
    }
    // What is drawn: with the visual table, by priority (the lowest number first), each shown covering its slots and
    // one meeting those covered left out (unless both share an overlap group); without it, everything.
    bool shown[kSlots] = {};
    std::uint32_t covered = 0;
    if (visuals != nullptr) {
        int order[kSlots];
        for (int i = 0; i < kSlots; ++i) order[i] = i;
        const auto priorityOf = [&](int slot) {
            const VisualChangeTable::Row* v = slots[slot] != nullptr ? visuals->ForItem(*slots[slot]) : nullptr;
            return v != nullptr ? v->priority : 250;
        };
        std::stable_sort(order, order + kSlots, [&](int a, int b) { return priorityOf(a) < priorityOf(b); });
        int coveredGroup = 0;
        for (const int slot : order) {
            if (slots[slot] == nullptr) continue;
            const VisualChangeTable::Row* v = visuals->ForItem(*slots[slot]);
            const std::uint32_t mine = v != nullptr ? v->slots : 0u;
            if ((mine & covered) != 0 && (v == nullptr || v->group == 0 || v->group != coveredGroup)) continue;
            shown[slot] = true;
            covered |= mine;
            if (v != nullptr && v->group != 0) coveredGroup = v->group;
        }
    } else {
        for (int slot = 0; slot < kSlots; ++slot) shown[slot] = slots[slot] != nullptr;
        if (slots[1] != nullptr || slots[10] != nullptr) covered |= 0x02;
        if (slots[4] != nullptr || slots[10] != nullptr) covered |= 0x10;
    }
    const std::string folder = std::string("res\\item\\") + (out.europe ? "europe\\" : "china\\") + (out.female ? "woman_item\\" : "man_item\\") +
                               (wornClass >= 2 ? "light" : "clothes") + (out.europe ? "_01_" : "_00_");
    if (garments && (visuals != nullptr ? (covered & 0x02) == 0 : slots[1] == nullptr)) out.items.push_back(folder + "ba.bsr");
    if (garments && (visuals != nullptr ? (covered & 0x10) == 0 : slots[4] == nullptr)) out.items.push_back(folder + "la.bsr");
    // The sets whole in what is worn: their pieces in the set's look, as the 1.188 client's set operator dresses them.
    const std::vector<int> whole = WholeSets(items, worn);
    for (int slot = 0; slot < kSlots; ++slot) {
        const ItemRow* row = slots[slot];
        if (row == nullptr || !shown[slot]) continue;
        const bool fullSet = row->setId != 0 && std::binary_search(whole.begin(), whole.end(), row->setId);
        // The weapon's and the shield's plus glow (for the models of places 6 and 7).
        const EquipGlow* glow = glows != nullptr && (slot == 6 || slot == 7) ? glows->Find(row->codename, slotPlus[slot]) : nullptr;
        for (std::string& path : WornModels(fs, items, *row, fullSet)) out.items.push_back(glow != nullptr ? WithGlow(path, *glow) : std::move(path));
    }
    out.covered = covered;
    out.stance = StanceOf(slots[6]);
    // The fortress hammer (TID4 16) is held as its race's one-handed sword: skilleffect.txt plays its swing
    // (ANI_HAMMER) from the SWORD set for China and ONEHAND_SWORD for Europe (inferred for the stance itself).
    if (slots[6] != nullptr && slots[6]->typeId[3] == 16) out.stance = out.europe ? "onehand_sword" : "sword";
    return true;
}

std::vector<std::string> WornModels(const FileSystem& fs, const ItemTable& items, const ItemRow& row, bool fullSet) {
    std::vector<std::string> out;
    if (fullSet && !row.fullSetModelPath.empty()) {
        const std::string whole = fs.Exists(row.fullSetModelPath) ? row.fullSetModelPath : "res\\" + row.fullSetModelPath;
        if (fs.Exists(whole)) {
            out.push_back(whole);
            return out;
        }
    }
    const bool dual = EquipSlot(row) == 6 && (row.typeId[3] == 9 || row.typeId[3] == 13);
    if (dual) {
        // An axe or dagger has no model of its own name, only its hands' (loads the
        // right one); the left one too when there is one (inferred: the pair is worn).
        const ItemRow* source = row.modelPath.empty() ? items.Find(row.linkedCodename) : &row;
        if (source == nullptr || source->modelPath.empty()) return out;
        const std::string path = "res\\" + source->modelPath;
        for (const char* hand : {"_r", "_l"}) {
            if (fs.Exists(WithSuffix(path, hand))) out.push_back(WithSuffix(path, hand));
        }
        return out;
    }
    std::string path = items.ResolveModel(fs, row);
    if (!path.empty()) out.push_back(std::move(path));
    return out;
}

// --- The jobs' bandits ------------------------------------------------------------------

bool BanditBodies::Load(const FileSystem& fs, const CharacterTable& characters, std::string& error) {
    const auto file = fs.Read("server_dep\\silkroad\\textdata\\usableresobjiddata.txt");
    if (!file) {
        error = "usableresobjiddata.txt not found";
        return false;
    }
    return Parse(core::DecodeTextFile(file->Bytes()), characters, error);
}

bool BanditBodies::Parse(std::string_view text, const CharacterTable& characters, std::string& error) {
    lists = {};
    for (std::size_t start = 0; start < text.size();) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = TrimCell(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.starts_with("//")) continue;
        // Service, ID.
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos) continue;
        const std::string_view service = TrimCell(line.substr(0, tab)), id = TrimCell(line.substr(tab + 1));
        if (service != "1") continue;
        std::uint32_t refObjId = 0;
        for (const char ch : id) {
            if (ch < '0' || ch > '9') break;
            refObjId = refObjId * 10 + std::uint32_t(ch - '0');
        }
        const ObjectRow* row = characters.Find(refObjId);
        if (row == nullptr || row->typeId[0] != 1 || row->typeId[1] != 1) continue; // its character rows (the items go elsewhere)
        if (row->country == 0) lists[0].push_back(refObjId);
        if (row->country == 1) lists[1].push_back(refObjId);
    }
    if (Empty()) {
        error = "usableresobjiddata.txt: no character rows";
        return false;
    }
    return true;
}

bool IsBandit(const ObjectRow& row) {
    return row.typeId[0] == 1 && row.typeId[1] == 2 && row.typeId[2] == 1 && (row.typeId[3] == 2 || row.typeId[3] == 3);
}

int BanditDegree(int level) {
    static constexpr int kBelow[] = {8, 16, 24, 32, 42, 52, 64, 76, 90, 104, 120, 141, 164};
    for (int i = 0; i < int(std::size(kBelow)); ++i) {
        if (level < kBelow[i]) return i + 1;
    }
    return 14;
}

BanditOutfit OutfitOfBandit(const ObjectRow& bandit, std::uint8_t appearance, int weaponKind, const BanditBodies& bodies, const CharacterTable& characters,
                            const ItemTable& items) {
    BanditOutfit out;
    // Every bandit row's Country is 3 (China), the European ones' too: those go by their codename (MOB_EU_...).
    const bool europe = bandit.country == 1 || bandit.codename.starts_with("MOB_EU_");
    const auto& list = bodies.lists[europe ? 1 : 0];
    if (list.empty()) return out;
    out.body = list[appearance % list.size()];
    const ObjectRow* body = characters.Find(out.body);
    const bool woman = body != nullptr && body->codename.find("_WOMAN_") != std::string::npos;
    const std::string race = europe ? "EU" : "CH";
    const bool hunter = bandit.typeId[3] == 3;
    const int degree = BanditDegree(bandit.level);
    // A piece at the degree, else the next lower one there is.
    const auto add = [&](const auto& name) {
        for (int d = degree; d >= 1; --d) {
            const std::string code = name(d);
            if (const ItemRow* row = items.Find(std::string_view(code))) {
                out.worn.push_back(row->refObjId);
                return;
            }
        }
    };
    const auto two = [](int d) { return (d < 10 ? "0" : "") + std::to_string(d); };
    // The weapon by the kind (1.188's names), a shield for the one-handed sword, blade and staff.
    static constexpr const char* kWeapons[16] = {nullptr, nullptr, "SWORD", "BLADE", "SPEAR", "TBLADE", "BOW", "SWORD",
                                                 "TSWORD", "AXE", "DARKSTAFF", "TSTAFF", "CROSSBOW", "DAGGER", "HARP", "STAFF"};
    const int kind = weaponKind >= 2 && weaponKind <= 15 ? weaponKind : 2;
    add([&](int d) { return "ITEM_" + race + "_" + kWeapons[kind] + "_" + two(d) + "_A"; });
    if (kind == 2 || kind == 3 || kind == 7 || kind == 15) add([&](int d) { return "ITEM_" + race + "_SHIELD_" + two(d) + "_A"; });
    // A hunter's armour by its weapon: clothes, light or heavy; the parts CA (or HA, the helm), SA,
    // BA, LA, AA, FA. Ours: the helm by the byte's bit 6 (the original's a 50 % roll each client keeps).
    if (hunter) {
        static constexpr const char* kArmour[16] = {"CLOTHES", "CLOTHES", "CLOTHES", "HEAVY", "CLOTHES", "HEAVY", "HEAVY", "HEAVY",
                                                    "HEAVY",   "HEAVY",   "CLOTHES", "CLOTHES", "CLOTHES", "CLOTHES", "LIGHT", "LIGHT"};
        const std::string set = std::string("ITEM_") + race + "_" + (woman ? "W" : "M") + "_" + kArmour[kind] + "_";
        const bool helm = (appearance & 0x40) != 0;
        for (const char* part : {helm ? "HA" : "CA", "SA", "BA", "LA", "AA", "FA"}) add([&](int d) { return set + two(d) + "_" + part + "_A"; });
    }
    // The job suit: tier 02 or 03 for the Chinese (ours: the byte's bit 5, the original's each client's roll), by
    // level for the European (below 40 01, 60 03, 80 05, else 07).
    const int tier = europe ? (bandit.level < 40 ? 1 : bandit.level < 60 ? 3 : bandit.level < 80 ? 5 : 7) : 2 + ((appearance >> 5) & 1);
    const std::string suit = "ITEM_" + race + "_" + (woman ? "F" : "M") + "_TRADE_" + (hunter ? "HUNTER_" : "THIEF_") + two(tier);
    if (const ItemRow* row = items.Find(std::string_view(suit))) out.worn.push_back(row->refObjId);
    return out;
}

} // namespace sr::assets
