// The jobs' rules: the formulas against the note's worked examples, worked from the 1.188
// database's rows (no game files, no database needed).

#include <doctest/doctest.h>

#include "sr/assets/GameData.h"
#include "sr/game/Jobs.h"

using namespace sr;

namespace {

// The database's constants as far as the examples need them: GoldMin (3.5 L + 25, the _RefDropGold rows at levels
// 1, 20 and 60: 28, 95, 235) and the job levels' experience (_RefLevel.JobExp_*: the same for the three jobs).
game::JobRules Rules() {
    game::JobRules r;
    r.goldMin.assign(141, 0);
    for (int level = 1; level <= 140; ++level) r.goldMin[std::size_t(level)] = std::uint32_t(3.5 * level + 25);
    const std::uint64_t need[] = {0, 70875, 2388750, 8793750, 38745000, 91665000, 240187500, 0x7FFFFFFF};
    for (auto& t : r.levelExp) t.assign(std::begin(need), std::end(need));
    return r;
}

} // namespace

TEST_CASE("Jobs: the suit gives the state; the hostile pairs; the league's fees") {
    assets::ItemRow suit;
    suit.typeId = {3, 1, 7, 2};
    CHECK(game::SuitJob(suit) == game::JobType::Thief);
    CHECK(game::JobStateOf(&suit) == 2);
    suit.typeId = {3, 1, 7, 5}; // another TID4 of the special dress: no league's
    CHECK(game::JobStateOf(&suit) == game::kNoJob);
    CHECK(game::JobStateOf(nullptr) == game::kNoJob);
    CHECK(game::IsHostileJob(2, 1));
    CHECK(game::IsHostileJob(3, 2));
    CHECK_FALSE(game::IsHostileJob(1, 3));
    CHECK_FALSE(game::IsHostileJob(2, 2));
    CHECK_FALSE(game::IsHostileJob(2, game::kNoJob));
    CHECK(game::JobJoinFee(20) == 0);
    CHECK(game::JobJoinFee(60) == 200000);
    CHECK(game::AliasChangeFee(60) == 3000000);
}

TEST_CASE("Jobs: the market's quote and price; the worked example at Donwhang to the gold") {
    // NPC_WC_SPECIAL's row for ITEM_ETC_TRADE_CH_01: BaseQuot 1.88, LB 1.58, UB 2.18 (32-bit floats: 1.88 is
    // 1.8799999...), BaseStock 50,000, Fluctuate 250, CurStock 42,499 (just past the UB point).
    game::MarketRow row;
    row.baseQuote = 1.88f;
    row.lowQuote = 1.58f;
    row.highQuote = 2.18f;
    row.baseStock = 50000;
    row.fluctuate = 250;
    row.stock = 42499;
    CHECK(row.Quote() == 218);
    CHECK(row.Price() == 834);
    // 40 sold: 33,360, a margin of 18,040 over their 15,320; the trader's job experience at one star 14,432.
    const std::uint64_t payout = 40u * row.Price();
    CHECK(payout == 33360);
    const std::uint64_t margin = payout - 40u * game::kGoodsPrice;
    CHECK(margin == 18040);
    CHECK(game::SaleJobExp(game::JobType::Trader, margin, 1) == 14432);
    CHECK(game::SaleJobExp(game::JobType::Trader, margin, 3) == 9020);
    row.Sold(40);
    CHECK(row.stock == 42539);
    CHECK(row.Quote() == 217);
    CHECK(row.Price() == 831);
    // The low end: 605 a good.
    row.stock = 50000 + 30 * 250;
    CHECK(row.Quote() == 158);
    CHECK(row.Price() == 605);
    // A sale's stock held at the low quote's.
    row.Sold(40);
    CHECK(row.stock == 50000 + 30 * 250);
    // The 10-minute tick takes the stock back toward the UB point, one hundredth at a time, and no farther. The quote is
    // cut to hundredths from the float: 1.8799999 + 0.29 reads 1.58, and at rest on the UB point 2.17 (the original's
    // 0.01 below the decimal).
    row.Recover();
    CHECK(row.Quote() == 158);
    row.stock = 42500;
    row.Recover();
    CHECK(row.stock == 42500);
    CHECK(row.Quote() == 217);
    // The same 40 stolen, sold in the thieves' town: 11,490; the thief's experience on the margin of 15,320: 6,128.
    CHECK(game::StolenGoodsPayout(40) == 11490);
    CHECK(game::SaleJobExp(game::JobType::Thief, 40u * game::kGoodsPrice, 0) == 6128);
}

