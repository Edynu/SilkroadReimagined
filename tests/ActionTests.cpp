// The Action page's actions through the game server: sitting and standing as
// everyone sees them, the recovery while sitting, the helper's mark, the walking pace, trace (set
// SR_DB_URL; otherwise skipped).

#include <doctest/doctest.h>

#include "sr/game/Places.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

struct ActionServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit ActionServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        config.rules = rules;
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.databaseThreads = 1;
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::int64_t character = 0;
        proto::WorldEntered entered;
    };
    // A character of `level` at (x, z) of Jangan's region, `hp` of its health, entered (`before` given its id first).
    Player Make(const std::string& name, int level, float x, std::uint32_t hp = 0, const std::function<void(std::int64_t)>& before = {}) {
        Player p;
        REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        p.client = std::make_unique<Client>(net, game.Port(), [this] { game.Poll(2ms); });
        REQUIRE(p.client->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), account, config.id, 30, error);
        p.client->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.client->Expect(entered));
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        p.client->Send(m);
        proto::LobbyResult r;
        REQUIRE(p.client->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        const std::vector<server::CharacterRow> list = server::ListCharacters(t.Db(), account, config.id, 0, ok);
        REQUIRE(list.size() == 1);
        p.character = list[0].id;
        REQUIRE(server::SaveProgress(t.Db(), {{p.character, std::uint8_t(level), 0, 0, 0, 40, 40, 0}}));
        server::CharacterRow::Place here;
        here.region = assets::RegionId(168, 97);
        here.x = x;
        here.y = 0;
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
        if (hp != 0) REQUIRE(server::SaveVitals(t.Db(), p.character, hp, hp));
        if (before) before(p.character);
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        REQUIRE(p.client->Expect(p.entered));
        REQUIRE(p.entered.status == proto::WorldStatus::Ok);
        return p;
    }
};

// The next message of `T` about `entity` (others skipped).
template <typename T>
bool ExpectAbout(Client& c, std::uint32_t entity, T& out, std::chrono::milliseconds wait = 10s) {
    const auto end = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < end) {
        if (!c.Expect(out, std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()))) return false;
        if (out.entity == entity) return true;
    }
    return false;
}

} // namespace

TEST_CASE("Game server: sitting and standing as everyone sees them, the recovery while sitting, a walk standing up") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ActionServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Sitter", 20, 1000, 20);
    auto b = s.Make("Watcher", 20, 1040);
    proto::EntityAppeared seen;
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, seen));

    // Sit: through the change for 1.5 s, then sitting; the watcher sees both.
    a.client->Send(proto::SetMotion{proto::kMotionSit});
    proto::EntityMotion m;
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, m));
    CHECK(m.motion == proto::kMotionChanging);
    CHECK(m.to == proto::kMotionSit);
    const auto changing = std::chrono::steady_clock::now();
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, m));
    CHECK(m.motion == proto::kMotionSit);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - changing).count();
    CHECK(took == doctest::Approx(1.5).epsilon(0.25));

    // Sitting: 8 % of its most a recovery tick (at most half of it; every 4 s).
    // (the ticks told while it stood first: their rises are 0.8 %)
    proto::Vitals v;
    REQUIRE(a.client->Expect(v));
    while (a.client->Expect(v, 300ms)) {
    }
    std::uint32_t before = v.hp;
    const std::uint32_t most = v.maxHp;
    bool rose = false;
    for (int i = 0; i < 3 && !rose; ++i) {
        REQUIRE(a.client->Expect(v, 6s));
        rose = v.hp > before;
        if (rose) CHECK(v.hp - before == std::min(most / 2, std::max(1u, std::uint32_t(double(most) * 8.0 / 100.0))));
        before = v.hp;
    }
    CHECK(rose);

    // A newcomer sees it sitting.
    auto c = s.Make("Newcomer", 20, 1060);
    REQUIRE(ExpectAbout(*c.client, a.entered.entity, seen));
    REQUIRE(ExpectAbout(*c.client, a.entered.entity, m));
    CHECK(m.motion == proto::kMotionSit);

    // A walk of its own stands it up at once.
    a.client->Send(proto::MoveTo{{a.entered.position.region, 1100, 0, 1000}});
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, m));
    CHECK(m.motion == proto::kMotionStand);
    s.game.Stop();
}

