#include "sr/server/Roc.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <random>

#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/Stats.h"
#include "sr/server/Instances.h"

namespace sr::server {

namespace proto = game::proto;

namespace {

// The Peak (`_RefGame_World` 40 INS_ROC_TOP).
constexpr std::uint32_t kPeakWorld = 40;
constexpr std::uint32_t kPeakMaxUsers = 150;
constexpr std::uint32_t kPeakDurationMs = 17'640'000;
constexpr std::uint16_t kPeakRegion = 26045; // 0x65BD
constexpr int kScheduleMeetRoc = 1;          // SCHEDULE_DEF_MEET_ROC

// The seal stones (the initial stage): characterdata, region, x, y, z.
struct Stone {
    std::uint32_t refObjId;
    std::uint16_t region;
    float x, y, z;
};
constexpr std::array<Stone, 8> kStones{{
    {24161, 26301, 1031.79f, 17.90f, 165.58f},  // MOB_RM_SEALSTONE: Devil Shaitan
    {24278, 26045, 467.65f, 8.04f, 1805.65f},   // _2 .. _8: a wing tribe knight each
    {24279, 26045, 171.01f, -0.95f, 1311.78f},
    {24280, 26045, 436.02f, -4.36f, 779.72f},
    {24281, 26045, 999.71f, -4.05f, 550.84f},
    {24282, 26045, 1586.43f, -13.06f, 728.75f},
    {24283, 26045, 1582.73f, 17.09f, 1795.80f},
    {24284, 26045, 1858.34f, 26.25f, 1241.77f},
}};
// The stones' followers (their 'ssou'), counted down from 8 (the count at the gate's opening).
constexpr std::uint32_t kShaitan = 24260, kKnight = 24285;
constexpr int kFollowers = 8;
// The Roc and its place (region 0x65BD, / DC / D8).
constexpr std::uint32_t kRoc = 3877;
constexpr proto::WorldPosition kRocAt{kPeakRegion, 1028.90f, -7.29f, 1769.52f};
// Its spirits, each kind at its own place, at most three kinds at once.
struct Spirit {
    std::uint32_t refObjId;
    float x, y, z;
};
constexpr std::array<Spirit, 4> kSpirits{{
    {24156, 1339.62f, -0.52f, 1498.44f}, // MOB_RM_SPIRITS
    {24157, 715.95f, -27.52f, 1101.13f}, // _CLON
    {24158, 714.96f, -23.86f, 1497.42f}, // _CLON2
    {24159, 1338.30f, -16.68f, 1101.78f}, // _CLON3
}};
constexpr int kSpiritKindsAtOnce = 3;
// The exit where the Roc fell (INS_QUEST_TELEPORT).
constexpr std::uint32_t kQuestTeleport = 19556;
constexpr const char* kBerserkSkill = "MSKILL_RM_ROC_ATTACK10";

// The times (ms; the step machine, the Roc's handlers).
constexpr std::int64_t kUnbrokenMs = 3'600'000;   // followers still standing an hour after the opening: everyone out
constexpr std::int64_t kBreakWaitMs = 600'000;    // the last follower dead -> the entrance
constexpr std::int64_t kSceneMs = 42'000;         // the entrance scene (0xA410)
constexpr std::int64_t kBerserkCastMs = 3'600'000; // after the Roc came: its berserk
constexpr std::int64_t kSpiritsMs = 300'000;      // its spirits, again and again
constexpr std::int64_t kEscapeMs = 12'600'000;    // it flies away
constexpr std::int64_t kAfterDeathMs = 1'800'000; // killed: everyone out
// The escape's warnings after it came and their minutes.
constexpr std::array<std::int64_t, 6> kEscapeWarnMs{10'801'000, 11'701'000, 12'001'000, 12'301'000, 12'421'000, 12'541'000};
constexpr std::array<std::uint8_t, 6> kEscapeWarnMinutes{30, 15, 10, 5, 3, 1};
// The berserk's countdown: its first notice 30 minutes after the Roc came, then
// the minutes {30, 15, 10, 5, 3, 1} and the start (0), each after the wait before the next; then "advanced" every 30
// minutes, the countdown's step 12 the last.
constexpr std::array<std::uint8_t, 7> kBerserkMinutes{30, 15, 10, 5, 3, 1, proto::RocNotice::kStarted};
constexpr std::array<std::int64_t, 7> kBerserkWaitMs{900'000, 300'000, 300'000, 120'000, 120'000, 60'000, 1'800'000};
constexpr std::int64_t kBerserkFirstMs = 1'800'000, kBerserkAgainMs = 1'800'000;
constexpr int kBerserkLastStep = 12;
// The berserk's changes (UIIT_MSG_STATE_MSKILL_RM_ROC_ATTACK10: attack, defence, hit rate and parry up 10 %; its 'mom'
// 600000, 10, 6, 1 is not read further: ours a percent added for the rest of its life).
constexpr float kBerserkPercent = 10.0f;

} // namespace

struct Roc::State {
    RocHost& host;
    Instances& instances;
    Monsters& monsters;
    const assets::CharacterTable& characters;
    const assets::SkillTable* skills;
    RocConfig config;
    std::mt19937 random;

