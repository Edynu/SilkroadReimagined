#include <doctest/doctest.h>

// The fortress war: the original's rules as game/Fortress.h keeps them, the schedule, the objectstring.ifo places over
// the game files, and the
// war against the game server in-process: applying at the official, the war, the guard tower's seal,
// the heart's capture, the owner kept in the original's tables across a restart.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Model.h"
#include "sr/assets/Terrain.h"
#include "sr/assets/WorldMap.h"
#include "sr/core/Jobs.h"
#include "sr/game/Fortress.h"
#include "sr/game/Guild.h"
#include "sr/game/GuildProtocol.h"
#include "sr/game/Items.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/Fortress.h"
#include "sr/server/GameServer.h"
#include "sr/server/Import.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
using game::FortressError;
using Side = proto::FortressSide;

namespace {

std::filesystem::path FortressGameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir ? std::filesystem::path(dir) : std::filesystem::path();
}

game::FortressApplyFacts Eligible() {
    game::FortressApplyFacts f;
    f.requestsOpen = true;
    f.fortressKnown = true;
    f.atOfficial = true;
    f.guild = 7;
    f.master = true;
    f.guildLevel = 3;
    f.members = 7;
    f.gold = 1'000'000;
    f.fee = 1'000'000;
    return f;
}

std::int64_t LocalAt(int y, int mo, int d, int h, int mi) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_isdst = -1;
    return std::int64_t(std::mktime(&t));
}

} // namespace

TEST_CASE("Fortress war: the original's rules (applying, cancelling, who may hit what, the heart's taker)") {
    // Applying, in the original's order.
    CHECK(game::ApplyRefusal(Eligible()) == FortressError::None);
    auto f = Eligible();
    f.requestsOpen = false;
    f.guild = 0; // the period comes first
    CHECK(game::ApplyRefusal(f) == FortressError::NotApplyPeriod);
    f = Eligible();
    f.fortressKnown = false;
    CHECK(game::ApplyRefusal(f) == FortressError::NoFortress);
    f = Eligible();
    f.atOfficial = false;
    CHECK(game::ApplyRefusal(f) == FortressError::WrongNpc);
    f = Eligible();
    f.guild = 0;
    CHECK(game::ApplyRefusal(f) == FortressError::NotInGuild);
    f = Eligible();
    f.guildHoldsFortress = true;
    CHECK(game::ApplyRefusal(f) == FortressError::OwnsFortress);
    f = Eligible();
    f.side = Side::Defend; // only the owner's union defends
    CHECK(game::ApplyRefusal(f) == FortressError::WrongAlliance);
    f.alliedWithOwner = true;
    CHECK(game::ApplyRefusal(f) == FortressError::None);
    f.side = Side::Attack; // and it cannot attack
    CHECK(game::ApplyRefusal(f) == FortressError::WrongAlliance);
    f = Eligible();
    f.master = false;
    f.guildLevel = 1; // the master before the level
    CHECK(game::ApplyRefusal(f) == FortressError::OnlyMaster);
    f = Eligible();
    f.guildLevel = 2;
    CHECK(game::ApplyRefusal(f) == FortressError::GuildLevel);
    f = Eligible();
    f.members = 6;
    CHECK(game::ApplyRefusal(f) == FortressError::GuildMembers);
    f = Eligible();
    f.gold = 999'999;
    CHECK(game::ApplyRefusal(f) == FortressError::Gold);
    f.side = Side::Defend; // a defence is free
    f.alliedWithOwner = true;
    CHECK(game::ApplyRefusal(f) == FortressError::None);
    f = Eligible();
    f.applied = true;
    CHECK(game::ApplyRefusal(f) == FortressError::AlreadyApplied);

    // Cancelling.
    game::FortressCancelFacts c{true, true, true, 7, true, false, 1, 1};
    CHECK(game::CancelRefusal(c) == FortressError::None);
    c.atOfficial = false;
    CHECK(game::CancelRefusal(c) == FortressError::WrongNpcCancel);
    c.atOfficial = true;
    c.master = false;
    CHECK(game::CancelRefusal(c) == FortressError::OnlyMaster);
    c.master = true;
    c.appliedFortress = 0;
    CHECK(game::CancelRefusal(c) == FortressError::NoApplication);
    c.appliedFortress = 3;
    CHECK(game::CancelRefusal(c) == FortressError::NoFortress);

    // Structures: only in the war; never the owner's guild or union; an unowned one anyone's.
    CHECK(game::StructureRefusal(false, {5, 0}, {7, 0}) == FortressError::StructureOutOfWar);
    CHECK(game::StructureRefusal(true, {0, 0}, {7, 0}) == FortressError::None);
    CHECK(game::StructureRefusal(true, {5, 9}, {5, 9}) == FortressError::StructureOwnGuild);
    CHECK(game::StructureRefusal(true, {5, 9}, {6, 9}) == FortressError::StructureOwnUnion);
    CHECK(game::StructureRefusal(true, {5, 0}, {6, 0}) == FortressError::None);
    // Players: not of one union, not of one guild, the war on.
    CHECK(game::PlayersHostileInWar(true, {5, 0}, {6, 0}));
    CHECK_FALSE(game::PlayersHostileInWar(false, {5, 0}, {6, 0}));
    CHECK_FALSE(game::PlayersHostileInWar(true, {5, 9}, {6, 9}));
    CHECK_FALSE(game::PlayersHostileInWar(true, {5, 0}, {5, 0}));
    CHECK_FALSE(game::PlayersHostileInWar(true, {0, 0}, {0, 0}));
    CHECK(game::PlayersHostileInWar(true, {0, 0}, {6, 0}));
    // The heart's taker: the most damage, the first of equals.
    CHECK(game::TopDamager({}) == 0);
    CHECK(game::TopDamager({{3, 10}, {4, 30}, {5, 30}}) == 4);
    // Kinds by TID.
    CHECK(game::StructureKindOf(1, 2, 5, 1) == game::StructureKind::Heart);
    CHECK(game::StructureKindOf(1, 2, 5, 3) == game::StructureKind::Gate);
    CHECK(game::StructureKindOf(1, 2, 1, 1) == game::StructureKind::None);
    // The client's texts for the codes.
    CHECK(std::string(game::FortressErrorKey(0x280D)) == "UIIT_MSG_FORT_ETC_ERROR_TOOLESSGUILDMB");
    CHECK(std::string(game::FortressErrorKey(0x283A)).empty());
}

TEST_CASE("Fortress war: a weapon's kind and what it does to a structure") {
    // Followed through the binary's own arithmetic (some kinds multiply the same factor twice).
    CHECK(game::StructureWeaponOf(2).factor == doctest::Approx(0.30));
    CHECK(game::StructureWeaponOf(5).factor == doctest::Approx(0.4698));
    CHECK(game::StructureWeaponOf(7).factor == doctest::Approx(0.4356));
    CHECK(game::StructureWeaponOf(10).factor == doctest::Approx(0.2075));
    CHECK(game::StructureWeaponOf(11).factor == doctest::Approx(0.2075));
    CHECK(game::StructureWeaponOf(14).factor == doctest::Approx(0.25));
    CHECK(game::StructureWeaponOf(15).factor == doctest::Approx(0.2075));
    CHECK(game::StructureWeaponOf(16).factor == doctest::Approx(1.0));
    CHECK(game::StructureWeaponOf(10).magic);
    CHECK(game::StructureWeaponOf(11).magic);
    CHECK(game::StructureWeaponOf(14).magic);
    CHECK(game::StructureWeaponOf(15).magic);
    CHECK(!game::StructureWeaponOf(8).magic);
    CHECK(game::StructureWeaponOf(1).factor == 0.0); // no weapon: a hit of 1
}

TEST_CASE("Fortress war: the client knows which regions are at war (the enemy's red names)") {
    proto::FortressRecord peace;
    peace.id = 3;
    peace.regions = {300, 301};
    proto::FortressRecord war;
    war.id = 1;
    war.atWar = true;
    war.regions = {100, 101};
    proto::FortressList list;
    list.warOn = true;
    list.fortresses = {peace, war};
    proto::FortressList told;
    REQUIRE(proto::Decode(proto::Encode(list), told)); // the regions come over the wire
    game::FortressView view;
    view.Apply(proto::FortressEvent{told});
    CHECK(view.WarArea(101) == 1);
    CHECK(view.WarArea(301) == 0); // a fortress at peace
    CHECK(view.WarArea(999) == 0); // outside every area
}

TEST_CASE("Fortress war: the schedule's periods (`_Schedule` rows of SCHEDULE_DEF_SIEGE_*)") {
    // The export's: applications daily from 00:00 for 12 h, the war daily at 13:30 for 2 h.
    std::vector<game::ScheduleRow> rows;
    rows.push_back({game::kScheduleSiegeRequest, game::LocalSeconds("2008-01-01 00:00:00.000"), game::LocalSeconds("2040-01-01 00:00:00"), 1, 0, 0, 0, 0, 43200});
    rows.push_back({game::kScheduleSiegeWar, game::LocalSeconds("2008-01-01 00:00:00"), game::LocalSeconds("2040-01-01 00:00:00"), 1, 0, 13, 30, 0, 7200});
    const std::int64_t morning = LocalAt(2026, 11, 8, 9, 0);
    auto requests = game::SchedulePeriod(rows, game::kScheduleSiegeRequest, morning);
    REQUIRE(requests);
    CHECK(requests->Contains(morning));
    CHECK(requests->start == LocalAt(2026, 11, 8, 0, 0));
    auto war = game::SchedulePeriod(rows, game::kScheduleSiegeWar, morning);
    REQUIRE(war);
    CHECK_FALSE(war->Contains(morning));
    CHECK(war->start == LocalAt(2026, 11, 8, 13, 30));
    CHECK(war->end - war->start == 7200);
    CHECK(game::SchedulePeriod(rows, game::kScheduleSiegeWar, LocalAt(2026, 11, 8, 14, 0))->Contains(LocalAt(2026, 11, 8, 14, 0)));
    CHECK(game::SchedulePeriod(rows, game::kScheduleSiegeWar, LocalAt(2026, 11, 8, 16, 0))->start == LocalAt(2026, 11, 9, 13, 30));
    // Expired rows (the export's end in 2020) give nothing.
    rows[1].dateEnd = game::LocalSeconds("2020-01-01 00:00:00");
    CHECK_FALSE(game::SchedulePeriod(rows, game::kScheduleSiegeWar, morning));
    // Weekly (3): 1 Sunday .. 7 Saturday; 2026-11-08 is a Sunday.
    rows[1] = {game::kScheduleSiegeWar, 0, 0, 3, 7, 20, 0, 0, 3600};
    war = game::SchedulePeriod(rows, game::kScheduleSiegeWar, morning);
    REQUIRE(war);
    CHECK(war->start == LocalAt(2026, 11, 14, 20, 0));
}

namespace {
// A fortress war without a game server: what it says to everyone is kept.
struct QuietHost : server::FortressHost {
    std::vector<proto::FortressNotice> notices;
    void FortressSend(std::uint64_t, proto::MessageId, const std::vector<std::byte>&) override {}
    void FortressBroadcast(proto::MessageId id, const std::vector<std::byte>& payload) override {
        proto::FortressNotice n;
        if (id == proto::FortressNotice::kId && proto::Decode(payload, n)) notices.push_back(n);
    }
    std::optional<server::FortressGuild> FortressGuildOf(std::int64_t) const override { return std::nullopt; }
    std::optional<server::FortressGuild> FortressGuildById(std::uint32_t) const override { return std::nullopt; }
    std::string FortressNpc(std::uint32_t) const override { return {}; }
    double FortressDistance(std::uint32_t, std::uint32_t) const override { return -1.0; }
    bool FortressTakeGold(std::uint64_t, std::uint64_t) override { return false; }
    std::uint32_t SpawnStructure(const server::FortressSpec::Structure&, std::uint32_t, std::uint32_t, std::uint32_t, bool) override { return 0; }
    void RemoveStructure(std::uint32_t) override {}
    void StructureAppearance(std::uint32_t, proto::MessageId, std::vector<std::byte>) override {}
    void FortressSubmit(std::function<void(server::Database&)>, std::function<void()> done) override { done(); }
};
} // namespace

