#include "sr/game/Fortress.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace sr::game {

FortressError ApplyRefusal(const FortressApplyFacts& f) {
    // The original's checks, in their order.
    if (!f.requestsOpen) return FortressError::NotApplyPeriod;
    if (!f.fortressKnown) return FortressError::NoFortress;
    if (!f.atOfficial) return FortressError::WrongNpc;
    if (f.guild == 0) return FortressError::NotInGuild;
    if (f.guildHoldsFortress || f.ownerOfThis) return FortressError::OwnsFortress;
    // The owner's union defends, the others attack.
    if ((f.side == proto::FortressSide::Defend) != f.alliedWithOwner) return FortressError::WrongAlliance;
    if (!f.master) return FortressError::OnlyMaster;
    if (f.guildLevel < kFortressApplyGuildLevel) return FortressError::GuildLevel;
    if (f.members < std::size_t(kFortressApplyMembers)) return FortressError::GuildMembers;
    // Only an attack pays: the fortress's.
    if (f.side == proto::FortressSide::Attack && f.gold < f.fee) return FortressError::Gold;
    if (f.applied) return FortressError::AlreadyApplied;
    return FortressError::None;
}

FortressError EntryRefusal(const FortressEntryFacts& f) {
    // The original's checks, in their order.
    if (f.jobSuit) return FortressError::EntryJobSuit;
    if (!f.warOn) return FortressError::None;
    if (f.guild == 0 || !f.participant) return FortressError::EntryNotParticipant;
    if (f.holderSide) {
        if (f.maxAdmission > 0 && f.defendersInside >= std::size_t(f.maxAdmission / 2)) return FortressError::EntryFull;
        return FortressError::None;
    }
    if (f.closed) return FortressError::EntryClosed;
    const int most = f.held ? f.maxAdmission / 2 : f.maxAdmission;
    if (f.maxAdmission > 0 && f.attackersInside >= std::size_t(most)) return FortressError::EntryFull;
    return FortressError::None;
}

FortressError CancelRefusal(const FortressCancelFacts& f) {
    // The original's checks, in their order.
    if (!f.requestsOpen) return FortressError::NotApplyPeriod;
    if (!f.fortressKnown) return FortressError::NoFortress;
    if (!f.atOfficial) return FortressError::WrongNpcCancel;
    if (f.guild == 0) return FortressError::NotInGuild;
    if (!f.master) return FortressError::OnlyMaster;
    if (f.ownerOfThis) return FortressError::OwnsFortress;
    if (f.appliedFortress == 0) return FortressError::NoApplication;
    if (f.appliedFortress != f.fortress) return FortressError::NoFortress;
    return FortressError::None;
}

FortressError StructureRefusal(bool warOn, WarSide owner, WarSide attacker) {
    if (!warOn) return FortressError::StructureOutOfWar;
    if (owner.guild == 0) return FortressError::None; // no owner: lets anyone at it
    if (attacker.guild == owner.guild) return FortressError::StructureOwnGuild;
    if (owner.alliance != 0 && attacker.alliance == owner.alliance) return FortressError::StructureOwnUnion;
    return FortressError::None;
}

bool PlayersHostileInWar(bool warOn, WarSide a, WarSide b) {
    if (a.alliance != 0 && a.alliance == b.alliance) return false;
    if (a.guild == b.guild) return false;
    return warOn;
}

std::uint32_t TopDamager(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& damage) {
    std::uint32_t best = 0;
    std::uint64_t most = 0;
    for (const auto& [guild, dealt] : damage) {
        if (dealt > most) { // strictly more: the first of equals stays
            most = dealt;
            best = guild;
        }
    }
    return best;
}

// --- The schedule ----------------------------------------------------------------------------------

namespace {

std::int64_t MakeLocal(int year, int month, int day, int hour, int minute, int second) {
    std::tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = second;
    t.tm_isdst = -1;
    return std::int64_t(std::mktime(&t));
}

std::tm Local(std::int64_t seconds) {
    const std::time_t t = std::time_t(seconds);
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

} // namespace

std::int64_t FortressTax(std::uint64_t price, int rate, bool exempt) {
    if (rate == 0 || (rate > 0 && exempt)) return 0;
    return std::int64_t(price) * rate / 100; // C++ division cuts toward 0
}

std::int64_t LocalSeconds(const std::string& date) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3 || y < 1970) return 0;
    return MakeLocal(y, mo, d, h, mi, s);
}