    bool gateOpen = false;
    std::int64_t periodEnd = 0; // the schedule's (Unix s; 0: a developer's opening, shut by hand)
    std::int64_t lastOpened = 0; // the period opened last (its start)
    std::uint32_t layer = 0;
    Clock::time_point opened{};
    int step = 0;
    std::int64_t next = 0; // the step's time, from the opening (ms of the Peak's clock)
    int followers = kFollowers;
    std::vector<std::size_t> nests; // the stones', the Roc's, the spirits' (removed at the end)
    std::size_t rocNest = SIZE_MAX;
    std::int64_t rocCame = -1; // from the opening (-1: not yet)
    int crazyStep = 0;
    std::int64_t crazyNext = 0;
    int escapeStep = 0;
    bool berserkCast = false;
    std::int64_t spiritsNext = 0;
    std::array<std::size_t, 4> spiritNests{SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};

    State(RocHost& h, Instances& i, Monsters& m, const assets::CharacterTable& c, const assets::SkillTable* k, RocConfig cfg, std::uint32_t seed)
        : host(h), instances(i), monsters(m), characters(c), skills(k), config(std::move(cfg)), random(seed) {}

    // The Peak's clock: ms since the opening, sped up for tries.
    std::int64_t Elapsed(Clock::time_point now) const {
        const double ms = std::chrono::duration<double, std::milli>(now - opened).count();
        return std::int64_t(ms * std::max(0.001, config.timeScale));
    }
    proto::WorldPosition InPeak(proto::WorldPosition p) const {
        p.layer = layer;
        return p;
    }

    template <typename T>
    void ToLayer(const T& message) {
        const std::vector<std::byte> bytes = proto::Encode(message);
        for (const std::uint32_t entity : instances.PlayersIn(layer)) host.RocSend(entity, T::kId, bytes);
    }
    void LayerText(const char* key) {
        proto::InstanceNotice n{key};
        n.system = true;
        n.warning = true;
        ToLayer(n);
    }

    std::size_t AddMonster(std::uint32_t refObjId, const proto::WorldPosition& at, bool aggressive) {
        if (characters.Find(refObjId) == nullptr) {
            core::LogWarning("roc: characterdata has no {}", refObjId);
            return SIZE_MAX;
        }
        MonsterNest n;
        n.refObjId = refObjId;
        n.position = InPeak(at);
        n.maxCount = 1;
        n.respawn = false;
        n.radius = 500.0f;
        n.aggressive = aggressive;
        n.sight = aggressive ? 300.0f : 0.0f;
        const std::size_t index = monsters.AddNest(n);
        if (index != SIZE_MAX) nests.push_back(index);
        return index;
    }

