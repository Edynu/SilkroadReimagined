#include "sr/game/MagicPop.h"

#include <algorithm>
#include <charconv>

namespace sr::game::pop {

bool IsCard(const assets::ItemRow& row) { return row.typeId[0] == 3 && row.typeId[1] == 3 && row.typeId[2] == 14 && row.typeId[3] == 1; }
bool IsCoupon(const assets::ItemRow& row) { return row.typeId[0] == 3 && row.typeId[1] == 3 && row.typeId[2] == 14 && row.typeId[3] == 2; }

const assets::GachaEntry* WasteSet::Pick(int r) const {
    const auto it = std::lower_bound(sums.begin(), sums.end(), r);
    if (it == sums.end()) return entries.empty() ? nullptr : entries.back();
    return entries[std::size_t(it - sums.begin())];
}

WasteSet MakeWasteSet(const assets::GachaTable& table, int set) {
    WasteSet w;
    std::int64_t total = 0;
    for (const assets::GachaEntry& e : table.entries()) {
        if (e.set != set || e.ratio <= 0) continue;
        w.entries.push_back(&e);
        total += e.ratio;
    }
    if (total <= 0) {
        w.entries.clear();
        return w;
    }
    std::vector<int> scaled;
    int given = 0;
    for (const assets::GachaEntry* e : w.entries) {
        scaled.push_back(int(std::int64_t(e->ratio) * kRatioScale / total));
        given += scaled.back();
    }
    for (std::size_t i = 0; given < kRatioScale && !scaled.empty(); i = (i + 1) % scaled.size(), ++given) ++scaled[i];
    int sum = 0;
    for (int s : scaled) w.sums.push_back(sum += s);
    return w;
}

Roll RollPrize(const assets::GachaEntry& chosen, const WasteSet& waste, std::uint32_t draw1, std::uint32_t draw2) {
    const int r1 = int(draw1 % kRatioScale);
    if (r1 < chosen.ratio) return {true, &chosen};
    const int r2 = int(draw2 % kRatioScale);
    return {false, waste.Pick(r2)};
}

double TrueChance(int ratio) {
    // Values 0..2,767 come four times out of 32,768 draws, the rest three.
    const int low = std::clamp(ratio, 0, int(kDrawRange - 3 * kRatioScale)), high = std::clamp(ratio - low, 0, kRatioScale);
    return double(4 * low + 3 * high) / double(kDrawRange);
}

std::uint32_t AttributeCode(std::string_view attribute) {
    std::uint32_t code = 0;
    for (char c : attribute.substr(0, 4)) code = code << 8 | std::uint8_t(c);
    return code;
}

bool ParseOptions(std::string_view text, std::vector<Token>& out) {
    out.clear();
    if (text.find("[BIIV]") == std::string_view::npos) return false;
    std::size_t at = 0;
    while ((at = text.find('<', at)) != std::string_view::npos) {
        const std::size_t end = text.find('>', at);
        if (end == std::string_view::npos || end < at + 3 || text[at + 2] != ':') return false;
        Token t;
        t.kind = text[at + 1];
        std::string_view body = text.substr(at + 3, end - at - 3);
        std::vector<std::string_view> fields;
        for (std::size_t f = 0; f <= body.size();) {
            std::size_t comma = body.find(',', f);
            if (comma == std::string_view::npos) comma = body.size();
            fields.push_back(body.substr(f, comma - f));
            f = comma + 1;
        }
        const auto number = [](std::string_view s, std::uint64_t& v) { return std::from_chars(s.data(), s.data() + s.size(), v).ec == std::errc{}; };
        std::uint64_t level = 0;
        if (t.kind == 'M') {
            if (fields.size() != 3 || fields[0].empty() || fields[0].size() > 4 || !number(fields[1], level) || !number(fields[2], t.value)) return false;
            t.attribute = std::string(fields[0]);
            t.level = int(level);
        } else if (t.kind == 'O' || t.kind == 'D' || t.kind == 'V') {
            if (fields.size() != 1 || !number(fields[0], t.value)) return false;
        } else {
            return false;
        }
        // A kind once (an M once per attribute).
        if (std::any_of(out.begin(), out.end(), [&](const Token& o) { return o.kind == t.kind && o.attribute == t.attribute; })) return false;
        out.push_back(std::move(t));
        if (out.size() > 5) return false;
        at = end + 1;
    }
    return true;
}

std::vector<std::uint64_t> CouponParams(const assets::GachaEntry& prize) {
    std::vector<std::uint64_t> params{prize.item, std::uint64_t(std::max(prize.count, 1))};
    std::vector<Token> tokens;
    if (!prize.options.empty() && ParseOptions(prize.options, tokens)) {
        for (const Token& t : tokens) {
            if (params.size() + 2 > 12) break;
            const std::uint64_t header = 1u | std::uint64_t(std::uint8_t(t.kind)) << 8 | std::uint64_t(t.level & 0xFF) << 16 |
                                         std::uint64_t(AttributeCode(t.attribute)) << 24;
            params.push_back(header);
            params.push_back(t.value);
        }
    }
    return params;
}

std::optional<CouponPrize> ReadCoupon(std::span<const std::uint64_t> params) {
    if (params.size() < 2 || params[0] == 0 || params[0] > 0xFFFFFFFFu) return std::nullopt;
    CouponPrize p;
    p.item = std::uint32_t(params[0]);
    p.count = int(std::clamp<std::uint64_t>(params[1], 1, 0xFFFF));
    for (std::size_t i = 2; i + 1 < params.size(); i += 2) {
        const std::uint64_t h = params[i];
        if ((h & 0xFF) != 1) continue;
        Token t;
        t.kind = char((h >> 8) & 0xFF);
        t.level = int((h >> 16) & 0xFF);
        const std::uint32_t code = std::uint32_t(h >> 24);
        for (int b = 3; b >= 0; --b) {
            const char c = char((code >> (8 * b)) & 0xFF);
            if (c != 0) t.attribute.push_back(c);
        }
        t.value = params[i + 1];
        p.tokens.push_back(std::move(t));
    }
    return p;
}

std::vector<std::uint64_t> ParamsOf(const ItemInstance& item) {
    std::vector<std::uint64_t> out;
    for (const ItemInstance::Option& o : item.options) out.push_back(std::uint64_t(o.id) | std::uint64_t(o.value) << 32);
    return out;
}

void SetParams(ItemInstance& item, std::span<const std::uint64_t> params) {
    item.options.clear();
    for (std::uint64_t p : params) item.options.push_back({std::uint32_t(p & 0xFFFFFFFFu), std::uint32_t(p >> 32)});
}

ItemInstance PrizeItem(const CouponPrize& prize, const assets::ItemRow& row, ItemInstance fresh, const std::vector<assets::MagicOptionRow>& options) {
    ItemInstance item = std::move(fresh);
    item.refId = row.refObjId;
    item.options.clear();
    const ItemKind kind = KindOf(row);
    const bool equipment = kind == ItemKind::Weapon || kind == ItemKind::Shield || kind == ItemKind::Armour || kind == ItemKind::Accessory;
    if (row.typeId[1] == 3) item.count = std::uint16_t(std::clamp(prize.count, 1, std::max(1, MaxStack(row))));
    for (const Token& t : prize.tokens) {
        switch (t.kind) {
        case 'M': {
            const std::uint32_t code = AttributeCode(t.attribute);
            for (const assets::MagicOptionRow& o : options) {
                if (o.attribute == code && o.degree == t.level) {
                    item.options.push_back({o.paramId, std::uint32_t(t.value)});
                    break;
                }
            }
            break;
        }
        case 'O':
            if (equipment) item.plus = std::uint8_t(std::min<std::uint64_t>(t.value, 12));
            break;
        case 'D': item.durability = std::uint32_t(std::min<std::uint64_t>(t.value, 0xFFFFFFFFu)); break;
        case 'V': item.variance = t.value; break;
        default: break;
        }
    }
    return item;
}

int Degree(const assets::ItemRow& row) {
    const ItemKind kind = KindOf(row);
    const bool equipment = kind == ItemKind::Weapon || kind == ItemKind::Shield || kind == ItemKind::Armour || kind == ItemKind::Accessory;
    return equipment ? (row.itemClass - 1) / 3 + 1 : row.itemClass;
}

bool InFilter(const assets::ItemRow& row, const std::array<int, 4>& typeId, int degree) {
    if (row.typeId != typeId) return false;
    return degree == 0 || Degree(row) == degree;
}

} // namespace sr::game::pop