TEST_CASE("Game server: the helper's mark, saved and seen by newcomers; the walking pace; trace") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ActionServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Helper", 30, 1000);
    auto b = s.Make("Seeker", 30, 1040);
    proto::EntityAppeared seen;
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, seen));

    // The helper's mark (its beginner's mark dropped above level 19): told around and saved.
    proto::EntityMarks marks;
    a.client->Send(proto::SetMarks{proto::kMarkHelper});
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, marks));
    CHECK(marks.marks == proto::kMarkHelper);
    // Saved: a write of its own after the mark is told around, so waited for.
    const auto savedMarks = [&] { return t.Db().Query("SELECT autoinvestexp FROM sro_vt_shard._char WHERE charid = $1", {a.character}).Int(0, 0); };
    for (int i = 0; i < 200 && savedMarks() != proto::kMarkHelper; ++i) s.game.Poll(5ms);
    CHECK(savedMarks() == proto::kMarkHelper);
    // The beginner's mark above level 19: refused (nothing changes).
    a.client->Send(proto::SetMarks{std::uint8_t(proto::kMarkBeginner | proto::kMarkHelper)});
    // A newcomer sees the mark after its appearance.
    auto c = s.Make("Later", 30, 1060);
    REQUIRE(ExpectAbout(*c.client, a.entered.entity, seen));
    REQUIRE(ExpectAbout(*c.client, a.entered.entity, marks));
    CHECK(marks.marks == proto::kMarkHelper);

    // Walking: its next move at its walk speed.
    proto::EntityMotion m;
    a.client->Send(proto::SetMotion{proto::kMotionWalk});
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, m));
    CHECK(m.walking == 1);
    a.client->Send(proto::MoveTo{{a.entered.position.region, 1010, 0, 1000}});
    proto::EntityMoved moved;
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, moved));
    CHECK(moved.speed == doctest::Approx(s.config.world.walkSpeed));
    a.client->Send(proto::SetMotion{proto::kMotionRun});
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, m));
    CHECK(m.walking == 0);

    // Trace: the seeker follows the helper as it runs off, and stops near it (as the third player sees them).
    REQUIRE(ExpectAbout(*c.client, b.entered.entity, seen));
    b.client->Send(proto::Trace{a.entered.entity});
    a.client->Send(proto::MoveTo{{a.entered.position.region, 1400, 0, 1000}});
    double last = 1e9;
    const auto until = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < until && last >= 120.0) {
        if (!c.client->Expect(moved, 5s)) break;
        if (moved.entity == b.entered.entity) last = std::fabs(moved.to.x - 1400.0f);
    }
    CHECK(last < 120.0);
    s.game.Stop();
}

TEST_CASE("Game server: an emote is told as it is to those who see the player and the player itself") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ActionServer s(t);
    s.config.devCommands = true;
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Greeter", 30, 1000);
    auto b = s.Make("Onlooker", 30, 1040);
    proto::EntityAppeared seen;
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, seen));

    a.client->Send(proto::Emote{6}); // laugh
    proto::EntityEmote e;
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, e));
    CHECK(e.gesture == 6);
    REQUIRE(ExpectAbout(*a.client, a.entered.entity, e));
    CHECK(e.gesture == 6);

    // Not from the dead.
    a.client->Send(proto::DevDie{});
    a.client->Send(proto::Emote{0});
    CHECK_FALSE(ExpectAbout(*b.client, a.entered.entity, e, 1500ms));
    s.game.Stop();
}