    bool OpenGate(Clock::time_point now, std::int64_t until) {
        if (gateOpen) return false;
        // One Peak at a time: an older one still standing is closed first.
        if (layer != 0) EndPeak("a new opening");
        GameWorldSpec spec;
        spec.id = kPeakWorld;
        spec.codename = "INS_ROC_TOP";
        spec.maxCount = 1;
        spec.maxUsers = kPeakMaxUsers;
        spec.durationMs = std::uint32_t(double(kPeakDurationMs) / std::max(0.001, config.timeScale));
        spec.emptyMs = spec.durationMs;
        spec.gapMs = 0;
        spec.noJobSuit = true;
        spec.start = {kPeakRegion, 1022.0f, -37.0f, 796.0f}; // _RefInstance_World_Start_Pos
        layer = instances.OpenWorld(spec, "the Roc's gate");
        if (layer == 0) {
            core::LogWarning("roc: no layer for the Peak");
            return false;
        }
        opened = now;
        gateOpen = true;
        periodEnd = until;
        step = 0;
        next = 0;
        followers = kFollowers;
        rocNest = SIZE_MAX;
        rocCame = -1;
        crazyStep = 0;
        escapeStep = 0;
        berserkCast = false;
        spiritNests.fill(SIZE_MAX);
        for (const Stone& s : kStones) AddMonster(s.refObjId, {s.region, s.x, s.y, s.z}, false);
        core::LogInfo("roc: the gate is open (the Peak's layer {}, {} seal stones)", layer, nests.size());
        host.RocShardNotice("UIIT_MSG_QUEST_ROC_APPEAR");
        return true;
    }

    void ShutGate() {
        if (!gateOpen) return;
        gateOpen = false;
        periodEnd = 0;
        core::LogInfo("roc: the gate is shut");
        host.RocShardNotice("UIIT_MSG_QUEST_ROC_DISAPPEAR");
    }

    // Everyone on the Peak to the mountain's gate.
    void Expel(const char* why) {
        const auto to = host.RocPoint(kMountainGate);
        const std::vector<std::uint32_t> inside = instances.PlayersIn(layer);
        core::LogInfo("roc: {} player(s) sent off the Peak ({})", inside.size(), why);
        for (const std::uint32_t entity : inside) instances.ExitSiege(entity, to ? &*to : nullptr);
    }

    void EndPeak(const char* why) {
        if (layer == 0) return;
        ShutGate();
        Expel(why);
        for (const std::size_t n : nests) monsters.RemoveNest(n);
        nests.clear();
        instances.CloseWorld(layer);
        core::LogInfo("roc: the Peak (layer {}) ends: {}", layer, why);
        layer = 0;
        step = 6;
    }

    void RaiseRoc(std::int64_t elapsed) {
        rocNest = AddMonster(kRoc, kRocAt, true);
        rocCame = elapsed;
        crazyStep = 0;
        crazyNext = elapsed + kBerserkFirstMs;
        escapeStep = 0;
        berserkCast = false;
        spiritsNext = elapsed + kSpiritsMs;
        core::LogInfo("roc: the Roc comes");
    }

    std::uint32_t RocMonster() const {
        if (rocNest == SIZE_MAX) return 0;
        const auto standing = monsters.NestMonsters(rocNest);
        return standing.empty() ? 0 : standing.front();
    }

    bool SpiritStands(std::size_t kind) const {
        if (spiritNests[kind] == SIZE_MAX || monsters.NestSpec(spiritNests[kind]) == nullptr) return false;
        const auto [alive, toCome] = monsters.NestCounts(spiritNests[kind]);
        return alive + toCome > 0;
    }

