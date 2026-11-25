#include "sr/server/Quests.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "sr/core/Log.h"
#include "sr/game/QuestItems.h"

namespace sr::server {

namespace proto = game::proto;
using game::QuestDef;
using game::QuestGoal;
using game::QuestGoalKind;

namespace {

std::int64_t UnixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool Contains(const std::vector<std::string>& list, const std::string& s) { return std::find(list.begin(), list.end(), s) != list.end(); }

// What a goal needs: its count, or one delivery or talk.
std::uint16_t Need(const QuestGoal& g) {
    switch (g.kind) {
    case QuestGoalKind::Kill:
    case QuestGoalKind::KillPlayer:
    case QuestGoalKind::Record:
    case QuestGoalKind::Drop: return std::uint16_t(std::min<std::uint32_t>(g.count, 0xFFFF));
    default: return 1;
    }
}

// The kinds the server plays now.
bool Playable(const QuestDef& q, std::string& why) {
    if (q.disabled) return why = "disabled", false;
    if (!q.special.empty()) return why = "the original's own script", false;
    if (q.goals.size() > proto::kQuestGoals) return why = "too many goals", false;
    return true;
}

// The class a monster's rarity counts as (the low half: 0 normal, 1 champion, 3 unique, 4 giant; a party
// monster as its base class).
bool ClassFits(const QuestGoal& g, std::uint8_t rarity) {
    if (g.classes.empty()) return true;
    const std::uint8_t base = rarity & 0x0F;
    for (game::MonsterClass c : g.classes) {
        if ((c == game::MonsterClass::Normal && base == 0) || (c == game::MonsterClass::Champion && base == 1) ||
            (c == game::MonsterClass::Unique && base == 3) || (c == game::MonsterClass::Giant && base == 4)) {
            return true;
        }
    }
    return false;
}

bool Night(int hour) { return hour < 4 || hour >= 20; }

// The server's local calendar day of a Unix time (month and day: the original compares only those two).
std::pair<int, int> LocalMonthDay(std::int64_t unix) {
    const std::time_t t = std::time_t(unix);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return {local.tm_mon, local.tm_mday};
}

// A 3D distance within `range` in one world (the quest kill's share: 1000 from the killer, the original's).
bool Near(const proto::WorldPosition& a, const proto::WorldPosition& b, double range) {
    if (a.layer != b.layer) return false;
    double dx = 0, dz = 0;
    if ((a.region & 0x8000) != 0 || (b.region & 0x8000) != 0) {
        if (a.region != b.region) return false;
        dx = double(a.x) - double(b.x);
        dz = double(a.z) - double(b.z);
    } else {
        const proto::FlatPosition fa = proto::ToFlat(a), fb = proto::ToFlat(b);
        dx = fa.x - fb.x;
        dz = fa.z - fb.z;
    }
    const double dy = double(a.y) - double(b.y);
    return dx * dx + dy * dy + dz * dz <= range * range;
}

} // namespace

std::pair<std::int64_t, std::int64_t> PackQuestProgress(const std::array<std::uint16_t, proto::kQuestGoals>& progress) {
    std::uint64_t a = 0, b = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        a |= std::uint64_t(progress[i]) << (16 * i);
        b |= std::uint64_t(progress[4 + i]) << (16 * i);
    }
    return {std::int64_t(a), std::int64_t(b)};
}

std::array<std::uint16_t, proto::kQuestGoals> UnpackQuestProgress(std::int64_t data1, std::int64_t data2) {
    std::array<std::uint16_t, proto::kQuestGoals> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = std::uint16_t(std::uint64_t(data1) >> (16 * i));
        out[4 + i] = std::uint16_t(std::uint64_t(data2) >> (16 * i));
    }
    return out;
}

Quests::Quests(QuestHost& host, Rates rates) : host_(host), rates_(rates) {}

Quests::Loaded Quests::Load(std::vector<QuestDef> quests) {
    Loaded loaded;
    quests_.clear();
    byId_.clear();
    byCode_.clear();
    for (auto& [entity, p] : players_) { // their rules go with the old definitions
        p.searches.clear();
        for (const auto& [quest, c] : p.catches) host_.QuestCosRemove(c.cos);
        p.catches.clear();
    }
    for (QuestDef& q : quests) {
        // The original's item handlers for its coded quests, as our rules (game/QuestItems.h): added when the
        // definition has none for the item.
        game::AddOriginalUseRules(q);
        // The record's places of the original's bytes (a catch's words 2 and 5, the supply's day 6) above the goals' counts.
        const bool catches = std::any_of(q.onUse.begin(), q.onUse.end(), [](const QuestDef::UseRule& u) { return u.capture.has_value(); });
        if ((catches && q.goals.size() > kCatchMinutesSlot) || (q.resupplyDaily && q.goals.size() > kSupplyDaySlot) ||
            (q.timerSeconds != 0 && q.goals.size() > kTimerSlot)) {
            core::LogWarning("quests: {} has {} goals: its catch's, daily supply's or timer's record words overlap them", q.codename, q.goals.size());
        }
        // DivideType 1 is a party quest (the original's test on the offer and on every kill: a party sharing experience and
        // items); a party quest's kills go to every holder near (the coded party quests set DivideType 1).
        if (q.divide == 1) q.party = true;
        if (q.party && q.divide == 0) q.divide = 1;
        std::string why;
        const bool play = Playable(q, why);
        if (!play) ++loaded.waiting;
        else ++loaded.offered;
        // Every quest is known by its codename (another's dependency); only the playable ones are offered.
        if (!play) {
            q.givers.clear();
            q.given = false;
        }
        byId_[q.id] = quests_.size();
        byCode_[q.codename] = quests_.size();
        quests_.push_back(std::move(q));
    }
    return loaded;
}

const QuestDef* Quests::Find(std::uint32_t id) const {
    const auto it = byId_.find(id);
    return it != byId_.end() ? &quests_[it->second] : nullptr;
}

const QuestDef* Quests::Find(std::string_view codename) const {
    const auto it = byCode_.find(std::string(codename));
    return it != byCode_.end() ? &quests_[it->second] : nullptr;
}

Quests::PlayerQuests* Quests::Of(std::uint32_t entity) {
    const auto it = players_.find(entity);
    return it != players_.end() ? &it->second : nullptr;
}
const Quests::PlayerQuests* Quests::Of(std::uint32_t entity) const {
    const auto it = players_.find(entity);
    return it != players_.end() ? &it->second : nullptr;
}

bool Quests::JobQuestUnderWay(std::uint32_t entity) const {
    const PlayerQuests* p = Of(entity);
    if (p == nullptr) return false;
    for (const auto& [id, r] : p->records) {
        if (r.status != proto::QuestStatus::Achieving && r.status != proto::QuestStatus::Achieved) continue;
        const game::QuestDef* q = Find(id);
        if (q == nullptr) continue;
        if (!q->jobs.empty()) return true;
        for (const game::QuestGoal& g : q->goals) {
            if (!g.jobs.empty()) return true;
        }
    }
    return false;
}

const QuestRecord* Quests::Record(std::uint32_t entity, std::uint32_t quest) const {
    const PlayerQuests* p = Of(entity);
    if (p == nullptr) return nullptr;
    const auto it = p->records.find(quest);
    return it != p->records.end() ? &it->second : nullptr;
}

void Quests::Send(std::uint32_t entity, proto::MessageId id, const std::vector<std::byte>& payload) {
    if (const auto who = host_.QuestPlayer(entity)) host_.QuestSend(who->peer, id, payload);
}
template <typename T>
void Quests::Send(std::uint32_t entity, const T& message) {
    Send(entity, T::kId, proto::Encode(message));
}

void Quests::Define(std::uint32_t entity, PlayerQuests& p, const QuestDef& q) {
    if (!p.told.insert(q.id).second) return;
    Send(entity, proto::QuestDefinition{game::WriteQuest(q)});
}

bool Quests::UnderWay(const QuestRecord& r) { return r.status == proto::QuestStatus::Achieving || r.status == proto::QuestStatus::Achieved; }

proto::QuestState Quests::StateOf(std::uint32_t entity, const QuestDef& q, const QuestRecord& r) {
    proto::QuestState s;
    s.quest = q.id;
    s.status = r.status;
    s.progress.assign(r.progress.begin(), r.progress.begin() + std::ptrdiff_t(std::min(q.goals.size(), r.progress.size())));
    if (q.timerSeconds != 0 && UnderWay(r)) {
        // The holder's clock: its end while in the world, else the seconds the record keeps.
        std::int64_t left = r.progress[kTimerSlot];
        if (const PlayerQuests* p = Of(entity); p != nullptr)
            if (const auto d = p->deadlines.find(q.id); d != p->deadlines.end())
                left = std::chrono::ceil<std::chrono::seconds>(d->second - Clock::now()).count();
        s.secondsLeft = std::uint32_t(std::max<std::int64_t>(1, left));
    }
    // A trial's or an instance's time on the client's gauge (the original's 0x30DA).
    if (s.secondsLeft == 0) s.secondsLeft = Gauge(entity, q.id);
    return s;
}

void Quests::Changed(std::uint32_t entity, PlayerQuests& p, const QuestDef& q, QuestRecord& r) {
    host_.QuestSave(p.character, r);
    Define(entity, p, q);
    Send(entity, proto::QuestChanged{StateOf(entity, q, r)});
    TellMarks(entity, p);
}

void Quests::TellMarks(std::uint32_t entity, PlayerQuests& p) {
    const auto who = host_.QuestPlayer(entity);
    if (!who) return;
    ResetDaily(p);
    std::map<std::string, proto::QuestMark> marks;
    // A hand-in outranks an offer, an offer a quest under way (ours: the order of 1.188's NPC table is not read).
    const auto rank = [](proto::QuestMark m) { return m == proto::QuestMark::End ? 3 : m == proto::QuestMark::Start ? 2 : 1; };
    const auto put = [&](const std::string& npc, proto::QuestMark m) {
        if (npc.empty()) return;
        const auto it = marks.find(npc);
        if (it == marks.end() || rank(m) > rank(it->second)) marks[npc] = m;
    };
    for (const QuestDef& q : quests_) {
        const auto it = p.records.find(q.id);
        if (it != p.records.end() && UnderWay(it->second)) {
            const QuestRecord& r = it->second;
            put(q.finisher, r.status == proto::QuestStatus::Achieved ? proto::QuestMark::End : proto::QuestMark::Going);
            for (std::size_t i = 0; i < q.goals.size(); ++i) {
                const QuestGoal& g = q.goals[i];
                const bool caught = g.kind == QuestGoalKind::Capture && p.catches.contains(q.id);
                if ((g.kind == QuestGoalKind::Deliver || g.kind == QuestGoalKind::Talk || caught) && r.progress[i] < 1) {
                    put(g.npc, proto::QuestMark::End);
                    for (const std::string& other : g.alsoAt) put(other, proto::QuestMark::End);
                }
            }
        } else if (OfferedTo(*who, p, q)) {
            for (const std::string& npc : q.givers) put(npc, proto::QuestMark::Start);
        }
    }
    if (marks == p.marks) return;
    p.marks = marks;
    proto::QuestMarks m;
    for (const auto& [npc, kind] : marks) m.marks.push_back({npc, kind});
    Send(entity, m);
}

void Quests::LevelChanged(std::uint32_t entity) {
    PlayerQuests* p = Of(entity);
    const auto who = host_.QuestPlayer(entity);
    if (p == nullptr || !who) return;
    // The quests given by level (the original's _RefCharDefault_Quest): each whose start level this rise reached, not
    // had before, its other conditions met.
    for (const QuestDef& q : quests_) {
        if (!q.given || q.minLevel <= p->level || q.minLevel > who->level || p->records.contains(q.id) || !OfferedTo(*who, *p, q)) continue;
        Start(entity, *p, q, who->name);
    }
    // The quest guide: at a rise, the quests of the new level, up to level
    // 40 of the player's race or both, none it has a record of; empty puts the icon away.
    if (who->level > p->level) {
        const auto text = [](const game::QuestText& t) { return !t.key.empty() ? t.key : t.literal; };
        proto::QuestGuide guide;
        for (const QuestDef& q : quests_) {
            if (q.level != who->level || p->records.contains(q.id) || q.disabled) continue;
            if (who->level <= 40 && !q.race.empty() && q.race != who->race) continue;
            guide.rows.push_back({q.id, text(q.name), text(q.guideNpc), text(q.guideCondition)});
        }
        Send(entity, guide);
    }
    p->level = who->level;
    TellMarks(entity, *p);
}

bool Quests::Evaluate(std::uint32_t entity, const QuestDef& q, QuestRecord& r) {
    if (!UnderWay(r)) return false;
    bool changed = false, done = true;
    for (std::size_t i = 0; i < q.goals.size() && i < r.progress.size(); ++i) {
        const QuestGoal& g = q.goals[i];
        if (g.kind == QuestGoalKind::Drop) {
            const std::uint16_t held = std::uint16_t(std::min<std::uint32_t>(host_.QuestItemCount(entity, g.item), Need(g)));
            if (held != r.progress[i]) {
                r.progress[i] = held;
                changed = true;
            }
        } else if (g.kind == QuestGoalKind::Record) {
            // A record's count (the beta quests): the first option's, capped; any option reached meets it.
            const auto who = host_.QuestPlayer(entity);
            const PlayerQuests* p = Of(entity);
            std::uint16_t now = 0;
            if (who && p != nullptr && !g.records.empty()) {
                now = std::uint16_t(std::min(RecordCount(*who, *p, g.records.front().first), int(Need(g))));
                for (const auto& [name, count] : g.records)
                    if (RecordCount(*who, *p, name) >= count) now = Need(g);
            }
            if (now != r.progress[i]) {
                r.progress[i] = now;
                changed = true;
            }
        } else if (g.kind == QuestGoalKind::Collect) {
            // The theme complete in the collection book.
            const std::uint16_t complete = host_.QuestThemeDone(entity, g.theme) ? 1 : 0;
            if (complete != r.progress[i]) {
                r.progress[i] = complete;
                changed = true;
            }
        }
        // A helper goal (hidden: the original's mission with its counting flag 0) never holds the quest back.
        done &= g.hidden || r.progress[i] >= Need(g);
    }
    // A tiered quest is done as soon as its smallest row is held (the original's mission goes on to its most: the
    // seeds keep dropping until 500).
    if (!q.tiers.empty()) {
        std::uint32_t least = q.tiers.front().count;
        for (const QuestDef::Tier& t : q.tiers) least = std::min(least, t.count);
        done = host_.QuestItemCount(entity, q.tiers.front().item) >= least;
    }
    const proto::QuestStatus status = done ? proto::QuestStatus::Achieved : proto::QuestStatus::Achieving;
    if (status != r.status) {
        r.status = status;
        changed = true;
    }
    return changed;
}

bool Quests::Started(const QuestDef& q) const {
    if (q.quota == 0) return true;
    const auto it = quotas_.find(q.id);
    if (it == quotas_.end()) return true;
    return it->second.night != Night(host_.QuestHour()) || it->second.left > 0;
}

void Quests::ResetDaily(PlayerQuests& p) {
    const std::int64_t now = UnixNow();
    std::optional<std::pair<int, int>> today;
    for (auto& [id, r] : p.records) {
        if (r.status != proto::QuestStatus::Done || r.ended == 0) continue;
        const QuestDef* q = Find(id);
        if (q == nullptr || !q->realClock || q->repeat == 0 || r.times < q->repeat) continue;
        if (!today) today = LocalMonthDay(now);
        if (LocalMonthDay(r.ended) == *today) continue;
        r.times = 0;
        host_.QuestSave(p.character, r);
        core::LogInfo("quests: {} offered again (a new day)", q->codename);
    }
}

bool Quests::Limited(const QuestHost::Player& who, const PlayerQuests& p, const QuestDef& q) const {
    return !OfferedTo(who, p, q) && OfferedTo(who, p, q, false);
}

