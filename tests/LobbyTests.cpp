#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "sr/assets/Dress.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/CharacterRules.h"
#include "sr/game/LobbyProtocol.h"
#include "sr/game/TitleSession.h"
#include "sr/server/GameServer.h"
#include "sr/server/GatewayServer.h"
#include "sr/server/LoginServer.h"
#include "sr/server/Store.h"
#include "ServerTestSupport.h"

using namespace sr;
using namespace sr::test;
using game::CreationCheck;
using game::NameCheck;
using Race = game::CreationRules::Race;

namespace {

// Rules without the game files: one Chinese man (model 100) with one armour set and a sword, a
// European woman (200) with hers and a crossbow.
std::shared_ptr<game::CharacterRules> TestRules() {
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->names.Forbid("gm");
    rules->names.Forbid("silkroad");
    rules->creation.AddFigure(100, Race::China, true);
    rules->creation.AddFigure(200, Race::Europe, false);
    rules->creation.AddArmourSet(Race::China, true, 1, 2, 3);
    rules->creation.AddArmourSet(Race::Europe, false, 11, 12, 13);
    rules->creation.AddWeapon(Race::China, 10);
    rules->creation.AddWeapon(Race::Europe, 20);
    return rules;
}

std::filesystem::path GameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir ? std::filesystem::path(dir) : std::filesystem::path();
}

} // namespace

TEST_CASE("Character rules: names") {
    const auto rules = TestRules();
    const game::NameRules& n = rules->names;
    CHECK(n.Check("Ab") == NameCheck::Ok);
    CHECK(n.Check("Twelve_Chars") == NameCheck::Ok);
    CHECK(n.Check("A") == NameCheck::BadLength);
    CHECK(n.Check("ThirteenChars") == NameCheck::BadLength);
    CHECK(n.Check("two words") == NameCheck::BadCharacter);
    CHECK(n.Check("caf\xC3\xA9") == NameCheck::BadCharacter); // not in the English ranges
    CHECK(n.Check("\xC3") == NameCheck::BadCharacter);         // cut UTF-8
    CHECK(n.Check("xGMx") == NameCheck::Forbidden);           // inside, any case
    CHECK(n.Check("SilkRoad7") == NameCheck::Forbidden);
}

TEST_CASE("Character rules: what creation allows") {
    const auto rules = TestRules();
    const game::CreationRules& c = rules->creation;
    CHECK(c.Check(100, 1, 2, 3, 10) == CreationCheck::Ok);
    CHECK(c.Check(200, 11, 12, 13, 20) == CreationCheck::Ok);
    CHECK(c.Check(999, 1, 2, 3, 10) == CreationCheck::BadModel);
    CHECK(c.Check(100, 11, 12, 13, 10) == CreationCheck::BadArmour); // another race's armour
    CHECK(c.Check(100, 1, 2, 13, 10) == CreationCheck::BadArmour);   // mixed pieces
    CHECK(c.Check(100, 1, 2, 3, 20) == CreationCheck::BadWeapon);    // a European weapon
    CHECK(c.Check(100, 1, 2, 3, 0) == CreationCheck::BadWeapon);
}