TEST_CASE("Game server: a turn where the player stands, seen by those around and by newcomers; none from the dead") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ActionServer s(t);
    s.config.devCommands = true;
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Turner", 30, 1000);
    auto b = s.Make("Watcher", 30, 1040);
    proto::EntityAppeared seen;
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, seen));

    // A quarter turn: the other sees it.
    const std::uint16_t quarter = proto::HeadingOf(1.5707963f);
    CHECK(quarter == 16384);
    a.client->Send(proto::Turn{quarter});
    proto::EntityTurned turned;
    REQUIRE(ExpectAbout(*b.client, a.entered.entity, turned));
    CHECK(turned.heading == quarter);

    // One who comes into sight later sees it facing that way.
    auto c = s.Make("Latecomer", 30, 1020);
    proto::EntityAppeared late;
    REQUIRE(ExpectAbout(*c.client, a.entered.entity, late));
    CHECK(std::abs(late.yaw - 1.5707963f) < 0.001f);

    // Not from the dead.
    a.client->Send(proto::DevDie{});
    a.client->Send(proto::Turn{proto::HeadingOf(3.1415926f)});
    CHECK_FALSE(ExpectAbout(*b.client, a.entered.entity, turned, 1500ms));
    s.game.Stop();
}

TEST_CASE("Game server: free PvP: a team after its wait, seen by others; the original's refusals; foes by team") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    ActionServer s(t);
    s.config.devCommands = true;
    s.config.freePvpSeconds = 0.3;
    // The PK switch on: no PK of level 30 innocents, the free PvP's foes alone.
    s.config.pkProhibited = true;
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Hawk", 30, 1000);
    auto b = s.Make("Turtle", 30, 1040);
    auto c = s.Make("Giraffe", 30, 1060);
    const std::uint32_t ea = a.entered.entity, eb = b.entered.entity, ec = c.entered.entity;
    proto::EntityAppeared seen;
    REQUIRE(ExpectAbout(*b.client, ea, seen));
    // A developer's strike lands only on a foe (the server's Hostile): its damage told back.
    const auto strikes = [&](ActionServer::Player& from, std::uint32_t target) {
        proto::EntityDamaged d;
        while (from.client->Expect(d, 100ms)) {
        } // news of earlier strikes seen
        from.client->Send(proto::DevStrike{target, 1});
        const auto end = std::chrono::steady_clock::now() + 800ms;
        while (std::chrono::steady_clock::now() < end) {
            if (!from.client->Expect(d, 200ms)) continue;
            if (d.target == target) return true;
        }
        return false;
    };
    CHECK_FALSE(strikes(a, eb)); // nobody in a team: no foes

    // A team: the change told at once, the team after its wait.
    a.client->Send(proto::FreePvp{1});
    proto::EntityFreePvp f;
    REQUIRE(ExpectAbout(*b.client, ea, f));
    CHECK(f.changing == 1);
    CHECK(f.to == 1);
    REQUIRE(ExpectAbout(*b.client, ea, f));
    CHECK(f.changing == 0);
    CHECK(f.team == 1);
    a.client->Send(proto::FreePvp{1}); // the team it has
    proto::FreePvpAnswer refused;
    REQUIRE(a.client->Expect(refused));
    CHECK(refused.code == proto::FreePvpCode::CannotChange);
    a.client->Send(proto::FreePvp{9});
    REQUIRE(a.client->Expect(refused));
    CHECK(refused.code == proto::FreePvpCode::CannotChange);

    // A walk gives a change up.
    b.client->Send(proto::FreePvp{2});
    REQUIRE(ExpectAbout(*b.client, eb, f));
    CHECK(f.changing == 1);
    b.client->Send(proto::MoveTo{proto::FromFlat({1080.0, 1000.0}, 0.0f)});
    REQUIRE(b.client->Expect(refused));
    CHECK(refused.code == proto::FreePvpCode::Cancelled);
    REQUIRE(ExpectAbout(*b.client, eb, f));
    CHECK(f.changing == 0);
    CHECK(f.team == 0);
    CHECK_FALSE(strikes(a, eb));
    // Teams 1 and 2: foes.
    b.client->Send(proto::FreePvp{2});
    REQUIRE(ExpectAbout(*b.client, eb, f));
    REQUIRE(ExpectAbout(*b.client, eb, f));
    CHECK(f.team == 2);
    CHECK(strikes(a, eb));
    CHECK(strikes(b, ea));
    // Two Giraffes (5) are foes of each other too; two of one other team are not.
    c.client->Send(proto::FreePvp{1});
    REQUIRE(ExpectAbout(*c.client, ec, f));
    REQUIRE(ExpectAbout(*c.client, ec, f));
    CHECK_FALSE(strikes(c, ea)); // both Red Hawk
    c.client->Send(proto::FreePvp{5});
    REQUIRE(ExpectAbout(*c.client, ec, f));
    REQUIRE(ExpectAbout(*c.client, ec, f));
    CHECK(f.team == 5);
    CHECK(strikes(c, ea));
    // Leaving the mode: no foes any more.
    a.client->Send(proto::FreePvp{0});
    REQUIRE(ExpectAbout(*b.client, ea, f));
    CHECK(f.changing == 1);
    REQUIRE(ExpectAbout(*b.client, ea, f));
    CHECK(f.team == 0);
    CHECK_FALSE(strikes(b, ea));
    s.game.Stop();
}