TEST_CASE("Fortress war: the war 30 minutes into its period, the original's warnings by their kinds") {
    using K = proto::FortressNoticeKind;
    std::vector<game::ScheduleRow> rows;
    rows.push_back({game::kScheduleSiegeWar, game::LocalSeconds("2008-01-01 00:00:00"), game::LocalSeconds("2040-01-01 00:00:00"), 1, 0, 13, 30, 0, 7200});
    server::FortressConfig config;
    config.schedule = rows;
    QuietHost host;
    server::Fortresses forts(host, {}, config);
    const std::int64_t start = LocalAt(2026, 11, 8, 13, 30), end = start + 7200;
    const auto at = [&](std::int64_t t) {
        host.notices.clear();
        forts.Tick(server::Fortresses::Clock::now(), t);
        std::vector<K> kinds;
        for (const proto::FortressNotice& n : host.notices) kinds.push_back(n.kind);
        return kinds;
    };
    CHECK(at(start - 60).empty());
    // The period's start: "30 minutes before the war" (kind 1), no war yet.
    CHECK(at(start) == std::vector<K>{K::WarSoon});
    CHECK_FALSE(forts.WarOn());
    CHECK(at(start + 1799).empty());
    // 30 minutes in: the war.
    CHECK(at(start + 1800) == std::vector<K>{K::WarBegin});
    CHECK(forts.WarOn());
    CHECK(at(end - 1800) == std::vector<K>{K::WarEnd30});
    CHECK(at(end - 1200) == std::vector<K>{K::WarEnd20});
    CHECK(at(end - 600) == std::vector<K>{K::WarEnd10});
    CHECK(at(end - 60) == std::vector<K>{K::WarEnd1});
    CHECK(at(end) == std::vector<K>{K::WarEnd});
    CHECK_FALSE(forts.WarOn());
    // The client's fixed minutes for each kind.
    CHECK(proto::WarningMinutes(K::WarSoon) == 30);
    CHECK(proto::WarningMinutes(proto::WarEndWarning(20)) == 20);
    CHECK(proto::WarningMinutes(proto::WarEndWarning(1)) == 1);
}

TEST_CASE("Fortress war: a structure is reached at its navigation object's outline (game files)") {
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    for (const char* code : {"STRUCTURE_GUARD_TOWER_01", "STRUCTURE_BIG_GATE_JA_01", "STRUCTURE_FORT_STONE_JA_01"}) {
        CAPTURE(code);
        const assets::ObjectRow* row = characters.Find(std::string_view(code));
        REQUIRE(row != nullptr);
        const std::string path = assets::CharacterTable::ResolveModel(fs, *row);
        assets::ObjectNavMesh nav;
        REQUIRE_MESSAGE(assets::LoadObjectNavMesh(fs, path, nav, error), error);
        std::vector<std::array<float, 6>> outline;
        for (const auto& e : nav.outline) {
            outline.push_back({nav.vertices[e.a][0], nav.vertices[e.a][1], nav.vertices[e.a][2], nav.vertices[e.b][0], nav.vertices[e.b][1], nav.vertices[e.b][2]});
        }
        REQUIRE_FALSE(outline.empty());
        // From 400 out on each side the contact lies on an outline edge, nearer than the centre.
        for (const auto& [px, pz] : std::vector<std::pair<double, double>>{{400, 0}, {-400, 0}, {0, 400}, {0, -400}, {300, 300}}) {
            const std::array<double, 3> c = server::OutlineContact(outline, px, pz);
            CHECK(std::hypot(px - c[0], pz - c[2]) < std::hypot(px, pz));
            double onEdge = 1e9;
            for (const auto& e : outline) {
                const double ex = double(e[3]) - e[0], ez = double(e[5]) - e[2], len2 = ex * ex + ez * ez;
                const double t = len2 > 0 ? std::clamp(((c[0] - e[0]) * ex + (c[2] - e[2]) * ez) / len2, 0.0, 1.0) : 0.0;
                onEdge = std::min(onEdge, std::hypot(c[0] - (e[0] + t * ex), c[2] - (e[2] + t * ez)));
            }
            CHECK(onEdge < 0.01);
        }
    }
}

TEST_CASE("Fortress war: navmesh\\objectstring.ifo's structure places (game files)") {
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    const auto file = fs.Read("navmesh\\objectstring.ifo");
    REQUIRE(file.has_value());
    std::vector<assets::NamedSpot> spots;
    REQUIRE_MESSAGE(assets::DecodeObjectStrings(file->Bytes(), spots, error), error);
    CHECK(spots.size() == 189);
    const auto find = [&](const char* name) {
        const auto it = std::find_if(spots.begin(), spots.end(), [&](const assets::NamedSpot& s) { return s.name == name; });
        return it != spots.end() ? &*it : nullptr;
    };
    // Jangan's heart and its first gate (_RefEventZone 84, 88).
    const assets::NamedSpot* heart = find("STRUCTURE_POS_JA_FORT_STONE_01");
    REQUIRE(heart != nullptr);
    CHECK(heart->region == assets::RegionId(71, 70));
    CHECK(heart->x == doctest::Approx(849.18).epsilon(0.001));
    CHECK(heart->z == doctest::Approx(1065.28).epsilon(0.001));
    const assets::NamedSpot* gate = find("STRUCTURE_POS_JA_GATE_02");
    REQUIRE(gate != nullptr);
    CHECK(gate->yaw == doctest::Approx(-1.5708).epsilon(0.001));
    // Every structure place of the three fortresses in service.
    int ja = 0, ht = 0, bj = 0;
    for (const assets::NamedSpot& s : spots) {
        if (s.name.starts_with("STRUCTURE_POS_JA_")) ++ja;
        if (s.name.starts_with("STRUCTURE_POS_HT_")) ++ht;
        if (s.name.starts_with("STRUCTURE_POS_BJ_")) ++bj;
    }
    CHECK(ja > 0);
    CHECK(ht > 0);
    CHECK(bj > 0);
}

namespace {

constexpr std::uint32_t kOfficial = 19498; // NPC_CH_FORTRESS_OFFICIAL
constexpr std::uint32_t kManager = 19499;  // NPC_CH_FORTRESS_MANAGER1
constexpr std::uint32_t kPulley = 19551;   // STRUCTURE_GATE_PULLEY_JA_01
constexpr std::uint32_t kAide = 19500, kSmith = 19501, kTrainer = 19502; // NPC_CH_FORTRESS_BATTLEAIDE1, _SMITH1, _TRAINER1
constexpr std::uint32_t kHeart = 19538, kTower = 19521, kGate = 19504; // Jangan's first-level structures
const proto::WorldPosition kStart{assets::RegionId(168, 98), 960.42f, 50.0f, 458.26f};

std::shared_ptr<game::CharacterRules> FortressRules() {
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
    rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
    return rules;
}

struct Player {
    std::unique_ptr<Client> c;
    std::string name;
    std::int64_t character = 0;
    std::uint32_t entity = 0;
    std::uint32_t official = 0;
    std::uint32_t manager = 0;
    std::uint32_t pulley = 0;
    std::uint32_t aide = 0, smith = 0, trainer = 0;
};

// A game server with Jangan's fortress laid out beside the Chinese start place (its official, its
// heart, one guard tower and one gate), structures of 3 health.
struct Rig {
    TestDatabase& t;
    core::JobSystem jobs{2};
    std::unique_ptr<server::GameServer> game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::function<void()> pump;
    std::string error;

    Rig(TestDatabase& db, std::shared_ptr<const assets::CharacterTable> characters, std::shared_ptr<const assets::SkillTable> skills) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        config.rules = FortressRules();
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.devCommands = true;
        config.characters = std::move(characters);
        config.skills = std::move(skills);
        config.npcs.push_back({kOfficial, "NPC_CH_FORTRESS_OFFICIAL", {kStart.region, 1000.0f, 50.0f, 458.0f}, 0.0f});
        config.npcs.push_back({kManager, "NPC_CH_FORTRESS_MANAGER1", {kStart.region, 1000.0f, 50.0f, 448.0f}, 0.0f});
        config.npcs.push_back({kPulley, "STRUCTURE_GATE_PULLEY_JA_01", {kStart.region, 990.0f, 50.0f, 465.0f}, 0.0f});
        config.npcs.push_back({kAide, "NPC_CH_FORTRESS_BATTLEAIDE1", {kStart.region, 1000.0f, 50.0f, 438.0f}, 0.0f});
        config.npcs.push_back({kSmith, "NPC_CH_FORTRESS_SMITH1", {kStart.region, 1000.0f, 50.0f, 428.0f}, 0.0f});
        config.npcs.push_back({kTrainer, "NPC_CH_FORTRESS_TRAINER1", {kStart.region, 1000.0f, 50.0f, 418.0f}, 0.0f});
        server::FortressSpec f;
        f.id = 1;
        f.codename = "FORTRESS_JANGAN";
        f.nameKey = "SN_FORTRESS_JANGAN";
        f.official = "NPC_CH_FORTRESS_OFFICIAL";
        f.fee = 1'000'000;
        f.regions = {kStart.region};
        f.structures.push_back({84, kHeart, game::StructureKind::Heart, "SN_STRUCTURE_FORT_STONE_01", 936000, {kStart.region, 975.0f, 50.0f, 470.0f}, 0});
        f.structures.push_back({85, kTower, game::StructureKind::GuardTower, "SN_STRUCTURE_GUARD_TOWER", 464000, {kStart.region, 945.0f, 50.0f, 470.0f}, 0});
        f.structures.push_back({88, kGate, game::StructureKind::Gate, "SN_STRUCTURE_BIG_GATE_01", 936000, {kStart.region, 960.0f, 50.0f, 490.0f}, 0, 0,
                                "structure_pos_ja_gate_01"});
        config.fortresses.push_back(std::move(f));
        config.fortress.structureHp = 3;
        config.fortress.heartSealSeconds = 1;
        config.fortress.warningMinutes.clear();
        pump = [this] { game->Poll(2ms); };
        Start();
    }
    void Start() {
        game = std::make_unique<server::GameServer>();
        REQUIRE_MESSAGE(game->Start(config, error), error);
    }
    void Restart() {
        game->Stop();
        Start();
    }
    void Settle() {
        for (int i = 0; i < 20 || (game->GetFortresses() && game->GetFortresses()->Writing()); ++i) pump();
    }
    Player Join(const std::string& name, std::int64_t gold = 3'000'000) {
        Player p;
        p.name = name;
        const std::string account = "acc" + name;
        bool ok = false;
        if (!server::FindAccount(t.Db(), account, ok)) {
            REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        }
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        p.c = std::make_unique<Client>(net, game->Port(), pump);
        REQUIRE(p.c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        p.c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.c->Expect(entered));
        proto::LobbyResult r;
        if (t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {name}).Rows() == 0) {
            proto::CharacterCreate m;
            m.name = name;
            m.model = 100;
            m.chest = 1;
            m.legs = 2;
            m.shoes = 3;
            m.weapon = 10;
            p.c->Send(m);
            REQUIRE(p.c->Expect(r));
            REQUIRE(r.status == proto::LobbyStatus::Ok);
            REQUIRE(t.Db().Query("UPDATE _char SET curlevel = 30, remaingold = $2 WHERE charname16 = $1", {name, gold}).Ok());
        }
        p.character = t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {name}).Int(0, 0);
        p.c->Send(proto::CharacterSelect{name});
        REQUIRE(p.c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        p.c->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.c->Expect(w));
        p.entity = w.entity;
        proto::EntityAppeared seen;
        while ((p.official == 0 || p.manager == 0 || p.pulley == 0 || p.aide == 0 || p.smith == 0 || p.trainer == 0) && p.c->Expect(seen)) {
            if (seen.character.model == kOfficial) p.official = seen.entity;
            if (seen.character.model == kManager) p.manager = seen.entity;
            if (seen.character.model == kPulley) p.pulley = seen.entity;
            if (seen.character.model == kAide) p.aide = seen.entity;
            if (seen.character.model == kSmith) p.smith = seen.entity;
            if (seen.character.model == kTrainer) p.trainer = seen.entity;
        }
        REQUIRE(p.official != 0);
        REQUIRE(p.manager != 0);
        return p;
    }
};

template <typename T>
T Next(Player& p) {
    T m;
    REQUIRE(p.c->Expect(m));
    return m;
}

proto::FortressAnswer Answer(Player& p, proto::FortressAction action) {
    proto::FortressAnswer a;
    do REQUIRE(p.c->Expect(a));
    while (a.action != action);
    return a;
}

proto::FortressNotice Notice(Player& p, proto::FortressNoticeKind kind, std::chrono::milliseconds timeout = 10s) {
    proto::FortressNotice n;
    do REQUIRE(p.c->Expect(n, timeout));
    while (n.kind != kind);
    return n;
}

