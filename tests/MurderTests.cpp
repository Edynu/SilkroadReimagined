// Murderers through the game server (steps 2-6): the hostility records and the assaulter's pink
// state, the attack check's codes, a murder's counts and the red state, a murderer's death (the rows, the item drop, the
// relief), the clocks (the 48 h keeper, the daily reset) and the murderer's refusals (set SR_DB_URL; otherwise skipped).

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/CosProtocol.h"
#include "sr/game/Pk.h"
#include "sr/game/PkProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

constexpr std::uint32_t kHerb = 900601, kKeepsake = 900602, kMonster = 900610, kHorseScroll = 900603, kHorse = 900611, kReturnScroll = 900604;

struct MurderServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    std::shared_ptr<std::atomic<std::int64_t>> clockShift = std::make_shared<std::atomic<std::int64_t>>(0);
    explicit MurderServer(TestDatabase& db) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        auto items = std::make_shared<assets::ItemTable>();
        assets::ItemRow herb;
        herb.refObjId = kHerb;
        herb.codename = "ITEM_ETC_TEST_HERB";
        herb.typeId = {3, 3, 1, 1};
        herb.inService = true;
        herb.maxStack = 50;
        items->Add(herb);
        assets::ItemRow keepsake = herb; // CanDrop 0: a death never drops it
        keepsake.refObjId = kKeepsake;
        keepsake.codename = "ITEM_ETC_TEST_KEEPSAKE";
        keepsake.canDrop = false;
        items->Add(keepsake);
        assets::ItemRow scroll = herb; // a ride's scroll (TID 3/3/3/2)
        scroll.refObjId = kHorseScroll;
        scroll.codename = "ITEM_COS_C_TEST_HORSE";
        scroll.typeId = {3, 3, 3, 2};
        scroll.paramTexts[0] = "COS_C_TEST_HORSE";
        items->Add(scroll);
        assets::ItemRow back = herb; // a return scroll
        back.refObjId = kReturnScroll;
        back.codename = "ITEM_ETC_SCROLL_RETURN_TEST";
        back.typeId = {3, 3, 3, 1};
        back.params[0] = 100;
        back.paramTexts[2] = "RESURRECT";
        items->Add(back);
        rules->items = items;
        config.rules = rules;
        auto characters = std::make_shared<assets::CharacterTable>();
        assets::ObjectRow monster;
        monster.refObjId = kMonster;
        monster.codename = "MOB_TEST_DUMMY";
        monster.typeId = {1, 2, 1, 1};
        monster.level = 60;
        monster.maxHp = 1000;
        monster.inService = true;
        characters->Add(monster);
        assets::ObjectRow horse;
        horse.refObjId = kHorse;
        horse.codename = "COS_C_TEST_HORSE";
        horse.typeId = {1, 2, 3, 1};
        horse.maxHp = 5000;
        horse.walkSpeed = 30;
        horse.runSpeed = 110;
        horse.inService = true;
        characters->Add(horse);
        config.characters = characters;
        config.levels.assign(120, 1'000'000);
        config.monsterExperience.assign(120, 1'000'000);
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.databaseThreads = 2;
        config.devCommands = true;
        config.hostileSeconds = 6;
        config.wallClock = [shift = clockShift] {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + shift->load();
        };
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::int64_t character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
    };
    // A character in Jangan's south field at (x, 1000) of `level` with `items`, entered; `before` runs on its row first.
    Player Make(const std::string& name, float x, int level, const std::vector<server::StoredItem>& items = {},
                const std::function<void(std::int64_t)>& before = {}) {
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
        server::CharacterRow::Place here;
        here.region = assets::RegionId(168, 97);
        here.x = x;
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{p.character, here}}));
        REQUIRE(server::SaveProgress(t.Db(), {{p.character, std::uint8_t(level), 0, 0, 0, 40, 40, 0}}));
        REQUIRE(server::SaveVitals(t.Db(), p.character, 100'000, 100'000));
        const std::int64_t jid = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {p.character}).Int(0, 0);
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {jid}).Ok());
        if (!items.empty()) REQUIRE(server::GiveItems(t.Db(), config.id, name, items, 0, error) == server::GiveResult::Done);
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {jid, int(config.id)}).Ok());
        if (before) before(p.character);
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.client->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        REQUIRE(p.client->Expect(p.bag));
        return p;
    }
    void Pump(int times = 50) {
        for (int i = 0; i < times; ++i) game.Poll(2ms);
    }
};

server::StoredItem Stored(std::uint32_t refId, std::uint16_t count) {
    server::StoredItem i;
    i.refId = refId;
    i.quantity = count;
    i.counted = true;
    return i;
}

template <typename T>
T Next(MurderServer::Player& p) {
    T m;
    REQUIRE(p.client->Expect(m));
    return m;
}

