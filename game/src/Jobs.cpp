#include "sr/game/Jobs.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "sr/assets/GameData.h"

namespace sr::game {

JobType SuitJob(const assets::ItemRow& item) {
    if (item.typeId[0] != 3 || item.typeId[1] != 1 || item.typeId[2] != 7) return JobType::None;
    const int x = item.typeId[3];
    return x >= 1 && x <= 3 ? JobType(x) : JobType::None;
}

bool IsJobSuit(const assets::ItemRow& item) { return SuitJob(item) != JobType::None; }

std::uint8_t JobStateOf(const assets::ItemRow* suit) {
    const JobType job = suit != nullptr ? SuitJob(*suit) : JobType::None;
    return job == JobType::None ? kNoJob : std::uint8_t(job);
}

bool IsHostileJob(std::uint8_t a, std::uint8_t b) {
    constexpr std::uint8_t trader = 1, thief = 2, hunter = 3;
    return (a == thief && (b == trader || b == hunter)) || (b == thief && (a == trader || a == hunter));
}

bool JobsMayParty(std::uint8_t joining, std::uint8_t first) {
    constexpr std::uint8_t trader = 1, thief = 2, hunter = 3;
    switch (joining) {
    case trader:
    case hunter: return first == trader || first == hunter;
    case thief: return first == thief;
    case kNoJob: return first == kNoJob;
    default: return false;
    }
}

bool JobMayListPurpose(std::uint8_t state, std::uint8_t purpose) {
    switch (purpose) {
    case 0:
    case 1: return state == kNoJob;
    case 2: return state == 1 || state == 3;
    case 3: return state == 2;
    default: return false;
    }
}

std::uint64_t JobJoinFee(int level) { return std::uint64_t(std::max(0, level - kJobJoinLevel)) * 5000u; }
std::uint64_t AliasChangeFee(int level) { return std::uint64_t(std::max(0, level)) * 50000u; }

JobType LeagueOfNpc(std::string_view codename) {
    // The league keepers of (the codename's NPC part).
    static constexpr std::string_view kTraders[] = {"NPC_CH_DOCTOR", "NPC_WC_DOCTOR", "NPC_KT_DESIGNER", "NPC_EU_MERCHANT", "NPC_CA_MERCHANT",
                                                    "NPC_SD_M_AREA_MERCHANT"};
    static constexpr std::string_view kHunters[] = {"NPC_CH_GENARAL_SW", "NPC_WC_GENARAL_SW", "NPC_KT_MINISTER", "NPC_EU_HUNTER", "NPC_CA_HUNTER",
                                                    "NPC_SD_M_AREA_HUNTER"};
    static constexpr std::string_view kThieves[] = {"NPC_TD_THIEF_SELL", "NPC_SD_T_AREA_THIEF"};
    const auto in = [&](const auto& list) { return std::find(std::begin(list), std::end(list), codename) != std::end(list); };
    if (in(kTraders)) return JobType::Trader;
    if (in(kThieves)) return JobType::Thief;
    if (in(kHunters)) return JobType::Hunter;
    return JobType::None;
}

bool IsSpecialtyGood(const assets::ItemRow& row) { return row.typeId[0] == 3 && row.typeId[1] == 3 && row.typeId[2] == 8; }
bool IsBargainGood(const assets::ItemRow& row) { return IsSpecialtyGood(row) && row.typeId[3] == 2; }

bool IsSpecialtyNpc(std::string_view codename) {
    static constexpr std::string_view kNpcs[] = {"NPC_CH_SPECIAL", "NPC_WC_SPECIAL", "NPC_KT_SPECIAL", "NPC_EU_SPECIAL", "NPC_CA_SPECIAL",
                                                 "NPC_CH_SPECIAL2", "NPC_WC_SPECIAL2", "NPC_TK_SPECIAL", "NPC_RM_SPECIAL", "NPC_AM_SPECIAL",
                                                 "NPC_SD_M_AREA_SPECIAL", "NPC_SD_M_AREA_SPECIAL2", "NPC_SD_M_AREA_SPECIAL3"};
    return std::find(std::begin(kNpcs), std::end(kNpcs), codename) != std::end(kNpcs);
}

bool IsStolenGoodsBuyer(std::string_view codename) { return codename == "NPC_TD_THIEF_BUY"; }

std::string_view TradeQuestOf(std::string_view npc) {
    if (npc == "NPC_CH_SPECIAL2") return "QNO_TRADE_CH_SPECIAL2_1";
    if (npc == "NPC_WC_SPECIAL2") return "QNO_TRADE_WC_SPECIAL2_1";
    if (npc == "NPC_TK_SPECIAL") return "QNO_TRADE_TK_SPECIAL_1";
    if (npc == "NPC_RM_SPECIAL") return "QNO_TRADE_RM_SPECIAL_1";
    if (npc == "NPC_AM_SPECIAL") return "QNO_TRADE_AM_SPECIAL_1";
    return {};
}

int CartClassOf(std::string_view codename) {
    return codename.starts_with("COS_T_CAMEL") || codename.starts_with("COS_T_COW") || codename.starts_with("COS_T_BEHEMOTH") ? 2 : 1;
}

int CargoStars(const JobRules& rules, std::uint64_t goods, int level, std::string_view codename) {
    return TradeStars(rules, TradeValue(rules, goods, level, CartCapacity(CartClassOf(codename))));
}

bool AliasLengthOk(std::string_view alias) { return alias.size() >= std::size_t(kAliasMinBytes) && alias.size() <= std::size_t(kAliasMaxBytes); }

std::uint32_t JobRules::GoldUnit(int level) const {
    if (goldMin.size() < 2) return 1;
    const std::size_t at = std::size_t(std::clamp<int>(level, 1, int(goldMin.size()) - 1));
    return std::max<std::uint32_t>(1, std::uint32_t(double(goldMin[at]) * 1.25));
}

std::uint64_t JobRules::ExpToNext(JobType type, int level) const {
    const int k = int(type) - 1;
    if (k < 0 || k > 2 || level >= kJobMaxLevel || level < 1) return std::numeric_limits<std::uint64_t>::max();
    const std::vector<std::uint64_t>& table = levelExp[std::size_t(k)];
    return std::size_t(level) < table.size() && table[std::size_t(level)] > 0 ? table[std::size_t(level)] : std::numeric_limits<std::uint64_t>::max();
}

int MarketRow::Quote() const {
    const double step = fluctuate > 0 ? double(stock - baseStock) / double(fluctuate) * 0.01 : 0.0;
    const double q = std::clamp(double(baseQuote) - step, double(lowQuote), double(highQuote));
    return int(std::trunc(q * 100.0));
}

void MarketRow::Sold(std::int64_t count) {
    const std::int64_t atLow = baseStock + std::llround(double(baseQuote - lowQuote) * 100.0) * fluctuate;
    stock = std::min(stock + count, std::max(atLow, stock));
}

std::uint32_t MarketRow::Price(std::uint32_t price) const {
    const std::int64_t p = std::int64_t(price) + std::int64_t(price) * (Quote() - 100) / 100;
    return std::uint32_t(std::max<std::int64_t>(0, p));
}

void MarketRow::Recover() {
    // The stocks at the high and the low quote bound it (the quote never leaves [LB, UB] in either direction).
    const std::int64_t atHigh = baseStock - std::llround(double(highQuote - baseQuote) * 100.0) * fluctuate;
    const std::int64_t atLow = baseStock + std::llround(double(baseQuote - lowQuote) * 100.0) * fluctuate;
    stock = std::max<std::int64_t>(0, stock - fluctuate);
    stock = std::clamp(stock, std::min(atHigh, atLow), std::max(atHigh, atLow));
}

std::uint64_t StolenGoodsPayout(std::uint64_t count) { return std::uint64_t(double(kGoodsPrice) * double(count) * 0.75); }

int CartCapacity(int classByte) { return classByte == 1 ? 40 : classByte == 2 ? 30 : classByte; }

std::uint32_t TradeValue(const JobRules& rules, std::uint64_t goods, int level, int capacity) {
    if (goods == 0 || capacity <= 0) return 0;
    return TradeValueByUnit(goods, rules.GoldUnit(std::clamp(level, 20, 140)), capacity);
}

std::uint32_t TradeValueByUnit(std::uint64_t goods, std::uint64_t unit, int capacity) {
    if (goods == 0 || capacity <= 0 || unit == 0) return 0;
    // (40 / capacity): as a fraction (a class-2 transport's 30 counts 1.33; inferred, the class-1 40 is exact).
    const double value = double(goods * 348u / unit) / (40.0 / double(capacity));
    return std::max<std::uint32_t>(1, std::uint32_t(value));
}

int TradeStars(const JobRules& rules, std::uint32_t value) { return TradeStarsBy(rules.scale, value); }

int TradeStarsBy(std::span<const std::uint32_t> scale, std::uint32_t value) {
    if (value == 0) return 0;
    for (int n = 1; n <= 4 && std::size_t(n) <= scale.size(); ++n) {
        if (value <= scale[std::size_t(n - 1)]) return n;
    }
    return 5;
}

int BanditCount(const JobRules& rules, std::uint32_t value, bool bargain, bool extra, bool twice) {
    const std::uint32_t factor = std::max<std::uint32_t>(1, rules.monsterFactor);
    // No value, no bandits; bargain goods aboard: their formula at any value; else one up to the first scale.
    if (value == 0) return 0;
    int count = 1;
    if (bargain) {
        count = int((double(value) * 1.55 - 1.0) / double(factor) + 1.0);
    } else if (value > rules.scale[0]) {
        count = int((value - 1) / factor) + 1;
    }
    if (extra) ++count;
    if (twice) count *= 2;
    return count;
}

int BanditLevel(int level, int stars, int roll, bool twiceStars) {
    return std::max(level - 4, 16) + std::clamp(roll, 0, 3) + stars * (twiceStars ? 2 : 1) - 1;
}

float BanditSpawnDistance(int roll, float r) {
    const float band = kBanditSpawnRadius / 3.0f;
    r = std::clamp(r, 0.0f, 1.0f);
    if (roll < 70) return 2.0f * band + band * r;
    if (roll < 90) return band + band * r;
    return band * r;
}

int BanditTacticsRow(bool againstTrader, int k) { return (againstTrader ? 2001 : 2011) + std::clamp(k, 0, 3); }

int BanditChangeTarget(int k) {
    static constexpr int kChange[4] = {0, 0, 1, 2}; // Tab_RefTactics btChangeTarget of 2001..2004 (2011..2014 alike)
    return kChange[std::clamp(k, 0, 3)];
}

int BanditZoneOf(std::string_view continent) {
    for (std::string_view c : {"CHINA", "West_China", "Oasis_Kingdom", "Roc"}) {
        if (continent == c) return 0;
    }
    for (std::string_view c : {"Eu", "Am", "Ca", "DELTA", "SD", "KingsValley"}) {
        if (continent == c) return 1;
    }
    return -1;
}

bool BanditHunts(BanditKind kind, std::uint8_t jobState) {
    if (kind == BanditKind::Thief) return jobState == std::uint8_t(JobType::Trader) || jobState == std::uint8_t(JobType::Hunter);
    if (kind == BanditKind::Hunter) return jobState == std::uint8_t(JobType::Thief);
    return false;
}

bool BanditFallback(BanditKind kind, std::uint8_t jobState) { return kind == BanditKind::Thief && jobState == kNoJob; }

void BanditTable::Add(int zone, BanditKind kind, int level, std::uint32_t refObjId) {
    if (zone < 0 || zone > 1 || kind == BanditKind::None || level < 1 || level > kMaxLevel) return;
    rows_[std::size_t(zone)][kind == BanditKind::Thief ? 0 : 1][std::size_t(level)].push_back(refObjId);
}

std::uint32_t BanditTable::Pick(int zone, BanditKind kind, int level, std::uint32_t roll) const {
    if (zone < 0 || zone > 1 || kind == BanditKind::None) return 0;
    const auto& byLevel = rows_[std::size_t(zone)][kind == BanditKind::Thief ? 0 : 1];
    for (int l = std::min(level, kMaxLevel); l >= 2; --l) {
        const auto& bucket = byLevel[std::size_t(l)];
        if (!bucket.empty()) return bucket[roll % bucket.size()];
    }
    return 0;
}

bool BanditTable::Empty() const {
    for (const auto& zone : rows_) {
        for (const auto& kind : zone) {
            for (const auto& bucket : kind) {
                if (!bucket.empty()) return false;
            }
        }
    }
    return true;
}

std::uint64_t SaleJobExp(JobType job, std::uint64_t margin, int stars) {
    double rate = 0.0;
    switch (job) {
    case JobType::Trader: rate = stars == 1 ? 0.8 : 0.5; break;
    case JobType::Thief: rate = 0.4; break;
    case JobType::Hunter: rate = 1.0; break;
    default: return 0;
    }
    return std::max<std::uint64_t>(1, std::uint64_t(double(margin) * rate));
}

std::uint64_t KillJobExp(const JobRules& rules, int victimLevel, int receiverLevel, bool trader) {
    const double a = rules.GoldUnit(victimLevel), b = rules.GoldUnit(receiverLevel);
    std::uint64_t exp = std::uint64_t(a * std::clamp(a / b, 0.5, 1.5));
    if (trader) exp /= 2;
    return std::max<std::uint64_t>(1, exp);
}

std::uint64_t JobDeathLoss(const JobRules& rules, int level) { return 3u * std::uint64_t(rules.GoldUnit(level)); }

bool GainJobExp(const JobRules& rules, JobInfo& info, std::uint64_t gain) {
    info.exp += gain;
    const std::uint64_t need = rules.ExpToNext(info.type, info.level);
    if (info.level >= kJobMaxLevel || info.exp < need) return false;
    info.exp -= need;
    ++info.level;
    return true;
}

void LoseJobExp(JobInfo& info, std::uint64_t loss) { info.exp = info.exp > loss ? info.exp - loss : 0; }

} // namespace sr::game