std::optional<Period> SchedulePeriod(const std::vector<ScheduleRow>& rows, int define, std::int64_t now) {
    std::optional<Period> best;
    const auto consider = [&](const Period& p, const ScheduleRow& row) {
        if (p.end <= now) return;
        if (row.dateStart != 0 && p.start < row.dateStart) return;
        if (row.dateEnd != 0 && p.start >= row.dateEnd) return;
        // The one holding now first, else the soonest.
        if (!best || (p.Contains(now) && !best->Contains(now)) || (p.Contains(now) == best->Contains(now) && p.start < best->start)) best = p;
    };
    for (const ScheduleRow& row : rows) {
        if (row.define != define || row.durationSeconds <= 0) continue;
        if (row.mainType != 1 && row.mainType != 3 && row.mainType != 4) continue; // other kinds not read
        // The days around now: a period still running, today, and the next weeks' (type 4: up to its weeks).
        const std::tm today = Local(now);
        const int cycle = std::max(1, row.typeDate);
        const int ahead = row.mainType == 4 ? 7 * cycle + 1 : 8;
        for (int offset = -7 * (row.mainType == 4 ? cycle : 1); offset <= ahead; ++offset) {
            const std::int64_t start = MakeLocal(today.tm_year + 1900, today.tm_mon + 1, today.tm_mday + offset, row.hour, row.minute, row.second);
            if (row.mainType == 3 || row.mainType == 4) {
                const std::tm day = Local(start);
                if (day.tm_wday + 1 != row.dayOfWeek) continue; // 1 Sunday .. 7 Saturday (tm_wday + 1)
            }
            if (row.mainType == 4) {
                // The week's number from DateStart's (1, then one more each Sunday), modulo the cycle (0 as the cycle),
                // must be SubInterval_Weeks.
                const std::tm first = Local(row.dateStart), day = Local(start);
                const std::int64_t firstSunday = MakeLocal(first.tm_year + 1900, first.tm_mon + 1, first.tm_mday - first.tm_wday, 12, 0, 0);
                const std::int64_t daySunday = MakeLocal(day.tm_year + 1900, day.tm_mon + 1, day.tm_mday - day.tm_wday, 12, 0, 0);
                if (row.dateStart != 0 && daySunday < firstSunday) continue;
                const std::int64_t week = 1 + (daySunday - firstSunday + 3600) / (7 * 86400);
                std::int64_t inCycle = week % cycle;
                if (inCycle == 0) inCycle = cycle;
                if (inCycle != std::max(1, row.weeks)) continue;
            }
            consider({start, start + row.durationSeconds}, row);
        }
    }
    return best;
}

// --- The client's picture --------------------------------------------------------------------------