TEST_CASE("Game server: a knock-back on a player (the KB socket stone in free PvP): pushed away, no walk or skill till it stands (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    const assets::SkillRow* kb = skills->Find(31250); // SKILL_ITEM_ETC_SOCKET_STONE_KB_E_05: kb 80 % at 50
    REQUIRE(kb != nullptr);
    ActionServer s(t);
    s.config.freePvpSeconds = 0.2;
    s.config.skills = skills;
    s.config.skillSeed = 3; // its 80 % roll repeatable
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Hawk", 30, 1000, 0, [&](std::int64_t character) { REQUIRE(server::SaveSkill(t.Db(), character, 0, kb->id)); });
    auto b = s.Make("Turtle", 30, 1012);
    const std::uint32_t ea = a.entered.entity, eb = b.entered.entity;
    // Teams 1 and 2: foes (the change, then the team after its wait).
    proto::EntityFreePvp f;
    a.client->Send(proto::FreePvp{1});
    REQUIRE(ExpectAbout(*a.client, ea, f));
    REQUIRE(ExpectAbout(*a.client, ea, f));
    REQUIRE(f.team == 1);
    b.client->Send(proto::FreePvp{2});
    REQUIRE(ExpectAbout(*b.client, eb, f));
    REQUIRE(ExpectAbout(*b.client, eb, f));
    REQUIRE(f.team == 2);
    // The stone on Turtle: knocked back 50 away from Hawk, for 2 s.
    a.client->Send(proto::UseSkill{kb->id, eb});
    proto::EntityKnocked k;
    REQUIRE(ExpectAbout(*b.client, eb, k));
    CHECK(k.kind == proto::KnockKind::Back);
    CHECK(k.attacker == ea);
    CHECK(k.ms == 2000);
    const proto::FlatPosition pushed = proto::ToFlat(k.to), from = proto::ToFlat(a.entered.position), was = proto::ToFlat(b.entered.position);
    CHECK(std::hypot(pushed.x - from.x, pushed.z - from.z) == doctest::Approx(std::hypot(was.x - from.x, was.z - from.z) + 50.0).epsilon(0.05));
    // Meanwhile no skill and no walk of its own.
    b.client->Send(proto::UseSkill{kb->id, ea});
    proto::SkillUseAnswer refused;
    REQUIRE(b.client->Expect(refused));
    CHECK(refused.status == proto::SkillUseStatus::NotYet);
    const proto::WorldPosition away = proto::FromFlat({pushed.x, pushed.z + 80.0}, k.to.y);
    const auto walksThere = [&](std::chrono::milliseconds wait) {
        const auto end = std::chrono::steady_clock::now() + wait;
        proto::EntityMoved m;
        while (std::chrono::steady_clock::now() < end) {
            if (!b.client->Expect(m, 100ms)) continue;
            if (m.entity == eb && std::abs(proto::ToFlat(m.to).z - (pushed.z + 80.0)) < 1.0) return true;
        }
        return false;
    };
    b.client->Send(proto::MoveTo{away});
    CHECK_FALSE(walksThere(600ms));
    // Standing again (2 s): the walk is taken.
    std::this_thread::sleep_for(1600ms);
    for (int i = 0; i < 20; ++i) s.game.Poll(2ms);
    b.client->Send(proto::MoveTo{away});
    CHECK(walksThere(2000ms));
    s.game.Stop();
}