bool Quests::OfferedTo(const QuestHost::Player& who, const PlayerQuests& p, const QuestDef& q, bool limits) const {
    if (q.givers.empty() && !q.given) return false; // not playable (yet)
    if (who.level < q.minLevel || (q.maxLevel != 0 && who.level > q.maxLevel)) return false;
    if (!q.race.empty() && q.race != who.race) return false;
    // (A script's LuaSetHwanLevel is never read by the original's offer test: no hwan level is asked.)
    // The job state of the suit worn, one of these (the start's flag 0x1000: the state equal).
    if (!q.jobs.empty() && std::find(q.jobs.begin(), q.jobs.end(), who.job) == q.jobs.end()) return false;
    const auto finished = [&](const std::string& code) {
        const QuestDef* d = Find(code);
        if (d == nullptr) return false;
        const auto it = p.records.find(d->id);
        return it != p.records.end() && it->second.times > 0;
    };
    for (const std::string& code : q.afterAll) {
        if (!finished(code)) return false;
        if (const auto t = q.afterTimes.find(code); t != q.afterTimes.end()) {
            const auto it = p.records.find(Find(code)->id);
            if (it->second.times < t->second) return false;
        }
    }
    // A counted deed (QuestDef::records; ours): one of each need's reached. `finished` and `finished:PREFIX` count the
    // different quests the character finished (the beta ones not), the others its deeds.
    if (!RecordsMet(who, p, q.records)) return false;
    // Not yet finished: none finished.
    if (std::any_of(q.before.begin(), q.before.end(), finished)) return false;
    // Never taken (state 5, none; one given up is 6 and refuses too): no record of it.
    for (const std::string& code : q.never) {
        const QuestDef* d = Find(code);
        if (d != nullptr && p.records.contains(d->id)) return false;
    }
    if (!q.afterOne.empty() && std::none_of(q.afterOne.begin(), q.afterOne.end(), finished)) return false;
    // Taken while others run (each one's state 1, 7 or 8).
    for (const std::string& code : q.during) {
        const QuestDef* d = Find(code);
        const auto it = d != nullptr ? p.records.find(d->id) : p.records.end();
        if (it == p.records.end() || !UnderWay(it->second)) return false;
    }
    if (q.party && !who.sharedParty) return false;
    // Held items (the start's flag 4): one of each in the bag.
    for (const std::string& item : q.startHolding) {
        if (host_.QuestItemCount(p.entity, item) == 0) return false;
    }
    if (!q.startHoldingOne.empty() &&
        std::none_of(q.startHoldingOne.begin(), q.startHoldingOne.end(), [&](const std::string& item) { return host_.QuestItemCount(p.entity, item) > 0; }))
        return false;
    if (!q.time.empty() && (q.time == "night") != Night(host_.QuestHour())) return false;
    // A premium quest: the running package names it and has uses left (QSP_ALL_POTION_3's start counts them,
    // the allowed less the used).
    if (q.premium) {
        const auto left = who.premiumQuests.find(q.codename);
        if (left == who.premiumQuests.end() || left->second <= 0) return false;
    }
    if (const auto it = p.records.find(q.id); it != p.records.end()) {
        if (UnderWay(it->second)) return false;
        // Given up, a quest that locks is not offered again (refuses status 6).
        if (it->second.status == proto::QuestStatus::Abandoned && q.giveUpLocks) return false;
        // A daily quest's count is left to its talk (`limits` false); the others refuse at their limit.
        if (q.repeat != 0 && it->second.times >= q.repeat && (limits || !q.realClock)) return false;
    }
    return !limits || Started(q);
}

bool Quests::Offered(std::uint32_t entity, const QuestDef& q) const {
    const PlayerQuests* p = Of(entity);
    const auto who = host_.QuestPlayer(entity);
    return p != nullptr && who && OfferedTo(*who, *p, q);
}

namespace {
// Whether the NPC takes the goal (a delivery's, a talk's: its NPC, or for a talk any of the others it names).
bool GoalAt(const QuestGoal& g, const std::string& npc) {
    if (g.kind != QuestGoalKind::Deliver && g.kind != QuestGoalKind::Talk && g.kind != QuestGoalKind::Capture) return false;
    return g.npc == npc || Contains(g.alsoAt, npc);
}
} // namespace

bool Quests::Finishes(const QuestDef& q, const std::string& npc) const { return game::QuestFinishesAt(q, npc); }

bool Quests::Involves(const QuestDef& q, const QuestRecord& r, const std::string& npc) const {
    if (!UnderWay(r)) return false;
    if (Finishes(q, npc) || Contains(q.givers, npc) || Contains(q.npcs, npc)) return true;
    for (const QuestGoal& g : q.goals)
        if (GoalAt(g, npc)) return true;
    return false;
}

std::optional<std::size_t> Quests::DueAt(std::uint32_t entity, const QuestDef& q, const QuestRecord& r, const std::string& npc) {
    if (!UnderWay(r)) return std::nullopt;
    for (std::size_t i = 0; i < q.goals.size(); ++i) {
        const QuestGoal& g = q.goals[i];
        if (!GoalAt(g, npc) || r.progress[i] >= 1) continue;
        // A capture's goal is due with its COS caught and standing beside the player (the original's NPC talk: "fail pay
        // quest. because don't have Catch Mob" without it, e.g.).
        if (g.kind == QuestGoalKind::Capture) {
            const PlayerQuests* p = Of(entity);
            const auto c = p != nullptr ? p->catches.find(q.id) : decltype(p->catches)::const_iterator{};
            if (p == nullptr || c == p->catches.end() || c->second.rule == nullptr || !c->second.rule->capture || g.monsters.empty() ||
                c->second.rule->capture->cos != g.monsters.front() || !host_.QuestCosStanding(c->second.cos)) {
                continue;
            }
        }
        return i;
    }
    return std::nullopt;
}

void Quests::Enter(std::uint32_t entity, std::vector<QuestRecord> records) {
    const auto who = host_.QuestPlayer(entity);
    if (!who) return;
    PlayerQuests& p = players_[entity];
    p = PlayerQuests{};
    p.entity = entity;
    p.character = who->character;
    p.level = who->level;
    for (QuestRecord& r : records) p.records[r.quest] = r;
    proto::QuestList list;
    const Clock::time_point now = Clock::now();
    for (auto& [id, r] : p.records) {
        const QuestDef* q = Find(id);
        if (q == nullptr) continue;
        if (r.times > 0) list.finished.push_back({id, r.times});
        if (!UnderWay(r)) continue;
        // A time limit goes on from the seconds the record kept (none kept: out of time at the first tick).
        if (q->timerSeconds != 0) p.deadlines[id] = now + std::chrono::seconds(r.progress[kTimerSlot]);
        if (Evaluate(entity, *q, r)) host_.QuestSave(p.character, r);
        Define(entity, p, *q);
        list.active.push_back(StateOf(entity, *q, r));
    }
    Send(entity, list);
    ResumeCatches(entity, p);
    TellMarks(entity, p);
}

void Quests::Leave(std::uint32_t entity) {
    // A caught COS goes with its owner; its record keeps its minutes, and the world entry on this or the other game server
    // brings it back (ResumeCatches).
    if (PlayerQuests* p = Of(entity)) {
        for (const auto& [quest, c] : p->catches) host_.QuestCosRemove(c.cos);
        // A time limit stops with its holder out of the world: its seconds left kept for the next entry.
        const Clock::time_point now = Clock::now();
        for (const auto& [quest, until] : p->deadlines) {
            KeepTimer(entity, *p, quest, now, false);
            if (const auto r = p->records.find(quest); r != p->records.end() && UnderWay(r->second)) {
                r->second.progress[kTimerSlot] = std::max<std::uint16_t>(r->second.progress[kTimerSlot], 1);
                host_.QuestSave(p->character, r->second);
            }
        }
    }
    players_.erase(entity);
}

bool Quests::Handle(std::uint32_t entity, proto::MessageId id, std::span<const std::byte> payload) {
    switch (id) {
    case proto::QuestTalk::kId: {
        proto::QuestTalk t;
        if (!proto::Decode(payload, t)) return false;
        Talk(entity, t);
        return true;
    }
    case proto::QuestAccept::kId: {
        proto::QuestAccept a;
        if (!proto::Decode(payload, a)) return false;
        Accept(entity, a);
        return true;
    }
    case proto::QuestHandIn::kId: {
        proto::QuestHandIn h;
        if (!proto::Decode(payload, h)) return false;
        HandIn(entity, h);
        return true;
    }
    case proto::QuestAbandon::kId: {
        proto::QuestAbandon a;
        if (!proto::Decode(payload, a)) return false;
        Abandon(entity, a.quest, proto::QuestEnd::Abandoned);
        return true;
    }
    case proto::QuestEnterReply::kId: {
        proto::QuestEnterReply r;
        if (!proto::Decode(payload, r)) return false;
        EnterReply(entity, r);
        return true;
    }
    default: return false;
    }
}

void Quests::TellNpc(std::uint32_t entity, std::uint32_t npc) {
    PlayerQuests* p = Of(entity);
    const auto who = host_.QuestPlayer(entity);
    const std::string code = host_.QuestNpcNear(entity, npc);
    if (p == nullptr || !who || code.empty()) return;
    ResetDaily(*p);
    proto::NpcQuests rows;
    rows.npc = npc;
    // A party quest is listed without the party too: its row says the `not-party` talk (Talk). A daily quest at its count
    // and a quest whose quota is spent are listed: their talk says `count-limit`.
    QuestHost::Player inParty = *who;
    inParty.sharedParty = true;
    for (const QuestDef& q : quests_) {
        const auto it = p->records.find(q.id);
        if (it != p->records.end() && Involves(q, it->second, code)) {
            Evaluate(entity, q, it->second);
            const bool handIn = (Finishes(q, code) && it->second.status == proto::QuestStatus::Achieved) || DueAt(entity, q, it->second, code);
            rows.rows.push_back({q.id, handIn ? proto::NpcQuestKind::HandIn : proto::NpcQuestKind::UnderWay});
        } else if (Contains(q.givers, code) && OfferedTo(q.party ? inParty : *who, *p, q, false)) {
            rows.rows.push_back({q.id, proto::NpcQuestKind::Offer});
        } else if (q.resupplyAfterFinish && Contains(q.givers, code) && it != p->records.end() && it->second.times > 0 && !UnderWay(it->second)) {
            rows.rows.push_back({q.id, proto::NpcQuestKind::UnderWay}); // its daily supply (QuestDef::resupplyAfterFinish)
        } else {
            continue;
        }
        Define(entity, *p, q);
    }
    Send(entity, rows);
}

void Quests::Say(std::uint32_t entity, std::uint32_t npc, std::uint32_t quest, std::string moment, proto::QuestChoice choice, std::uint8_t goal) {
    Send(entity, proto::QuestSays{npc, quest, std::move(moment), goal, choice});
}

void Quests::Answer(std::uint32_t entity, std::uint32_t quest, proto::QuestCode code) { Send(entity, proto::QuestAnswer{quest, code}); }

void Quests::Talk(std::uint32_t entity, const proto::QuestTalk& t) {
    PlayerQuests* p = Of(entity);
    const auto who = host_.QuestPlayer(entity);
    const QuestDef* q = Find(t.quest);
    if (p == nullptr || !who) return;
    const std::string npc = host_.QuestNpcNear(entity, t.npc);
    if (npc.empty()) return Answer(entity, t.quest, proto::QuestCode::TooFar);
    if (q == nullptr) return Answer(entity, t.quest, proto::QuestCode::NotOffered);
    ResetDaily(*p);
    const auto it = p->records.find(q->id);
    if (it != p->records.end() && Involves(*q, it->second, npc)) {
        QuestRecord& r = it->second;
        if (Evaluate(entity, *q, r)) Changed(entity, *p, *q, r);
        if (const auto due = DueAt(entity, *q, r, npc)) {
            const QuestGoal& g = q->goals[*due];
            const bool has = g.kind != QuestGoalKind::Deliver ||
                             std::all_of(g.items.begin(), g.items.end(), [&](const QuestGoal::Items& i) { return host_.QuestItemCount(entity, i.item) >= i.count; });
            if (has) return Say(entity, t.npc, q->id, {}, proto::QuestChoice::HandIn, std::uint8_t(*due));
            return Say(entity, t.npc, q->id, "not-done", proto::QuestChoice::None);
        }
        if (Finishes(*q, npc) && r.status == proto::QuestStatus::Achieved) {
            if (!NeedsHeld(entity, *who, *q)) return Say(entity, t.npc, q->id, "missing", proto::QuestChoice::None);
            // No room for the reward once the pay's items are taken: the bag-full words in place of `done` (the original's
            // dry run of the pay at the talk).
            if (!RewardFits(entity, *q)) return Say(entity, t.npc, q->id, "bag-full", proto::QuestChoice::None);
            return Say(entity, t.npc, q->id, "done", proto::QuestChoice::HandIn);
        }
        // Its tools given again at a giver when none is left (QuestDef::resupply).
        if (!q->resupply.empty() && Contains(q->givers, npc) &&
            std::all_of(q->resupply.begin(), q->resupply.end(), [&](const QuestGoal::Items& i) { return host_.QuestItemCount(entity, i.item) == 0; })) {
            // Once a game day (QuestDef::resupplyDaily): the day's given already, "come again tomorrow" (EASTEU_5's _15);
            // else the day recorded as they are offered, before the grant (a full bag spends the day too).
            if (q->resupplyDaily) {
                const std::uint16_t day = host_.QuestDay();
                if (r.progress[kSupplyDaySlot] == day) return Say(entity, t.npc, q->id, "resupply-spent", proto::QuestChoice::None);
                r.progress[kSupplyDaySlot] = day;
                host_.QuestSave(p->character, r);
            }
            if (!host_.QuestGive(entity, q->resupply)) return Say(entity, t.npc, q->id, "bag-full", proto::QuestChoice::None);
            BagChanged(entity);
            return Say(entity, t.npc, q->id, "resupply", proto::QuestChoice::None);
        }
        // A talk rule's NPC (the secret door): its question with its row, or the checks' words.
        if (const QuestDef::UseRule* door = TalkRule(*q, r, npc)) return AskTalkRule(entity, *who, *q, *door, t.npc);
        // A trial's NPC with its goals not met: its words and its row (_04 and the
        // row _05 "Enter the Test Arena"); ours the `trial` talk with the hand-in's answer, which sends the player in.
        if (q->trial && q->trial->npc == npc && r.status == proto::QuestStatus::Achieving) return Say(entity, t.npc, q->id, "trial", proto::QuestChoice::HandIn);
        const bool some = OriginalState(r, q) != 1; // a goal progressed (7) or met (8)
        return Say(entity, t.npc, q->id, some && !Finishes(*q, npc) ? "middle" : "not-done", proto::QuestChoice::None);
    }
    if (!q->talkGame.empty() && Contains(q->givers, npc) && (OfferedTo(*who, *p, *q) || (it != p->records.end() && it->second.times > 0)))
        return PlayTalkGame(entity, *p, *q, t.npc, std::nullopt);
    // Finished, its supply once a game day at its givers (QuestDef::resupplyAfterFinish).
    if (q->resupplyAfterFinish && Contains(q->givers, npc) && it != p->records.end() && it->second.times > 0 && !UnderWay(it->second) &&
        !OfferedTo(*who, *p, *q)) {
        QuestRecord& r = it->second;
        const std::uint16_t day = host_.QuestDay();
        if (r.progress[kSupplyDaySlot] == day) return Say(entity, t.npc, q->id, "resupply-spent", proto::QuestChoice::None);
        if (!host_.QuestGive(entity, q->resupply)) return Say(entity, t.npc, q->id, "bag-full", proto::QuestChoice::None);
        r.progress[kSupplyDaySlot] = day;
        host_.QuestSave(p->character, r);
        BagChanged(entity);
        return Say(entity, t.npc, q->id, "resupply", proto::QuestChoice::None);
    }
    if (Contains(q->givers, npc) && OfferedTo(*who, *p, *q)) {
        const bool again = it != p->records.end() && it->second.times > 0;
        Define(entity, *p, *q);
        return Say(entity, t.npc, q->id, again ? "offer-again" : "offer", proto::QuestChoice::Accept);
    }
    // A party quest whose only want is the party: its giver says so (the original's BASIC_MENUSTRING_NOT_PARTY).
    if (q->party && !who->sharedParty && Contains(q->givers, npc)) {
        QuestHost::Player inParty = *who;
        inParty.sharedParty = true;
        if (OfferedTo(inParty, *p, *q, false)) {
            Define(entity, *p, *q);
            return Say(entity, t.npc, q->id, "not-party", proto::QuestChoice::None);
        }
    }
    // A daily quest at its count, or the window's quota spent: the count-limit words (the original's
    // BASIC_MENUSTRING_COUNT_LIMIT).
    if (Contains(q->givers, npc) && Limited(*who, *p, *q)) {
        Define(entity, *p, *q);
        return Say(entity, t.npc, q->id, "count-limit", proto::QuestChoice::None);
    }
    if (it != p->records.end() && q->repeat != 0 && it->second.times >= q->repeat) return Say(entity, t.npc, q->id, "count-limit", proto::QuestChoice::None);
    Answer(entity, t.quest, proto::QuestCode::NotOffered);
}

void Quests::Start(std::uint32_t entity, PlayerQuests& p, const QuestDef& q, const std::string& who) {
    QuestRecord& r = p.records[q.id];
    r.quest = q.id;
    r.status = proto::QuestStatus::Achieving;
    r.progress = {};
    r.started = UnixNow();
    r.ended = 0;
    Evaluate(entity, q, r);
    // A time limit: its seconds kept in the record, run on the holder's clock (Tick).
    if (q.timerSeconds != 0) {
        r.progress[kTimerSlot] = std::uint16_t(std::min<std::uint32_t>(q.timerSeconds, 0xFFFF));
        p.deadlines[q.id] = Clock::now() + std::chrono::seconds(r.progress[kTimerSlot]);
    }
    if (q.quota != 0) {
        Quota& quota = quotas_[q.id];
        const bool night = Night(host_.QuestHour());
        if (quota.night != night || quota.left == 0) quota = {q.quota, night};
        if (quota.left > 0) --quota.left;
    }
    core::LogInfo("quests: {} {} {} ({})", who, q.given ? "is given" : "takes", q.codename, q.id);
    Define(entity, p, q);
    Changed(entity, p, q, r);
}