TEST_CASE("Character rules: from the game files") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    game::CharacterRules rules;
    REQUIRE_MESSAGE(rules.Load(fs, error), error);
    CHECK(rules.creation.Figures() == 52);    // 13 per race and sex
    CHECK(rules.creation.ArmourSets() == 12); // 3 kinds per race and sex
    CHECK(rules.creation.Weapons() == 14);    // 5 Chinese, 9 European
    CHECK(rules.names.ForbiddenWords() > 100);
    MESSAGE(rules.names.ForbiddenWords() << " forbidden words");
    CHECK(rules.names.Check("Tester") == NameCheck::Ok);
    CHECK(rules.names.Check("silkroad1") == NameCheck::Forbidden);
    CHECK(rules.names.Check("Caf\xC3\xA9") == NameCheck::BadCharacter);

    assets::CharacterTable characters;
    assets::ItemTable items;
    REQUIRE(characters.Load(fs, error));
    REQUIRE(items.Load(fs, error));
    const auto id = [&](const char* codename) { return items.Find(codename)->refObjId; };
    const std::uint32_t man = characters.Find("CHAR_CH_MAN_ADVENTURER")->refObjId;
    const std::uint32_t woman = characters.Find("CHAR_EU_WOMAN_WITCH")->refObjId;
    CHECK(rules.creation.Check(man, id("ITEM_CH_M_HEAVY_01_BA_A_DEF"), id("ITEM_CH_M_HEAVY_01_LA_A_DEF"), id("ITEM_CH_M_HEAVY_01_FA_A_DEF"),
                               id("ITEM_CH_SWORD_01_A_DEF")) == CreationCheck::Ok);
    CHECK(rules.creation.Check(woman, id("ITEM_EU_W_CLOTHES_01_BA_A_DEF"), id("ITEM_EU_W_CLOTHES_01_LA_A_DEF"),
                               id("ITEM_EU_W_CLOTHES_01_FA_A_DEF"), id("ITEM_EU_HARP_01_A_DEF")) == CreationCheck::Ok);
    CHECK(rules.creation.Check(man, id("ITEM_CH_M_HEAVY_01_BA_A_DEF"), id("ITEM_CH_M_HEAVY_01_LA_A_DEF"), id("ITEM_CH_M_HEAVY_01_FA_A_DEF"),
                               id("ITEM_EU_HARP_01_A_DEF")) == CreationCheck::BadWeapon);
    CHECK(rules.creation.Check(woman, id("ITEM_EU_M_CLOTHES_01_BA_A_DEF"), id("ITEM_EU_M_CLOTHES_01_LA_A_DEF"),
                               id("ITEM_EU_M_CLOTHES_01_FA_A_DEF"), id("ITEM_EU_HARP_01_A_DEF")) == CreationCheck::BadArmour);

    // The European screen: no armour, and the armour its weapon's class allows.
    CHECK(rules.creation.Check(woman, 0, 0, 0, id("ITEM_EU_HARP_01_A_DEF")) == CreationCheck::Ok);
    CHECK(rules.creation.Check(man, 0, 0, 0, id("ITEM_CH_SWORD_01_A_DEF")) == CreationCheck::BadArmour);
    CHECK(rules.creation.Check(woman, id("ITEM_EU_W_HEAVY_01_BA_A_DEF"), id("ITEM_EU_W_HEAVY_01_LA_A_DEF"),
                               id("ITEM_EU_W_HEAVY_01_FA_A_DEF"), id("ITEM_EU_HARP_01_A_DEF")) == CreationCheck::BadArmour);
    CHECK(rules.creation.Check(woman, id("ITEM_EU_W_HEAVY_01_BA_A_DEF"), id("ITEM_EU_W_HEAVY_01_LA_A_DEF"),
                               id("ITEM_EU_W_HEAVY_01_FA_A_DEF"), id("ITEM_EU_TSWORD_01_A_DEF")) == CreationCheck::Ok);
    CHECK(rules.creation.Check(woman, id("ITEM_EU_W_LIGHT_01_BA_A_DEF"), id("ITEM_EU_W_LIGHT_01_LA_A_DEF"),
                               id("ITEM_EU_W_LIGHT_01_FA_A_DEF"), id("ITEM_EU_STAFF_01_A_DEF")) == CreationCheck::Ok);
    // A one-handed weapon comes with the race's shield (itemdata column 93).
    CHECK(rules.creation.ShieldFor(id("ITEM_CH_SWORD_01_A_DEF")) == id("ITEM_CH_SHIELD_01_A_DEF"));
    CHECK(rules.creation.ShieldFor(id("ITEM_CH_BLADE_01_A_DEF")) == id("ITEM_CH_SHIELD_01_A_DEF"));
    CHECK(rules.creation.ShieldFor(id("ITEM_EU_SWORD_01_A_DEF")) == id("ITEM_EU_SHIELD_01_A_DEF"));
    CHECK(rules.creation.ShieldFor(id("ITEM_EU_STAFF_01_A_DEF")) == id("ITEM_EU_SHIELD_01_A_DEF"));
    CHECK(rules.creation.ShieldFor(id("ITEM_EU_DARKSTAFF_01_A_DEF")) == id("ITEM_EU_SHIELD_01_A_DEF"));
    for (const char* twoHanded : {"ITEM_CH_BOW_01_A_DEF", "ITEM_CH_SPEAR_01_A_DEF", "ITEM_CH_TBLADE_01_A_DEF", "ITEM_EU_AXE_01_A_DEF",
                                  "ITEM_EU_DAGGER_01_A_DEF", "ITEM_EU_HARP_01_A_DEF", "ITEM_EU_TSWORD_01_A_DEF", "ITEM_EU_TSTAFF_01_A_DEF",
                                  "ITEM_EU_CROSSBOW_01_A_DEF"}) {
        CHECK_MESSAGE(rules.creation.ShieldFor(id(twoHanded)) == 0, twoHanded);
    }

    // Dressing: the stance, the garments, the hands of a dagger.
    assets::DressedCharacter d;
    const std::uint32_t worn[] = {id("ITEM_EU_W_LIGHT_01_FA_A_DEF"), id("ITEM_EU_DAGGER_01_A_DEF")};
    REQUIRE_MESSAGE(assets::Dress(fs, characters, items, woman, worn, assets::PackShape(4, 0), d, error), error);
    CHECK(d.europe);
    CHECK(d.female);
    CHECK(d.stance == "dagger");
    CHECK(d.height == doctest::Approx(1.06f));
    const auto has = [&](std::string_view path) { return std::find(d.items.begin(), d.items.end(), path) != d.items.end(); };
    CHECK(has("res\\item\\europe\\woman_item\\light_01_ba.bsr")); // light shoes: light garments
    CHECK(has("res\\item\\europe\\woman_item\\light_01_la.bsr"));
    CHECK(has("res\\item\\europe\\weapon\\dagger_01_r.bsr"));
    CHECK(has("res\\item\\europe\\weapon\\dagger_01_l.bsr"));
    REQUIRE(d.volume.size() == 8); // women have Bone01 too
    CHECK(d.volume[0].first == "Bip01 Spine");
    CHECK(d.volume[0].second == doctest::Approx(0.95f)); // volume 0: the low end
    REQUIRE(assets::Dress(fs, characters, items, man, {}, assets::kNeutralShape, d, error));
    CHECK(d.stance == "default");
    CHECK(d.height == doctest::Approx(1.0f));
    CHECK(has("res\\item\\china\\man_item\\clothes_00_ba.bsr")); // nothing worn: clothes garments
    for (const auto& [bone, factor] : d.volume) CHECK(factor == doctest::Approx(1.0f));
}