// The next PvP state `p` hears of `entity` (none within the time: -1).
int StateOf(MurderServer::Player& p, std::uint32_t entity, std::chrono::milliseconds wait = 5s) {
    const auto end = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < end) {
        proto::EntityPvpState m;
        if (p.client->Expect(m, 50ms) && m.entity == entity) return m.state;
    }
    return -1;
}

// Whether `p` hears `entity` take PvP state `state` within the time.
bool Hears(MurderServer::Player& p, std::uint32_t entity, int state, std::chrono::milliseconds wait = 5s) {
    const auto end = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < end) {
        proto::EntityPvpState m;
        if (p.client->Expect(m, 50ms) && m.entity == entity && m.state == state) return true;
    }
    return false;
}

// The refusal of an attack (Ok: none came).
proto::SkillUseStatus Refusal(MurderServer::Player& p, std::uint32_t target) {
    p.client->Send(proto::AttackTarget{target});
    proto::SkillUseAnswer r;
    if (!p.client->Expect(r, 600ms)) return proto::SkillUseStatus::Ok;
    return r.status;
}

// Whether `by`'s developer strike on `target` lands (seen by `target`).
bool Strikes(MurderServer& s, MurderServer::Player& by, MurderServer::Player& target, std::uint32_t damage = 1) {
    by.client->Send(proto::DevStrike{target.entity, damage});
    const auto end = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < end) {
        proto::EntityDamaged d;
        if (target.client->Expect(d, 50ms) && d.target == target.entity && d.attacker == by.entity) return true;
    }
    (void)s;
    return false;
}

proto::PkCounters LastCounters(MurderServer::Player& p, std::chrono::milliseconds wait = 2s) {
    proto::PkCounters last{255, 255, 0xFFFFFFFFu};
    proto::PkCounters c;
    const auto end = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < end) {
        if (p.client->Expect(c, 50ms)) last = c;
    }
    return last;
}

} // namespace

TEST_CASE("Game server: the assaulter: pink for the records' time, struck back and by a third freely, a level 19 refused") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    MurderServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Abel", 1000, 30);
    auto b = s.Make("Brutus", 1010, 30);
    auto c = s.Make("Cato", 1020, 30);
    auto d = s.Make("Decius", 1030, 19);
    s.Pump(100);
    // At the entry, each told its counts: all 0.
    CHECK(LastCounters(a, 300ms).points == 0);
    // An innocent strikes an innocent: an assaulter (state 1), told to those near.
    REQUIRE(Strikes(s, a, b));
    CHECK(StateOf(c, a.entity) == 1);
    // The victim strikes back freely (the assaulter holds records: his natural enemy), a third player too.
    CHECK(Refusal(b, a.entity) == proto::SkillUseStatus::Ok);
    CHECK(Strikes(s, b, a));
    CHECK(Strikes(s, c, a));
    // A level 19 on a level 30 innocent: 0x3016; a level 30 on the level 19: 0x3017.
    CHECK(Refusal(d, b.entity) == proto::SkillUseStatus::YourLevelTooLow);
    CHECK(Refusal(c, d.entity) == proto::SkillUseStatus::TargetLevelTooLow);
    // The records run out (6 s here, the original's 20): innocent again; the others too.
    CHECK(StateOf(c, a.entity, 12s) == 0);
    CHECK(Hears(a, b.entity, 0, 8s));
    s.game.Stop();
}