void Quests::Accept(std::uint32_t entity, const proto::QuestAccept& a) {
    PlayerQuests* p = Of(entity);
    const auto who = host_.QuestPlayer(entity);
    const QuestDef* q = Find(a.quest);
    if (p == nullptr || !who) return;
    if (!who->free) return Answer(entity, a.quest, proto::QuestCode::Busy);
    const std::string npc = host_.QuestNpcNear(entity, a.npc);
    if (npc.empty()) return Answer(entity, a.quest, proto::QuestCode::TooFar);
    ResetDaily(*p);
    if (q == nullptr || !Contains(q->givers, npc) || !OfferedTo(*who, *p, *q)) return Answer(entity, a.quest, proto::QuestCode::NotOffered);
    const std::size_t underWay = std::count_if(p->records.begin(), p->records.end(), [](const auto& kv) {
        return kv.first < proto::kCountedQuestIds && UnderWay(kv.second);
    });
    if (underWay >= proto::kQuestsAtOnce) return Answer(entity, a.quest, proto::QuestCode::TooMany);
    // A quest without goals is a story told at its giver (the original's QNO_WC_FERRY4_1 conversation): no room for
    // its reward refuses it at once; else it starts, its pages are told and it is paid in the same talk.
    if (q->goals.empty() && q->reward.choose == 0) {
        std::vector<QuestGoal::Items> items;
        for (const auto& i : q->reward.items) items.push_back({i.item, i.count});
        if (!items.empty() && !host_.QuestRoom(entity, items)) {
            Answer(entity, a.quest, proto::QuestCode::BagFull);
            return Say(entity, a.npc, q->id, "bag-full", proto::QuestChoice::None);
        }
    }
    if (!q->startItems.empty() && !host_.QuestGive(entity, q->startItems)) {
        Answer(entity, a.quest, proto::QuestCode::BagFull);
        return Say(entity, a.npc, q->id, "bag-full", proto::QuestChoice::None);
    }
    Start(entity, *p, *q, who->name);
    // Its start's tools count as the day's supply.
    if (q->resupplyDaily && q->resupplyFromStart) {
        QuestRecord& started = p->records[q->id];
        started.progress[kSupplyDaySlot] = host_.QuestDay();
        host_.QuestSave(p->character, started);
    }
    Answer(entity, q->id, proto::QuestCode::Ok);
    // Taken again: its `accepted-again` words where it has them (the coded quests' talk handlers, QNO_WC_WAREHOUSE_W_2's _08).
    const bool again = p->records[q->id].times > 0 && std::any_of(q->talks.begin(), q->talks.end(), [&](const game::QuestTalk& t) {
        return t.npc == npc && t.moment == "accepted-again";
    });
    Say(entity, a.npc, q->id, again ? "accepted-again" : "accepted", proto::QuestChoice::None);
    if (q->goals.empty()) Pay(entity, *p, *q, p->records[q->id], a.npc, {});
}

void Quests::HandIn(std::uint32_t entity, const proto::QuestHandIn& h) {
    PlayerQuests* p = Of(entity);
    const auto who = host_.QuestPlayer(entity);
    const QuestDef* q = Find(h.quest);
    if (p == nullptr || !who) return;
    if (!who->free) return Answer(entity, h.quest, proto::QuestCode::Busy);
    const std::string npc = host_.QuestNpcNear(entity, h.npc);
    if (npc.empty()) return Answer(entity, h.quest, proto::QuestCode::TooFar);
    if (q != nullptr && !q->talkGame.empty() && Contains(q->givers, npc)) {
        if (h.choices.size() != 1) return Answer(entity, h.quest, proto::QuestCode::BadChoice);
        return PlayTalkGame(entity, *p, *q, h.npc, h.choices[0]);
    }
    const auto it = q != nullptr ? p->records.find(q->id) : p->records.end();
    if (it == p->records.end() || !UnderWay(it->second)) return Answer(entity, h.quest, proto::QuestCode::NotUnderWay);
    QuestRecord& r = it->second;
    // A talk rule's rows (the question's row, then SN_TALK_COMMON_DENY): the first picked opens its instance.
    if (const QuestDef::UseRule* door = TalkRule(*q, r, npc); door != nullptr && h.choices.size() == 1) {
        if (h.choices[0] != 0) return Answer(entity, q->id, proto::QuestCode::Ok); // denied: the talk ends
        Answer(entity, q->id, PickTalkRule(entity, *who, *q, *door, h.npc, Clock::now()) ? proto::QuestCode::Ok : proto::QuestCode::Busy);
        return;
    }
    bool changed = false;
    if (const auto due = DueAt(entity, *q, r, npc)) {
        const QuestGoal& g = q->goals[*due];
        // What the NPC hands back (QuestGoal::received): no room for it refuses the delivery with its `bag-full`.
        if (!g.received.empty() && !host_.QuestRoom(entity, g.received)) {
            Answer(entity, q->id, proto::QuestCode::BagFull);
            return Say(entity, h.npc, q->id, "bag-full", proto::QuestChoice::None);
        }
        if (g.kind == QuestGoalKind::Deliver && !host_.QuestTake(entity, g.items)) return Answer(entity, q->id, proto::QuestCode::NoItems);
        if (g.kind == QuestGoalKind::Capture) DropCatch(entity, *p, q->id, nullptr); // the caught COS handed over
        if (!g.received.empty()) host_.QuestGive(entity, g.received);
        r.progress[*due] = 1;
        changed = true;
    }
    changed |= Evaluate(entity, *q, r);
    if (changed) Changed(entity, *p, *q, r); // kept before the reward (the items handed over stay handed over)
    if (!q->tiers.empty() && Finishes(*q, npc) && r.status == proto::QuestStatus::Achieved) return PayTier(entity, *p, *q, r, h.npc, h.choices);
    // Its goals met by this hand-in, a reward to choose and none chosen yet: the finisher's `done` and the reward's choice
    // (ours; the beta quests' weapons), not a refusal.
    if (Finishes(*q, npc) && r.status == proto::QuestStatus::Achieved && q->reward.choose != 0 && h.choices.empty()) {
        Answer(entity, q->id, proto::QuestCode::Ok);
        return Say(entity, h.npc, q->id, "done", proto::QuestChoice::HandIn);
    }
    if (Finishes(*q, npc) && r.status == proto::QuestStatus::Achieved) return Pay(entity, *p, *q, r, h.npc, h.choices);
    // A trial's row picked at its NPC (the talk's answer 5: the recall point kept, then the entry).
    if (!changed && q->trial && q->trial->npc == npc && r.status == proto::QuestStatus::Achieving) {
        Answer(entity, q->id, EnterTrial(entity, *who, *q, Clock::now()) ? proto::QuestCode::Ok : proto::QuestCode::Busy);
        return;
    }
    if (!changed) return Answer(entity, q->id, proto::QuestCode::NotDone);
    Answer(entity, q->id, proto::QuestCode::Ok);
    Say(entity, h.npc, q->id, "middle", proto::QuestChoice::None);
}

int Quests::RecordCount(const QuestHost::Player& who, const PlayerQuests& p, const std::string& name) const {
    if (!name.starts_with("finished")) {
        const auto it = who.deeds.find(name);
        return it != who.deeds.end() ? it->second : 0;
    }
    const std::string prefix = name.size() > 9 && name[8] == ':' ? name.substr(9) : std::string();
    int n = 0;
    for (const auto& [id, r] : p.records) {
        const QuestDef* d = r.times > 0 ? Find(id) : nullptr;
        if (d != nullptr && !d->codename.starts_with("QBETA_") && d->codename.starts_with(prefix)) ++n;
    }
    return n;
}

bool Quests::RecordsMet(const QuestHost::Player& who, const PlayerQuests& p, const std::vector<QuestDef::RecordNeed>& needs) const {
    for (const QuestDef::RecordNeed& need : needs)
        if (std::none_of(need.any.begin(), need.any.end(), [&](const auto& a) { return RecordCount(who, p, a.first) >= a.second; })) return false;
    return true;
}

bool Quests::NeedsHeld(std::uint32_t entity, const QuestHost::Player& who, const QuestDef& q) {
    if (who.gold < q.needGold) return false;
    if (!q.needRecords.empty()) {
        const PlayerQuests* p = Of(entity);
        if (p == nullptr || !RecordsMet(who, *p, q.needRecords)) return false;
    }
    return std::all_of(q.needItems.begin(), q.needItems.end(), [&](const QuestGoal::Items& i) { return host_.QuestItemCount(entity, i.item) >= i.count; });
}

std::vector<QuestGoal::Items> Quests::PayItems(std::uint32_t entity, const QuestDef& q) const {
    std::vector<QuestGoal::Items> out;
    // Every held piece of an item (at least `least`), once.
    const auto all = [&](const std::string& item, std::uint32_t least) {
        const std::uint32_t held = std::max(least, host_.QuestItemCount(entity, item));
        const auto same = std::find_if(out.begin(), out.end(), [&](const QuestGoal::Items& i) { return i.item == item; });
        if (same != out.end()) same->count = std::max(same->count, held);
        else if (held > 0) out.push_back({item, held});
    };
    // A counting gathering's item, every piece held (the original's removal loop: counted, a stack removed, again while any
    // is left); a helper's (hidden) only when it pays by the piece.
    for (const QuestGoal& g : q.goals)
        if (g.kind == QuestGoalKind::Drop && (!g.hidden || g.apiece)) all(g.item, g.count);
    // What a delivery's NPC handed back (QuestGoal::received), every piece held.
    for (const QuestGoal& g : q.goals)
        if (g.kind == QuestGoalKind::Deliver)
            for (const QuestGoal::Items& i : g.received) all(i.item, 0);
    // Every one of its leftover tools (QuestDef::payTake; a capture's traps and caught item).
    for (const std::string& item : q.payTake) all(item, 0);
    // The items needed besides the goals, their counts.
    out.insert(out.end(), q.needItems.begin(), q.needItems.end());
    return out;
}

bool Quests::RewardFits(std::uint32_t entity, const QuestDef& q) const {
    if (q.reward.choose != 0 || q.reward.items.empty()) return true; // a choice's room is asked at its pay
    std::vector<QuestGoal::Items> given;
    for (const auto& i : q.reward.items) given.push_back({i.item, i.count});
    return host_.QuestRoomAfter(entity, given, PayItems(entity, q));
}

void Quests::Pay(std::uint32_t entity, PlayerQuests& p, const QuestDef& q, QuestRecord& r, std::uint32_t npc, std::span<const std::uint8_t> choices) {
    // A reward by country (_RefQuestReward.IsCheckCountry, 17 rows): only the player's country's items, a China item's
    // codename ITEM_CH_, a Europe one's ITEM_EU_, the others for both (ours: where the original filters them is not
    // traced; BAEKRYOUNG_17 lists both countries' rings).
    const auto who = host_.QuestPlayer(entity);
    const auto forCountry = [&](const std::string& item) {
        if (!q.reward.byCountry || !who) return true;
        const bool europe = who->race == "europe";
        if (item.starts_with("ITEM_CH_")) return !europe;
        if (item.starts_with("ITEM_EU_")) return europe;
        return true;
    };
    // The reward's items: all of them (by country), or the ones chosen (none of the other country's).
    std::vector<QuestGoal::Items> given;
    if (q.reward.choose != 0) {
        std::set<std::uint8_t> picked(choices.begin(), choices.end());
        if (picked.size() != q.reward.choose || picked.size() != choices.size() || *picked.rbegin() >= q.reward.items.size())
            return Answer(entity, q.id, proto::QuestCode::BadChoice);
        for (std::uint8_t i : picked) {
            if (!forCountry(q.reward.items[i].item)) return Answer(entity, q.id, proto::QuestCode::BadChoice);
            given.push_back({q.reward.items[i].item, q.reward.items[i].count});
        }
    } else {
        for (const auto& i : q.reward.items) {
            if (forCountry(i.item)) given.push_back({i.item, i.count});
        }
    }
    // What is needed besides the goals (items, gold): short of it, the finisher says so.
    if (const auto who = host_.QuestPlayer(entity); !who || !NeedsHeld(entity, *who, q)) {
        Answer(entity, q.id, proto::QuestCode::NoItems);
        return Say(entity, npc, q.id, "missing", proto::QuestChoice::None);
    }
    // The gathered items (and the needed ones) go to the NPC, then the reward into the bag (the gathered ones back if it
    // has no room), then the gold needed.
    const std::vector<QuestGoal::Items> gathered = PayItems(entity, q);
    // A goal paid by the piece gives up every piece held, each paid (QuestGoal::apiece).
    std::int64_t pieceGold = 0, pieceExp = 0, pieceSp = 0;
    for (const QuestGoal& g : q.goals) {
        if (g.kind != QuestGoalKind::Drop || !g.apiece) continue;
        const std::uint32_t held = std::max(g.count, host_.QuestItemCount(entity, g.item));
        pieceGold += std::min<std::int64_t>(g.apieceGold * held, 10'000'000); // the original's cap, before the rate
        pieceExp += g.apieceExp * held;
        pieceSp += g.apieceSp * held;
    }
    if (!gathered.empty() && !host_.QuestTake(entity, gathered)) {
        Evaluate(entity, q, r);
        Changed(entity, p, q, r);
        return Answer(entity, q.id, proto::QuestCode::NotDone);
    }
    if (!given.empty() && !(q.reward.bound ? host_.QuestGiveBound(entity, given) : host_.QuestGive(entity, given))) {
        if (!gathered.empty()) host_.QuestGive(entity, gathered);
        Answer(entity, q.id, proto::QuestCode::BagFull);
        return Say(entity, npc, q.id, "bag-full", proto::QuestChoice::None);
    }
    if (q.needGold != 0 && !host_.QuestTakeGold(entity, q.needGold)) {
        if (!given.empty()) host_.QuestTake(entity, given);
        if (!gathered.empty()) host_.QuestGive(entity, gathered);
        Answer(entity, q.id, proto::QuestCode::NoItems);
        return Say(entity, npc, q.id, "missing", proto::QuestChoice::None);
    }
    QuestHost::Reward reward;
    reward.exp = std::uint64_t(std::llround(double(q.reward.exp + pieceExp) * rates_.experience));
    reward.skillExp = std::uint64_t(std::llround(double(q.reward.skillExp + pieceSp) * rates_.skill));
    reward.sp = std::uint32_t(std::llround(double(q.reward.sp) * rates_.skill));
    // The gold times rate-gold, at most 10,000,000 times it (the original's cap of a reward).
    const double goldCap = 10'000'000.0 * std::max(rates_.gold, 0.0);
    reward.gold = std::uint64_t(std::min(goldCap, std::round(double(std::min<std::uint64_t>(q.reward.gold, 10'000'000) + std::uint64_t(pieceGold)) * rates_.gold)));
    reward.inventory = q.reward.inventory;
    reward.hwan = q.reward.hwan;
    reward.points = q.reward.points;
    Finished(entity, p, q, r, npc, reward);
}

void Quests::PayTier(std::uint32_t entity, PlayerQuests& p, const QuestDef& q, QuestRecord& r, std::uint32_t npc, std::span<const std::uint8_t> choices) {
    // The row picked (the original's five talk rows), enough of its item held, those taken, its skill points paid
    // (ExchangeReward); short of them, the NPC's `missing` talk (its fifth line: "not enough for the reward you chose").
    if (choices.size() != 1 || choices[0] >= q.tiers.size()) return Answer(entity, q.id, proto::QuestCode::BadChoice);
    const QuestDef::Tier& tier = q.tiers[choices[0]];
    if (host_.QuestItemCount(entity, tier.item) < tier.count || !host_.QuestTake(entity, {{tier.item, tier.count}})) {
        Answer(entity, q.id, proto::QuestCode::NotDone);
        return Say(entity, npc, q.id, "missing", proto::QuestChoice::None);
    }
    QuestHost::Reward reward;
    reward.sp = std::uint32_t(std::llround(double(tier.sp) * rates_.skill));
    Finished(entity, p, q, r, npc, reward);
}

void Quests::Finished(std::uint32_t entity, PlayerQuests& p, const QuestDef& q, QuestRecord& r, std::uint32_t npc, const QuestHost::Reward& reward) {
    host_.QuestReward(entity, reward);
    if (q.premium) host_.QuestPremiumSpent(entity, q.codename); // one of the package's uses
    r.status = proto::QuestStatus::Done;
    r.times = std::uint16_t(std::min(0xFFFF, int(r.times) + 1));
    r.progress = {};
    r.ended = UnixNow();
    p.deadlines.erase(q.id);
    // A catch still held ends with it (the pay's state 4 clears the record's words: its minutes no longer run).
    if (const auto c = p.catches.find(q.id); c != p.catches.end()) {
        host_.QuestCosRemove(c->second.cos);
        p.catches.erase(c);
    }
    host_.QuestSave(p.character, r);
    // Others cleared as never done (QuestDef::finishResets: the original resets the record, status 0 and count 0, and saves
    // it; ours keeps it given up with no count, so it is offered again by its own rules and holds back no other quest).
    for (const std::string& code : q.finishResets) {
        const QuestDef* other = Find(code);
        const auto it = other != nullptr ? p.records.find(other->id) : p.records.end();
        if (it == p.records.end() || UnderWay(it->second)) continue;
        it->second.status = proto::QuestStatus::Abandoned;
        it->second.times = 0;
        it->second.progress = {};
        host_.QuestSave(p.character, it->second);
    }
    // Quests counting finished ones (a `record` goal: the beta quests) told their new progress at once.
    for (auto& [id, other] : p.records) {
        const QuestDef* d = id != q.id && UnderWay(other) ? Find(id) : nullptr;
        if (d == nullptr || std::none_of(d->goals.begin(), d->goals.end(), [](const QuestGoal& g) { return g.kind == QuestGoalKind::Record; })) continue;
        if (Evaluate(entity, *d, other)) Changed(entity, p, *d, other);
    }
    TellMarks(entity, p);
    if (const auto who = host_.QuestPlayer(entity)) core::LogInfo("quests: {} finishes {} ({} times)", who->name, q.codename, r.times);
    Send(entity, proto::QuestGone{q.id, proto::QuestEnd::Finished, r.times});
    proto::QuestRewarded told{q.id, reward.exp, reward.skillExp, reward.gold, reward.sp, reward.inventory, {}};
    Send(entity, told);
    Answer(entity, q.id, proto::QuestCode::Ok);
    // Ours: the window's last words "Quest completed." (the finisher said its `done` before the reward; `done-now` is the
    // goals' moment, "...completed. Go and report to ...", told on the quest notice when they are met).
    Say(entity, npc, q.id, "finished", proto::QuestChoice::None);
}

