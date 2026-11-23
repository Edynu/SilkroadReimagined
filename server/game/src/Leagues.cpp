#include "sr/server/Leagues.h"

#include <memory>

#include "sr/core/Log.h"
#include "sr/server/JobStore.h"

namespace sr::server {

namespace proto = game::proto;
using proto::JobCode;
using proto::JobRequest;

Leagues::Leagues(LeagueHost& host) : host_(host) {}

bool Leagues::IsRequest(proto::MessageId id) {
    return id == proto::JobJoin::kId || id == proto::JobLeave::kId || id == proto::JobAlias::kId || id == proto::JobPreviousQuery::kId ||
           id == proto::JobRankQuery::kId || id == proto::JobOutcome::kId;
}

bool Leagues::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload) {
    const auto with = [&]<typename T>(T message, void (Leagues::*handle)(std::uint32_t, const T&)) {
        if (!proto::Decode(payload, message)) return false;
        (this->*handle)(entity, message);
        return true;
    };
    switch (id) {
    case proto::JobJoin::kId: return with(proto::JobJoin{}, &Leagues::Join);
    case proto::JobLeave::kId: return with(proto::JobLeave{}, &Leagues::Leave);
    case proto::JobAlias::kId: return with(proto::JobAlias{}, &Leagues::Alias);
    case proto::JobPreviousQuery::kId: return with(proto::JobPreviousQuery{}, &Leagues::Previous);
    case proto::JobRankQuery::kId: return with(proto::JobRankQuery{}, &Leagues::Rank);
    case proto::JobOutcome::kId: return with(proto::JobOutcome{}, &Leagues::Outcome);
    default: return false;
    }
}

void Leagues::Answer(std::uint64_t peer, JobRequest request, JobCode code, std::uint8_t action) {
    host_.LeagueSend(peer, proto::JobAnswer::kId, proto::Encode(proto::JobAnswer{request, code, action}));
}

std::optional<LeagueHost::Member> Leagues::Again(std::uint32_t entity, std::int64_t character) {
    waiting_.erase(entity);
    auto m = host_.LeagueMember(entity);
    if (!m || m->character != character) return std::nullopt;
    return m;
}

void Leagues::Join(std::uint32_t entity, const proto::JobJoin& r) {
    const auto m = host_.LeagueMember(entity);
    if (!m || waiting_.contains(entity)) return;
    const auto refuse = [&](JobCode code) { Answer(m->peer, JobRequest::Join, code); };
    // The checks in the original's order.
    const game::JobType keeper = host_.LeagueKeeperNear(entity, r.npc);
    if (keeper == game::JobType::None) return refuse(JobCode::InvalidTarget);
    if (r.type < 1 || r.type > 3) return refuse(JobCode::NotInLeague);
    const auto type = game::JobType(r.type);
    if (keeper != type) return refuse(JobCode::NotOffered);
    if (m->level < game::kJobJoinLevel) return refuse(JobCode::LevelTooLow);
    if (m->job->type != game::JobType::None) return refuse(JobCode::InALeague);
    const std::uint64_t fee = game::JobJoinFee(m->level);
    if (m->gold < fee) return refuse(JobCode::NotEnoughGold);
    if (*m->leftAt != 0 && host_.LeagueNow() - *m->leftAt < game::kJobRejoinSeconds) return refuse(JobCode::RejoinWait);
    // The fee and the join in one transaction (the original charges once its job has succeeded, and saves both together).
    waiting_.insert(entity);
    struct Joined {
        bool ran = false;
        game::JobInfo info;
    };
    auto joined = std::make_shared<Joined>();
    const std::int64_t character = m->character;
    host_.LeaguePay(character, fee,
                    [joined, character, type](Database& tx) -> std::optional<std::uint64_t> {
                        joined->ran = true;
                        if (!JoinLeague(tx, character, type, joined->info)) return std::nullopt;
                        return 0;
                    },
                    [this, joined, entity, character](bool ok, std::uint64_t) {
                             const auto again = Again(entity, character);
                             if (!ok) {
                                 if (again) Answer(again->peer, JobRequest::Join, joined->ran ? JobCode::Failed : JobCode::NotEnoughGold);
                                 return;
                             }
                             if (!again) return; // gone: the database has it for its next entry
                             const game::JobInfo before = *again->job;
                             *again->job = joined->info;
                             again->job->reward = before.reward;
                             *again->leftAt = 0;
                             core::LogInfo("leagues: {} joined league {} at job level {}", character, int(joined->info.type), joined->info.level);
                             Answer(again->peer, JobRequest::Join, JobCode::Ok);
                             host_.LeagueChanged(entity);
                         });
}

