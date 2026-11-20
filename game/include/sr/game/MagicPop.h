#pragma once

// Magic POP's rules both sides share: the card and the coupon, the roll, the prize's
// options string, the coupon's params, the item a coupon becomes, the selection window's filter. Headless.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sr/assets/Gacha.h"
#include "sr/assets/GameData.h"
#include "sr/game/Items.h"

namespace sr::game::pop {

// The card (TID 3/3/14/1) and the coupon (3/3/14/2).
bool IsCard(const assets::ItemRow& row);
bool IsCoupon(const assets::ItemRow& row);
// A coupon's win flag (its Param1: 1 WIN, 0 LOSE), what the play's answer reports.
inline bool CouponWon(const assets::ItemRow& coupon) { return coupon.params[0] != 0; }

// The original's random draw: the statically linked MSVC rand(), 0..32,767 (inferred RAND_MAX from the CRT); a roll
// takes it mod 10,000, which favours 0..2,767 (3,000 wins 35.9 %, not 30 %). Kept: the
// server plays "as the original's did".
constexpr std::uint32_t kDrawRange = 32768;
constexpr int kRatioScale = 10000;

// A machine's waste set (the consolation prizes) by its running sums, scaled to 10,000 at load as the original
// (each weight / total x 10,000, truncated, the remainder spread from the first; zero weights left out).
struct WasteSet {
    std::vector<const assets::GachaEntry*> entries;
    std::vector<int> sums; // c1 < c2 < ... < cn = 10,000
    // The first entry whose running sum is >= r (0..9,999): the original's lower bound.
    const assets::GachaEntry* Pick(int r) const;
};
WasteSet MakeWasteSet(const assets::GachaTable& table, int set);

struct Roll {
    bool won = false;
    const assets::GachaEntry* prize = nullptr; // the chosen entry when won, else the waste set's (null: none there)
};
// The roll: r1 = draw1 mod 10,000 against the chosen entry's Ratio; lost, r2 = draw2 mod
// 10,000 picks the consolation prize. `draw1`, `draw2`: 0..kDrawRange - 1. A pure function, so tests drive it.
Roll RollPrize(const assets::GachaEntry& chosen, const WasteSet& waste, std::uint32_t draw1, std::uint32_t draw2);
// The chance a Ratio really has under the original's draw (ten-thousandths of a percent... as a fraction 0..1).
double TrueChance(int ratio);

// A token of the prize's options string: `<M:attr,L,V>` a magic option of the
// attribute (its four-letter code, `_RefMagicOpt.Param1`) whose MLevel is L with value V; `<O:n>` the plus; `<D:n>` the
// durability; `<V:n>` the variance.
struct Token {
    char kind = 0;
    std::string attribute; // M only
    int level = 0;
    std::uint64_t value = 0;
};
// The string's tokens: it must hold the marker "[BIIV]"; at most five tokens; false otherwise.
bool ParseOptions(std::string_view text, std::vector<Token>& out);
// An attribute's code as `_RefMagicOpt.Param1` keeps it ("int" -> 0x696E74).
std::uint32_t AttributeCode(std::string_view attribute);

// The coupon's params: [0] the prize's item, [1] its count, then two per token: a header (bits 0..7 1,
// 8..15 the letter, 16..23 L, 24..55 the attribute's code) and the value. At most 12.
std::vector<std::uint64_t> CouponParams(const assets::GachaEntry& prize);
struct CouponPrize {
    std::uint32_t item = 0;
    int count = 1;
    std::vector<Token> tokens;
};
std::optional<CouponPrize> ReadCoupon(std::span<const std::uint64_t> params);
// An item's options as the coupon's raw params (each option {low 32 bits, high 32 bits}) and back.
std::vector<std::uint64_t> ParamsOf(const ItemInstance& item);
void SetParams(ItemInstance& item, std::span<const std::uint64_t> params);

// What a coupon becomes: `fresh` the prize as a new item (an equipment's durability and white stats as a
// drop's, inferred), its count for an expendable, then the tokens: M -> the
// option (`options`' row of that attribute and MLevel; an attribute not there is left out, ours), O -> the plus (0..12,
// equipment only), D -> the durability, V -> the variance. Its serial is the coupon's (in place).
ItemInstance PrizeItem(const CouponPrize& prize, const assets::ItemRow& row, ItemInstance fresh, const std::vector<assets::MagicOptionRow>& options);

// The selection window's filter: an item of the node's type and, with a degree chosen, of
// that degree (equipment: (ItemClass - 1) / 3 + 1; others: ItemClass).
bool InFilter(const assets::ItemRow& row, const std::array<int, 4>& typeId, int degree);
int Degree(const assets::ItemRow& row);

} // namespace sr::game::pop
