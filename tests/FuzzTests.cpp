// The game server against messages a modified client could send: every message id
// of the world's range, each with payloads that decode for most messages (all zeros of several lengths, the other
// player's entity in front) and random ones. The server must not die, must log no error (a handler's exception is
// one: Peers::Pump), and must still take a new player at the end. Set SR_DB_URL; otherwise skipped.
// SR_FUZZ_SEED and SR_FUZZ_ROUNDS (random payloads an id, default 4) for longer runs; SR_FUZZ_LOG=info keeps the
// handlers' lines.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/server/GameServer.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

constexpr std::uint32_t kHerb = 900701, kScroll = 900702, kMonster = 900710;

struct FuzzServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    int made = 0;

    explicit FuzzServer(TestDatabase& db) : t(db) {
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
        assets::ItemRow scroll = herb; // a return scroll
        scroll.refObjId = kScroll;
        scroll.codename = "ITEM_ETC_SCROLL_RETURN_TEST";
        scroll.typeId = {3, 3, 3, 1};
        scroll.params[0] = 100;
        items->Add(scroll);
        rules->items = items;
        config.rules = rules;
        auto characters = std::make_shared<assets::CharacterTable>();
        assets::ObjectRow monster;
        monster.refObjId = kMonster;
        monster.codename = "MOB_TEST_DUMMY";
        monster.typeId = {1, 2, 1, 1};
        monster.level = 10;
        monster.maxHp = 1000;
        monster.inService = true;
        characters->Add(monster);
        config.characters = characters;
        config.levels.assign(120, 1'000'000);
        config.monsterExperience.assign(120, 1'000'000);
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.databaseThreads = 2;
        config.devCommands = false; // a public server's
    }

    struct Player {
        std::unique_ptr<Client> client;
        std::uint32_t entity = 0;
    };
    // A new character in Jangan's south field, entered, with some herbs and scrolls in its bag.
    Player Make() {
        Player p;
        const std::string name = std::format("Fuzz{}", ++made);
        REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        const std::int64_t account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        p.client = std::make_unique<Client>(net, game.Port(), [this] { game.Poll(1ms); });
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
        server::CharacterRow::Place here;
        here.region = assets::RegionId(168, 97);
        here.x = 1000.0f + float(made % 10);
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{list[0].id, here}}));
        REQUIRE(server::SaveProgress(t.Db(), {{list[0].id, std::uint8_t(40), 0, 0, 0, 40, 40, 0}}));
        const std::int64_t jid = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {list[0].id}).Int(0, 0);
        REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {jid}).Ok());
        server::StoredItem herbs, scrolls;
        herbs.refId = kHerb;
        herbs.quantity = 20;
        herbs.counted = true;
        scrolls.refId = kScroll;
        scrolls.quantity = 5;
        scrolls.counted = true;
        REQUIRE(server::GiveItems(t.Db(), config.id, name, {herbs, scrolls}, 0, error) == server::GiveResult::Done);
        REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, $2)", {jid, int(config.id)}).Ok());
        p.client->Send(proto::CharacterSelect{name});
        REQUIRE(p.client->Expect(r));
        p.client->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.client->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        return p;
    }
};

void PutU32(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4 && at + i < b.size(); ++i) b[at + i] = std::byte((v >> (8 * i)) & 0xFF);
}

} // namespace

TEST_CASE("Fuzz: every world message id with zeros, entities and random bytes; the game server lives on") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    FuzzServer s(t);
    REQUIRE_MESSAGE(s.game.Start(s.config, s.error), s.error);
    const char* seedText = std::getenv("SR_FUZZ_SEED");
    const char* roundsText = std::getenv("SR_FUZZ_ROUNDS");
    const std::uint32_t seed = seedText != nullptr ? std::uint32_t(std::strtoul(seedText, nullptr, 10)) : 20261120u;
    const int rounds = roundsText != nullptr ? std::max(1, std::atoi(roundsText)) : 4;
    MESSAGE("fuzz seed ", seed, ", ", rounds, " random payloads an id");
    std::mt19937 rng(seed);

    FuzzServer::Player fuzzer = s.Make(), target = s.Make();
    const std::uint64_t errorsBefore = core::LoggedErrors();
    // Every refusal at info would be tens of thousands of lines: kept only with SR_FUZZ_LOG=info.
    const char* logText = std::getenv("SR_FUZZ_LOG");
    if (logText == nullptr || std::string_view(logText) != "info") core::SetLogLevel(core::LogLevel::Warning);
    int sent = 0, replaced = 0;
    const auto alive = [&](FuzzServer::Player& p) { return p.client->Connection().State() != net::ConnectionState::Closed; };

    for (std::uint32_t id = 0x1000; id < 0x1500; ++id) {
        // The likeliest to decode first: the target in front, then zeros; random bytes; the empty and odd sizes last.
        // A close (the guilds', the academy's and the parties' handlers close on a request that does not decode, as
        // a logout does) costs a new character: after kMostCloses an id is left (its decodable payloads came first).
        std::vector<std::vector<std::byte>> payloads;
        for (std::size_t n : {4, 8, 12, 16}) {
            std::vector<std::byte> b(n, std::byte{0});
            PutU32(b, 0, target.entity);
            payloads.push_back(std::move(b));
        }
        for (std::size_t n : {2, 6, 24, 32}) payloads.emplace_back(n, std::byte{0});
        for (int r = 0; r < rounds; ++r) {
            std::vector<std::byte> b(std::size_t(rng() % 64), std::byte{0});
            for (std::byte& x : b) x = std::byte(rng() & 0xFF);
            for (std::size_t at = 0; at + 4 <= b.size(); at += 4) {
                const std::uint32_t pick = rng() % 8;
                if (pick == 0) PutU32(b, at, target.entity);
                if (pick == 1) PutU32(b, at, fuzzer.entity);
                if (pick == 2) PutU32(b, at, rng() % 64); // a small number: a slot, an index, a count
            }
            payloads.push_back(std::move(b));
        }
        payloads.emplace_back();
        payloads.emplace_back(1, std::byte{0});
        payloads.emplace_back(64, std::byte{0});
        constexpr int kMostCloses = 3;
        int closes = 0;
        for (const std::vector<std::byte>& b : payloads) {
            if (!alive(fuzzer)) {
                ++replaced;
                if (++closes >= kMostCloses) break;
                fuzzer = s.Make(); // a new player goes on from the next payload
            }
            fuzzer.client->Connection().Send(proto::MessageId(id), b);
            ++sent;
            s.game.Poll(0ms);
        }
        if (!alive(fuzzer)) fuzzer = s.Make();
        for (int i = 0; i < 3; ++i) s.game.Poll(1ms);
        if (!alive(target)) target = s.Make();
    }
    for (int i = 0; i < 200; ++i) s.game.Poll(1ms); // what is still on its way (the database's answers)
    core::SetLogLevel(core::LogLevel::Info);
    MESSAGE(sent, " messages sent, the fuzzing player closed ", replaced, " times");
    CHECK(core::LoggedErrors() == errorsBefore); // no handler threw, nothing went wrong in between

    // Still serving: a new player enters, walks.
    FuzzServer::Player after = s.Make();
    CHECK(after.entity != 0);
    s.game.Stop();
}