void Leagues::Leave(std::uint32_t entity, const proto::JobLeave& r) {
    const auto m = host_.LeagueMember(entity);
    if (!m || waiting_.contains(entity)) return;
    const auto refuse = [&](JobCode code) { Answer(m->peer, JobRequest::Leave, code); };
    const game::JobType keeper = host_.LeagueKeeperNear(entity, r.npc);
    if (keeper == game::JobType::None) return refuse(JobCode::InvalidTarget);
    if (m->job->type == game::JobType::None) return refuse(JobCode::NotInLeague);
    if (keeper != m->job->type) return refuse(JobCode::NotOffered);
    if (m->jobState != game::kNoJob) return refuse(JobCode::SuitWorn);
    if (m->jobQuest) return refuse(JobCode::QuestUnderWay);
    waiting_.insert(entity);
    auto left = std::make_shared<bool>(false);
    const std::int64_t character = m->character, now = host_.LeagueNow();
    host_.LeagueDatabase([left, character, now](Database& db) { *left = LeaveLeague(db, character, now); },
                         [this, left, entity, character, now] {
                             const auto again = Again(entity, character);
                             if (!again) return;
                             if (!*left) return Answer(again->peer, JobRequest::Leave, JobCode::Failed);
                             const game::JobInfo before = *again->job;
                             *again->job = {}; // the contribution and the reward too (LeaveLeague), as the original's leave
                             again->alias->clear();
                             *again->leftAt = now;
                             core::LogInfo("leagues: {} left league {}", character, int(before.type));
                             Answer(again->peer, JobRequest::Leave, JobCode::Ok);
                             host_.LeagueChanged(entity);
                         });
}

void Leagues::Alias(std::uint32_t entity, const proto::JobAlias& r) {
    const auto m = host_.LeagueMember(entity);
    if (!m || waiting_.contains(entity)) return;
    const std::uint8_t action = r.action == proto::kAliasSet ? proto::kAliasSet : proto::kAliasCheck;
    const auto refuse = [&](JobCode code) { Answer(m->peer, JobRequest::Alias, code, action); };
    const game::JobType keeper = host_.LeagueKeeperNear(entity, r.npc);
    if (keeper == game::JobType::None) return refuse(JobCode::InvalidTarget);
    if (m->job->type == game::JobType::None) return refuse(JobCode::NotInLeague);
    if (keeper != m->job->type) return refuse(JobCode::NotOffered);
    if (m->jobState != game::kNoJob) return refuse(JobCode::SuitWorn);
    if (!game::AliasLengthOk(r.name) || !host_.AliasLettersOk(r.name)) return refuse(JobCode::NameNotAllowed);
    const std::int64_t character = m->character;
    const auto code = [](int free) {
        return free == 1 ? JobCode::Ok : free == -1 ? JobCode::NameIsCharacter : free == -2 ? JobCode::NameIsAlias : JobCode::Failed;
    };
    if (action == proto::kAliasCheck) {
        waiting_.insert(entity);
        auto free = std::make_shared<int>(0);
        host_.LeagueDatabase([free, name = r.name](Database& db) { *free = AliasFree(db, name); },
                             [this, free, entity, character, code] {
                                 if (const auto again = Again(entity, character)) Answer(again->peer, JobRequest::Alias, code(*free), proto::kAliasCheck);
                             });
        return;
    }
    // A change costs level x 50,000 (the first alias is free), in one transaction with the alias.
    const std::uint64_t fee = m->alias->empty() ? 0 : game::AliasChangeFee(m->level);
    if (m->gold < fee) return refuse(JobCode::NotEnoughGold);
    waiting_.insert(entity);
    auto given = std::make_shared<std::pair<bool, int>>(false, 0); // ran, AssignAlias's answer
    host_.LeaguePay(character, fee,
                    [given, character, name = r.name](Database& tx) -> std::optional<std::uint64_t> {
                        given->first = true;
                        given->second = AssignAlias(tx, character, name);
                        if (given->second != 1) return std::nullopt;
                        return 0;
                    },
                    [this, given, entity, character, code, name = r.name](bool ok, std::uint64_t) {
                             const auto again = Again(entity, character);
                             if (!again) return;
                             if (!ok) {
                                 return Answer(again->peer, JobRequest::Alias, given->first ? code(given->second == 1 ? 0 : given->second) : JobCode::NotEnoughGold,
                                               proto::kAliasSet);
                             }
                             *again->alias = name;
                             core::LogInfo("leagues: {} took the alias {}", character, name);
                             Answer(again->peer, JobRequest::Alias, JobCode::Ok, proto::kAliasSet);
                             host_.LeagueChanged(entity);
                         });
}

void Leagues::Previous(std::uint32_t entity, const proto::JobPreviousQuery& r) {
    const auto m = host_.LeagueMember(entity);
    if (!m || waiting_.contains(entity)) return;
    if (host_.LeagueKeeperNear(entity, r.npc) == game::JobType::None) return Answer(m->peer, JobRequest::Previous, JobCode::InvalidTarget);
    waiting_.insert(entity);
    auto old = std::make_shared<std::pair<bool, OldJobs>>();
    const std::int64_t character = m->character;
    host_.LeagueDatabase([old, character](Database& db) { old->first = LoadOldJobs(db, character, old->second); },
                         [this, old, entity, character] {
                             const auto again = Again(entity, character);
                             if (!again) return;
                             if (!old->first) return Answer(again->peer, JobRequest::Previous, JobCode::NoPrevious);
                             const OldJobs& o = old->second;
                             host_.LeagueSend(again->peer, proto::JobPrevious::kId,
                                              proto::Encode(proto::JobPrevious{std::uint8_t(o.traderLevel), std::uint8_t(o.thiefLevel), std::uint8_t(o.hunterLevel),
                                                                               o.traderExp, o.thiefExp, o.hunterExp}));
                         });
}