int Quests::DevFinish(std::uint32_t entity, std::string_view codename) {
    PlayerQuests* p = Of(entity);
    const QuestDef* first = Find(codename);
    if (p == nullptr || first == nullptr) return 0;
    int marked = 0;
    std::set<std::uint32_t> seen;
    const auto mark = [&](auto& self, const QuestDef& q) -> void {
        if (!seen.insert(q.id).second) return;
        for (const std::string& before : q.afterAll) {
            if (const QuestDef* b = Find(before)) self(self, *b);
        }
        QuestRecord& r = p->records[q.id];
        const bool underWay = r.quest != 0 && UnderWay(r);
        if (r.quest != 0 && r.status == proto::QuestStatus::Done && &q != first) return; // done already
        if (const auto c = p->catches.find(q.id); c != p->catches.end()) {
            host_.QuestCosRemove(c->second.cos);
            p->catches.erase(c);
        }
        r.quest = q.id;
        r.status = proto::QuestStatus::Done;
        r.times = std::uint16_t(std::min(0xFFFF, int(r.times) + 1));
        r.progress = {};
        if (r.started == 0) r.started = UnixNow();
        r.ended = UnixNow();
        host_.QuestSave(p->character, r);
        if (underWay) Send(entity, proto::QuestGone{q.id, proto::QuestEnd::Finished, r.times});
        ++marked;
    };
    mark(mark, *first);
    TellMarks(entity, *p);
    if (const auto who = host_.QuestPlayer(entity)) core::LogInfo("quests: {} has {} marked finished, {} in all (developer)", who->name, first->codename, marked);
    return marked;
}

void Quests::Abandon(std::uint32_t entity, std::uint32_t quest, proto::QuestEnd why) {
    PlayerQuests* p = Of(entity);
    const QuestDef* q = Find(quest);
    if (p == nullptr || q == nullptr) return;
    const auto it = p->records.find(quest);
    if (it == p->records.end() || !UnderWay(it->second)) return Answer(entity, quest, proto::QuestCode::NotUnderWay);
    // Its quest items back: what it gave on starting and what it gathered.
    std::vector<QuestGoal::Items> back;
    const auto add = [&](const std::string& item) {
        if (std::any_of(back.begin(), back.end(), [&](const QuestGoal::Items& i) { return i.item == item; })) return;
        if (const std::uint32_t n = host_.QuestItemCount(entity, item); n > 0) back.push_back({item, n});
    };
    if (!q->giveUpKeepsItems) {
        for (const auto& i : q->startItems) add(i.item);
        for (const QuestGoal& g : q->goals) {
            if (g.kind == QuestGoalKind::Drop) add(g.item);
            // A delivery's handed-back items too (the original's abort takes every stack of them).
            if (g.kind == QuestGoalKind::Deliver)
                for (const QuestGoal::Items& i : g.received) add(i.item);
        }
    }
    // A caught COS goes with it.
    if (p->catches.contains(quest)) DropCatch(entity, *p, quest, nullptr);
    if (!back.empty()) host_.QuestTake(entity, back);
    QuestRecord& r = it->second;
    r.status = proto::QuestStatus::Abandoned;
    r.progress = {};
    r.ended = UnixNow();
    p->deadlines.erase(quest);
    host_.QuestSave(p->character, r);
    // Its start given back to the window's quota (the original's give-up: one more, up to the quota), within the same window.
    if (q->quota != 0) {
        if (const auto quota = quotas_.find(q->id); quota != quotas_.end() && quota->second.night == Night(host_.QuestHour()) && quota->second.left < q->quota) {
            ++quota->second.left;
        }
    }
    // Out of time: the quest's own words for it (BASIC_MENUSTRING_TIME_LIMIT_AUTO_ABORT), where it has them.
    if (why == proto::QuestEnd::TimeOut) {
        const auto said = std::find_if(q->talks.begin(), q->talks.end(), [](const game::QuestTalk& t) { return t.moment == "time-out"; });
        if (said != q->talks.end()) host_.QuestSay(entity, !said->text.key.empty() ? said->text.key : said->text.literal);
    }
    TellMarks(entity, *p);
    Send(entity, proto::QuestGone{quest, why, r.times});
    if (why == proto::QuestEnd::Abandoned) Answer(entity, quest, proto::QuestCode::Ok);
}

void Quests::ShareKill(std::uint32_t killer, const Credit& credit) {
    const QuestHost::KillParty party = host_.QuestKillParty(killer);
    if (party.id == 0) {
        // Alone, or in a party not sharing both experience and items: the killer's own quests; a party quest counts only in
        // such a party (QuestDef::party, the original's DivideType 1 test on every kill).
        PlayerQuests* p = Of(killer);
        if (p == nullptr) return;
        for (auto& [id, r] : p->records) {
            const QuestDef* q = Find(id);
            if (q != nullptr && !q->party && UnderWay(r)) credit(killer, *p, *q, r);
        }
        return;
    }
    // The members near the killer: in its world, within 1000 of it (3D; the original's vt+0x51C).
    std::optional<proto::WorldPosition> at;
    for (const auto& [entity, place] : party.members)
        if (entity == killer) at = place;
    std::vector<bool> near(party.members.size(), false);
    for (std::size_t i = 0; i < party.members.size(); ++i) {
        const auto& [entity, place] = party.members[i];
        near[i] = entity == killer || (at && place && Near(*place, *at, 1000.0));
    }
    // The record of a near member's quest under way (none: not near, not held).
    const auto held = [&](std::size_t i, std::uint32_t quest) -> std::pair<PlayerQuests*, QuestRecord*> {
        if (!near[i]) return {nullptr, nullptr};
        PlayerQuests* p = Of(party.members[i].first);
        if (p == nullptr) return {nullptr, nullptr};
        const auto it = p->records.find(quest);
        if (it == p->records.end() || !UnderWay(it->second)) return {nullptr, nullptr};
        return {p, &it->second};
    };
    std::set<std::uint32_t> quests;
    for (std::size_t i = 0; i < party.members.size(); ++i) {
        if (!near[i]) continue;
        if (const PlayerQuests* p = Of(party.members[i].first))
            for (const auto& [id, r] : p->records)
                if (UnderWay(r)) quests.insert(id);
    }
    if (turns_.size() > 65536) turns_.clear(); // (a turn begun again is harmless)
    for (const std::uint32_t id : quests) {
        const QuestDef* q = Find(id);
        if (q == nullptr) continue;
        if (q->divide == 2 || q->divide == 3) {
            // The killer only.
            for (std::size_t i = 0; i < party.members.size(); ++i) {
                if (party.members[i].first != killer) continue;
                if (auto [p, r] = held(i, id); r != nullptr) credit(killer, *p, *q, *r);
            }
        } else if (q->divide == 1 || q->divide == 4 || q->divide == 5) {
            // Every holder near.
            for (std::size_t i = 0; i < party.members.size(); ++i)
                if (auto [p, r] = held(i, id); r != nullptr) credit(party.members[i].first, *p, *q, *r);
        } else {
            // One holder at a time: the first from the party's turn whose quest takes the kill; the turn moves past it.
            std::size_t& turn = turns_[{party.id, id}];
            for (std::size_t k = 0; k < party.members.size(); ++k) {
                const std::size_t i = (turn + k) % party.members.size();
                auto [p, r] = held(i, id);
                if (r == nullptr || !credit(party.members[i].first, *p, *q, *r)) continue;
                turn = i + 1;
                break;
            }
        }
    }
}

void Quests::Killed(std::uint32_t monster, const std::string& codename, std::uint8_t rarity, std::uint32_t killer) {
    std::vector<std::pair<std::uint32_t, const QuestDef::KillRule*>> killRules;
    // A quest's take of the kill: its kill goals count, its first gathering goal fed by the monster rolls its drop (one a
    // kill, the original's); whether anything took it.
    const auto credit = [&](std::uint32_t entity, PlayerQuests& p, const QuestDef& q, QuestRecord& r) {
        // Its goals met (8), only a goal paid by the piece goes on dropping (it skips the state 8 test).
        const bool met = r.status == proto::QuestStatus::Achieved;
        if (r.status != proto::QuestStatus::Achieving && !met) return false;
        if (met && std::none_of(q.goals.begin(), q.goals.end(), [](const QuestGoal& g) { return g.apiece; })) return false;
        bool changed = false, named = false, counted = false, gathered = false;
        for (std::size_t i = 0; i < q.goals.size() && i < r.progress.size(); ++i) {
            const QuestGoal& g = q.goals[i];
            if (met && !g.apiece) continue;
            if (g.kind == QuestGoalKind::Kill && Contains(g.monsters, codename)) named = true;
            // A berserk kill goal counts only for a holder in berserk (the hwan ghosts' kill handler body mode 1).
            const auto inBerserk = [&] {
                const auto who = host_.QuestPlayer(entity);
                return who && who->berserk;
            };
            if (g.kind == QuestGoalKind::Kill && r.progress[i] < Need(g) && Contains(g.monsters, codename) && ClassFits(g, rarity) &&
                (!g.berserk || inBerserk())) {
                ++r.progress[i];
                changed = counted = true;
            } else if (g.kind == QuestGoalKind::Drop && !gathered && (r.progress[i] < Need(g) || g.apiece)) {
                // (a goal paid by the piece drops on past its count)
                for (const QuestGoal::Source& s : g.sources) {
                    if (s.monster != codename && s.monster != "any") continue;
                    gathered = true;
                    double percent = s.percent;
                    if (s.byLevel) {
                        const auto who = host_.QuestPlayer(entity);
                        const int level = host_.QuestMonsterLevel(codename);
                        if (who && level > 0) percent *= game::QuestLevelScale(who->level, level);
                    }
                    if (std::uniform_real_distribution<double>(0.0, 100.0)(random_) < percent) host_.QuestDrop(entity, monster, g.item);
                    break;
                }
            }
        }
        if (met) return gathered; // (only its pieces dropped)
        // Its kill handler's rules (QuestDef::onKill): for a kill its kill goal counted (none naming it: any), kept for after
        // the share (they change the bag).
        bool ruled = false;
        for (const QuestDef::KillRule& k : q.onKill) {
            if (k.monster == codename && (counted || !named)) {
                killRules.push_back({entity, &k});
                ruled = true;
            }
        }
        changed |= Evaluate(entity, q, r);
        if (changed) Changed(entity, p, q, r);
        return counted || gathered || ruled;
    };
    ShareKill(killer, credit);
    std::vector<std::uint32_t> bags;
    for (const auto& [entity, k] : killRules) {
        const auto held = [&](const std::string& item) { return host_.QuestItemCount(entity, item); };
        if (!k->need.empty() && held(k->need) == 0) continue;
        if (!k->full.Empty() && !host_.QuestFreePlace(entity)) {
            host_.QuestSay(entity, k->full.key.empty() ? k->full.literal : k->full.key);
            continue;
        }
        if (k->cap != 0 && !k->give.empty() && held(k->give) >= k->cap) continue;
        if (!k->give.empty() && !host_.QuestGive(entity, {{k->give, 1}})) continue;
        if (!k->take.empty() && held(k->take) > 0) host_.QuestTake(entity, {{k->take, 1}});
        for (const std::string& item : k->clear)
            if (const std::uint32_t n = held(item); n > 0) host_.QuestTake(entity, {{item, n}});
        if (std::find(bags.begin(), bags.end(), entity) == bags.end()) bags.push_back(entity);
    }
    for (const std::uint32_t entity : bags) BagChanged(entity);
}

void Quests::PlayerKilled(std::uint32_t killer, int level, std::uint16_t region, const std::string& job) {
    // Shared as a monster's kill (the original's event 0x35 takes the same way).
    ShareKill(killer, [&](std::uint32_t entity, PlayerQuests& p, const QuestDef& q, QuestRecord& r) {
        if (r.status != proto::QuestStatus::Achieving) return false;
        bool changed = false;
        for (std::size_t i = 0; i < q.goals.size() && i < r.progress.size(); ++i) {
            const QuestGoal& g = q.goals[i];
            if (g.kind != QuestGoalKind::KillPlayer || r.progress[i] >= Need(g)) continue;
            if (level < g.minLevel || (g.region != 0 && std::int16_t(region) != g.region)) continue;
            if (!g.jobs.empty() && !Contains(g.jobs, job)) continue;
            ++r.progress[i];
            changed = true;
        }
        const bool took = changed;
        changed |= Evaluate(entity, q, r);
        if (changed) Changed(entity, p, q, r);
        return took;
    });
}

bool Quests::PickUpRefused(std::uint32_t entity, const std::string& item) const {
    const PlayerQuests* p = Of(entity);
    if (p == nullptr) return false;
    bool any = false;
    for (const auto& [id, r] : p->records) {
        if (!UnderWay(r)) continue;
        const QuestDef* q = Find(id);
        if (q == nullptr) continue;
        for (const QuestGoal& g : q->goals) {
            if (g.kind != QuestGoalKind::Drop || g.item != item) continue;
            if (g.apiece || host_.QuestItemCount(entity, item) < g.count) return false; // still wanted
            any = true;
        }
    }
    return any;
}

void Quests::UniqueDied(const std::string& codename) {
    // Every player under way with a quest waiting for that death, in its region and holding its item (the
    // original's QNO_UNIQUE_DEAD_MA_* handlers: the player's region, the unique's codename, the item counted, one
    // taken, the other added): the swaps first, then the bags recounted (the gathering goal counts the new item).
    struct Swap {
        std::uint32_t entity;
        const QuestDef::UniqueDead* on;
        const QuestDef* quest;
    };
    std::vector<Swap> swaps;
    for (auto& [entity, p] : players_) {
        std::optional<QuestHost::Player> who;
        for (const auto& [id, r] : p.records) {
            if (r.status != proto::QuestStatus::Achieving) continue;
            const QuestDef* q = Find(id);
            if (q == nullptr) continue;
            for (const QuestDef::UniqueDead& u : q->onUniqueDead) {
                if (u.monster != codename) continue;
                if (!who) who = host_.QuestPlayer(entity);
                if (who && (!u.region || who->region == *u.region)) swaps.push_back({entity, &u, q});
            }
        }
    }
    std::vector<std::uint32_t> changed;
    for (const Swap& s : swaps) {
        if (host_.QuestItemCount(s.entity, s.on->take) == 0 || !host_.QuestTake(s.entity, {{s.on->take, 1}})) continue;
        host_.QuestGive(s.entity, {{s.on->give, 1}});
        core::LogInfo("quests: {} saw {} fall: {} for {}", s.quest->codename, codename, s.on->take, s.on->give);
        if (std::find(changed.begin(), changed.end(), s.entity) == changed.end()) changed.push_back(s.entity);
    }
    for (const std::uint32_t entity : changed) BagChanged(entity);
}

void Quests::BagChanged(std::uint32_t entity) {
    PlayerQuests* p = Of(entity);
    if (p == nullptr) return;
    for (auto& [id, r] : p->records) {
        const QuestDef* q = Find(id);
        if (q != nullptr && Evaluate(entity, *q, r)) Changed(entity, *p, *q, r);
    }
}

void Quests::KeepTimer(std::uint32_t entity, PlayerQuests& p, std::uint32_t quest, Clock::time_point now, bool save) {
    (void)entity;
    const auto d = p.deadlines.find(quest);
    const auto r = p.records.find(quest);
    if (d == p.deadlines.end() || r == p.records.end() || !UnderWay(r->second)) return;
    const auto left = std::chrono::ceil<std::chrono::seconds>(d->second - now).count();
    const std::uint16_t word = std::uint16_t(std::clamp<std::int64_t>(left, 0, 0xFFFF));
    if (word == r->second.progress[kTimerSlot]) return;
    r->second.progress[kTimerSlot] = word;
    if (save) host_.QuestSave(p.character, r->second);
}