TEST_CASE("Jobs: the trade scale's value and stars; the bandits' count and level") {
    const game::JobRules r = Rules();
    CHECK(r.GoldUnit(60) == 293);
    CHECK(r.GoldUnit(20) == 118);
    // A level-60 trader, a full class-1 horse of 60 places: 2,400 goods, a value of 2,850: five stars.
    const std::uint32_t value = game::TradeValue(r, 2400, 60, game::CartCapacity(1));
    CHECK(value == 2850);
    CHECK(game::TradeStars(r, value) == 5);
    // A level-20 trader's donkey: 1,280 goods 3,774 (5 stars); 4 stacks, 160 goods, 471 (1 star); none: no stars.
    CHECK(game::TradeValue(r, 1280, 20, 40) == 3774);
    CHECK(game::TradeStars(r, game::TradeValue(r, 160, 20, 40)) == 1);
    CHECK(game::TradeValue(r, 160, 20, 40) == 471);
    CHECK(game::TradeStars(r, 0) == 0);
    CHECK(game::TradeStars(r, 511) == 2);
    // A tiny load still counts 1.
    CHECK(game::TradeValue(r, 1, 140, 40) == 1);
    // The bandits at that five-star load: 10, 11 at the 50 % roll, 22 when doubled; within the first scale one.
    CHECK(game::BanditCount(r, 2850, false, false, false) == 10);
    CHECK(game::BanditCount(r, 2850, false, true, false) == 11);
    CHECK(game::BanditCount(r, 2850, false, true, true) == 22);
    CHECK(game::BanditCount(r, 400, false, false, false) == 1);
    CHECK(game::BanditCount(r, 0, false, false, false) == 0); // no value, none
    CHECK(game::BanditCount(r, 400, true, false, false) == int((400 * 1.55 - 1) / 306 + 1)); // bargain goods at any value: 3
    CHECK(game::BanditCount(r, 2850, true, false, false) == int((2850 * 1.55 - 1) / 306) + 1);
    // Their levels: 60..63 for the five-star level-60 trader.
    CHECK(game::BanditLevel(60, 5, 0, false) == 60);
    CHECK(game::BanditLevel(60, 5, 3, false) == 63);
    CHECK(game::BanditLevel(10, 1, 0, false) == 16);
}

