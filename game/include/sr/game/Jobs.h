#pragma once

// The jobs: trader, thief, hunter (the original's "trijob"). Headless, shared by the game server
// (which keeps and applies them) and the client (which shows them). The rules and numbers are the original's
// (its game server, the 1.188 database); the messages are ours (JobProtocol.h).
//
// - A character joins a job's league at its NPC (`_CharTrijob`: the type, the job level 1..7, its experience, the
//   week's contribution, the outcome waiting) and makes an alias (`_Char.NickName16`).
// - The job's state at run time comes from the suit worn in place 8 (itemdata TID 3/1/7/x, x 1 trader, 2 thief,
//   3 hunter); without one it is kNoJob (4), whatever the league.
// - The trade's numbers (the market's quote and price, the trade scale's stars, the bandits' count and level) and
//   the experience (from a sale, from a kill, a job death's loss, a level gained) are pure functions here, over the
//   database's constants (JobRules).

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace sr::assets {
struct ItemRow;
}

namespace sr::game {

// A league's type (`_CharTrijob.JobType`; the procedures' OBJ_JOBSTATE_TRADER 1 / ROBBER 2 / HUNTER 3).
enum class JobType : std::uint8_t { None = 0, Trader = 1, Thief = 2, Hunter = 3 };

// The job's state at run time: 1, 2, 3 from the suit worn, kNoJob without one.
inline constexpr std::uint8_t kNoJob = 4;

// A suit's job (TID 3/1/7/x with x 1..3: its JobType; any other item None).
JobType SuitJob(const assets::ItemRow& item);
// A league's own suit (3/1/7/1..3): only these carry
// the job's rules. The rest of 3/1/7 (the event suit 4, the PvP cape 5, the whole-body trader and hunter suits 6 and 7)
// are worn in the job place as ordinary equipment: skips every job check for them,
// does not count them, and they give no job state.
bool IsJobSuit(const assets::ItemRow& item);
// The state a suit worn gives (kNoJob for none or an item that is no league's suit).
std::uint8_t JobStateOf(const assets::ItemRow* suit);
// Whether two job states fight: thief and trader, thief and hunter.
bool IsHostileJob(std::uint8_t a, std::uint8_t b);
// The parties' pair rule, on the job states from the suits worn: a trader or a
// hunter with a trader or a hunter, a thief with a thief, no job with no job.
bool JobsMayParty(std::uint8_t joining, std::uint8_t first);
// Who may list a party matching purpose: 0 hunting and 1 quest without a
// suit, 2 trade for a trader or a hunter, 3 thieves for a thief.
bool JobMayListPurpose(std::uint8_t state, std::uint8_t purpose);

// --- The league ----------------------------------------------------------------------------

inline constexpr int kJobJoinLevel = 20;                 // the least character level to join (0x4819)
inline constexpr int kJobMaxLevel = 7;                   // the job level's cap
inline constexpr int kJobRejoinSeconds = 7 * 24 * 3600;  // the wait after leaving a league (0x4824; timed job 2)
inline constexpr int kAliasMinBytes = 2, kAliasMaxBytes = 12;
inline constexpr double kSuitChangeSeconds = 10.0;       // putting the suit on or off
inline constexpr double kSuitLockSeconds = 10.0;         // after it is on: no attacks on players, no taking it off (inferred: 10 ticks of 1 s)

// The fee to join: (level - 20) x 5,000. An alias's change: level x 50,000; the first is
// free. Leaving costs nothing.
std::uint64_t JobJoinFee(int level);
std::uint64_t AliasChangeFee(int level);

// The league an NPC keeps (its function 0x14 trader, 0x15 thief, 0x16 hunter, which the original's server gives by
// the codename): None for any other.
JobType LeagueOfNpc(std::string_view codename);
// An alias's length (2..12 bytes; the characters are the name rules', game::NameRules).
bool AliasLengthOk(std::string_view alias);

// A character's league as `_CharTrijob` keeps it.
struct JobInfo {
    JobType type = JobType::None;
    int level = 1;
    std::uint64_t exp = 0;
    std::uint32_t contribution = 0; // this week's (thieves and hunters; a trader's donations)
    std::uint32_t reward = 0;       // the outcome waiting at the hunter's NPC
    bool operator==(const JobInfo&) const = default;
};

// --- The database's constants -------------------------------------------------------------------------------------

struct JobRules {
    // `_RefShardContentConfig`: TRADESCALE1..5_AMOUNT_MAX, TRADE_MON_FACTOR, TRADE_SAFETY_OPTION / NUM (the code's
    // defaults, used only when a row is missing; the scale's first is 408 there).
    std::array<std::uint32_t, 5> scale{510, 918, 1428, 2142, 2856};
    std::uint32_t monsterFactor = 306;
    int safetyOption = 0; // 0 counted, 1 never, 2 unlimited
    int safetyCount = 8;
    // `_RefDropGold.GoldMin` by monster level (index the level; 0 unused).
    std::vector<std::uint32_t> goldMin;
    // `_RefLevel.JobExp_Trader / _Robber / _Hunter` by job level (index the level; 0 unused) and GUST_Mob_Exp by
    // character level.
    std::array<std::vector<std::uint64_t>, 3> levelExp;
    std::vector<std::uint64_t> mobExp;