// A ranking: any league's at any keeper; the NPC (3), the type (0x4828), the kind
// (0x480F); the rows from the week's tables (`Tab_RefRanking_*`, written by SroDb --job-week).
void Leagues::Rank(std::uint32_t entity, const proto::JobRankQuery& r) {
    const auto m = host_.LeagueMember(entity);
    if (!m || waiting_.contains(entity)) return;
    const auto refuse = [&](JobCode code) { Answer(m->peer, JobRequest::Rank, code, r.kind); };
    if (host_.LeagueKeeperNear(entity, r.npc) == game::JobType::None) return refuse(JobCode::InvalidTarget);
    if (r.type < 1 || r.type > 3) return refuse(JobCode::NotInLeague);
    if (r.kind > 1) return refuse(JobCode::NotOffered);
    waiting_.insert(entity);
    auto rows = std::make_shared<std::vector<JobRankRow>>();
    const std::int64_t character = m->character;
    const auto type = game::JobType(r.type);
    const int kind = r.kind;
    host_.LeagueDatabase(
        [rows, type, kind](Database& db) {
            std::string error;
            LoadJobRanking(db, type, kind, *rows, error);
        },
        [this, rows, entity, character, r] {
            const auto again = Again(entity, character);
            if (!again) return;
            proto::JobRanking out{r.type, r.kind, {}};
            for (const JobRankRow& row : *rows) {
                out.rows.push_back({std::uint8_t(std::clamp(row.rank, 0, 255)), row.alias, std::uint8_t(std::clamp(row.level, 0, 255)),
                                    std::uint32_t(std::clamp<std::int64_t>(row.value, 0, 0xFFFFFFFF)), std::uint8_t(row.country & 0xFF)});
            }
            host_.LeagueSend(again->peer, proto::JobRanking::kId, proto::Encode(out));
        });
}

// The week's outcome: the NPC (3), a thief's or hunter's league (0x4828), the NPC that league's (0x480F),
// the action (0x480F), a collection only of something (0x482A). Asked: what there is (even 0); collected: paid as gold,
// the reward 0 (`TakeJobReward`).
void Leagues::Outcome(std::uint32_t entity, const proto::JobOutcome& r) {
    const auto m = host_.LeagueMember(entity);
    if (!m || waiting_.contains(entity)) return;
    const auto refuse = [&](JobCode code) { Answer(m->peer, JobRequest::Outcome, code, r.action); };
    const game::JobType keeper = host_.LeagueKeeperNear(entity, r.npc);
    if (keeper == game::JobType::None) return refuse(JobCode::InvalidTarget);
    const game::JobType league = m->job->type;
    if (league != game::JobType::Thief && league != game::JobType::Hunter) return refuse(JobCode::NotInLeague);
    if (keeper != league || r.action > 1) return refuse(JobCode::NotOffered);
    waiting_.insert(entity);
    const std::int64_t character = m->character;
    if (r.action == 1) {
        // Collected: the reward out of the row and into the gold in one transaction (the original pays it and clears it
        // with one character save).
        host_.LeaguePay(character, 0,
                        [character](Database& tx) -> std::optional<std::uint64_t> {
                            return std::uint64_t(std::clamp<std::int64_t>(TakeJobReward(tx, character), 0, 0xFFFFFFFF));
                        },
                        [this, entity, character, r](bool ok, std::uint64_t paid) {
                            const auto again = Again(entity, character);
                            if (!again) return;
                            if (!ok || paid == 0) return Answer(again->peer, JobRequest::Outcome, JobCode::NoOutcome, r.action);
                            again->job->reward = 0;
                            host_.LeagueSend(again->peer, proto::JobOutcomeAnswer::kId,
                                             proto::Encode(proto::JobOutcomeAnswer{r.npc, r.action, std::uint32_t(paid)}));
                        });
        return;
    }
    auto amount = std::make_shared<std::int64_t>(0);
    host_.LeagueDatabase([amount, character](Database& db) { *amount = db.Query("SELECT reward FROM _chartrijob WHERE charid = $1", {character}).Int(0, 0); },
                         [this, amount, entity, character, r] {
                             const auto again = Again(entity, character);
                             if (!again) return;
                             const std::uint32_t paid = std::uint32_t(std::clamp<std::int64_t>(*amount, 0, 0xFFFFFFFF));
                             host_.LeagueSend(again->peer, proto::JobOutcomeAnswer::kId, proto::Encode(proto::JobOutcomeAnswer{r.npc, r.action, paid}));
                         });
}

} // namespace sr::server