std::vector<FortressView::Line> FortressView::Apply(const proto::FortressEvent& event) {
    std::vector<Line> lines;
    ++revision_;
    if (const auto* list = std::get_if<proto::FortressList>(&event)) {
        list_ = *list;
    } else if (const auto* info = std::get_if<proto::FortressApplyInfo>(&event)) {
        apply_ = *info;
        applyShow_ = true;
    } else if (const auto* answer = std::get_if<proto::FortressAnswer>(&event)) {
        answers_.push_back(*answer);
        const proto::FortressRecord* f = Fortress(answer->fortress);
        const std::string key = f != nullptr ? f->nameKey : std::string();
        if (answer->ok && answer->action == proto::FortressAction::Apply) {
            lines.push_back({answer->side == proto::FortressSide::Defend ? "UIIT_MSG_FORT_OFFICIAL_UNIONAPPLY_COMPLETE" : "UIIT_MSG_FORT_OFFICIAL_WARAPPLY_COMPLETE",
                             {}, key, {}, -1});
            apply_.applied = answer->fortress;
            apply_.side = answer->side;
        } else if (answer->ok && answer->action == proto::FortressAction::Cancel) {
            lines.push_back({answer->side == proto::FortressSide::Defend ? "UIIT_MSG_FORT_OFFICIAL_UNIONAPPLY_CANCEL" : "UIIT_MSG_FORT_OFFICIAL_WARAPPLY_CANCEL",
                             {}, key, {}, -1});
            apply_.applied = 0;
        } else if (answer->ok && answer->action == proto::FortressAction::TaxRate) {
            // "Tax rate of [%s] has been changed to [%d]%%." (1.188: the answer {1, 1, u16 rate}).
            Line line{"UIIT_MSG_FORT_MANAGER_TAXCHANGE_COMPLETE", {}, key, {}, -1};
            line.value = std::to_string(answer->amount);
            lines.push_back(line);
            if (tax_.fortress == answer->fortress) tax_.rate = std::int16_t(answer->amount);
        } else if (answer->ok && answer->action == proto::FortressAction::TaxLevy) {
            // "[%s] gold was levied as the tax." (1.188: {2, 1, i64 amount}; the shown tax less it).
            std::string digits = std::to_string(answer->amount), gold;
            for (std::size_t i = 0; i < digits.size(); ++i) {
                if (i > 0 && (digits.size() - i) % 3 == 0) gold += ',';
                gold += digits[i];
            }
            Line line{"UIIT_MSG_FORT_MANAGER_TAXLEVY_COMPLETE", {}, {}, {}, -1};
            line.value = gold;
            lines.push_back(line);
            if (tax_.fortress == answer->fortress) tax_.tax = std::max<std::int64_t>(0, tax_.tax - answer->amount);
        } else if (answer->ok && (answer->action == proto::FortressAction::Repair || answer->action == proto::FortressAction::Upgrade)) {
            // "[%s] Repair is complete." / "[%s] Upgrade is complete.": the structure as the map last showed it (ours).
            Line line{answer->action == proto::FortressAction::Repair ? "UIIT_MSG_FORT_BATTLEAIDE_REPAIR_COMPLETE" : "UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_COMPLETE",
                      {}, {}, {}, -1};
            for (const proto::FortressStructureRecord& r : structureList_.structures) {
                if (r.zone == answer->zone) line.structureRef = r.refObjId;
            }
            lines.push_back(std::move(line));
        } else if (answer->ok && answer->action == proto::FortressAction::Build) {
            // "[%s] Construction is complete." (the answer {0x0A, 1, fortress, zone}, the zone's structure named;
            // ours carries the structure's reference object in `amount`).
            Line line{"UIIT_MSG_FORT_BATTLEAIDE_CONSTRUCTION_COMPLETE", {}, {}, {}, -1};
            line.structureRef = std::uint32_t(answer->amount);
            lines.push_back(std::move(line));
        } else if (answer->ok && (answer->action == proto::FortressAction::Hired || answer->action == proto::FortressAction::Hire)) {
            hired_[answer->fortress] = std::uint8_t(answer->amount);
            if (answer->action == proto::FortressAction::Hire) {
                const std::uint8_t bit = std::uint8_t(answer->amount);
                (void)bit;
                lines.push_back({"UIIT_MSG_FORT_MANAGER_EMPLOY_COMPLETE", {}, {}, {}, -1});
            }
        } else if (answer->ok && (answer->action == proto::FortressAction::ForgeCancel || answer->action == proto::FortressAction::TrainCancel)) {
            Line line{"UIIT_MSG_FORT_SMITH_PRODUCT_FAIL", {}, {}, {}, -1}; // "Production of [%s] is cancelled."
            line.itemRef = std::uint32_t(answer->amount);
            lines.push_back(std::move(line));
        } else if (answer->ok && answer->action == proto::FortressAction::Bless) {
            lines.push_back({"UIIT_MSG_FORTRESS_BUFF", {}, {}, {}, -1});
        } else if (answer->ok && answer->action == proto::FortressAction::Introduce) {
            lines.push_back({"UIIT_MSG_GUILD_INTRODUCE_CHANGE", {}, {}, {}, -1}); // 0x1A has no body
        } else if (answer->ok && answer->action == proto::FortressAction::DismissObject) {
            lines.push_back({"UIIT_MSG_FORT_COMMON_DISMISS_COMPLETE", {}, {}, {}, -1}); // 0xB05E 0x16 has no body
        } else if (answer->ok && answer->action == proto::FortressAction::UseItem && (answer->amount == 6 || answer->amount == 7)) {
            lines.push_back({"UIIT_MSG_FORT_COMMON_SET_COMPLETE", {}, {}, {}, -1}); // a guard or a tablet's monster (ours: the item's TID4)
        } else if (!answer->ok) {
            if (const char* text = FortressErrorKey(answer->error); text != nullptr && *text != 0) lines.push_back({text, {}, {}, {}, -1});
        }

    } else if (const auto* tax = std::get_if<proto::FortressTaxInfo>(&event)) {
        tax_ = *tax;
        taxShow_ = true;
    } else if (const auto* structures = std::get_if<proto::FortressStructureList>(&event)) {
        structureList_ = *structures;
        structuresShow_ = true;
    } else if (const auto* score = std::get_if<proto::FortressScore>(&event)) {
        score_ = *score;
    } else if (const auto* schedule = std::get_if<proto::FortressSchedule>(&event)) {
        schedule_ = *schedule;
    } else if (const auto* intro = std::get_if<proto::FortressIntroduction>(&event)) {
        introduction_ = *intro;
    } else if (const auto* forge = std::get_if<proto::FortressForge>(&event)) {
        forge_ = *forge;
        forgeShow_ = true;
    } else if (const auto* dungeon = std::get_if<proto::FortressDungeon>(&event)) {
        dungeon_ = *dungeon;
        dungeonShow_ = true;
    } else if (const auto* s = std::get_if<proto::EntityStructure>(&event)) {
        structures_[s->entity] = *s;
    } else if (const auto* n = std::get_if<proto::FortressNotice>(&event)) {
        const proto::FortressRecord* f = Fortress(n->fortress);
        const std::string fortressKey = f != nullptr ? f->nameKey : std::string();
        // The stock client's lines for the original's kinds: the warnings
        // with its own fixed minutes; the period changes, the tax and the heart's opening print nothing.
        using K = proto::FortressNoticeKind;
        switch (n->kind) {
        case K::RequestsOpen: list_.requestsOpen = true; break;
        case K::RequestsClosed: list_.requestsOpen = false; break;
        case K::TaxOpen: list_.taxPeriod = true; break;
        case K::TaxClosed: list_.taxPeriod = false; break;
        case K::WarSoon: lines.push_back({"UIIT_MSG_FORT_WAR_BEFOREBEGIN", {}, {}, {}, proto::WarningMinutes(n->kind)}); break;
        case K::WarBegin:
            list_.warOn = true;
            score_ = {};
            lines.push_back({"UIIT_MSG_FORT_WAR_BEGIN", {}, {}, {}, -1});
            break;
        case K::WarEnd30:
        case K::WarEnd20:
        case K::WarEnd10:
        case K::WarEnd1: lines.push_back({"UIIT_MSG_FORT_WAR_BEFOREEND", {}, {}, {}, proto::WarningMinutes(n->kind)}); break;
        case K::WarEnd:
            list_.warOn = false;
            score_ = {};
            lines.push_back({"UIIT_MSG_FORT_WAR_END", {}, {}, {}, -1});
            break;
        case K::Conquer: lines.push_back({"UIIT_MSG_FORT_WAR_CONQUER", n->guild, fortressKey, {}, -1}); break;
        case K::StructureDestroyed:
            lines.push_back({n->guild.empty() || n->nameKey.find("HEADQUARTERS") == std::string::npos ? "UIIT_MSG_FORT_STRUCTURE_STATUS_DESTROY"
                                                                                                     : "UIIT_MSG_FORT_CAMP_STATUS_DESTROY",
                             n->guild, {}, n->nameKey, -1});
            break;
        case K::SealReleasing: lines.push_back({"UIIT_MSG_FORT_STRUCTURE_STATUS_CANCEL", {}, {}, {}, -1}); break;
        case K::HeartOpen: break;
        case K::RankUp: lines.push_back({"UIIT_MSG_FORT_BATTLERANK_GRANT", n->guild, n->nameKey, {}, -1}); break;
        case K::Hired: hired_[n->fortress] = std::uint8_t(n->value); break;
        case K::TaxRate: {
            // The rate in the list and the open tax window (no line).
            const std::int16_t rate = std::int16_t(std::uint16_t(n->value));
            for (proto::FortressRecord& r : list_.fortresses) {
                if (r.id == n->fortress) r.taxRate = std::uint16_t(rate);
            }
            if (tax_.fortress == n->fortress) tax_.rate = rate;
            break;
        }
        case K::RewardMaster: lines.push_back({"UIIT_STT_FORT_REWARD_MESSAGE_MASTER", {}, {}, {}, -1}); break;
        case K::Reward:
            if (n->value == 0) lines.push_back({"UIIT_STT_FORT_REWARD_MESSAGE_MASTER", {}, {}, {}, -1}); // the client prints only 0
            break;
        case K::DungeonCreated: lines.push_back({"UIIT_MSG_FORTRESS_DUNGEON_CREATE_COMPLATE_MSG", {}, {}, {}, -1}); break;
        case K::DungeonClosed: lines.push_back({"UIIT_MSG_FORTRESS_DUNGEON_CLOSE_COMPLATE_MSG", {}, {}, {}, -1}); break;
        case K::ForgeDone: {
            Line line{"UIIT_MSG_FORT_SMITH_PRODUCT_COMPLETE", {}, {}, {}, -1};
            line.itemRef = n->value;
            lines.push_back(std::move(line));
            break;
        }
        default: break;
        }
    }
    return lines;
}