TEST_CASE("Game server: the ticket, one session per account, and the lobby") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::CreateAccount(t.Db(), "player", "secret1", error) == server::CreateAccountResult::Created);
    bool ok = false;
    const std::int64_t account = server::FindAccount(t.Db(), "player", ok)->id;

    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = TestRules();
    config.maxCharacters = 2;
    config.deleteSeconds = 3600;
    config.reportInterval = 50ms;
    REQUIRE_MESSAGE(game.Start(config, error), error);
    net::NetService net(1);
    const auto pump = [&] { game.Poll(2ms); };

    const auto enter = [&](Client& c, const std::vector<std::uint8_t>& ticket) {
        REQUIRE(c.Greet());
        proto::LobbyEnter e;
        e.ticket = ticket;
        c.Send(e);
        proto::LobbyEntered r;
        REQUIRE(c.Expect(r));
        return r;
    };

    // Before the ticket, nothing else is taken.
    Client rude(net, game.Port(), pump);
    REQUIRE(rude.Greet());
    rude.Send(proto::CharacterListRequest{});
    CHECK(rude.WaitClosed());

    // A made-up ticket is refused.
    Client forged(net, game.Port(), pump);
    CHECK(enter(forged, std::vector<std::uint8_t>(32, 7)).status == proto::LobbyEnterStatus::InvalidTicket);

    Client c(net, game.Port(), pump);
    proto::LobbyEntered entered = enter(c, server::IssueTicket(t.Db(), account, config.id, 30, error));
    REQUIRE(entered.status == proto::LobbyEnterStatus::Accepted);
    CHECK(entered.maxCharacters == 2);
    CHECK(game.Online() == 1);

    // The same account a second time: refused while the first is in.
    Client twin(net, game.Port(), pump);
    CHECK(enter(twin, server::IssueTicket(t.Db(), account, config.id, 30, error)).status == proto::LobbyEnterStatus::AlreadyLoggedIn);

    const auto result = [&](auto request) {
        c.Send(request);
        proto::LobbyResult r;
        REQUIRE(c.Expect(r));
        return r.status;
    };
    const auto list = [&] {
        c.Send(proto::CharacterListRequest{});
        proto::CharacterList l;
        REQUIRE(c.Expect(l));
        return l;
    };
    const auto create = [&](std::string name, std::uint32_t model = 100, std::uint32_t weapon = 10) {
        proto::CharacterCreate m;
        m.name = std::move(name);
        m.model = model;
        m.height = 0x35;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = weapon;
        return result(m);
    };

    CHECK(list().characters.empty());
    CHECK(create("Hero") == proto::LobbyStatus::Ok);
    CHECK(create("HERO") == proto::LobbyStatus::NameTaken);
    CHECK(create("x") == proto::LobbyStatus::BadName);
    CHECK(create("TheGMx") == proto::LobbyStatus::NameNotAllowed);
    CHECK(create("Other", 999) == proto::LobbyStatus::InvalidCreation);
    CHECK(create("Other", 100, 0) == proto::LobbyStatus::NoWeapon);
    CHECK(create("Other", 100, 20) == proto::LobbyStatus::InvalidCreation);
    CHECK(result(proto::CharacterNameCheck{"hero"}) == proto::LobbyStatus::NameTaken);
    CHECK(result(proto::CharacterNameCheck{"Second"}) == proto::LobbyStatus::Ok);
    CHECK(create("Second") == proto::LobbyStatus::Ok);
    CHECK(create("Third") == proto::LobbyStatus::TooManyCharacters);

    proto::CharacterList l = list();
    REQUIRE(l.characters.size() == 2);
    const proto::CharacterSummary& hero = l.characters[0];
    CHECK(hero.name == "Hero");
    CHECK(hero.model == 100);
    CHECK(hero.height == 0x35);
    CHECK(hero.level == 1);
    CHECK(hero.strength == 20);
    CHECK(hero.hp == 200);
    CHECK_FALSE(hero.deleting);
    REQUIRE(hero.equipment.size() == 4);
    CHECK(hero.equipment[0].item == 1); // chest
    CHECK(hero.equipment[3].item == 10); // weapon

    // Deleting waits, can be undone, and blocks selecting.
    CHECK(result(proto::CharacterDelete{"second"}) == proto::LobbyStatus::Ok);
    l = list();
    CHECK(l.characters[1].deleting);
    CHECK(l.characters[1].deleteMinutes == 60);
    CHECK(result(proto::CharacterSelect{"Second"}) == proto::LobbyStatus::Deleting);
    CHECK(result(proto::CharacterDelete{"Second"}) == proto::LobbyStatus::Deleting);
    CHECK(result(proto::CharacterRestore{"Second"}) == proto::LobbyStatus::Ok);
    CHECK(result(proto::CharacterRestore{"Second"}) == proto::LobbyStatus::NotDeleting);
    CHECK_FALSE(list().characters[1].deleting);
    CHECK(result(proto::CharacterSelect{"Nobody"}) == proto::LobbyStatus::NotFound);
    CHECK(result(proto::CharacterSelect{"Hero"}) == proto::LobbyStatus::Ok);

    // A guild's master is not deleted while it leads its guild (the original refuses it).
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate) "
                         "SELECT 77, charid, charname16, 0, 1, 0, localtimestamp FROM sro_vt_shard._char WHERE charname16 = 'Second'")
                .Ok());
    CHECK(result(proto::CharacterDelete{"Second"}) == proto::LobbyStatus::GuildMaster);
    REQUIRE(t.Db().Query("UPDATE sro_vt_shard._guildmember SET memberclass = 10 WHERE guildid = 77").Ok());
    // A friendship both ways with Hero, and a letter to it: gone with it.
    const std::string second = "(SELECT charid FROM sro_vt_shard._char WHERE charname16 = 'Second')", heroId = "(SELECT charid FROM sro_vt_shard._char WHERE charname16 = 'Hero')";
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._friend (charid, friendcharid, friendcharname) SELECT " + second + ", " + heroId + ", 'Hero'").Ok());
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._friend (charid, friendcharid, friendcharname) SELECT " + heroId + ", " + second + ", 'Second'").Ok());
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._memo (charid, fromcharname, message, date, status) SELECT " + second + ", 'Hero', 'hi', localtimestamp, 0").Ok());

    // Once the deletion time has passed, the character is gone (out of its guild too); its name stays taken, as the
    // original keeps its _Char row.
    CHECK(result(proto::CharacterDelete{"Second"}) == proto::LobbyStatus::Ok);
    // (the original's _deletedchar keeps when it was asked; the wait is the server's, an hour here)
    t.Db().Query("UPDATE sro_vt_shard._deletedchar d SET deleteddate = localtimestamp - interval '8 days' FROM sro_vt_shard._char c "
                 "WHERE c.charid = d.charid AND c.charname16 = 'Second'");
    CHECK(list().characters.size() == 1);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_shard._guildmember WHERE guildid = 77").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_shard._friend").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_shard._memo").Int(0, 0) == 0);
    CHECK(result(proto::CharacterNameCheck{"Second"}) == proto::LobbyStatus::NameTaken);

    // The game server reports its players for the gateway.
    bool counted = false;
    for (int i = 0; i < 200 && !counted; ++i) {
        pump();
        const auto servers = server::ListGameServers(t.Db(), 15, ok);
        counted = !servers.empty() && servers[0].online == 1 && servers[0].reporting;
    }
    CHECK(counted);

    // Leaving gives the session back: the account can come in again.
    c.Connection().Close();
    for (int i = 0; i < 500 && game.Online() != 0; ++i) pump();
    CHECK(game.Online() == 0);
    bool released = false;
    for (int i = 0; i < 200 && !released; ++i) {
        pump();
        released = !server::HasLiveSession(t.Db(), account, 15, ok);
    }
    CHECK(released);
    Client again(net, game.Port(), pump);
    CHECK(enter(again, server::IssueTicket(t.Db(), account, config.id, 30, error)).status == proto::LobbyEnterStatus::Accepted);

    // Stopping says goodbye: listed offline, sessions ended.
    game.Stop();
    CHECK_FALSE(server::ListGameServers(t.Db(), 15, ok)[0].reporting);
    CHECK_FALSE(server::HasLiveSession(t.Db(), account, 15, ok));
}