void Quests::Tick(Clock::time_point steady) {
    // The time limits run on their holders' clocks: out of time, the quest is lost; the seconds left kept in the record every
    // 10 seconds (the original's each second; and at the world's leave).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> late;
    for (auto& [entity, p] : players_) {
        for (const auto& [id, until] : p.deadlines) {
            if (until <= steady) {
                late.emplace_back(entity, id);
                continue;
            }
            const auto left = std::chrono::ceil<std::chrono::seconds>(until - steady).count();
            KeepTimer(entity, p, id, steady, left % 10 == 0);
        }
    }
    for (const auto& [entity, id] : late) Abandon(entity, id, proto::QuestEnd::TimeOut);
    // The item uses' waits run out (the original's countdown, its event 0x18 each second of the player's update).
    struct Due {
        std::uint32_t entity, quest;
        const QuestDef::UseRule* rule;
        int stage;
    };
    std::vector<Due> due;
    for (auto& [entity, p] : players_) {
        for (auto it = p.searches.begin(); it != p.searches.end();) {
            if (it->second.until > steady) {
                ++it;
                continue;
            }
            due.push_back({entity, it->first, it->second.rule, it->second.stage});
            it = p.searches.erase(it);
        }
    }
    for (const Due& d : due) {
        if (PlayerQuests* p = Of(d.entity)) EndSearch(d.entity, *p, d.quest, *d.rule, d.stage, 0);
    }
    // Raised monsters whose time is over go without dying (with
    // 3, 0xFF); one killed first is gone already (its nest goes by itself).
    std::erase_if(raised_, [&](const RaisedMonster& m) {
        if (m.until > steady) return false;
        host_.QuestDespawn(m.nest);
        return true;
    });
    // The captures (each minute, the COS's events): the COS dead, its minutes running out.
    struct Lost {
        std::uint32_t entity, quest;
        const game::QuestText* text;
    };
    std::vector<Lost> lost;
    struct Raise {
        std::uint32_t entity;
        const QuestDef::UseRule::Capture* capture;
    };
    std::vector<Raise> raises;
    for (auto& [entity, p] : players_) {
        for (auto& [quest, c] : p.catches) {
            if (c.rule == nullptr || !c.rule->capture) continue;
            const QuestDef::UseRule::Capture& cap = *c.rule->capture;
            // A caught COS not out yet (its owner not standing in the world at its entry): brought back once it stands.
            if (cap.item.empty() && c.cos == 0) {
                if (const auto rec = p.records.find(quest); rec != p.records.end() && rec->second.progress[kCatchMarkSlot] == kCaught) {
                    c.cos = host_.QuestResummon(entity, cap.cos);
                    if (c.cos != 0) core::LogInfo("quests: {}'s {} back for quest {} ({} minutes)", entity, cap.cos, quest, c.minutes);
                }
            }
            // Its own death (the event 0x19 on the COS, e.g.); a COS not brought back (0) only runs out its minutes.
            if (cap.item.empty() && c.cos != 0 && !host_.QuestCosStanding(c.cos)) {
                lost.push_back({entity, quest, &cap.cosDied});
                continue;
            }
            const auto rec = p.records.find(quest);
            while (steady >= c.nextMinute && c.minutes > 0) {
                c.nextMinute += std::chrono::minutes(1);
                // IVY_1's guardian, raised before the minute is counted.
                if (!cap.raise.empty()) raises.push_back({entity, &cap});
                --c.minutes;
                if (rec != p.records.end()) { // kept at each minute 
                    rec->second.progress[kCatchMinutesSlot] = std::uint16_t(c.minutes);
                    host_.QuestSave(p.character, rec->second);
                }
                if (c.minutes == 0) {
                    lost.push_back({entity, quest, &cap.ran});
                    break;
                }
                for (const auto& [left, text] : cap.warnings) {
                    if (left == c.minutes && !text.Empty()) host_.QuestSay(entity, text.key.empty() ? text.literal : text.key);
                }
                if (c.minutes % 10 == 0) SetGauge(entity, quest, c.minutes * 60, steady); // the display every ten minutes
            }
        }
    }
    // A quest's own raises (QuestDef::raises; QNO_RM_SLAVE2_2's timer): the countdown set when it is first seen
    // under way, then a raise each time it runs out while it stays under way.
    struct OwnRaise {
        std::uint32_t entity;
        const QuestDef::Raise* raise;
    };
    std::vector<OwnRaise> own;
    for (auto& [entity, p] : players_) {
        for (auto it = p.raiseAt.begin(); it != p.raiseAt.end();) {
            const auto rec = p.records.find(it->first.first);
            it = rec == p.records.end() || !UnderWay(rec->second) ? p.raiseAt.erase(it) : std::next(it);
        }
        for (const auto& [id, r] : p.records) {
            const QuestDef* q = UnderWay(r) ? Find(id) : nullptr;
            if (q == nullptr) continue;
            for (std::size_t i = 0; i < q->raises.size(); ++i) {
                const auto period = std::chrono::seconds(q->raises[i].seconds);
                auto [at, fresh] = p.raiseAt.try_emplace({id, i}, steady + period);
                if (fresh || steady < at->second) continue;
                at->second = steady + period;
                own.push_back({entity, &q->raises[i]});
            }
        }
    }
    for (const OwnRaise& r : own) {
        const auto who = host_.QuestPlayer(r.entity);
        if (!who || who->town || who->dead) continue; // a living holder outside a town
        const game::SpawnOffset o = game::QuestSpawnOffset(r.raise->nearest, r.raise->farthest, random_);
        proto::WorldPosition at = who->position;
        if ((at.region & 0x8000) != 0) {
            at.x += o.dx;
            at.z += o.dz;
        } else {
            const proto::FlatPosition c = proto::ToFlat(who->position);
            at = proto::FromFlat({c.x + o.dx, c.z + o.dz}, who->position.y);
            at.layer = who->position.layer;
        }
        host_.QuestSpawn(r.entity, r.raise->monster, at);
    }
    for (const Raise& r : raises) {
        const auto who = host_.QuestPlayer(r.entity);
        if (!who || who->town) continue; // not in a town (the region's byte +2)
        const game::SpawnOffset o = game::QuestSpawnOffset(r.capture->raiseNear, r.capture->raiseFar, random_);
        proto::WorldPosition at = who->position;
        if ((at.region & 0x8000) != 0) {
            at.x += o.dx;
            at.z += o.dz;
        } else {
            const proto::FlatPosition c = proto::ToFlat(who->position);
            at = proto::FromFlat({c.x + o.dx, c.z + o.dz}, who->position.y);
            at.layer = who->position.layer;
        }
        host_.QuestSpawn(r.entity, r.capture->raise, at);
    }
    for (const Lost& l : lost) {
        if (PlayerQuests* p = Of(l.entity)) DropCatch(l.entity, *p, l.quest, l.text);
    }
    TickWorlds(steady);
    TickTrials(steady);
}

// --- A talk game (QuestDef::talkGame) ------------------------------------------------------------------------------------

namespace {

// Huhoan's drinks (QNO_CH_BIGMAN_1): each liquor held a row (in this order), its strength added when poured, and the
// row's words; Huhoan's words after a pour still short of his mood, by the row's place in the list (the talk's _36.._41,
// by place, not by drink).
struct Drink {
    std::string_view item;
    int strength;
    std::string_view row;
};
constexpr Drink kDrinks[] = {
    {"ITEM_QNO_CH_EUROPE_3_04", 10, "SN_TALK_QNO_CH_BIGMAN_1_02"}, // wine
    {"ITEM_QNO_CH_EUROPE_3_05", 20, "SN_TALK_QNO_CH_BIGMAN_1_03"}, // champagne
    {"ITEM_QNO_CH_EUROPE_3_06", 40, "SN_TALK_QNO_CH_BIGMAN_1_04"}, // whisky
    {"ITEM_QNO_CH_POTION_5_04", 30, "SN_TALK_QNO_CH_BIGMAN_1_05"}, // bamboo refined wine
    {"ITEM_QNO_CH_POTION_5_05", 50, "SN_TALK_QNO_CH_BIGMAN_1_06"}, // kaoliang wine
    {"ITEM_QNO_CH_POTION_5_06", 60, "SN_TALK_QNO_CH_BIGMAN_1_07"}, // pome wine
};
constexpr std::string_view kAfterPour[] = {"SN_TALK_QNO_CH_BIGMAN_1_36", "SN_TALK_QNO_CH_BIGMAN_1_37", "SN_TALK_QNO_CH_BIGMAN_1_38",
                                           "SN_TALK_QNO_CH_BIGMAN_1_39", "SN_TALK_QNO_CH_BIGMAN_1_40", "SN_TALK_QNO_CH_BIGMAN_1_41"};
constexpr std::size_t kAsk = 100, kTrade = 101, kRefuse = 102;
constexpr std::string_view kFlute = "ITEM_QNO_WC_FERRY4_1_01", kBook = "ITEM_QNO_CH_BIGMAN_1_01";
constexpr std::uint64_t kBookGold = 50'000; // its row _29 "Exchange 50,000 GOLD" (the gold taken)

} // namespace

void Quests::PlayTalkGame(std::uint32_t entity, PlayerQuests& p, const QuestDef& q, std::uint32_t npc, std::optional<std::uint8_t> row) {
    if (q.talkGame != "huhoan-drinks") return Answer(entity, q.id, proto::QuestCode::NotOffered);
    const auto says = [&](std::string_view text, proto::QuestChoice choice, std::vector<std::string> rows) {
        proto::QuestSays s{npc, q.id, {}, 0, choice};
        s.text = std::string(text);
        s.rows = std::move(rows);
        Send(entity, s);
    };
    const auto end = [&](std::string_view text) {
        p.games.erase(q.id);
        says(text, proto::QuestChoice::None, {});
    };
    PlayerQuests::TalkGame& g = p.games[q.id];
    // The drinks held as rows, then (after the first pour) the account book's question (its row _15).
    const auto pourRows = [&](std::string_view text, bool ask) {
        g.rows.clear();
        std::vector<std::string> rows;
        for (std::size_t k = 0; k < std::size(kDrinks); ++k) {
            if (host_.QuestItemCount(entity, std::string(kDrinks[k].item)) == 0) continue;
            g.rows.push_back(k);
            rows.emplace_back(kDrinks[k].row);
        }
        if (ask) {
            g.rows.push_back(kAsk);
            rows.emplace_back("SN_TALK_QNO_CH_BIGMAN_1_15");
        }
        says(text, proto::QuestChoice::Rows, std::move(rows));
    };
    const auto rec = p.records.find(q.id);
    if (!row) {
        // The book already his: "the wine is all mine" (_42). Else a new round: his mood drawn, 30..40 by twos
        // ((rand % 6 + 15) x 2), nothing poured; _01 and the drinks.
        if (rec != p.records.end() && rec->second.times > 0) return end("SN_TALK_QNO_CH_BIGMAN_1_42");
        g = {};
        g.target = (std::uniform_int_distribution<int>(0, 5)(random_) + 15) * 2;
        return pourRows("SN_TALK_QNO_CH_BIGMAN_1_01", false);
    }
    if (g.target == 0 || *row >= g.rows.size()) {
        p.games.erase(q.id);
        return Answer(entity, q.id, proto::QuestCode::BadChoice);
    }
    const std::size_t pick = g.rows[*row];
    if (pick == kRefuse) return end("SN_TALK_COMMON_END");
    if (pick == kTrade) {
        // The book for the flute (an item changed), else for 50,000 gold (short: _30); no room: _35; then _33,
        // Huhoan's quest finished (its record, count one) and the lottery's book in the bag.
        const bool flute = host_.QuestItemCount(entity, std::string(kFlute)) > 0;
        if (!flute) {
            const auto who = host_.QuestPlayer(entity);
            if (!who || who->gold < kBookGold) return end("SN_TALK_QNO_CH_BIGMAN_1_30");
        }
        if (!host_.QuestRoom(entity, {{std::string(kBook), 1}})) return end("SN_TALK_QNO_CH_BIGMAN_1_35");
        if (flute ? !host_.QuestTake(entity, {{std::string(kFlute), 1}}) : !host_.QuestTakeGold(entity, kBookGold))
            return end("SN_TALK_QNO_CH_BIGMAN_1_30");
        host_.QuestGive(entity, {{std::string(kBook), 1}});
        QuestRecord& r = p.records[q.id];
        r.quest = q.id;
        r.status = proto::QuestStatus::Done;
        r.times = std::uint16_t(std::min(0xFFFF, int(r.times) + 1));
        r.progress = {};
        if (r.started == 0) r.started = UnixNow();
        r.ended = UnixNow();
        host_.QuestSave(p.character, r);
        if (const auto who = host_.QuestPlayer(entity)) core::LogInfo("quests: {} wins Huhoan's account book ({})", who->name, flute ? "the flute" : "gold");
        BagChanged(entity);
        TellMarks(entity, p);
        return end("SN_TALK_QNO_CH_BIGMAN_1_33");
    }
    if (pick == kAsk) {
        // Asked: only at his mood exactly does he talk of the book (the target equal to the sum; else _16, the
        // drinks spent).
        if (g.sum != g.target) return end("SN_TALK_QNO_CH_BIGMAN_1_16");
        g.exchange = true;
        const bool flute = host_.QuestItemCount(entity, std::string(kFlute)) > 0;
        g.rows = {kTrade, kRefuse};
        return says(flute ? "SN_TALK_QNO_CH_BIGMAN_1_25" : "SN_TALK_QNO_CH_BIGMAN_1_28", proto::QuestChoice::Rows,
                    flute ? std::vector<std::string>{"SN_TALK_QNO_CH_BIGMAN_1_26", "SN_TALK_QNO_CH_BIGMAN_1_27"}
                          : std::vector<std::string>{"SN_TALK_QNO_CH_BIGMAN_1_29", "SN_TALK_QNO_CH_BIGMAN_1_31"});
    }
    // A pour: the cup taken (none: _32), its strength added; the first "Kya" (_08), then at his mood or past it _17, short
    // of it the row's place's words.
    const Drink& d = kDrinks[pick];
    if (!host_.QuestTake(entity, {{std::string(d.item), 1}})) return end("SN_TALK_QNO_CH_BIGMAN_1_32");
    BagChanged(entity);
    g.sum += d.strength;
    if (!g.poured) {
        g.poured = true;
        return pourRows("SN_TALK_QNO_CH_BIGMAN_1_08", true);
    }
    if (g.sum >= g.target) return pourRows("SN_TALK_QNO_CH_BIGMAN_1_17", true);
    return pourRows(kAfterPour[std::min<std::size_t>(*row, std::size(kAfterPour) - 1)], true);
}

// --- An item used from the bag (QuestDef::UseRule) ------------------------------------

std::uint8_t Quests::OriginalState(const QuestRecord& r, const QuestDef* q) {
    if (r.status == proto::QuestStatus::Achieved) return 8;
    if (r.status != proto::QuestStatus::Achieving) return 4;
    // The original's 7: a kill or gathering goal has progressed (its missions change 1 to 7 at their first count). A catch's
    // minutes count (its catch the capture's progress); its mark and the supply's day above the goals do not (the loss sets
    // the state back to 1).
    for (std::size_t i = 0; i < r.progress.size(); ++i) {
        if (q != nullptr && i >= q->goals.size() && (i == kCatchMarkSlot || i == kSupplyDaySlot)) continue;
        if (r.progress[i] != 0) return 7;
    }
    return 1;
}

namespace {

// The original's place check: the 3D distance from the point,
// across regions, within the radius.
bool Within(const proto::WorldPosition& at, const QuestDef::UseRule::Place& p) {
    const proto::FlatPosition a = proto::ToFlat(at), b = proto::ToFlat(proto::WorldPosition{p.region, p.x, p.y, p.z, 0});
    const double dx = a.x - b.x, dz = a.z - b.z, dy = double(at.y) - double(p.y);
    return dx * dx + dy * dy + dz * dz <= double(p.radius) * double(p.radius);
}

} // namespace