TEST_CASE("Places: the worlds' kinds by their code names; free PvP's refusals by place and state") {
    using sr::game::PlaceKind;
    using sr::game::Refusal;
    CHECK(sr::game::PlaceKindOfWorld("INS_DEFAULT") == PlaceKind::Field);
    CHECK(sr::game::PlaceKindOfWorld("INS_FORT_JA") == PlaceKind::Fortress);
    CHECK(sr::game::PlaceKindOfWorld("INS_ARENA_FLAG_16") == PlaceKind::Arena);
    CHECK(sr::game::PlaceKindOfWorld("INS_EVENT_RANDOMMATCH") == PlaceKind::EventWorld);
    CHECK(sr::game::PlaceKindOfWorld("INS_GOD_TOGUI_35_50_LEVEL_1") == PlaceKind::ForgottenWorld);
    CHECK(sr::game::PlaceKindOfWorld("INS_PHARAOH_NOVICE") == PlaceKind::Instance);
    sr::game::PlaceTable places;
    places.Add(100, PlaceKind::Arena);
    places.Add(101, PlaceKind::Field);
    CHECK(places.Of(100) == PlaceKind::Arena);
    CHECK(places.Of(101) == PlaceKind::Field);
    CHECK(places.Of(7) == PlaceKind::Field);
    sr::game::ActivityState s;
    CHECK(sr::game::FreePvpRefusal(s) == Refusal::None);
    s.place = PlaceKind::Fortress;
    CHECK(sr::game::FreePvpRefusal(s) == Refusal::None); // a fortress's area outside its war
    s.fortressWar = true;
    CHECK(sr::game::FreePvpRefusal(s) == Refusal::FortressWar);
    s = {};
    s.place = PlaceKind::ForgottenWorld;
    CHECK(sr::game::FreePvpRefusal(s) == Refusal::ForgottenWorld);
    s.place = PlaceKind::EventWorld;
    CHECK(sr::game::FreePvpRefusal(s) == Refusal::EventWorld);
    s.riding = true;
    CHECK(sr::game::FreePvpRefusal(s) == Refusal::Riding);
    s.jobSuit = true;
    CHECK(sr::game::FreePvpRefusal(s) == Refusal::JobSuit);
}

TEST_CASE("Game server: free PvP is refused in the battle arena's maps, as the original's tables name them") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    // Jangan's region marked an arena's map (the imported _RefGame_World and _RefInstance_World_Region).
    REQUIRE(t.Db().Query("CREATE TABLE sro_vt_shard._refgame_world (id int, worldcodename128 varchar(128))").Ok());
    REQUIRE(t.Db().Query("CREATE TABLE sro_vt_shard._refinstance_world_region (worldid int, regionid smallint)").Ok());
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._refgame_world VALUES (45, 'INS_ARENA_FLAG_8')").Ok());
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._refinstance_world_region VALUES (45, $1)", {int(std::int16_t(assets::RegionId(168, 97)))}).Ok());
    ActionServer s(t);
    s.config.freePvpSeconds = 0.3;
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Arenaman", 30, 1000);
    a.client->Send(proto::FreePvp{1});
    proto::FreePvpAnswer refused;
    REQUIRE(a.client->Expect(refused));
    CHECK(refused.code == proto::FreePvpCode::CannotEnter);
    s.game.Stop();
}