    // The spirits topped up to three kinds, the missing ones picked at random.
    void RaiseSpirits() {
        std::vector<std::size_t> missing;
        int standing = 0;
        for (std::size_t k = 0; k < kSpirits.size(); ++k) {
            if (SpiritStands(k)) ++standing;
            else missing.push_back(k);
        }
        std::shuffle(missing.begin(), missing.end(), random);
        for (std::size_t i = 0; standing < kSpiritKindsAtOnce && i < missing.size(); ++i, ++standing) {
            const Spirit& s = kSpirits[missing[i]];
            spiritNests[missing[i]] = AddMonster(s.refObjId, {kPeakRegion, s.x, s.y, s.z}, true);
        }
    }

    void CastBerserk(Clock::time_point now) {
        const std::uint32_t roc = RocMonster();
        const assets::SkillRow* skill = skills != nullptr ? skills->Find(std::string_view(kBerserkSkill)) : nullptr;
        if (roc == 0) return;
        std::vector<game::StatChange> changes;
        for (const std::uint16_t stat : {game::stat::kPhyAttackMin, game::stat::kPhyAttackMax, game::stat::kMagAttackMin, game::stat::kMagAttackMax,
                                         game::stat::kPhyDefence, game::stat::kMagDefence, game::stat::kHitRate, game::stat::kParry}) {
            changes.push_back({stat, 1, kBerserkPercent});
        }
        monsters.Boost(roc, skill, std::move(changes), std::chrono::hours(6), now);
        core::LogInfo("roc: the Roc goes berserk");
    }