bool Quests::UseItem(std::uint32_t entity, const std::string& item) {
    PlayerQuests* p = Of(entity);
    if (p == nullptr) return false;
    const auto say = [&](const game::QuestText& t) {
        if (!t.Empty()) host_.QuestSay(entity, t.key.empty() ? t.literal : t.key);
    };
    const auto steady = Clock::now();
    std::optional<QuestHost::Player> who;
    // Every quest the player has (finished ones too: their handler refuses with its words), in turn; the first that
    // takes it ends the offer (the dispatcher's loop over the player's quest map).
    for (auto& [id, r] : p->records) {
        const QuestDef* q = Find(id);
        if (q == nullptr) continue;
        for (const QuestDef::UseRule& u : q->onUse) {
            if (u.item != item || u.trap) continue; // a trap's rule is its catch's (Trapped), not the bag's
            if (!who) who = host_.QuestPlayer(entity);
            if (!who) return false;
            const std::uint8_t state = OriginalState(r, q);
            const bool stateFits = u.statuses.empty() ? (state == 1 || state == 7)
                                                      : std::find(u.statuses.begin(), u.statuses.end(), state) != u.statuses.end();
            if (!stateFits || (u.firstTime && r.times > 0)) {
                // Its goals met: its own words when it has them (QNO_EU_ADVENTURER_1's _09).
                say(!stateFits && state == 8 && !u.met.Empty() ? u.met : u.refuse);
                continue;
            }
            if (u.oneAtATime && p->searches.contains(id)) {
                say(u.busy);
                continue;
            }
            // A quest's instance only from the shared world (the player's world 1, before
            // any words), silently.
            if (u.instance && who->position.layer != 0) continue;
            // A free place and the items needed, silently (IVY_3's Punisher's Blood; ORNAMENT_4's calabash
            // and stone, STONEBOARD_4's tablet and head, before their place).
            if (u.room && !host_.QuestFreePlace(entity)) continue;
            if (std::any_of(u.need.begin(), u.need.end(), [&](const std::string& i) { return host_.QuestItemCount(entity, i) == 0; })) continue;
            const auto atPlace = [&] {
                return u.at.empty() || std::any_of(u.at.begin(), u.at.end(), [&](const QuestDef::UseRule::Place& at) { return Within(who->position, at); });
            };
            // The place before the party where the handler asks it so (IVY_3).
            if (u.placeFirst && u.stages.empty() && !atPlace()) {
                say(u.away);
                continue;
            }
            if (u.party && !who->sharedParty) {
                say(u.noParty);
                continue;
            }
            if (u.capture && p->catches.contains(id)) {
                say(u.capture->already);
                continue;
            }
            const auto capped = [&] { return u.cap != 0 && host_.QuestItemCount(entity, u.capItem) >= u.cap; };
            if (u.capFirst && capped()) {
                say(u.capped);
                continue;
            }
            // The place: the stage in turn (the record's step), else any of its places.
            int stage = -1;
            if (!u.stages.empty()) {
                const std::size_t step = r.progress[kStepSlot];
                if (step >= u.stages.size() || !Within(who->position, u.stages[step].at)) {
                    say(u.away);
                    continue;
                }
                stage = int(step);
            } else if (!u.placeFirst && !atPlace()) {
                say(u.away);
                continue;
            }
            // The sites in turn (IVY_3's Horn Trumpet): the first the player stands at without its item.
            const QuestDef::UseRule::Site* site = nullptr;
            if (!u.sites.empty()) {
                for (const QuestDef::UseRule::Site& s : u.sites) {
                    if (Within(who->position, s.at) && host_.QuestItemCount(entity, s.item) == 0) {
                        site = &s;
                        break;
                    }
                }
                if (site == nullptr) {
                    say(u.away);
                    continue;
                }
            }
            if (capped()) {
                say(u.capped);
                continue;
            }
            // The selected monster (its distance, its codename).
            std::optional<QuestHost::Selected> target;
            if (!u.targets.empty()) {
                target = host_.QuestSelected(entity);
                if (!target) {
                    say(u.noTarget.Empty() ? u.wrongTarget : u.noTarget);
                    continue;
                }
                const proto::WorldPosition& t = target->position;
                if (u.targetRange > 0 && !Within(who->position, {t.region, t.x, t.y, t.z, u.targetRange})) {
                    say(u.tooFar);
                    continue;
                }
                if (std::find(u.targets.begin(), u.targets.end(), target->codename) == u.targets.end()) {
                    say(u.wrongTarget);
                    continue;
                }
            }
            if (!u.full.Empty() && !host_.QuestFreePlace(entity)) {
                say(u.full);
                continue;
            }
            // A quest's instance: its party into it; the item itself kept (the handler returns 0 after opening it,
            // the dispatcher goes on, then refuses it 2).
            if (u.instance) {
                if (OpenWorld(entity, *who, *q, u, steady)) core::LogInfo("quests: {} opens {} for {}", who->name, u.instance->world, q->codename);
                continue;
            }
            // Taken.
            core::LogInfo("quests: {} uses {} for {}", who->name, item, q->codename);
            if (!u.need.empty()) {
                // The gift first; given, one of each item needed taken; not given: refused, kept.
                if (!u.give.empty() && !host_.QuestGive(entity, {{u.give, 1}})) continue;
                for (const std::string& i : u.need) host_.QuestTake(entity, {{i, 1}});
                say(u.got);
                BagChanged(entity);
                return true;
            }
            if (site != nullptr) {
                // The site's monster about the player (the quest service's spawn, its radius 20 + 80 x rand),
                // its five-minute timer and its kill's drop (the quest's hidden gathering goals: the signs).
                const game::SpawnOffset o = game::QuestSpawnOffset(site->spawn.near, site->spawn.far, random_);
                proto::WorldPosition at = who->position;
                if ((at.region & 0x8000) != 0) {
                    at.x += o.dx;
                    at.z += o.dz;
                } else {
                    const proto::FlatPosition c = proto::ToFlat(who->position);
                    at = proto::FromFlat({c.x + o.dx, c.z + o.dz}, who->position.y);
                    at.layer = who->position.layer;
                }
                const auto nest = host_.QuestSpawn(entity, site->spawn.monster, at);
                if (!nest) continue; // its reference not found: 0, refused
                if (site->spawn.seconds != 0) raised_.push_back({*nest, steady + std::chrono::seconds(site->spawn.seconds)});
                say(u.got);
                return true;
            }
            if (!u.spawns.empty()) {
                // The monsters raised about the player (each the original's spawn with its own radius); none raised (no such
                // monster: the handler's reference lookup fails) refuses it.
                int made = 0;
                for (const QuestDef::UseRule::Spawn& s : u.spawns) {
                    for (std::uint32_t i = 0; i < s.count; ++i) {
                        const game::SpawnOffset o = game::QuestSpawnOffset(s.near, s.far, random_);
                        proto::WorldPosition at = who->position;
                        if ((at.region & 0x8000) != 0) {
                            at.x += o.dx;
                            at.z += o.dz;
                        } else {
                            const proto::FlatPosition c = proto::ToFlat(who->position);
                            at = proto::FromFlat({c.x + o.dx, c.z + o.dz}, who->position.y);
                            at.layer = who->position.layer;
                        }
                        const auto nest = host_.QuestSpawn(entity, s.monster, at);
                        if (!nest) continue;
                        ++made;
                        if (s.seconds != 0) raised_.push_back({*nest, steady + std::chrono::seconds(s.seconds)});
                    }
                }
                if (made == 0) {
                    core::LogInfo("quests: {}'s {} raised nothing", who->name, item);
                    continue;
                }
                say(u.got);
                return true;
            }
            if (u.capture) {
                // The roll (rand % 101 against the bound); caught, the monster goes without dying and
                // the COS comes in its place 
                const QuestDef::UseRule::Capture& c = *u.capture;
                const int chance = game::CaptureChance(c.roll, c.step, c.below, who->level, host_.QuestMonsterLevel(c.cos));
                if (std::uniform_int_distribution<int>(0, 100)(random_) >= chance) {
                    say(u.failed);
                    return true;
                }
                const std::uint32_t cos = host_.QuestCatch(entity, target ? target->entity : 0, c.cos);
                if (cos == 0) continue; // the target could not be taken: 0, refused
                // The record's minutes and mark (byte +4, word +0xA = 10), saved; the display.
                HoldCatch(*p, id, u, cos, c.minutes, kCaught, steady);
                SetGauge(entity, id, c.minutes * 60, steady);
                core::LogInfo("quests: {} catches {} for {} ({} minutes)", who->name, c.cos, q->codename, c.minutes);
                say(u.got);
                TellMarks(entity, *p);
                return true;
            }
            // A search: its wait begins (a second use of its quest's item while one runs starts it again, as the original's
            // countdown byte is set again), or it ends at once.
            const std::uint32_t seconds = stage >= 0 ? u.stages[std::size_t(stage)].seconds : u.seconds;
            if (seconds == 0) return EndSearch(entity, *p, id, u, stage, state);
            p->searches[id] = {&u, steady + std::chrono::seconds(seconds), stage};
            host_.QuestSearchStarted(entity, id, seconds);
            return true;
        }
    }
    return false;
}

bool Quests::Searching(std::uint32_t entity, std::uint32_t quest) const {
    const PlayerQuests* p = Of(entity);
    return p != nullptr && p->searches.contains(quest);
}

void Quests::CancelSearch(std::uint32_t entity, std::uint32_t quest) {
    const auto it = players_.find(entity);
    if (it != players_.end() && it->second.searches.erase(quest) > 0) core::LogInfo("quests: entity {} cancels its search for quest {}", entity, quest);
}

bool Quests::EndSearch(std::uint32_t entity, PlayerQuests& p, std::uint32_t quest, const QuestDef::UseRule& u, int stage, std::uint8_t state) {
    const auto rec = p.records.find(quest);
    if (rec == p.records.end()) return true; // given up meanwhile: its record is gone
    const auto say = [&](const game::QuestText& t) {
        if (!t.Empty()) host_.QuestSay(entity, t.key.empty() ? t.literal : t.key);
    };
    // The roll: a stage's (rand() % 101 <= its bound, as `chance` in 101), else rand() % outOf against the handler's bound.
    bool won = true;
    if (stage >= 0) won = std::uniform_int_distribution<std::uint32_t>(0, 100)(random_) < u.stages[std::size_t(stage)].chance;
    else if (u.outOf != 0) won = std::uniform_int_distribution<std::uint32_t>(0, u.outOf - 1)(random_) < u.chance;
    const bool kept = std::find(u.keptIn.begin(), u.keptIn.end(), state) != u.keptIn.end();
    if (stage >= 0 && won) {
        // The step goes on after a won roll, the grant or not.
        rec->second.progress[kStepSlot] = std::uint16_t(rec->second.progress[kStepSlot] + 1);
        host_.QuestSave(p.character, rec->second);
    }
    if (!won && u.orGive.empty()) {
        say(u.failed);
        return !kept;
    }
    const std::string& gift = won ? u.give : u.orGive;
    if (gift.empty()) return !kept;
    // Nothing given at the limit (the witch's 60 boxes) or without the item exchanged; the item is used.
    if (u.limit != 0 && host_.QuestItemCount(entity, u.limitItem) >= u.limit) return !kept;
    if (!u.take.empty() && (host_.QuestItemCount(entity, u.take) == 0 || !host_.QuestTake(entity, {{u.take, 1}}))) return !kept;
    // The original's grant fails without a word (a bag filled meanwhile): nothing given. The coin's grant failing
    // returns 0: its amulet kept.
    if (!host_.QuestGive(entity, {{gift, 1}})) {
        if (!u.take.empty()) host_.QuestGive(entity, {{u.take, 1}});
        return u.orGive.empty() && !kept;
    }
    say(u.got);
    BagChanged(entity); // its gathering goal counts it
    return !kept;
}

void Quests::DropCatch(std::uint32_t entity, PlayerQuests& p, std::uint32_t quest, const game::QuestText* text, std::uint32_t minutesLeft) {
    const auto it = p.catches.find(quest);
    if (it == p.catches.end()) return;
    const QuestDef::UseRule* rule = it->second.rule;
    host_.QuestCosRemove(it->second.cos);
    if (minutesLeft == 0) {
        p.catches.erase(it);
    } else {
        it->second.cos = 0;
        it->second.minutes = minutesLeft;
    }
    // A caught item: every one taken (e.g. GORIA_6's).
    if (rule != nullptr && rule->capture && !rule->capture->item.empty()) {
        if (const std::uint32_t held = host_.QuestItemCount(entity, rule->capture->item); held > 0) {
            host_.QuestTake(entity, {{rule->capture->item, held}});
            BagChanged(entity);
        }
    }
    // The record: no minutes left and lost (marked 5, no minutes), or GORIA_6's death (its minutes 5, its mark 0); the
    // display cleared.
    if (const auto rec = p.records.find(quest); rec != p.records.end() && UnderWay(rec->second)) {
        rec->second.progress[kCatchMinutesSlot] = std::uint16_t(minutesLeft);
        rec->second.progress[kCatchMarkSlot] = minutesLeft == 0 ? kLost : 0;
        host_.QuestSave(p.character, rec->second);
    }
    SetGauge(entity, quest, 0, Clock::now());
    if (text != nullptr && !text->Empty()) host_.QuestSay(entity, text->key.empty() ? text->literal : text->key);
    TellMarks(entity, p);
}

void Quests::HoldCatch(PlayerQuests& p, std::uint32_t quest, const QuestDef::UseRule& rule, std::uint32_t cos, std::uint32_t minutes, std::uint16_t mark,
                       Clock::time_point now) {
    p.catches[quest] = {&rule, cos, minutes, now + std::chrono::minutes(1)};
    if (const auto rec = p.records.find(quest); rec != p.records.end()) {
        rec->second.progress[kCatchMinutesSlot] = std::uint16_t(minutes);
        rec->second.progress[kCatchMarkSlot] = mark;
        host_.QuestSave(p.character, rec->second);
    }
}

void Quests::ResumeCatches(std::uint32_t entity, PlayerQuests& p) {
    // On the world entry: a COS caught (states 1/7/8, marked 10) and none of its kind held: brought back beside its
    // owner with the minutes the record kept (EASTEU_11_1's, FLYSHIP1_3's alike); a caught item's: only its display, its
    // minutes still counted while any are left. The minutes run only while its owner is in the world. The other game
    // server reads the same record: a move there brings it back the same way.
    const auto now = Clock::now();
    for (auto& [id, r] : p.records) {
        if (!UnderWay(r) || p.catches.contains(id)) continue;
        const QuestDef* q = Find(id);
        if (q == nullptr) continue;
        const std::uint32_t minutes = r.progress[kCatchMinutesSlot] & 0xFF;
        const std::uint16_t mark = r.progress[kCatchMarkSlot];
        if (minutes == 0) continue;
        for (const QuestDef::UseRule& u : q->onUse) {
            if (!u.capture) continue;
            if (u.capture->item.empty()) {
                if (mark != kCaught) break;
                const std::uint32_t cos = host_.QuestResummon(entity, u.capture->cos);
                p.catches[id] = {&u, cos, minutes, now + std::chrono::minutes(1)}; // 0: once it stands (Tick)
                if (cos != 0) core::LogInfo("quests: {}'s {} back for {} ({} minutes)", entity, u.capture->cos, q->codename, minutes);
            } else {
                p.catches[id] = {&u, 0, minutes, now + std::chrono::minutes(1)};
            }
            SetGauge(entity, id, minutes * 60, now);
            break;
        }
    }
}

void Quests::Died(std::uint32_t entity) {
    PlayerQuests* p = Of(entity);
    if (p == nullptr) return;
    // On their holder's death: a COS runs away; a caught item only while one is held
    // (GENERAL_1), then every one taken.
    struct Gone {
        std::uint32_t quest;
        const game::QuestText* text;
        std::uint32_t minutesLeft;
    };
    std::vector<Gone> gone;
    for (const auto& [quest, c] : p->catches) {
        if (c.rule == nullptr || !c.rule->capture) continue;
        const QuestDef::UseRule::Capture& cap = *c.rule->capture;
        if (!cap.item.empty() && host_.QuestItemCount(entity, cap.item) == 0) continue;
        gone.push_back({quest, &cap.ownerDied, cap.item.empty() ? 0u : cap.diedMinutes});
    }
    for (const Gone& g : gone) DropCatch(entity, *p, g.quest, g.text, g.minutesLeft);
}

void Quests::Trapped(std::uint32_t entity, std::uint32_t monster, const std::string& codename) {
    PlayerQuests* p = Of(entity);
    const auto who = host_.QuestPlayer(entity);
    if (p == nullptr || !who) return;
    const auto say = [&](const game::QuestText& t) {
        if (!t.Empty()) host_.QuestSay(entity, t.key.empty() ? t.literal : t.key);
    };
    const auto now = Clock::now();
    // Every quest of the player hears the event: each `trap` rule for that monster, in the quests' order.
    for (auto& [id, r] : p->records) {
        const QuestDef* q = Find(id);
        if (q == nullptr) continue;
        for (const QuestDef::UseRule& u : q->onUse) {
            if (!u.trap || !u.capture || std::find(u.targets.begin(), u.targets.end(), codename) == u.targets.end()) continue;
            const QuestDef::UseRule::Capture& c = *u.capture;
            const std::uint8_t state = OriginalState(r, q);
            const bool fits = u.statuses.empty() ? (state == 1 || state == 7) : std::find(u.statuses.begin(), u.statuses.end(), state) != u.statuses.end();
            if (!fits) break; // silently
            if (!u.full.Empty() && !host_.QuestFreePlace(entity)) {
                say(u.full);
                break;
            }
            // One held: its minutes running (the record's byte +4; GENERAL_1 asks nothing).
            const auto held = p->catches.find(id);
            if (!c.already.Empty() && held != p->catches.end() && held->second.minutes > 0) {
                say(c.already);
                break;
            }
            // One roll, several catches (QNO_*_HWAN_1_4_1): the first outcome under its bound gives its
            // item and its words, none `failed`; the monster stays when the rule says so; nothing held, nothing taken back.
            if (!c.outcomes.empty()) {
                const int roll = std::uniform_int_distribution<int>(0, 100)(random_);
                const auto it = std::find_if(c.outcomes.begin(), c.outcomes.end(), [&](const auto& o) { return roll < o.below; });
                if (it == c.outcomes.end()) {
                    say(u.failed);
                    break;
                }
                if (!host_.QuestGive(entity, {{it->item, 1}})) break;
                if (!c.stays) host_.QuestTakeMonster(monster);
                core::LogInfo("quests: {} traps {} for {}: {} (roll {})", who->name, codename, q->codename, it->item, roll);
                say(it->said);
                BagChanged(entity);
                TellMarks(entity, *p);
                break;
            }
            // The roll: rand() % 101 below 50 less 3 a level the quest's COS record stands above the player; these setups set no
            // such record, its level 0: always 50 in 101.
            const int level = c.cos.empty() ? 0 : host_.QuestMonsterLevel(c.cos);
            const int chance = game::CaptureChance(c.roll, c.step, c.below, who->level, level);
            if (std::uniform_int_distribution<int>(0, 100)(random_) >= chance) {
                say(u.failed);
                break;
            }
            // Caught: one item (a grant that fails: nothing more), its minutes and mark, the display, the monster out
            // without dying, the words.
            if (!c.item.empty() && !host_.QuestGive(entity, {{c.item, 1}})) break;
            host_.QuestTakeMonster(monster);
            HoldCatch(*p, id, u, 0, c.minutes, kCaught, now);
            SetGauge(entity, id, c.minutes * 60, now);
            core::LogInfo("quests: {} traps {} for {} ({} minutes)", who->name, codename, q->codename, c.minutes);
            say(u.got);
            BagChanged(entity);
            TellMarks(entity, *p);
            break;
        }
    }
}