TEST_CASE("Servers: the login refuses an account that is already in a game") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::CreateAccount(t.Db(), "player", "secret1", error) == server::CreateAccountResult::Created);
    server::GameServer game;
    server::GameServerConfig gameConfig;
    gameConfig.listen = Local();
    gameConfig.databaseUrl = t.Url();
    gameConfig.rules = TestRules();
    REQUIRE_MESSAGE(game.Start(gameConfig, error), error);

    core::JobSystem jobs(1);
    server::LoginServer login;
    server::LoginConfig loginConfig;
    loginConfig.listen = Local();
    loginConfig.databaseUrl = t.Url();
    loginConfig.jobs = &jobs;
    REQUIRE_MESSAGE(login.Start(loginConfig, error), error);

    net::NetService net(1);
    const auto pump = [&] {
        game.Poll(1ms);
        login.Poll(1ms);
    };
    const auto logIn = [&] {
        Client c(net, login.Port(), pump);
        REQUIRE(c.Greet());
        proto::LoginRequest request;
        request.account = "player";
        request.password = "secret1";
        request.server = gameConfig.id;
        c.Send(request);
        proto::LoginResult r;
        REQUIRE(c.Expect(r));
        return r;
    };

    // The whole way: login, then the game server with its ticket.
    proto::LoginResult first = logIn();
    REQUIRE(first.status == proto::LoginStatus::Accepted);
    CHECK(first.port == game.Port());
    Client player(net, first.port, pump);
    REQUIRE(player.Greet());
    proto::LobbyEnter enter;
    enter.ticket = first.ticket;
    player.Send(enter);
    proto::LobbyEntered entered;
    REQUIRE(player.Expect(entered));
    REQUIRE(entered.status == proto::LobbyEnterStatus::Accepted);

    CHECK(logIn().status == proto::LoginStatus::AlreadyLoggedIn);
    player.Connection().Close();
    for (int i = 0; i < 500 && game.Online() != 0; ++i) pump();
    bool ok = false;
    for (int i = 0; i < 200 && server::HasLiveSession(t.Db(), server::FindAccount(t.Db(), "player", ok)->id, 15, ok); ++i) pump();
    CHECK(logIn().status == proto::LoginStatus::Accepted);
}