TEST_CASE("Jobs: experience: a kill's, a job death's loss, one level a gain, never lost") {
    const game::JobRules r = Rules();
    // Level 60 on level 60: 293, a trader half.
    CHECK(game::KillJobExp(r, 60, 60, false) == 293);
    CHECK(game::KillJobExp(r, 60, 60, true) == 146);
    // A much lower victim: at least half of A; a much higher one at most 1.5 x A.
    CHECK(game::KillJobExp(r, 20, 100, false) == std::uint64_t(118 * 0.5));
    CHECK(game::KillJobExp(r, 100, 20, false) == std::uint64_t(r.GoldUnit(100) * 1.5));
    CHECK(game::JobDeathLoss(r, 60) == 879);
    game::JobInfo info;
    info.type = game::JobType::Trader;
    CHECK_FALSE(game::GainJobExp(r, info, 70000));
    CHECK(info.level == 1);
    // A gain worth two levels climbs one; the rest is carried.
    CHECK(game::GainJobExp(r, info, 70875 + 2388750));
    CHECK(info.level == 2);
    CHECK(info.exp == 70000 + 2388750);
    CHECK(game::GainJobExp(r, info, 0)); // the carried rest is enough for the next
    CHECK(info.level == 3);
    // A loss stops at 0 and keeps the level.
    game::LoseJobExp(info, 1'000'000'000);
    CHECK(info.exp == 0);
    CHECK(info.level == 3);
    // The cap.
    info.level = game::kJobMaxLevel;
    CHECK_FALSE(game::GainJobExp(r, info, 1'000'000'000));
    CHECK(info.level == game::kJobMaxLevel);
}

TEST_CASE("Jobs: the bandits' place, tactics rows, lists and choice") {
    // The band of 150 / 3: 10-15 m below 70, 5-10 m to 89, within 5 m above.
    CHECK(game::BanditSpawnDistance(0, 0.0f) == doctest::Approx(100.0f));
    CHECK(game::BanditSpawnDistance(69, 1.0f) == doctest::Approx(150.0f));
    CHECK(game::BanditSpawnDistance(70, 0.0f) == doctest::Approx(50.0f));
    CHECK(game::BanditSpawnDistance(89, 1.0f) == doctest::Approx(100.0f));
    CHECK(game::BanditSpawnDistance(90, 0.5f) == doctest::Approx(25.0f));
    CHECK(game::BanditSpawnDistance(100, 1.0f) == doctest::Approx(50.0f));
    // Tab_RefTactics 2001..2004 / 2011..2014: btChangeTarget 0, 0, 1, 2.
    CHECK(game::BanditTacticsRow(true, 3) == 2004);
    CHECK(game::BanditTacticsRow(false, 0) == 2011);
    CHECK(game::BanditChangeTarget(0) == 0);
    CHECK(game::BanditChangeTarget(1) == 0);
    CHECK(game::BanditChangeTarget(2) == 1);
    CHECK(game::BanditChangeTarget(3) == 2);
    // The continents' lists.
    CHECK(game::BanditZoneOf("CHINA") == 0);
    CHECK(game::BanditZoneOf("Oasis_Kingdom") == 0);
    CHECK(game::BanditZoneOf("Eu") == 1);
    CHECK(game::BanditZoneOf("DELTA") == 1);
    CHECK(game::BanditZoneOf("Pharaoh") == -1);
    // Whom they go for: thief monsters traders and hunters (a player without a job only failing those), hunter monsters
    // thieves only.
    CHECK(game::BanditHunts(game::BanditKind::Thief, 1));
    CHECK(game::BanditHunts(game::BanditKind::Thief, 3));
    CHECK_FALSE(game::BanditHunts(game::BanditKind::Thief, 2));
    CHECK_FALSE(game::BanditHunts(game::BanditKind::Thief, game::kNoJob));
    CHECK(game::BanditFallback(game::BanditKind::Thief, game::kNoJob));
    CHECK(game::BanditHunts(game::BanditKind::Hunter, 2));
    CHECK_FALSE(game::BanditHunts(game::BanditKind::Hunter, 1));
    CHECK_FALSE(game::BanditFallback(game::BanditKind::Hunter, game::kNoJob));
    // The table: the level's bucket, else the next lower with rows (not below 2); the kinds and zones apart.
    game::BanditTable table;
    CHECK(table.Empty());
    table.Add(0, game::BanditKind::Thief, 30, 101);
    table.Add(0, game::BanditKind::Thief, 30, 102);
    table.Add(0, game::BanditKind::Thief, 20, 103);
    table.Add(1, game::BanditKind::Hunter, 50, 201);
    CHECK_FALSE(table.Empty());
    CHECK(table.Pick(0, game::BanditKind::Thief, 30, 0) == 101);
    CHECK(table.Pick(0, game::BanditKind::Thief, 30, 1) == 102);
    CHECK(table.Pick(0, game::BanditKind::Thief, 29, 5) == 103);
    CHECK(table.Pick(0, game::BanditKind::Thief, 200, 0) == 101); // held to 140, then down
    CHECK(table.Pick(0, game::BanditKind::Thief, 19, 0) == 0);
    CHECK(table.Pick(0, game::BanditKind::Hunter, 50, 0) == 0);
    CHECK(table.Pick(1, game::BanditKind::Hunter, 60, 0) == 201);
    CHECK(table.Pick(-1, game::BanditKind::Thief, 30, 0) == 0);
}