    void TickPeak(Clock::time_point now) {
        if (layer == 0) return;
        if (!instances.Stands(layer)) { // its time over (Instances ended it)
            for (const std::size_t n : nests) monsters.RemoveNest(n);
            nests.clear();
            layer = 0;
            step = 6;
            gateOpen = false;
            return;
        }
        const std::int64_t elapsed = Elapsed(now);
        // The followers still standing an hour on.
        if (step == 0 && followers > 0 && elapsed >= kUnbrokenMs) return EndPeak("the seal stones' followers stood an hour");
        switch (step) {
        case 1:
            if (elapsed < next) break;
            ToLayer(proto::QuestScene{"roc_entrance"});
            // The scene plays its 42 s on the clients whatever the tries' speed: the Roc comes after it.
            next += config.realSceneTime ? std::int64_t(double(kSceneMs) * std::max(1.0, config.timeScale)) : kSceneMs;
            step = 2;
            ShutGate(); // the entrance shuts the gate
            break;
        case 2:
            if (elapsed < next) break;
            RaiseRoc(elapsed);
            step = 3;
            break;
        case 3:
        case 4:
            if (step == 3 && elapsed >= crazyNext) {
                if (crazyStep < int(kBerserkMinutes.size())) {
                    ToLayer(proto::RocNotice{proto::RocNotice::kBerserk, kBerserkMinutes[std::size_t(crazyStep)]});
                    crazyNext += kBerserkWaitMs[std::size_t(crazyStep)];
                } else {
                    ToLayer(proto::RocNotice{proto::RocNotice::kBerserk, proto::RocNotice::kAdvanced});
                    crazyNext += kBerserkAgainMs;
                }
                if (++crazyStep >= kBerserkLastStep) step = 4;
            }
            if (rocCame >= 0) {
                const std::int64_t since = elapsed - rocCame;
                if (!berserkCast && since >= kBerserkCastMs) {
                    berserkCast = true;
                    CastBerserk(now);
                }
                if (elapsed >= spiritsNext) {
                    spiritsNext += kSpiritsMs;
                    RaiseSpirits();
                }
                while (escapeStep < int(kEscapeWarnMs.size()) && since >= kEscapeWarnMs[std::size_t(escapeStep)]) {
                    ToLayer(proto::RocNotice{proto::RocNotice::kEscape, kEscapeWarnMinutes[std::size_t(escapeStep)]});
                    ++escapeStep;
                }
                if (since >= kEscapeMs) return EndPeak("the Roc flew away");
            }
            break;
        case 5:
            if (elapsed >= next) return EndPeak("the Roc's time after its death");
            break;
        default: break;
        }
    }
};

Roc::Roc(RocHost& host, Instances& instances, Monsters& monsters, const assets::CharacterTable& characters, const assets::SkillTable* skills,
         RocConfig config, std::uint32_t seed)
    : s_(std::make_unique<State>(host, instances, monsters, characters, skills, std::move(config), seed)) {}

Roc::~Roc() = default;

void Roc::Tick(Clock::time_point now) {
    State& s = *s_;
    if (s.config.enabled) {
        const std::int64_t unix = std::int64_t(std::time(nullptr));
        std::optional<game::Period> period;
        if (s.config.cycleOpen > 0 && s.config.cyclePeriod > 0) {
            const std::int64_t base = unix - unix % s.config.cyclePeriod;
            period = game::Period{base, base + s.config.cycleOpen};
        } else {
            period = game::SchedulePeriod(s.config.schedule, kScheduleMeetRoc, unix);
        }
        if (period && period->Contains(unix) && period->start != s.lastOpened && !s.gateOpen) {
            s.lastOpened = period->start;
            s.OpenGate(now, period->end);
        }
        if (s.gateOpen && s.periodEnd != 0 && unix >= s.periodEnd) s.ShutGate();
    }
    s.TickPeak(now);
}

bool Roc::Open(Clock::time_point now) { return s_->OpenGate(now, 0); }
void Roc::Shut() { s_->ShutGate(); }
void Roc::End() { s_->EndPeak("a developer's end"); }
void Roc::Break(Clock::time_point now) {
    State& s = *s_;
    if (s.layer == 0 || s.step != 0) return;
    s.followers = 1;
    Monsters::Damaged last;
    last.killed = true;
    last.refObjId = kKnight;
    proto::WorldPosition at;
    at.layer = s.layer;
    Killed(last, at, now);
}

void Roc::Killed(const Monsters::Damaged& killed, const proto::WorldPosition& at, Clock::time_point now) {
    State& s = *s_;
    if (s.layer == 0 || at.layer != s.layer) return;
    const std::int64_t elapsed = s.Elapsed(now);
    if ((killed.refObjId == kShaitan || killed.refObjId == kKnight) && s.step == 0 && s.followers > 0) {
        // one fewer; the last: the stones broken, the Roc in 10 minutes.
        if (--s.followers > 0) return;
        s.LayerText("UIIT_MSG_QUEST_ROC_BREAK_STONE");
        s.next = elapsed + kBreakWaitMs;
        s.step = 1;
        core::LogInfo("roc: the seal stones' followers are dead; the Roc in 10 minutes");
        return;
    }
    if (killed.refObjId == kRoc && s.rocCame >= 0) {
        // an exit where it fell; everyone out 30 minutes on. (The original credits every player of the Peak
        // with the kill for the quests: not built yet)
        s.instances.AddExit(s.layer, kQuestTeleport, at);
        for (const std::uint32_t player : s.instances.PlayersIn(s.layer)) s.host.RocDeed(player, "roc"); // the beta quests' degree 9
        s.rocCame = -1;
        s.rocNest = SIZE_MAX;
        s.next = elapsed + kAfterDeathMs;
        s.step = 5;
        core::LogInfo("roc: the Roc is dead");
    }
}

bool Roc::Admit(std::uint32_t entity) {
    State& s = *s_;
    if (!s.gateOpen || s.layer == 0) return false;
    const auto at = s.host.RocPoint(kPeakGate);
    return at && s.instances.AdmitAt(entity, s.layer, s.InPeak(*at));
}

bool Roc::GateOpen() const { return s_->gateOpen; }
std::uint32_t Roc::Layer() const { return s_->layer; }
int Roc::Step() const { return s_->step; }
int Roc::FollowersLeft() const { return s_->followers; }
std::uint32_t Roc::RocEntity() const { return s_->RocMonster(); }
int Roc::SpiritKinds() const {
    int n = 0;
    for (std::size_t k = 0; k < kSpirits.size(); ++k) n += s_->SpiritStands(k) ? 1 : 0;
    return n;
}

} // namespace sr::server