std::pair<std::uint32_t, std::uint32_t> Quests::Caught(std::uint32_t entity, std::uint32_t quest) const {
    const PlayerQuests* p = Of(entity);
    if (p == nullptr) return {};
    const auto it = p->catches.find(quest);
    return it != p->catches.end() ? std::pair{it->second.cos, it->second.minutes} : std::pair<std::uint32_t, std::uint32_t>{};
}

// --- A quest's instance (QuestDef::UseRule::World) -----------------------------------

bool Quests::OpenWorld(std::uint32_t entity, const QuestHost::Player& who, const QuestDef& q, const QuestDef::UseRule& u, Clock::time_point now) {
    const auto say = [&](std::uint32_t to, const game::QuestText& t) {
        if (!t.Empty()) host_.QuestSay(to, t.key.empty() ? t.literal : t.key);
    };
    const QuestDef::UseRule::World& w = *u.instance;
    if (who.position.layer != 0) return false; // only from the shared world (the player's world 1): silently
    const std::vector<std::uint32_t> party = host_.QuestPartyHere(entity);
    if (party.empty()) return false; // no party: silently
    if (!host_.QuestPartyMaster(entity)) {
        say(entity, w.notMaster);
        return false;
    }
    if (host_.QuestTransportOut(entity)) { // the walker's own check of its master
        say(entity, w.transport);
        return false;
    }
    const std::uint32_t layer = host_.QuestWorldOpen(entity, w.world);
    if (layer == 0 || !(u.start ? host_.QuestWorldEnterAt(entity, layer, proto::WorldPosition{u.start->region, u.start->x, u.start->y, u.start->z, 0}) : host_.QuestWorldEnter(entity, layer))) return false;
    QuestWorld world;
    world.quest = q.id;
    world.layer = layer;
    world.master = entity;
    world.rule = &u;
    world.next = now + std::chrono::seconds(w.after);
    world.inside.insert(entity);
    // The members this server holds (one elsewhere is not found):
    // no transport out (else its words to it), finished the quest before or holding the item (a talk rule's: holding the
    // quest, taken to its start).
    for (const std::uint32_t member : party) {
        if (member == entity) continue;
        const PlayerQuests* m = Of(member);
        if (m == nullptr) continue;
        if (host_.QuestTransportOut(member)) {
            say(member, w.transport);
            continue;
        }
        const auto rec = m->records.find(q.id);
        if (!u.npc.empty()) {
            if (rec == m->records.end() || !UnderWay(rec->second) || !TalkNeedHeld(member, q, u)) continue;
        } else {
            const bool finished = rec != m->records.end() && rec->second.times > 0;
            if (!finished && host_.QuestItemCount(member, u.item) == 0) continue;
        }
        const auto at = host_.QuestPlayer(member);
        if (!at || at->position.layer != 0) continue; // in the same world as its master
        // Asked first (the original's invitation, kind 7); the master went in at once.
        world.asked[member] = now + std::chrono::milliseconds(std::int64_t(proto::kQuestEnterAskSeconds * 1000.0));
        Send(member, proto::QuestEnterAsk{q.id});
    }
    worlds_.push_back(std::move(world));
    return true;
}

void Quests::EnterReply(std::uint32_t entity, const proto::QuestEnterReply& r) {
    for (QuestWorld& w : worlds_) {
        const auto asked = w.asked.find(entity);
        if (w.quest != r.quest || asked == w.asked.end()) continue;
        w.asked.erase(asked);
        if (r.yes == 0 || w.phase == QuestWorld::Phase::Done || w.phase == QuestWorld::Phase::Ending) return;
        // Still fit to go: in its master's party, in the shared world, alive, no transport out.
        const auto who = host_.QuestPlayer(entity);
        const std::vector<std::uint32_t> party = host_.QuestPartyHere(w.master);
        if (!who || who->dead || who->position.layer != 0 || std::find(party.begin(), party.end(), entity) == party.end()) return;
        if (host_.QuestTransportOut(entity)) {
            if (!w.rule->instance->transport.Empty()) host_.QuestSay(entity, w.rule->instance->transport.key.empty() ? w.rule->instance->transport.literal : w.rule->instance->transport.key);
            return;
        }
        const auto& start = w.rule->start;
        if (start ? host_.QuestWorldEnterAt(entity, w.layer, proto::WorldPosition{start->region, start->x, start->y, start->z, 0}) : host_.QuestWorldEnter(entity, w.layer)) {
            w.inside.insert(entity);
            core::LogInfo("quests: {} comes into {}'s instance", who->name, w.quest);
        }
        return;
    }
}

void Quests::TickWorlds(Clock::time_point now) {
    const auto say = [&](std::uint32_t to, const game::QuestText& t) {
        if (!t.Empty()) host_.QuestSay(to, t.key.empty() ? t.literal : t.key);
    };
    for (QuestWorld& w : worlds_) {
        const QuestDef::UseRule::World& spec = *w.rule->instance;
        std::erase_if(w.asked, [&](const auto& kv) { return kv.second <= now; }); // unanswered: not taken
        // Gone by another way (out of the game, a return scroll): no longer inside.
        std::erase_if(w.inside, [&](std::uint32_t m) {
            const auto who = host_.QuestPlayer(m);
            return !who || who->position.layer != w.layer;
        });
        // Out of its master's party: told, out 5 s later (UIIT_MSG_QUEST_INS_CONDITION; the party hook
        // and the tick's own check).
        const std::vector<std::uint32_t> party = host_.QuestPartyHere(w.master);
        for (const std::uint32_t m : w.inside) {
            if (std::find(party.begin(), party.end(), m) != party.end() || w.leaving.contains(m)) continue;
            host_.QuestSay(m, "UIIT_MSG_QUEST_INS_CONDITION");
            w.leaving[m] = now + std::chrono::seconds(5);
        }
        for (auto it = w.leaving.begin(); it != w.leaving.end();) {
            if (it->second > now) {
                ++it;
                continue;
            }
            if (w.inside.erase(it->first) != 0) host_.QuestWorldLeave(it->first);
            it = w.leaving.erase(it);
        }
        if (w.inside.empty()) continue;
        const auto all = [&](const game::QuestText& t) {
            for (const std::uint32_t m : w.inside) say(m, t);
        };
        // A character's line: the unique's (a general's), or a `replies` line's player, to everyone inside.
        if (w.unique != SIZE_MAX) {
            if (const std::uint32_t e = host_.QuestSpawnEntity(w.unique); e != 0) w.uniqueEntity = e;
        }
        const auto speak = [&](std::uint32_t speaker, const game::QuestText& t) {
            if (t.Empty()) return;
            for (const std::uint32_t m : w.inside) host_.QuestSpeak(m, speaker, t.key.empty() ? t.literal : t.key);
        };
        switch (w.phase) {
        case QuestWorld::Phase::Waiting: {
            if (now < w.next) break;
            // A general's stage: everyone inside moved there first, it comes 3 s on (BAEKRYOUNG_17's worlds in turn).
            if (!spec.generals.empty() && spec.generals[w.general].movesTo && !w.moved) {
                const QuestDef::UseRule::Place& p = *spec.generals[w.general].movesTo;
                for (const std::uint32_t m : w.inside) host_.QuestMoveInside(m, proto::WorldPosition{p.region, p.x, p.y, p.z, w.layer});
                w.moved = true;
                w.next = now + std::chrono::seconds(3);
                break;
            }
            // Its unique (or the general whose turn it is) 20..100 from the master (or the first one inside), in the instance's
            // layer (ORNAMENT_4's).
            const std::uint32_t by = w.inside.contains(w.master) ? w.master : *w.inside.begin();
            const auto who = host_.QuestPlayer(by);
            if (!who) break;
            const bool first = spec.generals.empty() || w.general == 0;
            const game::SpawnOffset o = game::QuestSpawnOffset(first ? spec.firstNear : spec.near, first ? spec.firstFar : spec.far, random_);
            proto::WorldPosition at = who->position;
            if ((at.region & 0x8000) != 0) {
                at.x += o.dx;
                at.z += o.dz;
            } else {
                const proto::FlatPosition c = proto::ToFlat(who->position);
                at = proto::FromFlat({c.x + o.dx, c.z + o.dz}, who->position.y);
                at.layer = who->position.layer;
            }
            const std::string& unique = spec.generals.empty() ? spec.unique : spec.generals[w.general].monster;
            const auto nest = host_.QuestSpawn(by, unique, at);
            if (!nest) break; // its ground not in yet: the next second
            w.unique = *nest;
            w.uniqueEntity = host_.QuestSpawnEntity(*nest);
            w.opener = by;
            w.uniqueAt = at;
            w.moved = false;
            // The opening: 2 s on its first line, each next 5 s on, the battle begun 5 s after the last (IVY_3's tick
            // EVENT_TIMER 2000, 5000 between Ivy's _11, the player's _12, Ivy's _13, the
            // begin; EASTEU_21's _14 then _21 alike; a general's line,
            // CinematicTime 2000, then 5000).
            w.phase = QuestWorld::Phase::Opening;
            w.line = 0;
            w.next = now + std::chrono::seconds(2);
            break;
        }
        case QuestWorld::Phase::Opening:
        case QuestWorld::Phase::Fighting: {
            const bool chain = !spec.generals.empty();
            if (host_.QuestSpawnStanding(w.unique) == 0) {
                // Slain (a general's too): its words; a general before the last, the next one `next` seconds on;
                // the last one, everyone out (the members' kills counted by the kill).
                if (chain && w.general + 1 < spec.generals.size()) {
                    speak(w.uniqueEntity, spec.generals[w.general].dies);
                    ++w.general;
                    if (spec.generals[w.general].movesTo) all(spec.moving); // "You will be teleported ... in 50 seconds." 
                    w.phase = QuestWorld::Phase::Waiting;
                    w.next = now + std::chrono::seconds(spec.next);
                    break;
                }
                speak(w.uniqueEntity, chain ? spec.generals[w.general].dies : spec.slain);
                if (spec.wayOut) {
                    // Its way out where it came, 20..100 added to its x and to its z (the original's rand / 32767 x 80 + 20
                    // each); the members leave by it, the time's end puts out the rest.
                    std::uniform_real_distribution<float> step(20.0f, 100.0f);
                    const float dx = step(random_), dz = step(random_);
                    proto::WorldPosition at = w.uniqueAt;
                    if ((at.region & 0x8000) != 0) {
                        at.x += dx;
                        at.z += dz;
                    } else {
                        const proto::FlatPosition c = proto::ToFlat(w.uniqueAt);
                        at = proto::FromFlat({c.x + dx, c.z + dz}, w.uniqueAt.y);
                        at.layer = w.uniqueAt.layer;
                    }
                    std::optional<proto::WorldPosition> to;
                    if (spec.out) to = proto::WorldPosition{spec.out->region, spec.out->x, spec.out->y, spec.out->z};
                    host_.QuestWayOut(at, to);
                    w.phase = QuestWorld::Phase::Done;
                    if (!w.ends) w.ends = now + std::chrono::minutes(spec.minutes);
                    break;
                }
                w.phase = QuestWorld::Phase::Ending;
                w.next = now;
                break;
            }
            if (w.phase == QuestWorld::Phase::Opening) {
                if (now < w.next) break;
                const std::vector<game::QuestText> lines = chain ? std::vector<game::QuestText>{spec.generals[w.general].says} : spec.comes;
                if (w.line < lines.size()) {
                    const bool reply = !chain && w.line < spec.replies.size() && spec.replies[w.line];
                    speak(reply ? w.opener : w.uniqueEntity, lines[w.line++]);
                    w.next = now + std::chrono::seconds(5);
                    break;
                }
                // Begun: its words, the members' gauge (20 minutes; the generals' 30), its time from now (the warnings at
                // 10 and 5 minutes left, the end at 20; the generals' clock from the first one's battle: 20 minutes, then
                // 5 twice).
                w.phase = QuestWorld::Phase::Fighting;
                if (!w.ends) {
                    all(spec.begun);
                    for (const std::uint32_t m : w.inside) SetGauge(m, w.quest, spec.minutes * 60, now);
                    w.ends = now + std::chrono::minutes(spec.minutes);
                }
                break;
            }
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(*w.ends - now).count();
            for (const auto& [minutes, text] : spec.warnings) {
                if (left <= std::int64_t(minutes) * 60 && w.warned.insert(minutes).second) all(text);
            }
            if (now >= *w.ends) {
                // Its time over: the unique's (the general's) words, it goes, everyone out 10 s later.
                speak(w.uniqueEntity, chain ? spec.generals[w.general].last : spec.over);
                host_.QuestDespawn(w.unique);
                w.phase = QuestWorld::Phase::Ending;
                w.next = now + std::chrono::seconds(10);
            }
            break;
        }
        case QuestWorld::Phase::Done:
            // The way out stands; the time's end: everyone left out 10 s later.
            if (w.ends && now >= *w.ends) {
                w.phase = QuestWorld::Phase::Ending;
                w.next = now + std::chrono::seconds(10);
            }
            break;
        case QuestWorld::Phase::Ending:
            if (now < w.next) break;
            for (const std::uint32_t m : w.inside) {
                SetGauge(m, w.quest, 0, now);
                host_.QuestWorldLeave(m);
            }
            w.inside.clear();
            break;
        }
    }
    std::erase_if(worlds_, [](const QuestWorld& w) { return w.inside.empty(); });
}

// --- A talk rule (QuestDef::UseRule::npc; the original's GENERALORDER_4 secret door) ------------------

const QuestDef::UseRule* Quests::TalkRule(const QuestDef& q, const QuestRecord& r, const std::string& npc) const {
    if (!UnderWay(r)) return nullptr;
    const std::uint8_t state = OriginalState(r, &q);
    for (const QuestDef::UseRule& u : q.onUse) {
        if (u.npc.empty() || u.npc != npc || !u.instance) continue;
        const bool fits = u.statuses.empty() ? (state == 1 || state == 7) : std::find(u.statuses.begin(), u.statuses.end(), state) != u.statuses.end();
        if (fits) return &u;
    }
    return nullptr;
}

// The party's master asked its question with its row (the original's talk: a member `master`, no party sharing both the
// quest's `not-party`, a transport out `transport`).
void Quests::AskTalkRule(std::uint32_t entity, const QuestHost::Player& who, const QuestDef& q, const QuestDef::UseRule& u, std::uint32_t npc) {
    const QuestDef::UseRule::World& w = *u.instance;
    const auto line = [&](const game::QuestText& t) {
        proto::QuestSays s{npc, q.id, {}, 0, proto::QuestChoice::None};
        s.text = t.key.empty() ? t.literal : t.key;
        Send(entity, s);
    };
    if (host_.QuestPartyHere(entity).empty() || !who.sharedParty) return Say(entity, npc, q.id, "not-party", proto::QuestChoice::None);
    if (!host_.QuestPartyMaster(entity)) return line(w.notMaster);
    if (host_.QuestTransportOut(entity)) return line(w.transport);
    proto::QuestSays s{npc, q.id, {}, 0, proto::QuestChoice::Rows};
    s.text = u.ask.key.empty() ? u.ask.literal : u.ask.key;
    s.rows = {u.row.key.empty() ? u.row.literal : u.row.key, "SN_TALK_COMMON_DENY"};
    Send(entity, s);
}

// The row picked: the party and the master asked again (the original's row: the quest's `not-party`, its `middle`), then
// the instance opened.
bool Quests::PickTalkRule(std::uint32_t entity, const QuestHost::Player& who, const QuestDef& q, const QuestDef::UseRule& u, std::uint32_t npc,
                          Clock::time_point now) {
    if (host_.QuestPartyHere(entity).empty() || !who.sharedParty) {
        Say(entity, npc, q.id, "not-party", proto::QuestChoice::None);
        return false;
    }
    if (!host_.QuestPartyMaster(entity)) {
        Say(entity, npc, q.id, "middle", proto::QuestChoice::None);
        return false;
    }
    if (std::any_of(worlds_.begin(), worlds_.end(), [&](const QuestWorld& w) { return w.quest == q.id && w.inside.contains(entity); })) return false;
    if (!TalkNeedHeld(entity, q, u)) return false; // silently (the original's master without the item: not taken)
    if (!OpenWorld(entity, who, q, u, now)) return false;
    core::LogInfo("quests: {} opens {} for {}", who.name, u.instance->world, q.codename);
    return true;
}