    // int(GoldMin(level) x 1.25) (the trade scale's and the kills' unit; level held to the table).
    std::uint32_t GoldUnit(int level) const;
    // The experience job level `level` of `type` needs to the next (none past the table: the cap).
    std::uint64_t ExpToNext(JobType type, int level) const;
};

// --- Trade ---------------------------------------------------------------------------------

inline constexpr std::uint32_t kGoodsPrice = 383;     // a specialty good's Price (and its package's cost)
inline constexpr std::uint32_t kGoodsSellPrice = 191; // its SellPrice: sold where it has no quote (its own town)
inline constexpr float kCartReach = 1000.0f;          // a trader's transport within 100 m to buy or sell

// One good at one specialty NPC's market (`_ItemQuotation`), the quotes in hundredths (1.88 is 188).
struct MarketRow {
    std::uint32_t id = 0; // its `_ItemQuotation` row
    std::uint32_t npc = 0, item = 0;
    // BaseQuot, Quot_LB, Quot_UB as the original keeps them: 32-bit floats (1.88 is 1.8799999...).
    float baseQuote = 1.0f, lowQuote = 1.0f, highQuote = 1.0f;
    std::int64_t baseStock = 0, fluctuate = 1, stock = 0;
    // The quote now in hundredths: q = BaseQuot - (CurStock - BaseStock) / Fluctuate x 0.01 in floating point, held to
    // [LB, UB], cut to hundredths (trunc(q x 100)): a quote at rest on a bound stored just below its decimal reads 0.01 less.
    int Quote() const;
    // A good's price at that quote (`price` its Price, 383): price + trunc(price x (quote - 1)).
    std::uint32_t Price(std::uint32_t price = kGoodsPrice) const;
    // `count` sold here: the stock rises (the quote falls by one hundredth each `fluctuate`), held at the low quote's stock.
    void Sold(std::int64_t count);
    // The 10-minute tick: the stock falls by `fluctuate`, not below 0 and kept
    // between the stocks of the high and the low quote.
    void Recover();
};
inline constexpr int kMarketTickSeconds = 600;
// The bargain goods' sale hours: `_Schedule` rows of SCHEDULE_DEF_SPECIAL_GOODS_SELL (`_RefScheduleDefine` 3; daily
// 13:30, 16:30, 19:30, 22:30 for an hour), announced 30 minutes before.
inline constexpr int kScheduleSpecialGoods = 3;
inline constexpr int kBargainNoticeSeconds = 30 * 60;

// A specialty good (itemdata TID 3/3/8): bargain goods are TID4 2.
bool IsSpecialtyGood(const assets::ItemRow& row);
bool IsBargainGood(const assets::ItemRow& row);
// The specialty NPCs (function 0x0C; the three town buyers inferred) and the thieves' town buyer
// of stolen goods (0x0D).
bool IsSpecialtyNpc(std::string_view codename);
bool IsStolenGoodsBuyer(std::string_view codename);
// The trade quest a specialty NPC asks done before it deals; empty: none.
std::string_view TradeQuestOf(std::string_view npcCodename);
// A transport's capacity class from its codename (camels, cows and the behemoth 2, the rest 1:;
// ours by name, the original's not matched to a column).
int CartClassOf(std::string_view codename);
// The trade's stars of a cargo of `goods` for a trader whose highest level is `level`, on a transport of `codename`.
int CargoStars(const JobRules& rules, std::uint64_t goods, int level, std::string_view codename);

// What the thieves' town buyer pays: trunc(383 x count x 0.75).
std::uint64_t StolenGoodsPayout(std::uint64_t count);

// A transport's capacity class: 1 -> 40, 2 -> 30,
// else the byte itself.
int CartCapacity(int classByte);
// The trade's value: goods x 348 / GoldUnit(level) / (40 / capacity),
// the level the trader's highest held to [20, 140]; a value in (0, 1) is 1, none without goods.
std::uint32_t TradeValue(const JobRules& rules, std::uint64_t goods, int level, int capacity);
// The same with the level's unit given (the client's, from TradeScale).
std::uint32_t TradeValueByUnit(std::uint64_t goods, std::uint64_t unit, int capacity);
// Its stars: 0 without goods; the first n of 1..4 the value is within
// TRADESCALEn_AMOUNT_MAX, else 5.
int TradeStars(const JobRules& rules, std::uint32_t value);
// The same over the scale's bounds given (the first four of TRADESCALEn_AMOUNT_MAX).
int TradeStarsBy(std::span<const std::uint32_t> scale, std::uint32_t value);

// --- The bandits ---------------------------------------------------------------------------

// How many come: 1 within the first scale, else 1 + (value - 1) / factor; with bargain
// goods aboard (value x 1.55 - 1) / factor + 1; then one more at 50 % (`extra`), doubled at 2 % (`twice`).
int BanditCount(const JobRules& rules, std::uint32_t value, bool bargain, bool extra, bool twice);
// Their level: max(L - 4, 16) + `roll` (0..3) + stars - 1, the stars counted twice at 2 %
// (`twiceStars`); L the greater of the character's level and its highest mastery level.
int BanditLevel(int level, int stars, int roll, bool twiceStars);

// What a bandit is: a thief monster (itemdata-like TID 1/2/1/2, `MOB_THIEF*`, `MOB_EU_THIEF*`)
// sent against a trader's caravan, a hunter monster (1/2/1/3) against a thief's.
enum class BanditKind : std::uint8_t { None = 0, Thief = 1, Hunter = 2 };
// The caravan's waves: one 60-120 s after it is registered and again after each (60,000 + rand / 32767 x 60,000 ms).
inline constexpr int kCaravanIntervalMs = 60'000;
// Their tactics rows (`Tab_RefTactics` 2001..2004, 2011..2014): sight 250, trace 500, aggressive.
inline constexpr float kBanditSight = 250.0f;
inline constexpr float kBanditTrace = 500.0f;
// Around the transport, within 150.
inline constexpr float kBanditSpawnRadius = 150.0f;
// In a fight they go between a player and his transport when the other is this much nearer.
inline constexpr float kBanditSwitchCloser = 15.0f;
// Gone without dying 20 s after their last hit given or taken, and on entering a town.
inline constexpr int kBanditLeaveMs = 20'000;
// A champion at 2 % (rarity 1: double health).
inline constexpr int kBanditChampionPercent = 2;

// A bandit's distance from the transport (a band of 150 / 3): `roll` 0..100 below 70: 100 + 50 r, 70..89:
// 50 + 50 r, else 50 r, `r` 0..1 (about 70 % at 10-15 m, 20 % at 5-10 m, 10 % within 5 m).
float BanditSpawnDistance(int roll, float r);
// The wave's tactics row, 2001 + k against a trader or 2011 + k against a thief, k 3 at 50 % else rand % 4:
// its btChangeTarget (0, 0, 1, 2): 0 keeps its target when hit by another, 1 turns to that one at once, 2 by the
// ordinary choice.
int BanditTacticsRow(bool againstTrader, int k);
int BanditChangeTarget(int k);
// The bandits' list of a continent (`_RefRegion.ContinentName`): 0 the
// `MOB_THIEF*` / `MOB_HUNTER*` monsters in CHINA, West_China, Oasis_Kingdom, Roc; 1 the `MOB_EU_*` ones in Eu, Am, Ca,
// DELTA, SD, KingsValley; -1 elsewhere (no bandits).
int BanditZoneOf(std::string_view continent);
// Whom a bandit goes for (selectors; a COS answers for its owner): a thief monster first the
// traders and hunters (job states 1 and 3) and theirs, never a thief or a thief's; a hunter monster only thieves and
// theirs. `BanditFallback`: a thief monster takes a player without a job when none of those is in sight (inferred).
bool BanditHunts(BanditKind kind, std::uint8_t jobState);
bool BanditFallback(BanditKind kind, std::uint8_t jobState);

// The bandits by zone, kind and level: every thief (1/2/1/2) and hunter (1/2/1/3) monster of the reference data, one
// bucket per level 1..140.
class BanditTable {
public:
    static constexpr int kMaxLevel = 140;
    void Add(int zone, BanditKind kind, int level, std::uint32_t refObjId);
    // The bucket of min(level, 140), else the next lower that has rows down to 2; one of its rows by `roll` (0: none).
    std::uint32_t Pick(int zone, BanditKind kind, int level, std::uint32_t roll) const;
    bool Empty() const;

private:
    // [zone 0..1][kind 0 thief, 1 hunter][level 0..140]
    std::array<std::array<std::array<std::vector<std::uint32_t>, kMaxLevel + 1>, 2>, 2> rows_;
};

// --- Experience ----------------------------------------------------------------------------

// A sale's job experience: the margin x the rate (a trader 0.8 at one star,
// else 0.5; a thief 0.4; a hunter 1.0), at least 1.
std::uint64_t SaleJobExp(JobType job, std::uint64_t margin, int stars);
// A kill's: A x clamp(A / B, 0.5, 1.5), A the victim's GoldUnit and B the
// receiver's, halved for a trader, at least 1.
std::uint64_t KillJobExp(const JobRules& rules, int victimLevel, int receiverLevel, bool trader);
// A death in a job kill: 3 x GoldUnit(level) job experience.
std::uint64_t JobDeathLoss(const JobRules& rules, int level);
// Job experience gained: at most one level a gain, the rest carried, the cap
// kJobMaxLevel. True: a level was gained.
bool GainJobExp(const JobRules& rules, JobInfo& info, std::uint64_t gain);
// Job experience lost: to 0 at most; the level never falls.
void LoseJobExp(JobInfo& info, std::uint64_t loss);

} // namespace sr::game