// A guild of the original's tables, its master a player and the rest names only.
void MakeGuild(TestDatabase& t, int id, const char* name, int level, const Player& master, int extraMembers, const Player* member = nullptr) {
    REQUIRE(t.Db()
                .Query("INSERT INTO _guild (id, name, lvl, gatheredsp, foundationdate, alliance, booty) OVERRIDING SYSTEM VALUE VALUES ($1, $2, $3, 0, now(), 0, 0)",
                       {id, name, level})
                .Ok());
    const auto add = [&](std::int64_t character, const std::string& n, int memberClass) {
        REQUIRE(t.Db()
                    .Query("INSERT INTO _guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate, permission) "
                           "VALUES ($1, $2, $3, $4, 30, 0, now(), $5)",
                           {id, character, n, memberClass, memberClass == 0 ? -1 : 0})
                    .Ok());
        REQUIRE(t.Db().Query("UPDATE _char SET guildid = $1 WHERE charid = $2", {id, character}).Ok());
    };
    add(master.character, master.name, 0);
    if (member != nullptr) add(member->character, member->name, 10);
    for (int i = 0; i < extraMembers; ++i) add(9'000'000 + id * 100 + i, std::string(name) + std::to_string(i), 10);
}

} // namespace

TEST_CASE("Fortress war over the wire: apply at the official, the war, the tower's seal, the heart taken, the owner kept (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    for (std::uint32_t id : {kOfficial, kHeart, kTower, kGate}) REQUIRE(characters->Find(id) != nullptr);

    Rig rig(t, characters, skills);
    // The fortress's rows made as `_InsertSiegeFortress` makes them.
    rig.Settle();
    CHECK(t.Db().Query("SELECT guildid, tempguildid FROM _siegefortress WHERE fortressid = 1").Rows() == 1);
    CHECK(t.Db().Query("SELECT 1 FROM _siegefortressstruct WHERE fortressid = 1").Rows() == 3);
    {
        // The characters, then their guilds in the original's tables (read at the start).
        Player a = rig.Join("Raider");
        Player e = rig.Join("Grunt");
        Player b = rig.Join("Keeper");
        Player c = rig.Join("Weakling");
        Player d = rig.Join("Loner");
        MakeGuild(t, 101, "Raiders", 3, a, 5, &e); // 7 members
        MakeGuild(t, 102, "Keepers", 3, b, 6);     // 7
        MakeGuild(t, 103, "Weak", 2, c, 2);        // level 2, 3 members
    }
    rig.Restart();
    Player a = rig.Join("Raider");
    Player e = rig.Join("Grunt");
    Player b = rig.Join("Keeper");
    Player c = rig.Join("Weakling");
    Player d = rig.Join("Loner");
    // Everyone is told the fortresses on entering: Jangan's, no owner, no period.
    const auto list = Next<proto::FortressList>(a);
    REQUIRE(list.fortresses.size() == 1);
    CHECK(list.fortresses[0].nameKey == "SN_FORTRESS_JANGAN");
    CHECK(list.fortresses[0].owner == 0);
    CHECK_FALSE(list.requestsOpen);
    CHECK_FALSE(list.warOn);
    // The structures stand, each told as one (EntityStructure after its appearance).
    const server::Fortresses* forts = rig.game->GetFortresses();
    REQUIRE(forts != nullptr);
    const std::uint32_t heart = forts->Structures(1, game::StructureKind::Heart)[0];
    const std::uint32_t tower = forts->Structures(1, game::StructureKind::GuardTower)[0];
    REQUIRE(heart != 0);
    REQUIRE(tower != 0);
    proto::EntityStructure told;
    do REQUIRE(a.c->Expect(told));
    while (told.entity != heart);
    CHECK(told.kind == std::uint8_t(game::StructureKind::Heart));
    CHECK(told.maxHp == 3);

    // Out of the application period (0x2814).
    a.c->Send(proto::FortressApply{a.official, 1, Side::Attack});
    CHECK(Answer(a, proto::FortressAction::Apply).error == std::uint16_t(FortressError::NotApplyPeriod));
    // The period opens (the GM's /SIEGE STARTREQ).
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartRequests});
    Notice(a, proto::FortressNoticeKind::RequestsOpen);
    // The refusals of the original's order.
    d.c->Send(proto::FortressApply{d.official, 1, Side::Attack});
    CHECK(Answer(d, proto::FortressAction::Apply).error == std::uint16_t(FortressError::NotInGuild));
    e.c->Send(proto::FortressApply{e.official, 1, Side::Attack});
    CHECK(Answer(e, proto::FortressAction::Apply).error == std::uint16_t(FortressError::OnlyMaster));
    c.c->Send(proto::FortressApply{c.official, 1, Side::Attack});
    CHECK(Answer(c, proto::FortressAction::Apply).error == std::uint16_t(FortressError::GuildLevel));
    a.c->Send(proto::FortressApply{a.official, 1, Side::Defend});
    CHECK(Answer(a, proto::FortressAction::Apply).error == std::uint16_t(FortressError::WrongAlliance));
    a.c->Send(proto::FortressApply{heart, 1, Side::Attack}); // not the official
    CHECK(Answer(a, proto::FortressAction::Apply).error == std::uint16_t(FortressError::WrongNpc));
    // Applied: the fee taken.
    a.c->Send(proto::FortressApply{a.official, 1, Side::Attack});
    CHECK(Answer(a, proto::FortressAction::Apply).ok);
    proto::GoldChanged gold;
    REQUIRE(a.c->Expect(gold));
    CHECK(gold.gold == 2'000'000);
    a.c->Send(proto::FortressApply{a.official, 1, Side::Attack});
    CHECK(Answer(a, proto::FortressAction::Apply).error == std::uint16_t(FortressError::AlreadyApplied));
    // Taken back (the fee is not given back), applied again.
    a.c->Send(proto::FortressCancel{a.official, 1});
    CHECK(Answer(a, proto::FortressAction::Cancel).ok);
    a.c->Send(proto::FortressApply{a.official, 1, Side::Attack});
    CHECK(Answer(a, proto::FortressAction::Apply).ok);
    REQUIRE(a.c->Expect(gold));
    CHECK(gold.gold == 1'000'000);
    b.c->Send(proto::FortressApply{b.official, 1, Side::Attack});
    CHECK(Answer(b, proto::FortressAction::Apply).ok);
    // The official's window: the row, the guild's application.
    a.c->Send(proto::FortressApplyOpen{a.official});
    const auto info = Next<proto::FortressApplyInfo>(a);
    REQUIRE(info.rows.size() == 1);
    CHECK(info.rows[0].fee == 1'000'000);
    CHECK(info.applied == 1);
    CHECK(info.requestsOpen);
    rig.Settle();
    CHECK(t.Db().Query("SELECT guildid FROM _siegefortressrequest WHERE fortressid = 1 ORDER BY guildid").Rows() == 2);
    CHECK(t.Db().Query("SELECT remaingold FROM _char WHERE charid = $1", {a.character}).Int(0, 0) == 1'000'000);

    // The war (STARTWAR).
    a.c->Send(proto::FortressDev{proto::FortressDevAction::EndRequests});
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Notice(a, proto::FortressNoticeKind::WarBegin);
    CHECK(forts->WarOn());
    // Enemies in its area: the two applied guilds; not one's own guild; not a guild out of the war.
    const std::uint16_t r = kStart.region;
    CHECK(forts->Hostile(a.character, r, b.character, r));
    CHECK_FALSE(forts->Hostile(a.character, r, e.character, r));
    CHECK_FALSE(forts->Hostile(a.character, r, d.character, r));
    CHECK_FALSE(forts->Hostile(a.character, r, b.character, assets::RegionId(169, 98))); // out of its area
    // Only the war's guilds go into its area (its gates' arrivals).
    CHECK(forts->MayEnter(a.character, r));
    CHECK_FALSE(forts->MayEnter(d.character, r));
    CHECK(forts->MayEnter(d.character, assets::RegionId(169, 98)));
    // Over the wire the game server's hostility takes the war's word (a developer's strike goes through
    // Hostile()): the Keepers' master is hit, the Raiders' own member is not.
    a.c->Send(proto::DevStrike{e.entity, 1});
    a.c->Send(proto::DevStrike{b.entity, 1});
    proto::EntityDamaged hurt;
    do REQUIRE(b.c->Expect(hurt));
    while (hurt.target != b.entity && hurt.target != e.entity);
    CHECK(hurt.target == b.entity);
    CHECK(hurt.attacker == a.entity);
    // Not in the war: may not hit its structures.
    d.c->Send(proto::AttackTarget{tower});
    CHECK(Answer(d, proto::FortressAction::Attack).error == std::uint16_t(FortressError::NotParticipant));
    // The heart is sealed while a guard tower stands.
    a.c->Send(proto::AttackTarget{heart});
    CHECK(Answer(a, proto::FortressAction::Attack).error == std::uint16_t(FortressError::HeartSealed));
    // The tower falls; the seal lifts after its time.
    a.c->Send(proto::AttackTarget{tower});
    const auto fell = Notice(a, proto::FortressNoticeKind::StructureDestroyed, 30s);
    CHECK(fell.nameKey == "SN_STRUCTURE_GUARD_TOWER");
    CHECK(Notice(a, proto::FortressNoticeKind::SealReleasing).value == 1);
    Notice(a, proto::FortressNoticeKind::HeartOpen);
    // The heart: the Raiders dealt it all its damage, they hold the fortress for now.
    a.c->Send(proto::AttackTarget{heart});
    const auto held = Notice(a, proto::FortressNoticeKind::Conquer, 30s);
    CHECK(held.guild == "Raiders");
    CHECK(held.fortress == 1);
    CHECK(forts->Holder(1) == 101);
    rig.Settle();
    CHECK(t.Db().Query("SELECT tempguildid FROM _siegefortress WHERE fortressid = 1").Int(0, 0) == 101);
    // Its structures are theirs now, standing again: the Raiders may not hit them, the Keepers may.
    const std::uint32_t newHeart = forts->Structures(1, game::StructureKind::Heart)[0];
    const std::uint32_t newTower = forts->Structures(1, game::StructureKind::GuardTower)[0];
    REQUIRE(newHeart != 0);
    REQUIRE(newTower != 0);
    CHECK(forts->MayHit(a.character, newTower) == FortressError::StructureOwnGuild);
    CHECK(forts->MayHit(b.character, newTower) == FortressError::None);
    CHECK(forts->MayHit(b.character, newHeart) == FortressError::HeartSealed);
    CHECK(t.Db().Query("SELECT 1 FROM _siegefortressstruct WHERE fortressid = 1 AND ownerguildid = 101").Rows() == 3);

    // The war ends (ENDWAR): the guild holding it owns it.
    a.c->Send(proto::FortressDev{proto::FortressDevAction::EndWar});
    Notice(b, proto::FortressNoticeKind::WarEnd);
    // The holder is the owner now, as the list says (no line of its own: the capture's "occupied" was the line).
    proto::FortressList ended;
    do REQUIRE(b.c->Expect(ended));
    while (ended.fortresses.empty() || ended.fortresses[0].owner != 101);
    CHECK(ended.fortresses[0].ownerName == "Raiders");
    CHECK(ended.fortresses[0].masterName == "Raider");
    CHECK(forts->Owner(1) == 101);
    CHECK(forts->Holder(1) == 0);
    CHECK(forts->GuildHoldsFortress(101));
    rig.Settle();
    const auto row = t.Db().Query("SELECT guildid, tempguildid FROM _siegefortress WHERE fortressid = 1");
    CHECK(row.Int(0, 0) == 101);
    CHECK(row.Int(0, 1) == 0);
    CHECK(t.Db().Query("SELECT 1 FROM _siegefortressrequest WHERE fortressid = 1").Rows() == 0);

    // A restart: the owner from the tables, told to whoever enters.
    rig.Restart();
    Player a2 = rig.Join("Raider");
    const auto after = Next<proto::FortressList>(a2);
    REQUIRE(after.fortresses.size() == 1);
    CHECK(after.fortresses[0].owner == 101);
    CHECK(after.fortresses[0].ownerName == "Raiders");
    // An owner applies for nothing (0x2811).
    a2.c->Send(proto::FortressDev{proto::FortressDevAction::StartRequests});
    a2.c->Send(proto::FortressApply{a2.official, 1, Side::Attack});
    CHECK(Answer(a2, proto::FortressAction::Apply).error == std::uint16_t(FortressError::OwnsFortress));

    // A second war, the Keepers against the owners: the tower falls, the heart stands, the fortress is kept. A fortress
    // kept keeps its damage (`_SiegeFortressFinished` resets the structures only for a new owner): the tower stays down
    // for the battle aide's repair.
    Player b2 = rig.Join("Keeper");
    b2.c->Send(proto::FortressApply{b2.official, 1, Side::Attack});
    CHECK(Answer(b2, proto::FortressAction::Apply).ok);
    a2.c->Send(proto::FortressDev{proto::FortressDevAction::EndRequests});
    a2.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Notice(b2, proto::FortressNoticeKind::WarBegin);
    const server::Fortresses* restarted = rig.game->GetFortresses(); // the restarted server's
    REQUIRE(restarted != nullptr);
    const std::uint32_t ownersTower = restarted->Structures(1, game::StructureKind::GuardTower)[0];
    REQUIRE(ownersTower != 0);
    b2.c->Send(proto::AttackTarget{ownersTower});
    CHECK(Notice(b2, proto::FortressNoticeKind::StructureDestroyed, 30s).nameKey == "SN_STRUCTURE_GUARD_TOWER");
    a2.c->Send(proto::FortressDev{proto::FortressDevAction::EndWar});
    Notice(b2, proto::FortressNoticeKind::WarEnd);
    CHECK(restarted->Owner(1) == 101);
    rig.Settle();
    const auto kept = t.Db().Query("SELECT hp, state, ownerguildid FROM _siegefortressstruct WHERE fortressid = 1 AND refeventstructid = 85");
    REQUIRE(kept.Rows() == 1);
    CHECK(kept.Int(0, 0) == 0);
    CHECK(kept.Int(0, 1) != 0);
    CHECK(kept.Int(0, 2) == 101);
    CHECK(t.Db().Query("SELECT hp FROM _siegefortressstruct WHERE fortressid = 1 AND refeventstructid = 84").Int(0, 0) == 3);
    rig.game->Stop();
}