TEST_CASE("Title session: from the gateway to selecting a character, as the client does it") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    REQUIRE(server::CreateAccount(t.Db(), "player", "secret1", error) == server::CreateAccountResult::Created);

    core::JobSystem jobs(1);
    server::LoginServer login;
    server::LoginConfig loginConfig;
    loginConfig.listen = Local();
    loginConfig.databaseUrl = t.Url();
    loginConfig.jobs = &jobs;
    REQUIRE_MESSAGE(login.Start(loginConfig, error), error);
    server::GameServer game;
    server::GameServerConfig gameConfig;
    gameConfig.listen = Local();
    gameConfig.databaseUrl = t.Url();
    gameConfig.rules = TestRules();
    gameConfig.id = 7;
    gameConfig.serverName = "Test";
    REQUIRE_MESSAGE(game.Start(gameConfig, error), error);
    server::GatewayServer gateway;
    server::GatewayConfig gatewayConfig;
    gatewayConfig.listen = Local();
    gatewayConfig.databaseUrl = t.Url();
    gatewayConfig.loginPort = login.Port();
    REQUIRE_MESSAGE(gateway.Start(gatewayConfig, error), error);

    net::NetService net(1);
    game::TitleSession session(net, {Certificate().pem, "localhost"}, "sr_tests");
    std::vector<game::TitleEvent> events;
    // Turns everything until an event of type T arrives (or an error, or time runs out).
    const auto next = [&]<typename T>(T& out) {
        const auto end = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            for (std::size_t i = 0; i < events.size(); ++i) {
                if (std::holds_alternative<game::TitleError>(events[i])) {
                    FAIL("title error: " << std::get<game::TitleError>(events[i]).reason);
                }
                if (std::holds_alternative<T>(events[i])) {
                    out = std::get<T>(events[i]);
                    events.erase(events.begin() + std::ptrdiff_t(i));
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() > end) return false;
            gateway.Poll(1ms);
            login.Poll(1ms);
            game.Poll(1ms);
            session.Update();
            for (auto& e : session.TakeEvents()) events.push_back(std::move(e));
        }
    };

    session.ConnectGateway("127.0.0.1", gateway.Port());
    proto::ServerList list;
    REQUIRE(next(list));
    REQUIRE(list.servers.size() == 1);
    CHECK(list.servers[0].name == "Test");
    CHECK(list.servers[0].state == proto::ServerState::Open);
    proto::Notices notices;
    REQUIRE(next(notices));

    session.LogIn("player", "wrong!!", 7);
    proto::LoginResult result;
    REQUIRE(next(result));
    CHECK(result.status == proto::LoginStatus::WrongPassword);
    CHECK_FALSE(session.InLobby());

    session.LogIn("player", "secret1", 7);
    REQUIRE(next(result));
    REQUIRE(result.status == proto::LoginStatus::Accepted);
    CHECK(result.ticket.empty()); // kept inside the session
    proto::LobbyEntered entered;
    REQUIRE(next(entered));
    REQUIRE(entered.status == proto::LobbyEnterStatus::Accepted);
    CHECK(session.InLobby());

    proto::CharacterCreate create;
    create.name = "Hero";
    create.model = 100;
    create.chest = 1;
    create.legs = 2;
    create.shoes = 3;
    create.weapon = 10;
    session.Create(create);
    proto::LobbyResult done;
    REQUIRE(next(done));
    CHECK(done.status == proto::LobbyStatus::Ok);
    session.ListCharacters();
    proto::CharacterList characters;
    REQUIRE(next(characters));
    REQUIRE(characters.characters.size() == 1);
    CHECK(characters.characters[0].name == "Hero");
    session.Select("Hero");
    REQUIRE(next(done));
    CHECK(done.action == proto::LobbyAction::Select);
    CHECK(done.status == proto::LobbyStatus::Ok);
    CHECK(game.Online() == 1);
    session.Disconnect();
}