TEST_CASE("Game server: a murder: the counts and the red state; a murderer killed freely, nothing counted") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    MurderServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    auto a = s.Make("Abel", 1000, 60);
    auto b = s.Make("Brutus", 1010, 50);
    auto c = s.Make("Cato", 1020, 60);
    s.Pump(100);
    LastCounters(a, 300ms);
    LastCounters(c, 300ms);
    // A level 60 kills an innocent level 50, his first murder: TotalPK 1, points 1,200, DailyPK 5; a murderer.
    a.client->Send(proto::DevStrike{b.entity, 10'000'000});
    CHECK(Hears(c, a.entity, 2));
    const proto::PkCounters counts = LastCounters(a);
    CHECK(counts.daily == 5);
    CHECK(counts.total == 1);
    CHECK(counts.points == 1'200);
    // The killer's PvP experience: GUST_Mob_Exp of the lower level (1,000,000 here) x the gap factor (the victim 10 below:
    // 1 - 0.15 x 7, held to 0.1) x the party factor 1 x rate 1 = 100,000.
    proto::ExperienceGained won;
    REQUIRE(a.client->Expect(won));
    CHECK(won.experience == 100'000);
    std::int64_t saved = 0;
    for (int i = 0; i < 200 && saved != 1'200; ++i) {
        s.Pump(5);
        saved = t.Db().Query("SELECT pkpenaltypoint FROM _char WHERE charid = $1", {a.character}).Int(0, 0);
    }
    CHECK(saved == 1'200);
    CHECK(t.Db().Query("SELECT dailypk * 100 + totalpk FROM _char WHERE charid = $1", {a.character}).Int(0, 0) == 501);
    // Cato kills the murderer: nothing counted, no record (still innocent).
    c.client->Send(proto::DevStrike{a.entity, 10'000'000});
    proto::ExperienceGained other;
    REQUIRE(c.client->Expect(other));
    CHECK(other.experience == 1'090'000); // the same level: the party factor 1.09; relation 5, no doubling
    CHECK(LastCounters(c, 500ms).total == 255); // no counts told
    CHECK(StateOf(b, c.entity, 1500ms) == -1);
    s.game.Stop();
}

TEST_CASE("Game server: a murderer's death to a monster: 6 %, 60 SP, 600 points and a DailyPK off, an item at 100 %") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    MurderServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const auto murderer = [&](std::int32_t points, int daily) {
        return [&t, points, daily](std::int64_t character) {
            REQUIRE(server::SavePkCounts(t.Db(), character, std::uint8_t(daily), 1, points));
            REQUIRE(t.Db().Query("UPDATE _char SET remainskillpoint = 100, expoffset = 500000 WHERE charid = $1", {character}).Ok());
        };
    };
    auto a = s.Make("Abel", 1000, 60, {Stored(kHerb, 5)}, murderer(30'000, 3));
    auto k = s.Make("Keeper", 1010, 60, {Stored(kKeepsake, 1)}, murderer(30'000, 0));
    s.Pump(100);
    CHECK(StateOf(k, a.entity, 500ms) == 2); // red from the entry (his spawn's state)
    // Killed with no player (a monster's relation 4): 6 % of 1,000,000 = 59,999 experience, 60 skill points, the herbs to
    // the ground (100 % at 30,000 points; the bag's only item), 600 points and 1 DailyPK off.
    a.client->Send(proto::DevDie{});
    proto::ItemMoved dropped;
    REQUIRE(a.client->Expect(dropped));
    CHECK(dropped.emptied.size() == 1);
    proto::Progress progress;
    std::uint32_t sp = 0;
    std::uint64_t experience = 0;
    for (int i = 0; i < 20 && a.client->Expect(progress, 500ms); ++i) {
        sp = progress.skillPoints;
        experience = progress.experience;
    }
    CHECK(experience == 500'000 - 59'999);
    CHECK(sp == 40);
    const proto::PkCounters after = LastCounters(a);
    CHECK(after.points == 29'400);
    CHECK(after.daily == 2);
    bool onGround = false;
    proto::EntityAppeared seen;
    for (int i = 0; i < 40 && !onGround; ++i) {
        if (k.client->Expect(seen, 50ms) && seen.kind == proto::EntityKind::GroundItem && seen.character.model == kHerb) onGround = true;
    }
    CHECK(onGround);
    // An item that may not drop (CanDrop 0) stays: nothing leaves the bag.
    k.client->Send(proto::DevDie{});
    proto::ItemMoved none;
    CHECK_FALSE(k.client->Expect(none, 1s));
    s.game.Stop();
}

TEST_CASE("Game server: the PK clocks: TotalPK one per 48 h online and offline, DailyPK reset at local midnight") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    MurderServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const std::int64_t now = s.config.wallClock();
    // Offline: TotalPK 3, its keeper's period begun 100 hours ago: two periods gone, 1 left.
    auto a = s.Make("Abel", 1000, 30, {}, [&](std::int64_t character) {
        REQUIRE(server::SavePkCounts(t.Db(), character, 7, 3, 0));
        REQUIRE(server::SavePkKeeper(t.Db(), character, server::CharacterRow::PkKeeper{game::kKeeperSeconds, now - 100 * 3600}));
    });
    const proto::PkCounters entry = Next<proto::PkCounters>(a);
    CHECK(entry.total == 1);
    CHECK(entry.daily == 7);
    // TotalPK without a keeper and no points at the entry: "Penalty ( record empty )", the points 1, a murderer.
    auto b = s.Make("Brutus", 1010, 30, {}, [&](std::int64_t character) { REQUIRE(server::SavePkCounts(t.Db(), character, 4, 2, 0)); });
    const proto::PkCounters fixed = Next<proto::PkCounters>(b);
    CHECK(fixed.points == 1);
    CHECK(fixed.daily == 4);
    // Online: the clock 48 h on: the last one goes, the keeper ends; the day changed too: DailyPK back to 0 for both.
    s.clockShift->store(48 * 3600);
    const proto::PkCounters online = LastCounters(a, 2500ms);
    CHECK(online.total == 0);
    CHECK(online.daily == 0);
    const proto::PkCounters other = LastCounters(b, 500ms);
    CHECK(other.daily == 0);
    CHECK(other.total == 2); // a murderer's keeper does not run
    std::int64_t rows = 1;
    for (int i = 0; i < 200 && rows != 0; ++i) {
        s.Pump(5);
        rows = t.Db().Query("SELECT count(*) FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 0", {a.character}).Int(0, 0);
    }
    CHECK(rows == 0);
    s.game.Stop();
}

TEST_CASE("Game server: a murderer's refusals: no ride (0x1876), no return scroll (0x1875), no gate (0x1C16), link kind 3 (0x1C19), the logout held") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    MurderServer s(t);
    s.config.logoutCountdown = 1s;
    // A gate beside the field's players (its point 1), leading to points 2 (plain) and 3 (restriction kind 3 on PvP state 1).
    const std::uint16_t field = assets::RegionId(168, 97);
    const auto row = [](std::initializer_list<std::string> cells) {
        std::string out;
        for (const std::string& cell : cells) out += (out.empty() ? "" : "\t") + cell;
        return out + "\n";
    };
    std::string gate = "1\t9001\tSTORE_TEST_GATE";
    for (int col = 3; col < 58; ++col) {
        gate += "\t";
        if (col == 41) gate += std::to_string(field);
        else if (col == 43) gate += "1000";
        else if (col == 45) gate += "1010";
        else gate += "0";
    }
    const auto link = [](int to, int kind, int a) {
        std::string l = "1\t1\t" + std::to_string(to) + "\t0\t0\t0\t" + std::to_string(kind) + "\t" + std::to_string(a);
        for (int col = 8; col < 21; ++col) l += "\t0";
        return l + "\n";
    };
    auto teleports = std::make_shared<assets::TeleportTable>();
    teleports->Decode(gate + "\n",
                      row({"1", "1", "GATE_A", "9001", "SN_A", "0", "0", "0", "0", "0", "0", "0", "1"}) +
                          row({"1", "2", "GATE_B", "0", "SN_B", std::to_string(field), "1500", "0", "1500", "0", "0", "0", "1"}) +
                          row({"1", "3", "GATE_C", "0", "SN_C", std::to_string(field), "1600", "0", "1600", "0", "0", "0", "1"}),
                      link(2, 0, 0) + link(3, 3, 1 << 1));
    s.config.teleports = teleports;
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const auto murderer = [&t](std::int64_t character) { REQUIRE(server::SavePkCounts(t.Db(), character, 0, 1, 1'200)); };
    auto m = s.Make("Mordred", 1000, 30, {Stored(kHorseScroll, 1), Stored(kReturnScroll, 1)}, murderer);
    auto v = s.Make("Victim", 1010, 30);
    auto a = s.Make("Assault", 1020, 30);
    s.Pump(100);
    const auto placeOf = [](const proto::InventoryList& bag, std::uint32_t refId) {
        for (const proto::ItemRecord& i : bag.items) {
            if (i.item == refId) return i.slot;
        }
        return std::uint8_t(0);
    };
    // The ride's scroll: "Cannot use a transport with murderer status."
    m.client->Send(proto::UseItem{placeOf(m.bag, kHorseScroll)});
    CHECK(Next<proto::CosRefused>(m).text == "UIIT_MSG_STRGERR_MURDERER_CANT_SUMMON_VEHICLE");
    // A return scroll: 0x1875.
    m.client->Send(proto::UseItem{placeOf(m.bag, kReturnScroll)});
    CHECK(Next<proto::ItemRefused>(m).code == 0x1875);
    // The gate structure: 0x1C16.
    m.client->Send(proto::UseGate{9001, 2});
    CHECK(Next<proto::GateRefused>(m).status == proto::GateStatus::Murderer);
    // An assaulter at a link of kind 3 on state 1: 0x1C19; the plain link takes him.
    REQUIRE(Strikes(s, a, v));
    REQUIRE(Hears(a, a.entity, 1));
    a.client->Send(proto::UseGate{9001, 3});
    CHECK(Next<proto::GateRefused>(a).status == proto::GateStatus::PvpState);
    // The logout in combat: held for the assaulter and the murderer (states 1 and 2), not for one innocent.
    a.client->Send(proto::LogoutRequest{proto::LogoutKind::Exit});
    CHECK(Next<proto::LogoutAnswer>(a).status == proto::LogoutStatus::InCombat);
    REQUIRE(Strikes(s, m, v)); // the murderer in combat too (a hit given)
    m.client->Send(proto::LogoutRequest{proto::LogoutKind::Exit});
    CHECK(Next<proto::LogoutAnswer>(m).status == proto::LogoutStatus::InCombat);
    v.client->Send(proto::LogoutRequest{proto::LogoutKind::Exit});
    const auto out = Next<proto::LogoutAnswer>(v);
    CHECK(out.status == proto::LogoutStatus::Counting);
    v.client->Send(proto::LogoutCancel{});
    s.game.Stop();
}
