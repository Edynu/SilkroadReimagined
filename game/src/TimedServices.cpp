#include "sr/game/TimedServices.h"

#include <algorithm>
#include <cstdlib>

namespace sr::game {

std::uint8_t TimedCategoryOf(const assets::ItemRow& row) {
    const auto& t = row.typeId;
    if (t[0] != 3 || t[1] != 3 || t[2] != 13) return 0;
    return t[3] == 4 ? kTimedGoldSilver : t[3] == 5 ? kTimedSkill : 0;
}

bool IsPremiumPackage(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 13, 14}; }

bool IsExpansion(const assets::ItemRow& row) { return row.typeId == std::array<int, 4>{3, 3, 13, 17}; }

bool DevilSpiritAwake(std::uint32_t data, std::int64_t now) { return data != 0 && std::int64_t(data) > now; }

TimedService TimedService::Begin(const assets::ItemRow& row, std::uint8_t category, std::int64_t now) {
    TimedService s;
    s.category = category;
    s.item = row.refObjId;
    s.start = now;
    s.days = std::max(1, int(std::max(0, row.params[0]) / kSecondsPerDay));
    s.daysLeft = s.days;
    s.dailyBonus = std::uint32_t(std::max(0, row.params[2]) / 1000);
    s.bonusToday = s.dailyBonus; // the first load (Data1 0): today's whole bonus
    s.expPercent = std::max(0, row.params[3]);
    s.skillPercent = std::max(0, row.params[4]);
    // The Open Market ticket (category 11): its Param2..4 are the market's (listings, seconds, the sale
    // fee), no bonus.
    if (category == 11) {
        s.dailyBonus = s.bonusToday = 0;
        s.expPercent = s.skillPercent = 0;
    }
    return s;
}

std::int32_t TimedService::TimeToKeep() const { return std::int32_t((std::uint32_t(secondsIntoDay) & 0xFFFFFF) << 8 | (std::uint32_t(daysLeft) & 0xFF)); }

TimedService TimedService::FromRecord(std::uint8_t category, std::uint32_t item, std::int32_t timeToKeep, std::int32_t data1, std::int32_t data2,
                                      std::int32_t data3, std::int32_t data4, std::int32_t data5, const assets::ItemRow* row) {
    TimedService s;
    if (row != nullptr) s = Begin(*row, category, data1);
    s.category = category;
    s.item = item;
    s.start = data1;
    s.daysLeft = std::uint32_t(timeToKeep) & 0xFF;
    s.secondsIntoDay = int(std::uint32_t(timeToKeep) >> 8);
    s.bonusToday = std::uint32_t(data2) & 0xFFFF;
    s.bonusYesterday = std::uint32_t(data2) >> 16;
    s.pausedSeconds = std::max(0, data3); // ours: the paused seconds (the original's Data3, inferred its paused days)
    s.paused = data4 != 0;
    s.pausedAt = data5;
    return s;
}

bool TimedService::Advance(std::int64_t now) {
    const std::int64_t stopped = pausedSeconds + (paused && pausedAt > 0 ? std::max<std::int64_t>(0, now - pausedAt) : 0);
    const std::int64_t ran = std::max<std::int64_t>(0, now - start - stopped);
    const std::int64_t elapsed = ran / kSecondsPerDay;
    const int before = days - daysLeft;
    if (elapsed > before) {
        const std::int64_t passed = elapsed - before;
        // Each day that passed: today's unused bonus becomes yesterday's (the older is lost), a new day's is given.
        bonusYesterday = passed == 1 ? bonusToday : dailyBonus;
        bonusToday = dailyBonus;
    }
    daysLeft = int(std::max<std::int64_t>(0, days - elapsed));
    secondsIntoDay = int(ran % kSecondsPerDay);
    return elapsed < days;
}

void TimedService::SpendBattle(double seconds) {
    if (paused || seconds <= 0) return;
    spendCarry += seconds;
    std::uint32_t whole = std::uint32_t(spendCarry);
    spendCarry -= double(whole);
    const std::uint32_t fromYesterday = std::min(whole, bonusYesterday);
    bonusYesterday -= fromYesterday;
    whole -= fromYesterday;
    bonusToday -= std::min(whole, bonusToday);
}

std::uint32_t TimedService::SecondsLeft(std::int64_t now) const {
    const std::int64_t stopped = pausedSeconds + (paused && pausedAt > 0 ? std::max<std::int64_t>(0, now - pausedAt) : 0);
    const std::int64_t end = start + stopped + std::int64_t(days) * kSecondsPerDay;
    return std::uint32_t(std::clamp<std::int64_t>(end - now, 0, 0x7FFFFFFF));
}

void TimedService::Pause(bool on, std::int64_t now) {
    if (on == paused) return;
    if (on) {
        paused = true;
        pausedAt = now;
    } else {
        pausedSeconds += std::max<std::int64_t>(0, now - pausedAt);
        paused = false;
        pausedAt = 0;
    }
}

void PremiumState::Advance(std::int64_t now, std::int64_t period) {
    if (item == 0 || period <= 0) return;
    if (periodStart == 0) periodStart = start;
    if (now - periodStart >= period) {
        periodStart += (now - periodStart) / period * period;
        used = {};
    }
}

namespace {
std::string Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}
} // namespace

std::vector<PremiumPart> PremiumPartsOf(const assets::ItemRow& row) {
    std::vector<PremiumPart> parts;
    for (int k = 1; k <= 3; ++k) {
        const std::string& text = row.paramTexts[std::size_t(k)];
        std::size_t at = 0;
        while ((at = text.find('[', at)) != std::string::npos) {
            const std::size_t end = text.find(']', at);
            if (end == std::string::npos) break;
            const std::string_view inner(text.data() + at + 1, end - at - 1);
            at = end + 1;
            const std::size_t colon = inner.find(':');
            if (colon == std::string_view::npos) continue;
            PremiumPart p;
            p.tag = Trim(inner.substr(0, colon));
            std::vector<std::string> cells;
            std::string_view rest = inner.substr(colon + 1);
            for (;;) {
                const std::size_t comma = rest.find(',');
                cells.push_back(Trim(rest.substr(0, comma)));
                if (comma == std::string_view::npos) break;
                rest.remove_prefix(comma + 1);
            }
            p.target = cells.empty() ? std::string() : cells[0];
            p.args.assign(cells.begin() + (cells.empty() ? 0 : 1), cells.end());
            if ((p.tag == "UIL1" || p.tag == "UQL1") && p.args.size() >= 3) {
                p.period = std::strtoll(p.args[1].c_str(), nullptr, 10);
                p.count = std::atoi(p.args[2].c_str());
            } else if (p.tag == "BFI1" && p.args.size() >= 2) {
                p.period = std::strtoll(p.args[1].c_str(), nullptr, 10);
            }
            parts.push_back(std::move(p));
        }
    }
    return parts;
}

} // namespace sr::game