// The original's OLDWOMAN_6 move: each party member holding the quest, and, its kill goal not met, the Pile
// of Rainbow Plant; ours a talk rule's `need` items, one whose goals are met excused.
bool Quests::TalkNeedHeld(std::uint32_t entity, const QuestDef& q, const QuestDef::UseRule& u) {
    if (u.need.empty()) return true;
    const PlayerQuests* p = Of(entity);
    if (p == nullptr) return false;
    if (const auto rec = p->records.find(q.id); rec != p->records.end() && OriginalState(rec->second, &q) == 8) return true;
    return std::all_of(u.need.begin(), u.need.end(), [&](const std::string& item) { return host_.QuestItemCount(entity, item) > 0; });
}

// --- A quest's trial (QuestDef::Trial; the hwan army tests) ---------------------------------

namespace {

// A place moved by an offset (across regions in the field; a dungeon's own coordinates), in a layer.
proto::WorldPosition Around(const QuestDef::UseRule::Place& p, const game::SpawnOffset& o, std::uint32_t layer) {
    proto::WorldPosition at{p.region, p.x, p.y, p.z, layer};
    if ((at.region & 0x8000) != 0) {
        at.x += o.dx;
        at.z += o.dz;
        return at;
    }
    const proto::FlatPosition c = proto::ToFlat(at);
    at = proto::FromFlat({c.x + o.dx, c.z + o.dz}, p.y);
    at.layer = layer;
    return at;
}

} // namespace

bool Quests::EnterTrial(std::uint32_t entity, const QuestHost::Player& who, const QuestDef& q, Clock::time_point now) {
    const QuestDef::Trial& t = *q.trial;
    // The record under way (1/7/8), not teleporting, in the shared world (world 1), silently; then a trade transport out
    // or, for the tests of strength, a job's suit worn: SN_TALK_QNO_CH_HWAN_1_1_10 for all eight (the original's quirk).
    if (who.position.layer != 0 || who.dead) return false;
    if (std::any_of(trials_.begin(), trials_.end(), [&](const TrialRun& r) { return r.player == entity; })) return false;
    const auto refuse = [&] {
        if (!t.refuse.Empty()) host_.QuestSay(entity, t.refuse.key.empty() ? t.refuse.literal : t.refuse.key);
        return false;
    };
    if (host_.QuestTransportOut(entity)) return refuse();
    if (t.noJob && who.jobSuit) return refuse();
    // TeleportQuestArgs (its world, 720 s or 900 s, 60 s) into a new instance of the quest's world, at its start.
    const std::uint32_t layer = host_.QuestWorldOpen(entity, t.world);
    proto::WorldPosition start{t.start.region, t.start.x, t.start.y, t.start.z, 0};
    if (layer == 0 || !host_.QuestWorldEnterAt(entity, layer, start)) return false;
    TrialRun run;
    run.quest = q.id;
    run.player = entity;
    run.layer = layer;
    run.spec = &t;
    run.next = now + std::chrono::seconds(t.after);
    trials_.push_back(run);
    core::LogInfo("quests: {} enters {} for {}", who.name, t.world, q.codename);
    return true;
}

void Quests::TrialPassed(std::uint32_t entity, const QuestDef& q) {
    PlayerQuests* p = Of(entity);
    if (p == nullptr) return;
    const auto it = p->records.find(q.id);
    if (it == p->records.end() || !UnderWay(it->second)) return;
    // The quest's state 8: its goals met; ours its kill goals at their count (a scene's: its talks and deliveries too, its
    // person inside).
    const bool scene = q.trial && q.trial->boss.empty(); // a scene's or an escort's
    for (std::size_t i = 0; i < q.goals.size() && i < it->second.progress.size(); ++i) {
        const QuestGoalKind k = q.goals[i].kind;
        // A scene's delivery: its items handed over there (BAEKRYOUNG_13's letter to ShinMoo).
        if (scene && k == QuestGoalKind::Deliver && it->second.progress[i] < Need(q.goals[i]) && !q.goals[i].items.empty()) {
            host_.QuestTake(entity, q.goals[i].items);
            BagChanged(entity);
        }
        if (k == QuestGoalKind::Kill || (scene && (k == QuestGoalKind::Talk || k == QuestGoalKind::Deliver))) {
            it->second.progress[i] = std::uint16_t(Need(q.goals[i]));
        }
    }
    Evaluate(entity, q, it->second);
    Changed(entity, *p, q, it->second);
}

void Quests::TickTrials(Clock::time_point now) {
    for (TrialRun& run : trials_) {
        const QuestDef::Trial& t = *run.spec;
        const QuestDef* q = Find(run.quest);
        const auto say = [&](const game::QuestText& text) {
            if (!text.Empty()) host_.QuestSay(run.player, text.key.empty() ? text.literal : text.key);
        };
        const auto who = host_.QuestPlayer(run.player);
        const PlayerQuests* p = Of(run.player);
        const auto rec = p != nullptr ? p->records.find(run.quest) : decltype(p->records.end()){};
        const bool underWay = p != nullptr && rec != p->records.end() && UnderWay(rec->second);
        const auto close = [&] {
            if (run.boss != SIZE_MAX) host_.QuestDespawn(run.boss);
            run.boss = SIZE_MAX;
            if (who && who->position.layer == run.layer) host_.QuestWorldLeave(run.player);
            SetGauge(run.player, run.quest, 0, now);
            run.layer = 0;
        };
        // Gone by another way (out of the game, a return scroll, a resurrection in town), or its quest no longer under way
        // (given up: the tick's state 4 / 6 check, puts it out; nobody under way left, the world ends).
        if (!who || q == nullptr || !underWay || (run.phase != TrialRun::Phase::Waiting && who->position.layer != run.layer)) {
            close();
            continue;
        }
        if (who->position.layer != run.layer) continue; // on its way in
        switch (run.phase) {
        case TrialRun::Phase::Waiting: {
            if (t.scene.empty() && !t.escort.empty()) {
                // An escort from the start (BAEKRYOUNG_16: WalYoung beside the player at once).
                run.phase = TrialRun::Phase::Scene;
                run.next = now;
                break;
            }
            if (!t.scene.empty()) {
                // A scene (CQNO_TQ_BAEKRYOUNG_10's update: step 0 the script played, step 1 until 39 s have passed, step 2
                // the talk with its NPC done for everyone inside and the instance closed).
                host_.QuestScene(run.player, t.scene);
                run.phase = TrialRun::Phase::Scene;
                run.next = now + std::chrono::seconds(t.sceneSeconds);
                break;
            }
            if (now < run.next) break;
            // Step 1 (Elapsed 10000 / 12000 ms): the boss at its place within its radius, the gauge, the soldiers from now on.
            const auto nest = host_.QuestSpawn(run.player, t.boss, Around(t.bossAt, game::QuestSpawnOffset(t.near, t.far, random_), run.layer));
            if (!nest) break; // its ground not in yet: the next second
            run.boss = *nest;
            run.phase = TrialRun::Phase::Fighting;
            run.ends = now + std::chrono::minutes(t.minutes);
            run.nextAdd = now;
            SetGauge(run.player, run.quest, t.minutes * 60, now);
            break;
        }
        case TrialRun::Phase::Fighting: {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(run.ends - now).count();
            const bool firstHalf = t.warnings.empty() || left > std::int64_t(t.warnings.front().first) * 60;
            // The adds (step 2:, every `every` seconds until the first warning).
            if (t.adds && firstHalf && now >= run.nextAdd) {
                host_.QuestSpawn(run.player, t.adds->monster, Around(t.adds->at, game::QuestSpawnOffset(t.adds->near, t.adds->far, random_), run.layer));
                run.nextAdd = now + std::chrono::seconds(t.adds->every);
            }
            for (const auto& [minutes, text] : t.warnings) {
                if (left <= std::int64_t(minutes) * 60 && run.warned.insert(minutes).second) {
                    say(text);
                    SetGauge(run.player, run.quest, std::uint32_t(std::max<std::int64_t>(0, left)), now);
                }
            }
            if (host_.QuestSpawnStanding(run.boss) == 0) {
                // Slain (step 3 at once; in berserk the charms taken
                // and the piece given, else _09; out 10 s later).
                run.boss = SIZE_MAX;
                if (t.berserk && !who->berserk) {
                    say(t.failed);
                } else {
                    if (t.berserk && !t.take.empty()) {
                        if (const std::uint32_t n = host_.QuestItemCount(run.player, t.take); n > 0 && host_.QuestTake(run.player, {{t.take, n}}) &&
                                                                                                 !t.give.empty()) {
                            host_.QuestGive(run.player, {{t.give, 1}});
                        }
                        BagChanged(run.player);
                    }
                    if (!t.berserk) {
                        say(t.passed);
                        TrialPassed(run.player, *q);
                    }
                }
                SetGauge(run.player, run.quest, 0, now);
                run.phase = TrialRun::Phase::Ending;
                run.next = now + std::chrono::seconds(t.out);
                break;
            }
            if (!t.lost.Empty() && who->dead) {
                // Everyone dead before step 3: the boss taken away without dying, its words, out 15 s later.
                host_.QuestDespawn(run.boss);
                run.boss = SIZE_MAX;
                say(t.lost);
                SetGauge(run.player, run.quest, 0, now);
                run.phase = TrialRun::Phase::Ending;
                run.next = now + std::chrono::seconds(15);
                break;
            }
            if (now >= run.ends) {
                // Its time over (step 3 / 4): survived, passed; a ritual's end, out without a word.
                if (t.passOnTime) {
                    say(t.passed);
                    TrialPassed(run.player, *q);
                }
                SetGauge(run.player, run.quest, 0, now);
                run.phase = TrialRun::Phase::Ending;
                run.next = now;
            }
            break;
        }
        case TrialRun::Phase::Scene:
            if (now < run.next) break;
            if (!t.escort.empty()) {
                // BAEKRYOUNG_14's step 2: the escorted COS beside the player, its words, the waves from a minute on, the time.
                run.cos = host_.QuestResummon(run.player, t.escort);
                for (const game::QuestText& text : t.escortSays) say(text);
                say(t.escortText);
                run.phase = TrialRun::Phase::Escort;
                run.ends = now + std::chrono::minutes(t.minutes);
                run.nextAdd = now + std::chrono::seconds(t.waves ? t.waves->every : 60);
                SetGauge(run.player, run.quest, t.minutes * 60, now);
                break;
            }
            TrialPassed(run.player, *q);
            if (t.finishHere) FinishTrial(run.player, *q);
            close();
            break;
        case TrialRun::Phase::Escort: {
            // Step 3: at one of its places (the player within its radius, 3D), the lines; the waves about the player; the
            // warnings; the time over, lost.
            const bool there = std::any_of(t.to.begin(), t.to.end(), [&](const QuestDef::UseRule::Place& p) {
                if (who->position.region != p.region) return false;
                const double dx = who->position.x - p.x, dy = who->position.y - p.y, dz = who->position.z - p.z;
                return std::sqrt(dx * dx + dy * dy + dz * dz) <= p.radius;
            });
            if (there) {
                SetGauge(run.player, run.quest, 0, now);
                run.phase = TrialRun::Phase::Arrived;
                run.line = 0;
                run.next = now;
                break;
            }
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(run.ends - now).count();
            for (const auto& [minutes, text] : t.warnings) {
                if (left <= std::int64_t(minutes) * 60 && run.warned.insert(minutes).second) say(text);
            }
            if (now >= run.ends) {
                say(t.lost);
                if (run.cos != 0) host_.QuestCosRemove(run.cos);
                run.cos = 0;
                close();
                LoseTrial(run.player, *q);
                break;
            }
            if (t.waves && now >= run.nextAdd) {
                for (std::uint32_t i = 0; i < t.waves->count; ++i) {
                    const QuestDef::UseRule::Place about{who->position.region, who->position.x, who->position.y, who->position.z, 0};
                    host_.QuestSpawn(run.player, t.waves->monster, Around(about, game::QuestSpawnOffset(t.waves->near, t.waves->far, random_), run.layer));
                }
                run.nextAdd = now + std::chrono::seconds(t.waves->every);
            }
            break;
        }
        case TrialRun::Phase::Arrived:
            // Step 4: the lines, each its time after the one before; then finished there and out.
            if (now < run.next) break;
            if (run.line < t.arrive.size()) {
                say(t.arrive[run.line].second);
                ++run.line;
                run.next = now + std::chrono::milliseconds(run.line < t.arrive.size() ? t.arrive[run.line].first : 2500);
                break;
            }
            // Its scene there (BAEKRYOUNG_16's 200 s: ShinMoo and WalYoung), once.
            if (!t.arriveScene.empty() && run.line == t.arrive.size()) {
                host_.QuestScene(run.player, t.arriveScene);
                ++run.line;
                run.next = now + std::chrono::seconds(t.arriveSceneSeconds);
                break;
            }
            if (run.cos != 0) host_.QuestCosRemove(run.cos);
            run.cos = 0;
            TrialPassed(run.player, *q);
            if (t.finishHere) FinishTrial(run.player, *q);
            close();
            break;
        case TrialRun::Phase::Ending:
            if (now >= run.next) close();
            break;
        }
    }
    std::erase_if(trials_, [](const TrialRun& r) { return r.layer == 0; });
    // A chain's next trial, once its player is back in the shared world (gone, or the quest no longer under way: dropped).
    for (auto it = pendingTrials_.begin(); it != pendingTrials_.end();) {
        const auto who = host_.QuestPlayer(it->first);
        const QuestDef* q = Find(it->second);
        const PlayerQuests* p = Of(it->first);
        const auto rec = p != nullptr ? p->records.find(it->second) : decltype(p->records.end()){};
        if (!who || q == nullptr || !q->trial || p == nullptr || rec == p->records.end() || !UnderWay(rec->second)) {
            it = pendingTrials_.erase(it);
            continue;
        }
        if (who->position.layer != 0 || who->dead) {
            ++it;
            continue;
        }
        EnterTrial(it->first, *who, *q, now);
        it = pendingTrials_.erase(it);
    }
}

void Quests::FinishTrial(std::uint32_t entity, const QuestDef& q) {
    PlayerQuests* p = Of(entity);
    if (p == nullptr) return;
    const auto it = p->records.find(q.id);
    if (it == p->records.end() || it->second.status != proto::QuestStatus::Achieved) return;
    // Paid there (no NPC), its words; then the chain's next one given and its trial entered once back outside.
    Pay(entity, *p, q, it->second, 0, {});
    if (!q.trial->finished.Empty()) host_.QuestSay(entity, q.trial->finished.key.empty() ? q.trial->finished.literal : q.trial->finished.key);
    if (q.trial->then.empty()) return;
    const QuestDef* next = Find(q.trial->then);
    if (next == nullptr) return;
    Start(entity, *p, *next, "the chain");
    if (next->trial) pendingTrials_.push_back({entity, next->id});
}

void Quests::LoseTrial(std::uint32_t entity, const QuestDef& q) {
    PlayerQuests* p = Of(entity);
    if (p == nullptr) return;
    Abandon(entity, q.id, proto::QuestEnd::Abandoned);
    // The chain begun again (the original's Leebaek: "start again from the To the Flame Sprite quest"): its first one's
    // record as never finished, offered again.
    if (q.trial->restart.empty()) return;
    const QuestDef* first = Find(q.trial->restart);
    if (first == nullptr) return;
    if (const auto it = p->records.find(first->id); it != p->records.end() && !UnderWay(it->second)) {
        QuestRecord& r = it->second;
        r.status = proto::QuestStatus::Abandoned;
        r.times = 0;
        r.progress = {};
        host_.QuestSave(p->character, r);
        TellMarks(entity, *p);
        Send(entity, proto::QuestGone{first->id, proto::QuestEnd::Abandoned, 0});
    }
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> Quests::Trials() const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    for (const TrialRun& r : trials_) out.push_back({r.player, r.layer});
    return out;
}

void Quests::SetGauge(std::uint32_t entity, std::uint32_t quest, std::uint32_t seconds, Clock::time_point now) {
    PlayerQuests* p = Of(entity);
    const QuestDef* q = Find(quest);
    if (p == nullptr || q == nullptr) return;
    if (seconds == 0 && p->gauges.erase(quest) == 0) return;
    if (seconds != 0) p->gauges[quest] = now + std::chrono::seconds(seconds);
    const auto it = p->records.find(quest);
    if (it != p->records.end()) Send(entity, proto::QuestChanged{StateOf(entity, *q, it->second)});
}

std::uint32_t Quests::Gauge(std::uint32_t entity, std::uint32_t quest) const {
    const PlayerQuests* p = Of(entity);
    if (p == nullptr) return 0;
    const auto it = p->gauges.find(quest);
    if (it == p->gauges.end()) return 0;
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(it->second - Clock::now()).count();
    return std::uint32_t(std::max<std::int64_t>(1, left));
}

std::vector<std::uint32_t> Quests::Worlds() const {
    std::vector<std::uint32_t> out;
    for (const QuestWorld& w : worlds_) out.push_back(w.layer);
    return out;
}

} // namespace sr::server