TEST_CASE("Fortress war: a gate's pulley opens and closes its gate; an open gate is not hit (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    Rig rig(t, characters, skills);
    Player a = rig.Join("Opener");
    const server::Fortresses* forts = rig.game->GetFortresses();
    const std::uint32_t gate = forts->Structures(1, game::StructureKind::Gate)[0];
    REQUIRE(gate != 0);
    // Another fortress's pulley (0x2829).
    a.c->Send(proto::FortressPulley{a.pulley, 3, 2});
    CHECK(Answer(a, proto::FortressAction::Pulley).error == std::uint16_t(FortressError::PulleyFortress));
    // Opened: the answer's state 2, the gate told open (bit 1).
    a.c->Send(proto::FortressPulley{a.pulley, 1, 2});
    const auto opened = Answer(a, proto::FortressAction::Pulley);
    CHECK(opened.ok);
    CHECK(opened.zone == 88);
    CHECK(opened.amount == 2);
    proto::EntityStructure told;
    do REQUIRE(a.c->Expect(told));
    while (told.entity != gate || told.state != 2);
    // In a war an open gate is not hit.
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Notice(a, proto::FortressNoticeKind::WarBegin);
    CHECK(forts->MayHit(a.character, gate) == FortressError::GateOpen);
    // Closed again: state 0.
    a.c->Send(proto::FortressPulley{a.pulley, 1, 0});
    CHECK(Answer(a, proto::FortressAction::Pulley).amount == 0);
    do REQUIRE(a.c->Expect(told));
    while (told.entity != gate || told.state != 0);
    CHECK(forts->MayHit(a.character, gate) != FortressError::GateOpen);
    rig.game->Stop();
}

TEST_CASE("Fortress war: barricades by the holder, war camps by the attackers: their manuals, limits, fall and rising (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    constexpr std::uint32_t kBarricade = 19550, kCampBase = 19543, kBarricadeManual = 10591, kCampManual = 10590;
    Rig rig(t, characters, skills);
    {
        Player a = rig.Join("Keeper");
        Player b = rig.Join("Raider");
        MakeGuild(t, 201, "Keepers", 3, a, 6);
        MakeGuild(t, 202, "Raiders", 3, b, 6);
    }
    // Jangan's: two barricade zones (one allowed) and two war camp foundations beside the start; the manuals' rows.
    server::FortressSpec& f = rig.config.fortresses[0];
    f.maxBarricade = 1;
    f.structures.push_back({200, kBarricade, game::StructureKind::Barricade, "SN_STRUCTURE_BARRICADE_01", 80000, {kStart.region, 950.0f, 50.0f, 450.0f}, 0});
    f.structures.push_back({201, kBarricade, game::StructureKind::Barricade, "SN_STRUCTURE_BARRICADE_01", 80000, {kStart.region, 940.0f, 50.0f, 450.0f}, 0});
    f.structures.push_back({210, kCampBase, game::StructureKind::Headquarters, "SN_STRUCTURE_HEADQUARTERS_00", 0, {kStart.region, 980.0f, 50.0f, 440.0f}, 0});
    f.structures.push_back({211, kCampBase, game::StructureKind::Headquarters, "SN_STRUCTURE_HEADQUARTERS_00", 0, {kStart.region, 985.0f, 50.0f, 430.0f}, 0});
    rig.config.fortress.items.push_back({kBarricadeManual, "ITEM_FORT_MANUAL_BARRICADE", {3, 3, 3, 8}, "STRUCTURE_BARRICADE_01", 1, 1, 0, 7, 100}); // no reuse in the test (30 s)
    rig.config.fortress.items.push_back({kCampManual, "ITEM_FORT_MANUAL_BATTLECAMP", {3, 3, 3, 8}, "STRUCTURE_HEADQUARTERS_01", 1, 1, 0, 3, 200}); // (30 minutes)
    rig.game->Stop();
    REQUIRE(t.Db().Query("UPDATE _siegefortress SET guildid = 201 WHERE fortressid = 1").Ok());
    REQUIRE(t.Db().Query("UPDATE _guildmember SET siegeauthority = 1 WHERE memberclass = 0").Ok()); // the masters: Commanders
    REQUIRE(t.Db().Query("INSERT INTO _siegefortressrequest (fortressid, guildid, requesttype) VALUES (1, 202, 0)").Ok());
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    for (const auto& [name, item] : {std::pair<const char*, std::uint32_t>{"Keeper", kBarricadeManual}, {"Raider", kCampManual}}) {
        server::StoredItem stack;
        stack.refId = item;
        stack.quantity = 3;
        stack.counted = true;
        REQUIRE_MESSAGE(server::GiveItems(t.Db(), 1, name, {stack, stack}, 0, error) == server::GiveResult::Done, error);
    }
    rig.Start();
    Player a = rig.Join("Keeper");
    Player b = rig.Join("Raider");
    const auto slotOf = [&](Player& p, std::uint32_t item) {
        return std::uint8_t(t.Db()
                                .Query("SELECT min(v.slot) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2",
                                       {p.character, std::int64_t(item)})
                                .Int(0, 0));
    };
    const std::uint8_t barricades = slotOf(a, kBarricadeManual), camps = slotOf(b, kCampManual);
    REQUIRE(barricades != 0);
    REQUIRE(camps != 0);
    const auto use = [&](Player& p, std::uint8_t slot, std::uint32_t zone) {
        p.c->Send(proto::FortressUseItem{slot, 1, zone});
        return Answer(p, proto::FortressAction::UseItem);
    };
    // Not in a war (Param3 1: 0x18E2).
    CHECK(use(a, barricades, 200).error == std::uint16_t(FortressError::ItemNotInWar));
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Notice(a, proto::FortressNoticeKind::WarBegin);
    const server::Fortresses* forts = rig.game->GetFortresses();
    // The holder's barricade on its zone: it stands, owned by the Keepers; the second over MaxBarricade (0x18EF).
    CHECK(use(a, barricades, 200).ok);
    REQUIRE(forts->Structures(1, game::StructureKind::Barricade).size() == 2);
    const std::uint32_t barricade = forts->Structures(1, game::StructureKind::Barricade)[0];
    CHECK(barricade != 0);
    CHECK(use(a, barricades, 201).error == std::uint16_t(FortressError::ItemLimit));
    // An attacker may not build a barricade; the holder no camp (a defender: 0x18E3); a camp of the zone's kind only (0x18D6).
    CHECK(use(b, camps, 200).error == std::uint16_t(FortressError::ItemNotUsable));
    // The attackers' camp on the nearest foundation (zone 0: ours, the nearest free one), then none more (0x18E8).
    CHECK(use(b, camps, 0).ok);
    const auto camp = forts->Structures(1, game::StructureKind::Headquarters);
    REQUIRE(camp.size() == 2);
    CHECK((camp[0] != 0) != (camp[1] != 0));
    CHECK(use(b, camps, 0).error == std::uint16_t(FortressError::ItemCampBuilt));
    // The Raiders rise at their camp (within 250 of it).
    const auto rise = rig.game->GetFortresses() != nullptr ? const_cast<server::Fortresses*>(forts)->RevivalPlace(b.character, kStart.region) : std::nullopt;
    REQUIRE(rise.has_value());
    // The Raiders break the barricade: its zone is free again.
    b.c->Send(proto::AttackTarget{barricade});
    Notice(b, proto::FortressNoticeKind::StructureDestroyed, 30s);
    CHECK(forts->Structures(1, game::StructureKind::Barricade)[0] == 0);
    rig.Settle();
    CHECK(t.Db().Query("SELECT state FROM _siegefortressstruct WHERE refeventstructid = 200").Int(0, 0) == 4);
    // The war's end: the camp is a foundation again.
    a.c->Send(proto::FortressDev{proto::FortressDevAction::EndWar});
    Notice(a, proto::FortressNoticeKind::WarEnd);
    for (const std::uint32_t e : forts->Structures(1, game::StructureKind::Headquarters)) CHECK(e == 0);
    rig.game->Stop();
}