std::uint32_t FortressView::WarArea(std::uint16_t region) const {
    for (const proto::FortressRecord& f : list_.fortresses) {
        if (f.atWar && std::find(f.regions.begin(), f.regions.end(), region) != f.regions.end()) return f.id;
    }
    return 0;
}

const proto::FortressRecord* FortressView::Fortress(std::uint32_t id) const {
    for (const proto::FortressRecord& f : list_.fortresses) {
        if (f.id == id) return &f;
    }
    return nullptr;
}

const proto::EntityStructure* FortressView::Structure(std::uint32_t entity) const {
    const auto it = structures_.find(entity);
    return it != structures_.end() ? &it->second : nullptr;
}

const char* FortressErrorKey(std::uint16_t code) {
    switch (FortressError(code)) {
    case FortressError::BuildNoReference:
    case FortressError::BuildNotBarricade:
    case FortressError::BuildWrongFortress:
    case FortressError::NoStructure:
    case FortressError::UpgradeBranch:
    case FortressError::WrongFortress:
    case FortressError::RepairFull:
    case FortressError::NoRevive: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_01";
    case FortressError::RepairGold: return "UIIT_MSG_FORT_BATTLEAIDE_REPAIR_FAIL";
    case FortressError::UpgradeRole: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06";
    case FortressError::UpgradeLevel: return "UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_FAIL2";
    case FortressError::UpgradeGold:
    case FortressError::UpgradePoints: return "UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_FAIL";
    case FortressError::UpgradeWait: return "UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_FAIL3";
    case FortressError::UpgradeNotWhole: return "UIIT_MSG_FORT_BATTLEAIDE_UPGRADE_FAIL4";
    case FortressError::ReturnUniform: return "UIIT_MSG_FORT_PORTALSTONE_FAIL_02";
    case FortressError::ReturnNotHolder: return "UIIT_MSG_FORT_PORTALSTONE_FAIL_03";
    case FortressError::ReturnWaiting:
    case FortressError::ReturnArena: return "UIIT_MSG_FORT_PORTALSTONE_FAIL_01";
    case FortressError::EntryJobSuit: return "UIIT_MSG_FORT_ENTRANCE_FORTRESS_FAIL_01";
    case FortressError::EntryFull: return "UIIT_MSG_FORT_PORTALSTONE_FAIL_04";
    case FortressError::EntryNotParticipant: return "UIIT_MSG_FORT_ENTRANCE_FORTRESS_FAIL_02";
    case FortressError::EntryNotMember: return "UIIT_MSG_FORT_NOT_POSSESS_FORTRESS_MEMBER";
    case FortressError::EntryClosed: return "UIIT_MSG_FORT_ETC_ERR_CANT_ENTER_5MINBEFORE";
    case FortressError::OnlyMaster: return "UIIT_MSG_FORT_ETC_ERROR_ONLYGUILDMASTER_ACTION";
    // The fortress manager's tax (1.188's table, group 0x1E).
    case FortressError::NotHolder: return "UIIT_MSG_FORT_POSITION_GRANT_FAIL_05";
    case FortressError::NotTaxPeriod: return "UIIT_MSG_FORT_MANAGER_TAX_ERROR_01";
    case FortressError::RateRange: return "UIIT_MSG_FORT_MANAGER_TAX_ERROR_02";
    case FortressError::SameRate: return "UIIT_MSG_FORT_MANAGER_TAX_ERROR_03";
    case FortressError::NotInGuild: return "UIIT_MSG_FORT_ETC_ERROR_NOTJOINGUILD";
    case FortressError::GuildLevel: return "UIIT_MSG_FORT_ETC_ERROR_TOOLESSGUILDLV";
    case FortressError::GuildMembers: return "UIIT_MSG_FORT_ETC_ERROR_TOOLESSGUILDMB";
    case FortressError::Gold: return "UIIT_MSG_INTERACTION_FAIL_NOT_ENOUGH_MONEY";
    case FortressError::AlreadyApplied: return "UIIT_MSG_FORT_OFFICIAL_FORTWAR_APPLY_ERROR_03";
    case FortressError::OwnsFortress: return "UIIT_MSG_FORT_ETC_ERROR_ALREADYPOSSESSFORT";
    case FortressError::WrongAlliance: return "UIIT_MSG_FORT_ETC_ERROR_WRONGALLIANCE";
    case FortressError::NoApplication: return "UIIT_MSG_FORT_OFFICIAL_FORTWAR_APPLY_ERROR_01";
    case FortressError::NotApplyPeriod: return "UIIT_MSG_FORT_OFFICIAL_FORTWAR_APPLY_ERROR_02";
    case FortressError::StructureOutOfWar: return "UIIT_MSG_FORT_BATTLE_ERR_CAN_ONLYPERIODFORT";
    case FortressError::StructureOwnGuild: return "UIIT_MSG_FORT_BATTLE_ERR_CANT_ATTACK_GUILD";
    case FortressError::StructureOwnUnion: return "UIIT_MSG_FORT_BATTLE_ERR_CANT_ATTACK_ALLYGUILD";
    case FortressError::HeartSealed: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_NOTDESTROYGUARD";
    case FortressError::HeartReleasing: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_DELAYTIME";
    case FortressError::NotParticipant: return "UIIT_MSG_FORT_ENTRANCE_FORTRESS_FAIL_02";
    case FortressError::PulleyFortress: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_03";
    case FortressError::ItemNotUsable:
    case FortressError::ItemWrongZone:
    case FortressError::ItemTooFar:
    case FortressError::ItemNotWalkable:
    case FortressError::ItemTargetFar:
    case FortressError::ItemInFortress:
    case FortressError::ItemZoneTaken: return "UIIT_MSG_FORT_NOT_USE_ITEM";
    case FortressError::ItemPosition:
    case FortressError::ItemPermission:
    case FortressError::ItemGroup: return "UIIT_STT_FORT_ITEMUSE_ERROR_POSITION";

    case FortressError::ItemNotInFortress: return "UIIT_STT_FORT_ITEMUSE_ERROR_FORTIN";
    case FortressError::ItemWrongTarget:
    case FortressError::ItemTargetState: return "UIIT_SKILL_USE_FAIL_WRONGTARGET";
    case FortressError::ItemNotInWar:
    case FortressError::ItemInWar: return "UIIT_STT_FORT_ITEMUSE_ERROR_FORTPERIOD";
    case FortressError::ItemDefender: return "UIIT_STT_FORT_ITEMUSE_ERROR_ONLY_ATTGUILD";
    case FortressError::ItemAttacker: return "UIIT_STT_FORT_ITEMUSE_ERROR_ONLY_DEFENGUILD";
    case FortressError::ItemNotHolder: return "UIIT_MSG_FORT_POSITION_GRANT_FAIL_05";
    case FortressError::ItemCampBuilt: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_NOTBUILD";
    case FortressError::ItemLimit: return "UIIT_MSG_FORT_ITEMUSE_ERR_NUMLIMIT";
    case FortressError::ItemAlreadyBuilt: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_ALREADY_BUILD";
    case FortressError::NotGuard: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_04";
    case FortressError::NotBarricade: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_05";
    case FortressError::NotOwnObject:
    case FortressError::DismissRole:
    case FortressError::RemoveRole: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_06";
    case FortressError::GateDestroyed: return "UIIT_MSG_FORT_STRUCTURE_ACTION_ERROR_02";
    case FortressError::HireDone: return "UIIT_MSG_FORT_MANAGER_EMPLOY_DUPLICATE";
    case FortressError::ForgeBusy: return "UIIT_MSG_FORT_SMITH_PRODUCT_ERROR_01";
    case FortressError::GuildPoints: return "UIIT_MSG_ERROR_GUILD_LEVEL_UP_GP_DEFICIT";
    case FortressError::ForgeCount: return "UIIT_MSG_FORT_SMITH_PRODUCT_ERROR_02";
    case FortressError::ForgePosition: return "UIIT_MSG_GUILDERR_PERMISSION_DENIED";
    case FortressError::ForgeNone: return "UIIT_MSG_FORT_SMITH_PRODUCT_ERROR_03";
    case FortressError::ForgeNotDone: return "UIIT_MSG_FORT_SMITH_PRODUCT_ERROR_04";
    case FortressError::ForgeTooMany: return "UIIT_MSG_FORT_SMITH_PRODUCT_ERROR_05";
    case FortressError::ForgeBagFull: return "UIIT_MSG_STRGERR_INVENTORY_FULL";
    case FortressError::InWar: return "UIIT_MSG_FORT_ETC_ERROR_WRONGACTION_FORTWAR";
    case FortressError::IntroLength: return "UIIT_MSG_FORTRESS_ERR_OVER_MAX_GUILD_INTRODUCTION_LEN";
    case FortressError::NoBless: return "UIIT_MSG_FORTRESS_ERR_NOT_FOUND_BLESS_BUFF";
    case FortressError::BlessFailed: return "UIIT_MSG_FORTRESS_ERR_CANT_ENGAGE_BLESS_BUFF";
    case FortressError::NoDungeon: return "UIIT_MSG_FORTRESS_ERR_NOT_FOUND_DUNGEON";
    case FortressError::DungeonCount: return "UIIT_MSG_FORTRESS_ERR_CANT_CREATE_DUNGEON_BY_MAX_COUNT";
    case FortressError::DungeonOpen: return "UIIT_MSG_FORTRESS_ERR_ALREADY_CREATED_DUNGEON";
    case FortressError::DungeonCreate: return "UIIT_MSG_FORTRESS_ERR_FAILED_CREATE_DUNGEON";
    case FortressError::DungeonClosed: return "UIIT_MSG_FORTRESS_ERR_NOT_CREATED_DUNGEON";
    case FortressError::DungeonPay: return "UIIT_MSG_FORTRESS_ERR_IMPOSSIBLE_ENTRY_DUNGEON";
    case FortressError::DungeonFull: return "UIIT_MSG_FORTRESS_ERR_IMPOSSIBLE_ENTRY_DUNGEON_BY_FULL";
    case FortressError::BlessWait: return "UIIT_SKILL_USE_FAIL_TIMEDELAY_COOL_TIME";
    case FortressError::DungeonTime: return "UIIT_MSG_FORTRESS_ERR_NOT_CREATED_TIME_DUNGEON";

    default: return ""; // 0x283A, 0x283B: the switch's default, printing nothing; 2 and 3 fall outside its table (not traced)
    }
}

} // namespace sr::game