// The rig's two guilds, Keepers holding Jangan (their master its Commander) and Raiders applied, each master with a stack of
// these items; the game server started again on them.
struct WarGuilds {
    Player a, b;
};
WarGuilds TwoGuilds(Rig& rig, TestDatabase& t, const std::vector<std::uint32_t>& keeperItems, const std::vector<std::uint32_t>& raiderItems) {
    {
        Player a = rig.Join("Keeper");
        Player b = rig.Join("Raider");
        MakeGuild(t, 201, "Keepers", 3, a, 6);
        MakeGuild(t, 202, "Raiders", 3, b, 6);
    }
    rig.game->Stop();
    REQUIRE(t.Db().Query("UPDATE _siegefortress SET guildid = 201 WHERE fortressid = 1").Ok());
    REQUIRE(t.Db().Query("UPDATE _guildmember SET siegeauthority = 1 WHERE memberclass = 0").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _siegefortressrequest (fortressid, guildid, requesttype) VALUES (1, 202, 0)").Ok());
    REQUIRE(t.Db().Query("UPDATE _guild SET gatheredsp = 1000000").Ok());
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    std::string error;
    for (const auto& [name, items] : {std::pair<const char*, const std::vector<std::uint32_t>*>{"Keeper", &keeperItems}, {"Raider", &raiderItems}}) {
        std::vector<server::StoredItem> stacks;
        for (const std::uint32_t item : *items) {
            server::StoredItem stack;
            stack.refId = item;
            stack.quantity = 3;
            stack.counted = true;
            stacks.push_back(stack);
        }
        if (!stacks.empty()) REQUIRE_MESSAGE(server::GiveItems(t.Db(), 1, name, stacks, 0, error) == server::GiveResult::Done, error);
    }
    rig.Start();
    return {rig.Join("Keeper"), rig.Join("Raider")};
}
std::uint8_t SlotOf(TestDatabase& t, const Player& p, std::uint32_t item) {
    return std::uint8_t(t.Db()
                            .Query("SELECT min(v.slot) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2",
                                   {p.character, std::int64_t(item)})
                            .Int(0, 0));
}

TEST_CASE("Fortress war: flags, repair kits and the shock bomb (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    constexpr std::uint32_t kFlag = 10592, kKit = 10594, kBomb = 10593;
    constexpr std::uint32_t kAggressive = 21240; // SKILL_FORT_STRUCTURE_AGGRESSIVE_FLAG, the flag's DefaultSkill_1
    Rig rig(t, characters, skills);
    rig.config.fortress.structureHp = 1000;
    rig.config.fortress.flagSeconds = 3;
    // The export's rows (the reuses left out: 5 and 30 minutes).
    rig.config.fortress.items.push_back({kFlag, "ITEM_FORT_AGGRESSIVE_FLAG", {3, 3, 3, 9}, "STRUCTURE_AGGRESSIVE_FLAG", 1, 1, 0, 3, 100});
    rig.config.fortress.items.push_back({kKit, "ITEM_FORT_REPAIR_KIT_01", {3, 3, 1, 10}, "SKILL_FORT_REPAIR_KIT_01", 1, 1, 0, 3, 150});
    rig.config.fortress.items.push_back({kBomb, "ITEM_FORT_SHOCK_BOMB", {3, 3, 6, 2}, "SKILL_FORT_SHOCK_BOMB", 1, 1, 0, 0, 150});
    auto [a, b] = TwoGuilds(rig, t, {kFlag, kKit}, {kBomb, kKit});
    const std::uint8_t flags = SlotOf(t, a, kFlag), kits = SlotOf(t, a, kKit), bombs = SlotOf(t, b, kBomb), raiderKits = SlotOf(t, b, kKit);
    REQUIRE(flags != 0);
    REQUIRE(bombs != 0);
    const auto use = [&](Player& p, proto::FortressUseItem m) {
        p.c->Send(m);
        return Answer(p, proto::FortressAction::UseItem);
    };
    const proto::WorldPosition near{kStart.region, kStart.x + 10.0f, kStart.y, kStart.z};
    // A flag outside the war (Param3 1: 0x18E2).
    CHECK(use(a, {flags, 1, 0, near.region, near.x, near.y, near.z}).error == std::uint16_t(FortressError::ItemNotInWar));
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Notice(a, proto::FortressNoticeKind::WarBegin);
    const server::Fortresses* forts = rig.game->GetFortresses();
    // Beyond the item's Range of 100 (0x18D8), then planted beside the Keeper: it stands for the Keepers, its aura on him.
    CHECK(use(a, {flags, 1, 0, near.region, near.x + 150.0f, near.y, near.z}).error == std::uint16_t(FortressError::ItemTooFar));
    CHECK(use(a, {flags, 1, 0, near.region, near.x, near.y, near.z}).ok);
    REQUIRE(forts->Objects(1, game::StructureKind::Flag).size() == 1);
    const std::uint32_t flag = forts->Objects(1, game::StructureKind::Flag)[0];
    proto::EntityStructure told;
    do REQUIRE(a.c->Expect(told));
    while (told.entity != flag);
    CHECK(told.kind == std::uint8_t(game::StructureKind::Flag));
    CHECK(told.owner == 201);
    proto::BuffAdded aura;
    do REQUIRE(a.c->Expect(aura, 5s));
    while (aura.skill != kAggressive);
    // Its own guild may not hit it; the Raiders may.
    CHECK(forts->MayHit(a.character, flag) == FortressError::StructureOwnGuild);
    CHECK(forts->MayHit(b.character, flag) == FortressError::None);
    // Its time over (3 s here, 120 s in the game): gone.
    // By the clock, not by a count of pumps (a pump's time is the server's, which says nothing of the flag's 3 s).
    for (const auto until = std::chrono::steady_clock::now() + 8s;
         std::chrono::steady_clock::now() < until && !forts->Objects(1, game::StructureKind::Flag).empty();) {
        rig.pump();
    }
    CHECK(forts->Objects(1, game::StructureKind::Flag).empty());
    // A repair kit: the Raiders' refused (not the holder, 0x18E5); on a whole structure 0x18ED; on the tower once hit, its
    // health back by 1 % (10 of 1,000) a pulse.
    const std::uint32_t tower = forts->Structures(1, game::StructureKind::GuardTower)[0];
    REQUIRE(tower != 0);
    CHECK(use(b, {raiderKits, 1, 0, 0, 0, 0, 0, tower}).error == std::uint16_t(FortressError::ItemNotHolder));
    CHECK(use(a, {kits, 1, 0, 0, 0, 0, 0, tower}).error == std::uint16_t(FortressError::ItemTargetState));
    CHECK(use(a, {kits, 1, 0, 0, 0, 0, 0, a.entity}).error == std::uint16_t(FortressError::ItemWrongTarget));
    b.c->Send(proto::AttackTarget{tower});
    proto::EntityDamaged hit;
    do REQUIRE(a.c->Expect(hit, 20s));
    while (hit.target != tower || hit.damage == 0);
    b.c->Send(proto::AttackTarget{0});
    const std::uint32_t after = hit.hp;
    REQUIRE(after < 1000);
    for (int i = 0; i < 200; ++i) rig.pump(); // the last swings land
    CHECK(use(a, {kits, 1, 0, 0, 0, 0, 0, tower}).ok);
    proto::StructureHealth healed;
    do REQUIRE(a.c->Expect(healed, 10s));
    while (healed.entity != tower);
    CHECK(healed.maxHp == 1000);
    CHECK(healed.hp > hit.hp);
    // The shock bomb on the Keeper (an enemy in the war): its skill cast.
    CHECK(use(b, {bombs, 1, 0, 0, 0, 0, 0, a.entity}).ok);
    a.c->Send(proto::FortressDev{proto::FortressDevAction::EndWar});
    Notice(a, proto::FortressNoticeKind::WarEnd);
    rig.game->Stop();
}

TEST_CASE("Fortress war: defensive positions built, guards and tablet monsters for their guild, summon scrolls (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    constexpr std::uint32_t kGuardItem = 10589, kTablet = 10588, kScroll = 24995, kDefensive00 = 19509, kDefensive01 = 19510;
    const assets::ObjectRow* guard30 = characters->Find(std::string_view("COS_GUARD_CH_SPEAR_01_030"));
    const assets::ObjectRow* hanga30 = characters->Find(std::string_view("MOB_FW_HANGA_030"));
    const assets::ObjectRow* taese30 = characters->Find(std::string_view("MOB_FW_TAESE_030"));
    REQUIRE(guard30 != nullptr);
    REQUIRE(hanga30 != nullptr);
    REQUIRE(taese30 != nullptr);
    Rig rig(t, characters, skills);
    rig.config.fortress.structureHp = 0;
    server::FortressSpec& f = rig.config.fortresses[0];
    f.maxGuard = 1;
    f.structures.push_back({300, kDefensive00, game::StructureKind::Defensive, "SN_STRUCTURE_SMALL_DEFENSIVE_POSITION_00", 0, {kStart.region, 990.0f, 50.0f, 430.0f}, 0});
    server::StructureReference d0, d1;
    d0.refObjId = kDefensive00;
    d0.codename = "STRUCTURE_SMALL_DEFENSIVE_POSITION_00";
    d0.gold = 5000;
    d0.guildPoints = 500;
    d0.upgrades[0] = "STRUCTURE_SMALL_DEFENSIVE_POSITION_01";
    d1.refObjId = kDefensive01;
    d1.codename = "STRUCTURE_SMALL_DEFENSIVE_POSITION_01";
    d1.reqLevel = 20;
    d1.maxHp = 50000;
    d1.below = d0.codename;
    rig.config.fortress.structureReferences = {d0, d1};
    // The export's rows (the reuses left out) and the variants at level 30.
    rig.config.fortress.items.push_back({kGuardItem, "ITEM_COS_GUARD_CH_SPEAR_01", {3, 3, 3, 7}, "COS_GUARD_CH_SPEAR_01_001", 1, 2, 0, 5, 100});
    rig.config.fortress.items.push_back({kTablet, "ITEM_FORT_TABLET_HANGA_GOLD", {3, 3, 3, 6}, "MOB_FW_HANGA_001", 1, 1, 0, 1, 100});
    rig.config.fortress.items.push_back({kScroll, "ITEM_FORT_SCROLL_TAESE", {3, 3, 3, 11}, "MOB_FW_TAESE_001", 1, 2, 0, 3, 100});
    rig.config.fortress.guards.push_back({1, guard30->refObjId, "COS_GUARD_CH_SPEAR_01_001", 30});
    rig.config.fortress.guards.push_back({1, hanga30->refObjId, "MOB_FW_HANGA_001", 30});
    rig.config.fortress.summons.push_back({0, taese30->refObjId, "MOB_FW_TAESE_001", 30});
    rig.config.files = &fs; // the navigation: monsters stand only on it
    auto [a, b] = TwoGuilds(rig, t, {kGuardItem, kTablet, kScroll}, {});
    for (int i = 0; i < 200; ++i) rig.pump(); // the start's navigation in
    const std::uint8_t guards = SlotOf(t, a, kGuardItem), tablets = SlotOf(t, a, kTablet), scrolls = SlotOf(t, a, kScroll);
    REQUIRE(guards != 0);
    const auto use = [&](Player& p, std::uint8_t slot, float dx) {
        p.c->Send(proto::FortressUseItem{slot, 1, 0, kStart.region, kStart.x + dx, kStart.y, kStart.z});
        return Answer(p, proto::FortressAction::UseItem);
    };
    const server::Fortresses* forts = rig.game->GetFortresses();
    // The defensive position built at the battle aide (the upgrade of its foundation: 5,000 gold); it stands then.
    CHECK(forts->Structures(1, game::StructureKind::Defensive)[0] == 0);
    a.c->Send(proto::FortressUpgrade{a.aide, 1, 300, 0});
    CHECK(Answer(a, proto::FortressAction::Upgrade).ok);
    CHECK(forts->Structures(1, game::StructureKind::Defensive)[0] != 0);
    // Outside the war: a tablet refused (Param3 1: 0x18E2); a guard placed; the second over MaxGuard (0x18EF).
    CHECK(use(a, tablets, 10.0f).error == std::uint16_t(FortressError::ItemNotInWar));
    CHECK(use(a, guards, 10.0f).ok);
    CHECK(use(a, guards, 20.0f).error == std::uint16_t(FortressError::ItemLimit));
    REQUIRE(forts->Objects(1, game::StructureKind::Guard).size() == 1);
    const std::uint32_t guard = forts->Objects(1, game::StructureKind::Guard)[0];
    rig.Settle();
    CHECK(t.Db().Query("SELECT count(*) FROM _siegefortressobject WHERE ownerguildid = 201 AND ownerlevel = 30 AND refobjid = $1", {std::int64_t(guard30->refObjId)})
              .Int(0, 0) == 1);
    // A scroll outside the war: its unique at the reader's level beside him.
    CHECK(use(a, scrolls, 0.0f).ok);
    bool unique = false;
    proto::EntityAppeared seen;
    for (int i = 0; i < 50 && !unique && a.c->Expect(seen, 200ms); ++i) unique = seen.character.model == taese30->refObjId;
    for (int i = 0; i < 100 && !unique; ++i) {
        rig.pump();
        while (!unique && a.c->Expect(seen, 10ms)) unique = seen.character.model == taese30->refObjId;
    }
    CHECK(unique);
    // In the war: the scroll refused (Param3 2: 0x18EE); the tablet's monster stands for the Keepers.
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Notice(a, proto::FortressNoticeKind::WarBegin);
    CHECK(use(a, scrolls, 0.0f).error == std::uint16_t(FortressError::ItemInWar));
    CHECK(use(a, tablets, 15.0f).ok);
    CHECK(forts->Objects(1, game::StructureKind::Summoned).size() == 1);
    // The guard goes for the Raider (at war against its side), never for the Keeper; the Keepers may not hit it.
    CHECK(forts->MayHit(a.character, guard) == FortressError::StructureOwnGuild);
    proto::EntityDamaged hit;
    bool struck = false;
    for (int i = 0; i < 400 && !struck; ++i) struck = b.c->Expect(hit, 50ms) && hit.target == b.entity && (hit.attacker == guard || forts->IsWarObject(hit.attacker));
    CHECK(struck);
    // Dismissed: not by the Raiders (0x282C); by the Keepers' Commander.
    b.c->Send(proto::FortressRemove{guard, 1});
    CHECK(Answer(b, proto::FortressAction::DismissObject).error == std::uint16_t(FortressError::NotOwnObject));
    a.c->Send(proto::FortressRemove{guard, 1});
    CHECK(Answer(a, proto::FortressAction::DismissObject).ok);
    CHECK(forts->Objects(1, game::StructureKind::Guard).empty());
    // The war's end: the tablet's monster goes with it (a fortress kept keeps its guards only).
    a.c->Send(proto::FortressDev{proto::FortressDevAction::EndWar});
    Notice(a, proto::FortressNoticeKind::WarEnd);
    CHECK(forts->Objects(1, game::StructureKind::Summoned).empty());
    rig.Settle();
    CHECK(t.Db().Query("SELECT count(*) FROM _siegefortressobject").Int(0, 0) == 0);
    rig.game->Stop();
}

TEST_CASE("Fortress manager: hiring, the war's schedule, the introduction, the GP history and the blessing (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    constexpr std::uint32_t kBlessSkill = 32088; // SKILL_FORT_BUFF_PHYSICAL_DMG_UP_01 (Jangan's BlessID 1)
    Rig rig(t, characters, skills);
    rig.config.fortress.blesses.push_back({1, 1, kBlessSkill, 700'000, 1'800});
    {
        Player helper = rig.Join("Helper");
        (void)helper;
    }
    auto [a, b] = TwoGuilds(rig, t, {}, {});
    // A member of the Keepers besides the master.
    rig.game->Stop();
    const std::int64_t helperId = t.Db().Query("SELECT charid FROM _char WHERE charname16 = 'Helper'").Int(0, 0);
    REQUIRE(t.Db().Query("INSERT INTO _guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate, permission) "
                         "VALUES (201, $1, 'Helper', 10, 30, 0, now(), 0)",
                         {helperId})
                .Ok());
    REQUIRE(t.Db().Query("UPDATE _char SET guildid = 201 WHERE charid = $1", {helperId}).Ok());
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    rig.Start();
    a = rig.Join("Keeper");
    b = rig.Join("Raider");
    Player h = rig.Join("Helper");
    const auto manage = [&](Player& p, proto::FortressAction action, std::uint32_t value = 0, std::string text = {}) {
        p.c->Send(proto::FortressManage{p.manager, action, 1, value, 0, std::move(text)});
    };
    const auto answer = [&](Player& p, proto::FortressAction action, std::uint32_t value = 0, std::string text = {}) {
        manage(p, action, value, std::move(text));
        return Answer(p, action);
    };
    // Hiring: none hired; a member is refused (the master only, 0x2807), the Raiders too (0x2806); the master hires the
    // smith (30,000 gold, 3,000 GP), the members are told (0x385F 0x12); again 0x2809.
    CHECK(answer(a, proto::FortressAction::Hired).amount == 0);
    CHECK(answer(h, proto::FortressAction::Hire, 2).error == std::uint16_t(FortressError::OnlyMaster));
    CHECK(answer(b, proto::FortressAction::Hire, 2).error == std::uint16_t(FortressError::NotHolder));
    const auto hired = answer(a, proto::FortressAction::Hire, 2);
    CHECK(hired.ok);
    CHECK(hired.amount == 2);
    proto::FortressNotice told = Notice(h, proto::FortressNoticeKind::Hired); // the one on entering: none hired
    CHECK(told.value == 0);
    told = Notice(h, proto::FortressNoticeKind::Hired);
    CHECK(told.value == 2);
    CHECK(answer(a, proto::FortressAction::Hire, 2).error == std::uint16_t(FortressError::HireDone));
    rig.Settle();
    CHECK(t.Db().Query("SELECT npchired FROM _siegefortress WHERE fortressid = 1").Int(0, 0) == 2);
    CHECK(t.Db().Query("SELECT remaingold FROM _char WHERE charname16 = 'Keeper'").Int(0, 0) == 3'000'000 - 30'000);
    // The war's schedule: the Raiders applied, the Keepers not.
    manage(b, proto::FortressAction::Schedule);
    CHECK(Next<proto::FortressSchedule>(b).applied);
    manage(a, proto::FortressAction::Schedule);
    CHECK_FALSE(Next<proto::FortressSchedule>(a).applied);
    // The introduction: 121 letters refused (0x283F), the member may not write (0x2807), the master writes it; it is in the
    // list, and read back by a member; the Raiders may not read it (0x2806).
    CHECK(answer(a, proto::FortressAction::Introduce, 0, std::string(121, 'x')).error == std::uint16_t(FortressError::IntroLength));
    CHECK(answer(h, proto::FortressAction::Introduce, 0, "ours").error == std::uint16_t(FortressError::OnlyMaster));
    CHECK(answer(a, proto::FortressAction::Introduce, 0, "Keepers keep Jangan").ok);
    proto::FortressList list;
    do REQUIRE(h.c->Expect(list));
    while (list.fortresses.empty() || list.fortresses[0].introduction != "Keepers keep Jangan");
    manage(h, proto::FortressAction::Introduction);
    CHECK(Next<proto::FortressIntroduction>(h).text == "Keepers keep Jangan");
    CHECK(answer(b, proto::FortressAction::Introduction).error == std::uint16_t(FortressError::NotHolder));
    // The blessing: the Raiders refused (0x283D); its buff on the master, its GP written; again at once 0x284A.
    CHECK(answer(b, proto::FortressAction::Bless, 1).error == std::uint16_t(FortressError::NotUnion));
    CHECK(answer(a, proto::FortressAction::Bless, 9).error == std::uint16_t(FortressError::NoBless));
    CHECK(answer(a, proto::FortressAction::Bless, 1).ok);
    proto::BuffAdded buff;
    do REQUIRE(a.c->Expect(buff));
    while (buff.skill != kBlessSkill);
    CHECK(answer(a, proto::FortressAction::Bless, 1).error == std::uint16_t(FortressError::BlessWait));
    // The GP history: the hire's and the blessing's rows (reasons 2 and 0).
    rig.Settle();
    for (int i = 0; i < 50; ++i) rig.pump();
    manage(a, proto::FortressAction::GpHistory);
    const auto history = Next<proto::GuildGpHistory>(a);
    std::set<int> reasons;
    for (const proto::GuildGpRecord& r : history.rows) reasons.insert(r.reason);
    CHECK(reasons.contains(game::kGpReasonNpc));
    CHECK(reasons.contains(game::kGpReasonFortressBuff));
    // Hiring is refused in the war (0x2818).
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Notice(a, proto::FortressNoticeKind::WarBegin);
    CHECK(answer(a, proto::FortressAction::Hire, 4).error == std::uint16_t(FortressError::InWar));
    rig.game->Stop();
}

TEST_CASE("Fortress smith: three repair kits made at 0.85 with a Production administrator, one work a side, collected into the bag (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    constexpr std::uint32_t kKit = 10594; // ITEM_FORT_REPAIR_KIT_01: 11,667 gold, 213 GP, 4 minutes each
    Rig rig(t, characters, skills);
    auto rules = std::make_shared<game::CharacterRules>(*rig.config.rules);
    rules->items = items;
    rig.config.rules = rules;
    rig.config.fortress.forges.push_back({1, kKit, 11'667, 213, 4, false});
    rig.config.fortress.forgeSecondsPerMinute = 0.25; // 612 s of the game's in 2.55 s
    auto [a, b] = TwoGuilds(rig, t, {}, {});
    // A Production administrator among the Keepers (one of the names).
    rig.game->Stop();
    REQUIRE(t.Db().Query("UPDATE _guildmember SET siegeauthority = 8 WHERE charid = (SELECT min(charid) FROM _guildmember WHERE guildid = 201 AND memberclass <> 0)").Ok());
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    rig.Start();
    a = rig.Join("Keeper");
    b = rig.Join("Raider");
    const auto forge = [&](Player& p, proto::FortressAction action, std::uint32_t item = 0, std::uint32_t count = 0) {
        p.c->Send(proto::FortressManage{p.smith, action, 1, item, count});
        return Answer(p, action);
    };
    const std::int64_t points = t.Db().Query("SELECT gatheredsp FROM _guild WHERE id = 201").Int(0, 0);
    // The Raiders may not (0x2806); 21 refused (0x2816); 3 begun: 29,750 gold and 543 GP (x 0.85).
    CHECK(forge(b, proto::FortressAction::ForgeStart, kKit, 3).error == std::uint16_t(FortressError::NotHolder));
    CHECK(forge(a, proto::FortressAction::ForgeStart, kKit, 21).error == std::uint16_t(FortressError::ForgeCount));
    CHECK(forge(a, proto::FortressAction::ForgeCollect).error == std::uint16_t(FortressError::ForgeNone));
    const auto begun = forge(a, proto::FortressAction::ForgeStart, kKit, 3);
    CHECK(begun.ok);
    proto::FortressForge work = Next<proto::FortressForge>(a);
    CHECK(work.has);
    CHECK(work.count == 3);
    CHECK_FALSE(work.done);
    // A second work at the smith (0x280A); not done yet (0x281A).
    CHECK(forge(a, proto::FortressAction::ForgeStart, kKit, 1).error == std::uint16_t(FortressError::ForgeBusy));
    CHECK(forge(a, proto::FortressAction::ForgeCollect).error == std::uint16_t(FortressError::ForgeNotDone));
    rig.Settle();
    CHECK(t.Db().Query("SELECT remaingold FROM _char WHERE charname16 = 'Keeper'").Int(0, 0) == 3'000'000 - 29'750);
    CHECK(t.Db().Query("SELECT amount FROM _siegefortressitemforge WHERE fortressid = 1 AND itemrefid = $1", {std::int64_t(kKit)}).Int(0, 0) == 3);
    // Its time (612 s of the game's at 0.85): the guild told it is done, then collected: 4 too many (0x281B), 3 into the bag.
    proto::FortressNotice done = Notice(a, proto::FortressNoticeKind::ForgeDone, 20s);
    CHECK(done.value == kKit);
    CHECK(forge(a, proto::FortressAction::ForgeCollect, 0, 4).error == std::uint16_t(FortressError::ForgeTooMany));
    CHECK(forge(a, proto::FortressAction::ForgeCollect, 0, 3).ok);
    rig.Settle();
    for (int i = 0; i < 50; ++i) rig.pump();
    CHECK(t.Db().Query("SELECT coalesce(sum(i.data), 0) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2",
                       {a.character, std::int64_t(kKit)})
              .Int(0, 0) == 3);
    CHECK(t.Db().Query("SELECT count(*) FROM _siegefortressitemforge").Int(0, 0) == 0);
    CHECK(points - t.Db().Query("SELECT gatheredsp FROM _guild WHERE id = 201").Int(0, 0) == 543);
    rig.game->Stop();
}

TEST_CASE("Fortress war: siege quests' SP experience and items, the war's rewards, the records kept over a restart (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    constexpr std::uint32_t kEvasion = 35881, kTaeseScroll = 24995, kMobExp = 777; // level 30's GUST_Mob_Exp here (ours)
    Rig rig(t, characters, skills);
    auto rules = std::make_shared<game::CharacterRules>(*rig.config.rules);
    rules->items = items;
    rig.config.rules = rules;
    rig.config.monsterExperience.assign(141, 0);
    rig.config.monsterExperience[30] = kMobExp;
    // The export's quests 1 (a tower: 15) and, ours for the test, a player's kill at one (an item and 10).
    rig.config.fortress.quests.push_back({1, 1, 1, false, {{2, 0, 15}}});
    rig.config.fortress.quests.push_back({9, 7, 1, true, {{1, kEvasion, 1}, {2, 0, 10}}});
    rig.config.fortress.rewards.push_back({1, 1, kTaeseScroll, 1});
    rig.config.fortress.rewards.push_back({1, 5, 60, 0});
    rig.config.fortress.battleRanks.push_back({1, 1, 21234});
    auto [a, b] = TwoGuilds(rig, t, {}, {});
    server::Fortresses* forts = const_cast<server::Fortresses*>(rig.game->GetFortresses());
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Notice(a, proto::FortressNoticeKind::WarBegin);
    // The Raider breaks the tower (no party): 15 x 777 as SP experience.
    const std::uint32_t tower = forts->Structures(1, game::StructureKind::GuardTower)[0];
    b.c->Send(proto::AttackTarget{tower});
    proto::ExperienceGained sp;
    do REQUIRE(b.c->Expect(sp, 30s));
    while (sp.skillExperience != 15 * kMobExp);
    // A player's kill: the quest's item and 10 x 777; his rank 1.
    forts->PlayerKilled(b.character, a.character);
    do REQUIRE(b.c->Expect(sp));
    while (sp.skillExperience != 10 * kMobExp);
    proto::FortressScore score = Next<proto::FortressScore>(b);
    CHECK(score.kills == 1);
    CHECK(score.rank == 1);
    rig.Settle();
    for (int i = 0; i < 50; ++i) rig.pump();
    const auto count = [&](const Player& p, std::uint32_t item) {
        return t.Db().Query("SELECT coalesce(sum(i.data), 0) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2",
                            {p.character, std::int64_t(item)})
            .Int(0, 0);
    };
    CHECK(count(b, kEvasion) == 1);
    // A restart in the war: the record read back; the next kill makes two.
    rig.game->Stop();
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    rig.Start();
    a = rig.Join("Keeper");
    b = rig.Join("Raider");
    forts = const_cast<server::Fortresses*>(rig.game->GetFortresses());
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Notice(a, proto::FortressNoticeKind::WarBegin);
    forts->PlayerKilled(b.character, a.character);
    do score = Next<proto::FortressScore>(b);
    while (score.kills == 0);
    CHECK(score.kills == 2);
    CHECK(score.rank == 1);
    // The war's end, the Keepers keeping Jangan: their master's scroll (0x385F 0x13) and 60 x 777 to him in the fortress.
    a.c->Send(proto::FortressDev{proto::FortressDevAction::EndWar});
    Notice(a, proto::FortressNoticeKind::RewardMaster);
    do REQUIRE(a.c->Expect(sp));
    while (sp.skillExperience != 60 * kMobExp);
    rig.Settle();
    for (int i = 0; i < 50; ++i) rig.pump();
    CHECK(count(a, kTaeseScroll) == 1);
    rig.game->Stop();
}

TEST_CASE("Fortress manager: the Cave of Discipline: made by the holder twice a day, entered for a fee, closed by its time (game files, export)") {
    const std::filesystem::path exportDir = ExportDir();
    const auto dir = FortressGameDir();
    if (dir.empty() || !std::filesystem::exists(exportDir / "SRO_VT_SHARD" / "schema.tsv")) {
        MESSAGE("SR_GAME_DIR or SR_VSRO_EXPORT missing - skipped");
        return;
    }
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::ImportOptions options;
    options.dir = exportDir;
    options.databases = {"SRO_VT_SHARD"};
    options.replace = true;
    server::ImportReport report;
    REQUIRE_MESSAGE(server::ImportDatabases(t.Db(), options, report, error), error);
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    Rig rig(t, characters, skills);
    auto rules = std::make_shared<game::CharacterRules>(*rig.config.rules);
    rules->items = items;
    rig.config.rules = rules;
    rig.config.files = &fs;
    rig.config.fortress.dungeons.push_back({1, 61, 2, 600'000, 3'000});
    rig.config.fortress.dungeonSeconds = 10;
    auto [a, b] = TwoGuilds(rig, t, {}, {});
    const auto act = [&](Player& p, proto::FortressAction action) {
        p.c->Send(proto::FortressManage{p.manager, action, 1});
        return Answer(p, action);
    };
    // The Raiders are neither the holder's guild nor its union (0x283D); the Keepers' query: none made today.
    CHECK(act(b, proto::FortressAction::DungeonQuery).error == std::uint16_t(FortressError::NotUnion));
    a.c->Send(proto::FortressManage{a.manager, proto::FortressAction::DungeonQuery, 1});
    proto::FortressDungeon state = Next<proto::FortressDungeon>(a);
    CHECK(state.count == 0);
    CHECK_FALSE(state.open);
    // Made by the Commander: told to the guild (0x385F 0x15); a second while it is open 0x2844.
    CHECK(act(a, proto::FortressAction::DungeonCreate).ok);
    Notice(a, proto::FortressNoticeKind::DungeonCreated);
    CHECK(act(a, proto::FortressAction::DungeonCreate).error == std::uint16_t(FortressError::DungeonOpen));
    // Entered: 600,000 gold and 3,000 GP, into its layer.
    const std::int64_t points = t.Db().Query("SELECT gatheredsp FROM _guild WHERE id = 201").Int(0, 0);
    CHECK(act(a, proto::FortressAction::DungeonEnter).ok);
    proto::WorldEntered inside;
    REQUIRE(a.c->Expect(inside, 20s));
    rig.Settle();
    for (int i = 0; i < 50; ++i) rig.pump();
    CHECK(t.Db().Query("SELECT remaingold FROM _char WHERE charname16 = 'Keeper'").Int(0, 0) == 3'000'000 - 600'000);
    CHECK(points - t.Db().Query("SELECT gatheredsp FROM _guild WHERE id = 201").Int(0, 0) == 3'000);
    // Entering made the place he entered from his recall point; the leave (0x20) takes him there, and his latest
    // instance entry being this layer, he enters again free.
    CHECK(t.Db().Query("SELECT telregion FROM _char WHERE charname16 = 'Keeper'").Int(0, 0) != 0);
    CHECK(act(a, proto::FortressAction::DungeonLeave).ok);
    proto::WorldEntered outside;
    REQUIRE(a.c->Expect(outside, 20s));
    rig.Settle();
    for (int i = 0; i < 50; ++i) rig.pump();
    CHECK(outside.position.region == std::uint16_t(t.Db().Query("SELECT telregion FROM _char WHERE charname16 = 'Keeper'").Int(0, 0)));
    CHECK(act(a, proto::FortressAction::DungeonEnter).ok);
    REQUIRE(a.c->Expect(inside, 20s));
    rig.Settle();
    for (int i = 0; i < 50; ++i) rig.pump();
    CHECK(t.Db().Query("SELECT remaingold FROM _char WHERE charname16 = 'Keeper'").Int(0, 0) == 3'000'000 - 600'000);
    // Its time over: closed, told (0x16), the Keeper back outside.
    Notice(a, proto::FortressNoticeKind::DungeonClosed, 30s);
    // Made again (2 today), closed by its time; the third of the day refused (0x2843).
    CHECK(act(a, proto::FortressAction::DungeonCreate).ok);
    Notice(a, proto::FortressNoticeKind::DungeonClosed, 60s);
    CHECK(act(a, proto::FortressAction::DungeonCreate).error == std::uint16_t(FortressError::DungeonCount));
    rig.Settle();
    CHECK(t.Db().Query("SELECT createddungeoncount FROM _siegefortress WHERE fortressid = 1").Int(0, 0) == 2);
    // The next day: the count starts again.
    rig.game->Stop();
    REQUIRE(t.Db().Query("UPDATE _siegefortress SET createddungeontime = createddungeontime - interval '1 day' WHERE fortressid = 1").Ok());
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    rig.Start();
    a = rig.Join("Keeper");
    CHECK(act(a, proto::FortressAction::DungeonCreate).ok);
    rig.game->Stop();
}

TEST_CASE("Fortress war: `_Schedule`'s MainInterval_Type 4: the weeks counted from DateStart's, modulo MainInterval_TypeDate, equal to SubInterval_Weeks (the tax row 446; SM)") {
    // Row 446's shape: TypeDate 2, Weeks 2, Saturday (7) at 21:35 for 8,700 s, from 2026-01-01 (a Thursday: its week, Sunday
    // 2025-12-28 to the 3rd, is week 1; the 4th..10th week 2, its Saturday the tax's; the 11th..17th week 3, off).
    game::ScheduleRow row;
    row.define = game::kScheduleSiegeTax;
    row.dateStart = LocalAt(2026, 1, 1, 0, 0);
    row.dateEnd = LocalAt(2030, 1, 1, 0, 0);
    row.mainType = 4;
    row.typeDate = 2;
    row.dayOfWeek = 7;
    row.weeks = 2;
    row.hour = 21;
    row.minute = 35;
    row.durationSeconds = 8700;
    const std::vector<game::ScheduleRow> rows{row};
    // From Friday the 2nd (week 1: its Saturday the 3rd off) the next is Saturday the 10th; it holds the 10th's evening.
    const auto first = game::SchedulePeriod(rows, game::kScheduleSiegeTax, LocalAt(2026, 1, 2, 12, 0));
    REQUIRE(first.has_value());
    CHECK(first->start == LocalAt(2026, 1, 10, 21, 35));
    CHECK(first->end - first->start == 8700);
    const auto during = game::SchedulePeriod(rows, game::kScheduleSiegeTax, LocalAt(2026, 1, 10, 22, 0));
    REQUIRE(during.has_value());
    CHECK(during->Contains(LocalAt(2026, 1, 10, 22, 0)));
    // From the 11th (week 3, off) the next is the 24th (week 4).
    const auto next = game::SchedulePeriod(rows, game::kScheduleSiegeTax, LocalAt(2026, 1, 11, 12, 0));
    REQUIRE(next.has_value());
    CHECK(next->start == LocalAt(2026, 1, 24, 21, 35));
    // A cycle of 3 weeks run in its first (TypeDate 3, Weeks 1): weeks 1, 4, 7: the 3rd, the 24th.
    game::ScheduleRow third = row;
    third.typeDate = 3;
    third.weeks = 1;
    const std::vector<game::ScheduleRow> thirds{third};
    CHECK(game::SchedulePeriod(thirds, game::kScheduleSiegeTax, LocalAt(2026, 1, 2, 12, 0))->start == LocalAt(2026, 1, 3, 21, 35));
    CHECK(game::SchedulePeriod(thirds, game::kScheduleSiegeTax, LocalAt(2026, 1, 4, 12, 0))->start == LocalAt(2026, 1, 24, 21, 35));
}

TEST_CASE("Fortress: the tax on a price: the rate's percent cut toward 0, a positive rate waived for the holder's, a negative one for everyone") {


    CHECK(sr::game::FortressTax(15'320, 10, false) == 1'532);
    CHECK(sr::game::FortressTax(383, 7, false) == 26); // 26.81 cut
    CHECK(sr::game::FortressTax(15'320, 10, true) == 0);
    CHECK(sr::game::FortressTax(15'320, -10, false) == -1'532);
    CHECK(sr::game::FortressTax(15'320, -10, true) == -1'532);
    CHECK(sr::game::FortressTax(383, -7, false) == -26);
    CHECK(sr::game::FortressTax(15'320, 0, false) == 0);
}

TEST_CASE("Fortress tax: of the export's 42 objects naming a fortress, only the 15 naming one in service are taxed (export)") {
    const std::filesystem::path exportDir = ExportDir();
    if (!std::filesystem::exists(exportDir / "SRO_VT_SHARD" / "schema.tsv")) {
        MESSAGE("SR_VSRO_EXPORT missing - skipped");
        return;
    }
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::ImportOptions options;
    options.dir = exportDir;
    options.databases = {"SRO_VT_SHARD"};
    options.replace = true;
    server::ImportReport report;
    REQUIRE_MESSAGE(server::ImportDatabases(t.Db(), options, report, error), error);
    // Every object row naming a fortress in AssocFile1_128 (smiths, armour, potions, accessories, specialty, gates, ferries).
    CHECK(t.Db().Query("SELECT count(*) FROM _refobjcommon WHERE assocfile1_128 LIKE 'FORTRESS!_%' ESCAPE '!'").Int(0, 0) == 42);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> taxed;
    REQUIRE_MESSAGE(server::LoadTaxedObjects(t.Db(), taxed, error), error);
    CHECK(taxed.size() == 15);
    std::map<std::uint32_t, int> byFortress;
    for (const auto& [object, fortress] : taxed) ++byFortress[fortress];
    CHECK(byFortress == std::map<std::uint32_t, int>{{1, 6}, {3, 6}, {6, 3}}); // Jangan, Hotan, Bijeokdan
    // Donwhang's, Constantinople's, Samarkand's, Heukmakdan's and the Evil Order's 27 get none.
    const std::int64_t potion = t.Db().Query("SELECT id FROM _refobjcommon WHERE codename128 = 'NPC_CH_POTION'").Int(0, 0);
    CHECK(std::any_of(taxed.begin(), taxed.end(), [&](const auto& p) { return p.first == std::uint32_t(potion) && p.second == 1; }));
    const std::int64_t constantinople = t.Db().Query("SELECT id FROM _refobjcommon WHERE assocfile1_128 = 'FORTRESS_CONSTANTINOPLE' LIMIT 1").Int(0, 0);
    CHECK(std::none_of(taxed.begin(), taxed.end(), [&](const auto& p) { return p.first == std::uint32_t(constantinople); }));
}

TEST_CASE("Fortress tax at the manager: the query, a rate and a levy by the holder's master in the tax period (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    REQUIRE(characters->Find(kManager) != nullptr);

    Rig rig(t, characters, skills);
    rig.Settle();
    {
        Player a = rig.Join("Holder");
        Player e = rig.Join("Member");
        Player d = rig.Join("Outsider");
        MakeGuild(t, 101, "Holders", 3, a, 5, &e);
    }
    // Jangan held by the guild, 50,000 gathered, no rate.
    REQUIRE(t.Db().Query("UPDATE _siegefortress SET guildid = 101, tax = 50000, taxratio = 0 WHERE fortressid = 1").Ok());
    rig.Restart();
    Player a = rig.Join("Holder");
    Player e = rig.Join("Member");
    Player d = rig.Join("Outsider");

    // Anyone may ask; out of the tax period.
    d.c->Send(proto::FortressTaxQuery{d.manager});
    auto info = Next<proto::FortressTaxInfo>(d);
    CHECK(info.fortress == 1);
    CHECK(info.rate == 0);
    CHECK(info.tax == 50'000);
    CHECK_FALSE(info.period);
    // Refusals: the range first, then the period.
    a.c->Send(proto::FortressTaxRate{a.manager, 21});
    CHECK(Answer(a, proto::FortressAction::TaxRate).error == std::uint16_t(FortressError::RateRange));
    a.c->Send(proto::FortressTaxRate{a.manager, 5});
    CHECK(Answer(a, proto::FortressAction::TaxRate).error == std::uint16_t(FortressError::NotTaxPeriod));
    a.c->Send(proto::FortressTaxLevy{a.manager, 10'000});
    CHECK(Answer(a, proto::FortressAction::TaxLevy).error == std::uint16_t(FortressError::NotTaxPeriod));
    // The tax period (the GM's /SIEGE STARTTAX).
    a.c->Send(proto::FortressDev{proto::FortressDevAction::StartTax});
    a.c->Send(proto::FortressTaxQuery{a.manager});
    CHECK(Next<proto::FortressTaxInfo>(a).period);
    a.c->Send(proto::FortressTaxRate{a.manager, 0});
    CHECK(Answer(a, proto::FortressAction::TaxRate).error == std::uint16_t(FortressError::SameRate));
    d.c->Send(proto::FortressTaxRate{d.manager, 5});
    CHECK(Answer(d, proto::FortressAction::TaxRate).error == std::uint16_t(FortressError::NotHolder));
    e.c->Send(proto::FortressTaxRate{e.manager, 5});
    CHECK(Answer(e, proto::FortressAction::TaxRate).error == std::uint16_t(FortressError::OnlyMaster));
    a.c->Send(proto::FortressTaxRate{a.official, 5}); // not the manager
    CHECK(Answer(a, proto::FortressAction::TaxRate).error == std::uint16_t(FortressError::NoFortress));
    // The master sets 5 %: saved, told.
    a.c->Send(proto::FortressTaxRate{a.manager, 5});
    CHECK(Answer(a, proto::FortressAction::TaxRate).ok);
    info = Next<proto::FortressTaxInfo>(a);
    CHECK(info.rate == 5);
    rig.Settle();
    CHECK(t.Db().Query("SELECT taxratio FROM _siegefortress WHERE fortressid = 1").Int(0, 0) == 5);
    // A levy beyond what was gathered fails; within it, the gold to the master.
    a.c->Send(proto::FortressTaxLevy{a.manager, 60'000});
    CHECK_FALSE(Answer(a, proto::FortressAction::TaxLevy).ok);
    e.c->Send(proto::FortressTaxLevy{e.manager, 20'000});
    CHECK(Answer(e, proto::FortressAction::TaxLevy).error == std::uint16_t(FortressError::OnlyMaster));
    a.c->Send(proto::FortressTaxLevy{a.manager, 20'000});
    CHECK(Answer(a, proto::FortressAction::TaxLevy).ok);
    CHECK(Next<proto::GoldChanged>(a).gold == 3'020'000);
    CHECK(Next<proto::FortressTaxInfo>(a).tax == 30'000);
    rig.Settle();
    CHECK(t.Db().Query("SELECT tax FROM _siegefortress WHERE fortressid = 1").Int(0, 0) == 30'000);
    // The period over: no more rates.
    a.c->Send(proto::FortressDev{proto::FortressDevAction::EndTax});
    a.c->Send(proto::FortressTaxRate{a.manager, -5});
    CHECK(Answer(a, proto::FortressAction::TaxRate).error == std::uint16_t(FortressError::NotTaxPeriod));
}

TEST_CASE("Fortress war: the client's barricade and war camp zones, each at its objectstring.ifo spot (game files)") {
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    assets::WorldMapData data;
    REQUIRE_MESSAGE(assets::LoadWorldMapData(fs, data, error), error);
    // eventzonedata.txt (the client's copy of _RefEventZone): 54 barricade zones (Jangan 17, Hotan 21, Bijeokdan 16) and 26 war
    // camp foundations (10, 8, 8), every one placed by its name.
    std::map<std::pair<std::string, std::string>, int> counts;
    for (const assets::FortressZoneRow& z : data.zones) {
        ++counts[{z.event, z.world}];
        CHECK_MESSAGE(z.placed, z.name);
    }
    CHECK(data.zones.size() == 80);
    CHECK(counts[{"BARRICADE", "INS_FORT_JA"}] == 17);
    CHECK(counts[{"BARRICADE", "INS_FORT_HT"}] == 21);
    CHECK(counts[{"BARRICADE", "INS_FORT_BJ"}] == 16);
    CHECK(counts[{"HEADQUARTERS", "INS_FORT_JA"}] == 10);
    CHECK(counts[{"HEADQUARTERS", "INS_FORT_HT"}] == 8);
    CHECK(counts[{"HEADQUARTERS", "INS_FORT_BJ"}] == 8);
    // Jangan's first barricade zone: the database's _RefEventZone 109, its structure STRUCTURE_BARRICADE_05, in Jangan's world.
    const auto it = std::find_if(data.zones.begin(), data.zones.end(), [](const assets::FortressZoneRow& z) { return z.id == 109; });
    REQUIRE(it != data.zones.end());
    CHECK(it->name == "STRUCTURE_POS_JA_BARRICADE_01");
    CHECK(it->structure == "STRUCTURE_BARRICADE_05");
    CHECK(it->region == assets::RegionId(69, 69));
}

TEST_CASE("An NPC's repair: the price of the missing durability") {
    assets::ItemRow sword; // ITEM_CH_SWORD_01_A's: CostRepair 198, CostRevive 297
    sword.typeId = {3, 1, 6, 2};
    sword.costRepair = 198;
    sword.costRevive = 297;
    // 52 of 62 missing at 198 / 62 = 3.19 a point: 166.06, the server's 166, the client's 166.
    game::RepairPrice p = game::PriceRepair(sword, 10, 62, 1'000'000, false);
    CHECK(p.points == 52);
    CHECK(p.gold == 166);
    CHECK_FALSE(p.revive);
    CHECK(game::PriceRepair(sword, 10, 62, 0, true).gold == 166);
    // 100 gold pays for 31 points (31 x 3.19 = 99.0).
    p = game::PriceRepair(sword, 10, 62, 100, false);
    CHECK(p.points == 31);
    CHECK(p.gold == 99);
    // Broken: from 1, its revival besides; the first point back too.
    p = game::PriceRepair(sword, 0, 62, 1'000'000, false);
    CHECK(p.revive);
    CHECK(p.points == 62);
    CHECK(p.gold == 194 + 297);
    // Gold under the revival's: nothing.
    CHECK(game::PriceRepair(sword, 0, 62, 200, false).points == 0);
    // Whole: nothing. A cheap row: at least 1 gold a point.
    CHECK(game::PriceRepair(sword, 62, 62, 1'000'000, false).gold == 0);
    sword.costRepair = 10;
    CHECK(game::PriceRepair(sword, 52, 62, 1'000'000, false).gold == 10);
    // Accessories are not repaired.
    assets::ItemRow ring;
    ring.typeId = {3, 1, 5, 3};
    CHECK_FALSE(game::Repairable(ring, game::ItemInstance{}, nullptr));
    CHECK(game::Repairable(sword, game::ItemInstance{}, nullptr));
}

TEST_CASE("Fortress smith: Item repairing mends the bag's and the worn pieces for gold; the battle aide's barricade (0x0A) (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto dir = FortressGameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir.string(), error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    auto items = std::make_shared<assets::ItemTable>();
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    constexpr std::uint32_t kSword = 71, kBarricade = 19550, kCampBase = 19543; // ITEM_CH_SWORD_01_A; STRUCTURE_BARRICADE_01, _HEADQUARTERS_00
    Rig rig(t, characters, skills);
    auto rules = std::make_shared<game::CharacterRules>(*rig.config.rules);
    rules->items = items;
    rig.config.rules = rules;
    server::FortressSpec& f = rig.config.fortresses[0];
    f.structures.push_back({200, kBarricade, game::StructureKind::Barricade, "SN_STRUCTURE_BARRICADE_01", 80000, {kStart.region, 950.0f, 50.0f, 450.0f}, 0});
    f.structures.push_back({210, kCampBase, game::StructureKind::Headquarters, "SN_STRUCTURE_HEADQUARTERS_00", 0, {kStart.region, 980.0f, 50.0f, 440.0f}, 0});
    auto [a, b] = TwoGuilds(rig, t, {}, {});
    // A worn-down sword (10 of its 62) in the Keepers' master's bag.
    rig.game->Stop();
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    server::StoredItem sword;
    sword.refId = kSword;
    sword.durability = 10;
    REQUIRE_MESSAGE(server::GiveItems(t.Db(), 1, "Keeper", {sword}, 0, error) == server::GiveResult::Done, error);
    rig.Start();
    a = rig.Join("Keeper");
    b = rig.Join("Raider");
    const std::uint8_t place = SlotOf(t, a, kSword);
    REQUIRE(place >= game::slot::kWorn);
    // Not a repairing NPC (the original's 5).
    a.c->Send(proto::NpcRepair{a.manager, 2, 0});
    proto::NpcRepaired r = Next<proto::NpcRepaired>(a);
    CHECK_FALSE(r.ok);
    CHECK(r.code == 5);
    // At the smith: every piece mended, the sword's 52 points for 166 gold.
    a.c->Send(proto::NpcRepair{a.smith, 2, 0});
    CHECK(Next<proto::GoldChanged>(a).gold == 3'000'000 - 166);
    r = Next<proto::NpcRepaired>(a);
    CHECK(r.ok);
    CHECK(r.gold == 166);
    REQUIRE(r.changed.size() == 1);
    CHECK(r.changed[0].slot == place);
    CHECK(r.changed[0].durability == 62);
    rig.Settle();
    for (int i = 0; i < 50; ++i) rig.pump();
    CHECK(t.Db().Query("SELECT i.data FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND v.slot = $2", {a.character, int(place)}).Int(0, 0) ==
          62);
    CHECK(t.Db().Query("SELECT remaingold FROM _char WHERE charname16 = 'Keeper'").Int(0, 0) == 3'000'000 - 166);
    // Nothing more to mend: done for nothing.
    a.c->Send(proto::NpcRepair{a.smith, 2, 0});
    CHECK(Next<proto::GoldChanged>(a).gold == 3'000'000 - 166);
    r = Next<proto::NpcRepaired>(a);
    CHECK(r.ok);
    CHECK(r.changed.empty());
    // No gold at all (0x1C07).
    rig.game->Stop();
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    REQUIRE(t.Db().Query("UPDATE _char SET remaingold = 0 WHERE charname16 = 'Raider'").Ok());
    rig.Start();
    a = rig.Join("Keeper");
    b = rig.Join("Raider");
    b.c->Send(proto::NpcRepair{b.smith, 2, 0});
    r = Next<proto::NpcRepaired>(b);
    CHECK_FALSE(r.ok);
    CHECK(r.code == 0x1C07);

    // The battle aide's barricade (0x0A), in the original's order: a battle aide (3), the zone (3), a barricade's zone (0x281F),
    // the zone of the fortress asked (0x2820); then built for the asker's guild, with no period, guild or position asked.
    const auto build = [&](Player& p, std::uint32_t npc, std::uint32_t fortress, std::uint32_t zone) {
        p.c->Send(proto::FortressBuild{npc, fortress, zone});
        return Answer(p, proto::FortressAction::Build);
    };
    CHECK(build(b, b.smith, 1, 200).error == std::uint16_t(FortressError::NoFortress));
    CHECK(build(b, b.aide, 1, 999).error == std::uint16_t(FortressError::NoFortress));
    CHECK(build(b, b.aide, 1, 210).error == std::uint16_t(FortressError::BuildNotBarricade));
    CHECK(build(b, b.aide, 2, 200).error == std::uint16_t(FortressError::BuildWrongFortress));
    const proto::FortressAnswer built = build(b, b.aide, 1, 200);
    CHECK(built.ok);
    CHECK(built.zone == 200);
    CHECK(built.amount == kBarricade);
    const server::Fortresses* forts = rig.game->GetFortresses();
    REQUIRE(forts->Structures(1, game::StructureKind::Barricade).size() == 1);
    CHECK(forts->Structures(1, game::StructureKind::Barricade)[0] != 0);
    rig.Settle();
    CHECK(t.Db().Query("SELECT ownerguildid FROM _siegefortressstruct WHERE refeventstructid = 200").Int(0, 0) == 202);
    rig.game->Stop();
}
