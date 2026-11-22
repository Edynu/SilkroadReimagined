// One shard, two game servers (S6): what players on different servers can do
// together. Whispers, global chat, parties through party matching; friends, whisper blocks, letters.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <typeinfo>
#include <string>
#include <thread>

#include "ServerTestSupport.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Jobs.h"
#include "sr/game/Academy.h"
#include "sr/game/AcademyProtocol.h"
#include "sr/game/OpenMarketProtocol.h"
#include "sr/game/CommunityProtocol.h"
#include "sr/game/EventProtocol.h"
#include "sr/game/ExchangeProtocol.h"
#include "sr/game/FlagWorld.h"
#include "sr/game/Arena.h"
#include "sr/server/Arena.h"
#include "sr/server/AcademyStore.h"
#include "sr/server/Academies.h"
#include "sr/server/EventStore.h"
#include "sr/game/FortressProtocol.h"
#include "sr/game/Guild.h"
#include "sr/game/GuildProtocol.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/MallProtocol.h"
#include "sr/assets/Shop.h"
#include "sr/assets/Gacha.h"
#include "sr/server/SilkStore.h"
#include "sr/server/Import.h"
#include "sr/game/StallProtocol.h"
#include "sr/assets/RegionInfo.h"
#include "sr/assets/StallNetworkData.h"
#include "sr/assets/FileSystem.h"
#include "sr/game/Party.h"
#include "sr/game/PkProtocol.h"
#include "sr/game/PlayerState.h"
#include "sr/game/TitleSession.h"
#include "sr/server/Skills.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/Guilds.h"
#include "sr/server/ShardLayout.h"
#include "sr/server/Store.h"
#include "sr/server/QuestStore.h"
#include "sr/server/Quests.h"
#include "sr/game/Quest.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
using Race = game::CreationRules::Race;
using S = proto::PartyStatus;

namespace {

constexpr std::uint32_t kGlobalChatItem = 3851; // ITEM_MALL_GLOBAL_CHATTING
constexpr std::uint32_t kStallPotion = 900201;    // a potion of the stall network's group 42

std::shared_ptr<game::CharacterRules> Rules() {
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, Race::China, true);
    rules->creation.AddArmourSet(Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(Race::China, 10);
    auto items = std::make_shared<assets::ItemTable>();
    assets::ItemRow row;
    row.refObjId = kGlobalChatItem;
    row.codename = "ITEM_MALL_GLOBAL_CHATTING";
    row.typeId = {3, 3, 3, 5};
    row.inService = true;
    row.maxStack = 50;
    items->Add(row);
    assets::ItemRow potion;
    potion.refObjId = kStallPotion;
    potion.codename = "ITEM_TEST_STALL_POTION";
    potion.typeId = {3, 3, 1, 1};
    potion.inService = true;
    potion.maxStack = 50;
    items->Add(potion);
    rules->items = std::move(items);
    return rules;
}

// Both servers' towns (the split world's east and west regions) and the network's groups.
std::shared_ptr<assets::RegionAreaTable> StallTowns() {
    auto towns = std::make_shared<assets::RegionAreaTable>();
    std::string error;
    REQUIRE(towns->Parse("#TOWN\tJangan\n168\t97\tALL\n#TOWN\tConstantinople\n79\t105\tALL\n", error));
    return towns;
}

std::shared_ptr<assets::StallNetworkData> StallGroups() {
    auto n = std::make_shared<assets::StallNetworkData>();
    n->ParseCategories("1\tWK_ARTICLES\tK\txxx\t0\t0\n1\tWK_ARTICLES_RECOVERY\tK\tWK_ARTICLES\t42\t0\n");
    n->ParseGroups("1\t42\t3\t3\t1\t1\n");
    return n;
}

struct Player {
    std::unique_ptr<Client> c;
    std::string name;
    std::int64_t character = 0;
    std::uint32_t entity = 0;
};

struct Server {
    TestDatabase& t;
    core::JobSystem jobs{2};
    std::unique_ptr<server::GameServer> game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::function<void()> pump;
    std::string error;

    // `extra`: the test's own settings over the shared ones (the game files, ...).
    Server(TestDatabase& db, std::uint16_t id, const SplitWorld* split = nullptr, const std::function<void(server::GameServerConfig&)>& extra = {})
        : t(db) {
        config.id = id;
        if (split != nullptr) split->Apply(config);
        config.listen = Local();
        config.databaseUrl = t.Url();
        config.rules = Rules();
        config.towns = StallTowns();
        config.stallNetwork = StallGroups();
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        if (extra) extra(config);
        pump = [this] { game->Poll(2ms); };
        game = std::make_unique<server::GameServer>();
        REQUIRE_MESSAGE(game->Start(config, error), error);
    }

    // `enter` false: made and chosen, but not in the world yet (Enter does that).
    Player Join(const std::string& name, int level = 30, bool enter = true) {
        Player p;
        p.name = name;
        const std::string account = "acc" + name;
        bool ok = false;
        REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        const std::int64_t id = server::FindAccount(t.Db(), account, ok)->id;
        p.c = std::make_unique<Client>(net, game->Port(), pump);
        REQUIRE(p.c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        p.c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.c->Expect(entered));
        REQUIRE(entered.status == proto::LobbyEnterStatus::Accepted);
        proto::CharacterCreate m;
        m.name = name;
        m.model = 100;
        m.chest = 1;
        m.legs = 2;
        m.shoes = 3;
        m.weapon = 10;
        p.c->Send(m);
        proto::LobbyResult r;
        REQUIRE(p.c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        REQUIRE(t.Db().Query("UPDATE _char SET curlevel = $2 WHERE charname16 = $1", {name, level}).Ok());
        p.character = t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {name}).Int(0, 0);
        p.c->Send(proto::CharacterSelect{name});
        REQUIRE(p.c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        if (enter) Enter(p);
        return p;
    }

    void Enter(Player& p) {
        p.c->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.c->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
    }
};

template <typename T>
T Next(Player& p) {
    T m;
    REQUIRE_MESSAGE(p.c->Expect(m), (p.name + " waiting for " + typeid(T).name()));
    return m;
}

proto::ChatAnswer Say(Player& p, proto::ChatKind kind, std::uint8_t index, const std::string& target, const std::string& text, std::uint8_t slot = 0) {
    proto::ChatSend s;
    s.kind = kind;
    s.index = index;
    s.target = target;
    s.text = text;
    s.slot = slot;
    p.c->Send(s);
    return Next<proto::ChatAnswer>(p);
}

// Both servers turning while either's clients wait, and each hearing of the other.
struct Pair {
    TestDatabase& t;
    Server a, b;
    explicit Pair(TestDatabase& db, const SplitWorld* split = nullptr, const std::function<void(server::GameServerConfig&)>& extra = {})
        : t(db), a(db, 1, split, extra), b(db, 2, split, extra) {
        a.pump = [this] {
            a.game->Poll(1ms);
            b.game->Poll(1ms);
        };
        b.pump = a.pump;
        for (int i = 0; i < 200; ++i) a.pump();
    }
};

// The character is put in a region and enters the world (on the server it was made on).
void Stand(TestDatabase& t, Player& p, std::uint16_t region) {
    REQUIRE(server::SavePlaces(t.Db(), {{p.character, {region, 1000.0f, 0.0f, 1000.0f, 0.0f}}}));
}

// Its client does what a client told to move does: on to the other server with the ticket.
proto::WorldEntered Arrive(Server& to, Player& p, const proto::ServerMove& move) {
    CHECK(move.port == to.game->Port());
    p.c->Connection().Close();
    p.c = std::make_unique<Client>(to.net, to.game->Port(), to.pump);
    REQUIRE(p.c->Greet());
    proto::LobbyEnter e;
    e.ticket = move.ticket;
    p.c->Send(e);
    REQUIRE(Next<proto::LobbyEntered>(p).status == proto::LobbyEnterStatus::Accepted);
    const auto w = Next<proto::WorldEntered>(p);
    REQUIRE(w.status == proto::WorldStatus::Ok);
    p.entity = w.entity;
    return w;
}

proto::PartyMatchForm Form(const char* title) {
    proto::PartyMatchForm f;
    f.settings = game::kPartyExpShare | game::kPartyItemShare;
    f.purpose = 0;
    f.levelMin = 1;
    f.levelMax = 110;
    f.title = title;
    return f;
}

// A listing another server made, once it has reached this one.
std::uint32_t FindListing(Player& p, const std::string& leader, Pair& pair) {
    for (int tries = 0; tries < 200; ++tries) {
        p.c->Send(proto::PartyMatchList{});
        const auto list = Next<proto::PartyMatchListing>(p);
        for (const proto::PartyMatchEntry& e : list.entries) {
            if (e.name == leader) return e.entry;
        }
        pair.a.pump();
    }
    FAIL("the listing never came");
    return 0;
}

// `applicant` asks to join `leader`'s listing and is taken.
void JoinThrough(Player& applicant, Player& leader, std::uint32_t entry) {
    applicant.c->Send(proto::PartyMatchJoin{entry});
    CHECK(Next<proto::PartyMatchAnswer>(applicant).status == S::Ok);
    CHECK(Next<proto::PartyMatchJoinRequest>(leader).name == applicant.name);
    leader.c->Send(proto::PartyMatchJoinAnswer{1});
    CHECK(Next<proto::PartyMatchAnswer>(applicant).status == S::Ok);
    CHECK(Next<proto::PartyMatchAnswer>(leader).status == S::Ok);
}

} // namespace

TEST_CASE("Two game servers: a whisper goes to the other server, and is refused only when nobody has that name") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Pair pair(t);
    Player ann = pair.a.Join("Ann"), bob = pair.b.Join("Bob");
    CHECK(Say(ann, proto::ChatKind::Whisper, 1, "bob", "hello over there").result == proto::ChatResult::Ok);
    const auto line = Next<proto::ChatLine>(bob);
    CHECK(line.kind == proto::ChatKind::Whisper);
    CHECK(line.name == "Ann");
    CHECK(line.text == "hello over there");
    CHECK(Say(bob, proto::ChatKind::Whisper, 2, "Ann", "and back").result == proto::ChatResult::Ok);
    CHECK(Next<proto::ChatLine>(ann).name == "Bob");
    CHECK(Say(ann, proto::ChatKind::Whisper, 3, "Nobody", "anyone?").result == proto::ChatResult::TargetNotFound);
}

TEST_CASE("Two game servers: friends made across servers: asked by name, both told, the refusals, and parted on both sides") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Pair pair(t);
    Player ann = pair.a.Join("Ann"), bob = pair.b.Join("Bob");
    CHECK(Next<proto::CommunityLists>(ann).friends.empty());
    CHECK(Next<proto::CommunityLists>(bob).friends.empty());
    const auto answer = [&](Player& p) { return Next<proto::CommunityAnswer>(p).code; };
    using C = proto::CommunityCode;

    ann.c->Send(proto::FriendAsk{"Nobody"});
    CHECK(answer(ann) == C::UnknownUser); // nobody answers for that name within the second
    ann.c->Send(proto::FriendAsk{"ANN"});
    CHECK(answer(ann) == C::UnknownUser);

    // Refused: across the servers.
    ann.c->Send(proto::FriendAsk{"bob"});
    auto offer = Next<proto::FriendProposal>(bob);
    CHECK(offer.name == "Ann");
    ann.c->Send(proto::FriendAsk{"bob"});
    CHECK(answer(ann) == C::InProgress);
    bob.c->Send(proto::FriendReply{offer.request, 0});
    CHECK(answer(ann) == C::Refused);
    CHECK(t.Db().Query("SELECT count(*) FROM _friend").Int(0, 0) == 0);

    // Accepted: written once, both lists gain the other, online.
    ann.c->Send(proto::FriendAsk{"bob"});
    offer = Next<proto::FriendProposal>(bob);
    bob.c->Send(proto::FriendReply{offer.request, 1});
    const auto forAnn = Next<proto::FriendAdded>(ann);
    const auto forBob = Next<proto::FriendAdded>(bob);
    CHECK(forAnn.record.name == "Bob");
    CHECK(forAnn.record.character == std::uint32_t(bob.character));
    CHECK(forAnn.record.online == 1);
    CHECK(forBob.record.name == "Ann");
    CHECK(forBob.record.online == 1);
    CHECK(t.Db().Query("SELECT count(*) FROM _friend").Int(0, 0) == 2);

    bob.c->Send(proto::FriendAsk{"Ann"});
    CHECK(answer(bob) == C::AlreadyFriend);

    // Parted, by the one on the other server: both lists, both rows.
    bob.c->Send(proto::FriendDelete{std::uint32_t(ann.character)});
    CHECK(Next<proto::FriendRemoved>(bob).character == std::uint32_t(ann.character));
    CHECK(Next<proto::FriendRemoved>(ann).character == std::uint32_t(bob.character));
    CHECK(t.Db().Query("SELECT count(*) FROM _friend").Int(0, 0) == 0);
}

TEST_CASE("Two game servers: a friend's lamp: entering, already there, leaving, and the server stopping") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Pair pair(t);
    Player ann = pair.a.Join("Ann", 30, false), bob = pair.b.Join("Bob", 30, false), cid = pair.b.Join("Cid", 30, false);
    for (const auto& [x, y] : {std::pair<Player*, Player*>{&ann, &bob}, {&bob, &ann}, {&ann, &cid}, {&cid, &ann}}) {
        REQUIRE(t.Db().Query("INSERT INTO _friend (charid, friendcharid, friendcharname, refobjid) VALUES ($1, $2, $3, 100)", {x->character, y->character, y->name}).Ok());
    }
    pair.a.Enter(ann);
    const auto lists = Next<proto::CommunityLists>(ann);
    REQUIRE(lists.friends.size() == 2);
    CHECK(lists.friends[0].online == 0);
    CHECK(lists.friends[1].online == 0);

    // Bob enters on the other server: Ann told of it (a line); Bob told Ann is there (the lamp only).
    pair.b.Enter(bob);
    const auto came = Next<proto::FriendPresence>(ann);
    CHECK(came.character == std::uint32_t(bob.character));
    CHECK(came.online == 1);
    CHECK(came.announce == 1);
    CHECK(Next<proto::CommunityLists>(bob).friends.size() == 1);
    const auto there = Next<proto::FriendPresence>(bob);
    CHECK(there.character == std::uint32_t(ann.character));
    CHECK(there.online == 1);
    CHECK(there.announce == 0);

    pair.b.Enter(cid);
    CHECK(Next<proto::FriendPresence>(ann).character == std::uint32_t(cid.character));

    // Cid goes.
    cid.c->Connection().Close();
    cid.c.reset();
    const auto left = Next<proto::FriendPresence>(ann);
    CHECK(left.character == std::uint32_t(cid.character));
    CHECK(left.online == 0);

    // The other server stops: Bob with it.
    pair.b.game->Stop();
    pair.a.pump = [&] { pair.a.game->Poll(1ms); };
    const auto gone = Next<proto::FriendPresence>(ann);
    CHECK(gone.character == std::uint32_t(bob.character));
    CHECK(gone.online == 0);
}

TEST_CASE("Two game servers: the whisper block list refuses whispers from the other server and this one") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Pair pair(t);
    Player ann = pair.a.Join("Ann"), bob = pair.b.Join("Bob"), cid = pair.a.Join("Cid");
    const auto change = [&](Player& p, std::uint8_t op, const char* name) {
        p.c->Send(proto::WhisperBlock{op, name});
        return Next<proto::WhisperBlockChanged>(p);
    };
    const auto added = change(ann, 1, "bob");
    CHECK(added.result == 0);
    CHECK(added.name == "Bob"); // the character's own spelling
    CHECK(change(ann, 1, "Bob").result == 1);
    CHECK(change(ann, 1, "Nobody").result == 2);
    CHECK(change(ann, 1, "Cid").result == 0);

    CHECK(Say(bob, proto::ChatKind::Whisper, 1, "Ann", "over there").result == proto::ChatResult::WhispersOff);
    CHECK(Say(cid, proto::ChatKind::Whisper, 1, "Ann", "next door").result == proto::ChatResult::WhispersOff);
    CHECK(Say(ann, proto::ChatKind::Whisper, 1, "Bob", "I may speak").result == proto::ChatResult::Ok);
    CHECK(Next<proto::ChatLine>(bob).name == "Ann");

    CHECK(change(ann, 2, "Bob").result == 0);
    CHECK(Say(bob, proto::ChatKind::Whisper, 2, "Ann", "again").result == proto::ChatResult::Ok);
    CHECK(Next<proto::ChatLine>(ann).text == "again");
    CHECK(t.Db().Query("SELECT count(*) FROM _blockedwhisperers").Int(0, 0) == 1);
}

TEST_CASE("Two game servers: a letter reaches a character on the other server or offline; the box's limit") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Pair pair(t);
    Player ann = pair.a.Join("Ann"), bob = pair.b.Join("Bob"), cid = pair.b.Join("Cid", 30, false);
    const auto write = [&](Player& p, const char* to, const char* text) {
        p.c->Send(proto::LetterWrite{to, text});
        return Next<proto::CommunityAnswer>(p).code;
    };
    using C = proto::CommunityCode;

    CHECK(write(ann, "bob", "Hello Bob\nsecond line") == C::Ok);
    CHECK(Next<proto::LetterArrived>(bob).unread == 1);
    bob.c->Send(proto::LettersAsk{});
    auto list = Next<proto::LetterListing>(bob);
    REQUIRE(list.rows.size() == 1);
    CHECK(list.rows[0].from == "Ann");
    CHECK(list.rows[0].read == 0);

    bob.c->Send(proto::LetterOpen{list.rows[0].id});
    const auto body = Next<proto::LetterBody>(bob);
    CHECK(body.from == "Ann");
    CHECK(body.text == "Hello Bob\nsecond line");
    bob.c->Send(proto::LettersAsk{});
    CHECK(Next<proto::LetterListing>(bob).rows[0].read == 1);

    // Only its owner's.
    ann.c->Send(proto::LetterDelete{list.rows[0].id});
    CHECK(Next<proto::LetterListing>(ann).rows.empty());
    bob.c->Send(proto::LettersAsk{});
    CHECK(Next<proto::LetterListing>(bob).rows.size() == 1);
    bob.c->Send(proto::LetterDelete{list.rows[0].id});
    CHECK(Next<proto::LetterListing>(bob).rows.empty());

    CHECK(write(ann, "Nobody", "x") == C::LetterUnknownUser);
    CHECK(write(ann, "bob", "") == C::LetterFailed);
    CHECK(write(ann, "Cid", "while away") == C::Ok); // offline
    pair.b.Enter(cid);
    CHECK(Next<proto::CommunityLists>(cid).unread == 1);

    for (int i = 0; i < 19; ++i) REQUIRE(write(ann, "Cid", "more") == C::Ok);
    CHECK(write(ann, "Cid", "too many") == C::TargetBoxFull);

    ann.c->Send(proto::LetterWriteMany{1, {"Bob"}, "guild news"});
    CHECK(Next<proto::CommunityAnswer>(ann).code == C::NoGuild);
}

TEST_CASE("A split world: a teleport to the other server's part moves the player there and back with all it has") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    Pair pair(t, &split);
    Player ann = pair.a.Join("Ann", 30, false);
    Stand(t, ann, kEast);
    server::StoredItem item;
    item.refId = kGlobalChatItem;
    item.quantity = 5;
    item.counted = true;
    // GiveItems is for a character nobody is playing: its session aside for a moment.
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    REQUIRE(server::GiveItems(t.Db(), 1, "Ann", {item}, 777, pair.a.error) == server::GiveResult::Done);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) SELECT userjid, 1 FROM _user WHERE charid = $1", {ann.character}).Ok());
    pair.a.Enter(ann);
    Next<proto::InventoryList>(ann);
    const auto session = [&] { return t.Db().Query("SELECT shard_id FROM sro_vt_account.sr_online_sessions").Int(0, 0); };
    CHECK(session() == 1);

    // A point in the other server's part: the character is written there, the session goes with it.
    ann.c->Send(proto::DevGoto{2});
    const auto move = Next<proto::ServerMove>(ann);
    CHECK(session() == 2);
    const auto there = Arrive(pair.b, ann, move);
    CHECK(there.position.region == kWest);
    auto list = Next<proto::InventoryList>(ann);
    CHECK(list.gold == 777);
    const auto carried = std::find_if(list.items.begin(), list.items.end(), [](const proto::ItemRecord& i) { return i.item == kGlobalChatItem; });
    REQUIRE(carried != list.items.end());
    CHECK(carried->quantity == 5);

    // A point here is an ordinary teleport; and back it comes.
    ann.c->Send(proto::DevGoto{1});
    const auto back = Next<proto::ServerMove>(ann);
    const auto home = Arrive(pair.a, ann, back);
    CHECK(home.position.region == kEast);
    CHECK(Next<proto::InventoryList>(ann).gold == 777);
    CHECK(session() == 1);

    // A ticket is good once.
    Client copy(pair.b.net, pair.a.game->Port(), pair.a.pump);
    REQUIRE(copy.Greet());
    proto::LobbyEnter e;
    e.ticket = back.ticket;
    copy.Send(e);
    proto::LobbyEntered refused;
    REQUIRE(copy.Expect(refused));
    CHECK(refused.status == proto::LobbyEnterStatus::InvalidTicket);
}

TEST_CASE("A split world: a character standing in the other server's part is sent there on entering, unless it is down") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    Pair pair(t, &split);
    Player ann = pair.a.Join("Ann", 30, false);
    Stand(t, ann, kWest);
    ann.c->Send(proto::WorldEnter{});
    const auto move = Next<proto::ServerMove>(ann);
    CHECK(Arrive(pair.b, ann, move).position.region == kWest);

    // Its server stopped: a character there cannot enter, and a gate there says so.
    Player bob = pair.a.Join("Bob", 30, false);
    Player cid = pair.a.Join("Cid", 30, false);
    Stand(t, cid, kEast);
    pair.a.Enter(cid);
    Stand(t, bob, kWest);
    ann.c->Connection().Close();
    pair.b.game->Stop();
    // Server 1 hears of it with its next report.
    for (int i = 0; i < 150; ++i) pair.a.pump();
    cid.c->Send(proto::DevGoto{2});
    CHECK(Next<proto::GateRefused>(cid).status == proto::GateStatus::ServerDown);
    bob.c->Send(proto::WorldEnter{});
    const auto entered = Next<proto::WorldEntered>(bob);
    CHECK(entered.status == proto::WorldStatus::ServerDown);
}

TEST_CASE("A split world: moving is not leaving: its party, its friends and its guild are told nothing false") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    Pair pair(t, &split);
    Player ann = pair.a.Join("Ann", 30, false), bob = pair.a.Join("Bob", 30, false);
    Stand(t, ann, kEast);
    Stand(t, bob, kEast);
    pair.a.Enter(ann);
    pair.a.Enter(bob);
    Next<proto::CommunityLists>(ann);
    Next<proto::CommunityLists>(bob);

    // Friends, and a party through matching.
    ann.c->Send(proto::FriendAsk{"Bob"});
    const auto offer = Next<proto::FriendProposal>(bob);
    bob.c->Send(proto::FriendReply{offer.request, 1});
    Next<proto::FriendAdded>(ann);
    Next<proto::FriendAdded>(bob);
    ann.c->Send(proto::PartyMatchRegister{Form("hunting")});
    REQUIRE(Next<proto::PartyMatchAnswer>(ann).status == S::Ok);
    JoinThrough(bob, ann, FindListing(bob, "Ann", pair));
    Next<proto::PartyInfo>(ann);
    Next<proto::PartyInfo>(bob);

    // Bob goes to the other server.
    bob.c->Send(proto::DevGoto{2});
    const auto move = Next<proto::ServerMove>(bob);
    Arrive(pair.b, bob, move);

    // Both see the party as it stands now, Bob still in it.
    const auto forAnn = Next<proto::PartyInfo>(ann);
    REQUIRE(forAnn.members.size() == 2);
    const auto forBob = Next<proto::PartyInfo>(bob);
    REQUIRE(forBob.members.size() == 2);
    const auto bobRecord = std::find_if(forAnn.members.begin(), forAnn.members.end(), [&](const auto& m) { return m.name == "Bob"; });
    REQUIRE(bobRecord != forAnn.members.end());
    CHECK(bobRecord->entity == 0u); // on another server now

    // Its friend's lamp moves without a word.
    const auto lamp = Next<proto::FriendPresence>(ann);
    CHECK(lamp.character == std::uint32_t(bob.character));
    CHECK(lamp.online == 1);
    CHECK(lamp.announce == 0);

    // The party still works, across.
    CHECK(Say(bob, proto::ChatKind::Party, 1, "", "over here").result == proto::ChatResult::Ok);
    CHECK(Next<proto::ChatLine>(ann).text == "over here");
    CHECK(ann.c->Expect(*std::make_unique<proto::PartyMemberLeft>(), 200ms) == false);
}

TEST_CASE("A split world: one that sets off and never arrives counts as gone, and its account is free again") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    Pair pair(t, &split);
    Player ann = pair.a.Join("Ann", 30, false), bob = pair.a.Join("Bob", 30, false);
    Stand(t, ann, kEast);
    Stand(t, bob, kEast);
    pair.a.Enter(ann);
    pair.a.Enter(bob);
    Next<proto::CommunityLists>(ann);
    Next<proto::CommunityLists>(bob);
    ann.c->Send(proto::FriendAsk{"Bob"});
    const auto offer = Next<proto::FriendProposal>(bob);
    bob.c->Send(proto::FriendReply{offer.request, 1});
    Next<proto::FriendAdded>(ann);
    Next<proto::FriendAdded>(bob);

    bob.c->Send(proto::DevGoto{2});
    Next<proto::ServerMove>(bob);
    bob.c->Connection().Close();
    const auto off = Next<proto::FriendPresence>(ann); // after moveWait (1 s here)
    CHECK(off.character == std::uint32_t(bob.character));
    CHECK(off.online == 0);
    CHECK(off.announce == 1);
}

TEST_CASE("A split world: a change of server is a logout and a login: short waits and skills' buffs end, a long wait stays (game files)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr || !std::filesystem::exists(dir)) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    std::string error;
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    const assets::SkillRow* buff = skills->Find(std::string_view("SKILL_CH_FIRE_GONGUP_A_01")); // a buff, 5 s to reuse
    const assets::SkillRow* longWait = skills->Find(std::string_view("SKILL_CH_WATER_HARMONY_A_01")); // 5 minutes to reuse
    const assets::SkillRow* scroll = skills->Find(std::string_view("SKILL_ETC_SPEED_UP_BASIC_01")); // the movement scroll's: 'cbuf', a timed job
    REQUIRE(buff != nullptr);
    REQUIRE(longWait != nullptr);
    REQUIRE(scroll != nullptr);
    CHECK(buff->cooldownMs < server::Skills::kLongCooldownMs);
    CHECK(longWait->cooldownMs >= server::Skills::kLongCooldownMs);
    SplitWorld split(t);
    split.skills = skills;
    Pair pair(t, &split);
    Player ann = pair.a.Join("Ann", 60, false);
    Stand(t, ann, kEast);
    REQUIRE(t.Db().Query("UPDATE _char SET mp = 3000 WHERE charid = $1", {ann.character}).Ok());
    for (const assets::SkillRow* s : {buff, longWait}) {
        REQUIRE(server::SaveMastery(t.Db(), ann.character, s->masteries[0], 60));
        REQUIRE(server::SaveSkill(t.Db(), ann.character, 0, s->id));
    }
    REQUIRE(server::SaveBuffs(t.Db(), ann.character, {{scroll->id, 600'000}})); // a movement scroll running
    pair.a.Enter(ann);
    REQUIRE(Next<proto::BuffAdded>(ann).skill == scroll->id);
    const auto use = [&](const assets::SkillRow* s) {
        ann.c->Send(proto::UseSkill{s->id, 0});
        return Next<proto::SkillUseAnswer>(ann).status;
    };
    REQUIRE(use(buff) == proto::SkillUseStatus::Ok);
    REQUIRE(Next<proto::BuffAdded>(ann).skill == buff->id);
    CHECK(use(buff) == proto::SkillUseStatus::Cooling); // waiting, here
    REQUIRE(use(longWait) == proto::SkillUseStatus::Ok);
    const auto written = [&] { return t.Db().Query("SELECT count(*) FROM _timedjob WHERE category = 6 AND jobid = 200 AND data1 = $1", {std::int64_t(longWait->id)}).Int(0, 0) == 1; };
    for (int i = 0; i < 500 && !written(); ++i) pair.a.pump();
    REQUIRE(written());

    ann.c->Send(proto::DevGoto{2});
    Arrive(pair.b, ann, Next<proto::ServerMove>(ann));
    // The long wait comes with it, told with the time it has left.
    proto::Cooldown cool;
    for (int i = 0; i < 8; ++i) {
        cool = Next<proto::Cooldown>(ann);
        if (cool.id == longWait->id) break;
    }
    CHECK(cool.id == longWait->id);
    CHECK(cool.ms > 280'000u);
    CHECK(cool.ms <= 300'000u);
    CHECK(use(longWait) == proto::SkillUseStatus::Cooling);
    // The scroll's buff came along; the skill's own did not, and its short wait is over.
    CHECK(Next<proto::BuffAdded>(ann).skill == scroll->id);
    proto::BuffAdded kept;
    CHECK_FALSE(ann.c->Expect(kept, 200ms));
    CHECK(use(buff) == proto::SkillUseStatus::Ok);
    REQUIRE(Next<proto::BuffAdded>(ann).skill == buff->id);
}

TEST_CASE("A split world: the client forgets what the last game server kept running") {
    game::PlayerState state;
    state.skillCooling[7] = {};
    state.groupCooling[8] = {};
    state.itemCooling[9] = {};
    state.effects.emplace_back();
    state.Apply(game::TitleEvent{proto::ServerMove{}});
    CHECK(state.skillCooling.empty());
    CHECK(state.groupCooling.empty());
    CHECK(state.itemCooling.empty());
    CHECK(state.effects.empty());
}

TEST_CASE("Two game servers: a global line needs its item, uses one up, and reaches both servers") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Pair pair(t);
    Player ann = pair.a.Join("Ann"), bob = pair.b.Join("Bob");
    CHECK(Say(ann, proto::ChatKind::Global, 1, "", "no item", game::slot::kWorn).result == proto::ChatResult::InvalidCommand);

    // A new login takes the item given while away.
    ann.c->Connection().Close();
    for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions").Int(0, 0) > 1; ++i) pair.a.pump();
    server::StoredItem item;
    item.refId = kGlobalChatItem;
    item.quantity = 2;
    item.counted = true;
    REQUIRE(server::GiveItems(t.Db(), 1, "Ann", {item}, 0, pair.a.error) == server::GiveResult::Done);
    const std::int64_t account = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {ann.character}).Int(0, 0);
    Player again;
    again.name = "Ann";
    again.character = ann.character;
    again.c = std::make_unique<Client>(pair.a.net, pair.a.game->Port(), pair.a.pump);
    REQUIRE(again.c->Greet());
    proto::LobbyEnter enter;
    enter.ticket = server::IssueTicket(t.Db(), account, 1, 30, pair.a.error);
    again.c->Send(enter);
    REQUIRE(Next<proto::LobbyEntered>(again).status == proto::LobbyEnterStatus::Accepted);
    again.c->Send(proto::CharacterSelect{"Ann"});
    REQUIRE(Next<proto::LobbyResult>(again).status == proto::LobbyStatus::Ok);
    again.c->Send(proto::WorldEnter{});
    REQUIRE(Next<proto::WorldEntered>(again).status == proto::WorldStatus::Ok);

    const server::QueryResult place = t.Db().Query("SELECT i.slot FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND it.refitemid = $2",
                                                   {again.character, std::int64_t(kGlobalChatItem)});
    REQUIRE(place.Rows() == 1);
    const auto slot = std::uint8_t(place.Int(0, 0));
    CHECK(Say(again, proto::ChatKind::Global, 2, "", "to everyone", slot).result == proto::ChatResult::Ok);
    auto line = Next<proto::ChatLine>(bob);
    CHECK(line.kind == proto::ChatKind::Global);
    CHECK(line.name == "Ann");
    CHECK(line.text == "to everyone");
    CHECK(Next<proto::ChatLine>(again).text == "to everyone"); // the speaker too
    CHECK(Next<proto::ItemMoved>(again).changed.front().quantity == 1);
}

TEST_CASE("Two game servers: the website's commands: an announcement, the console, a kick with a ban for a time, who plays") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Pair pair(t);
    Player ann = pair.a.Join("Ann"), bob = pair.b.Join("Bob");
    const std::string shard = pair.a.config.shardSchema;
    // SroWeb's row and its notice (sr_admin_commands, NOTIFY sr_admin_<shard> with the id).
    const auto command = [&](int server, const std::string& kind, const std::string& text) {
        const server::QueryResult row = t.Db().Query("INSERT INTO sro_vt_account.sr_admin_commands (shard, server_id, kind, text, created_by) "
                                                     "VALUES ($1, $2, $3, $4, 'tester') RETURNING id",
                                                     {shard, server, kind, text});
        REQUIRE(row.Ok());
        const std::int64_t id = row.Int(0, 0);
        REQUIRE(server::Notify(t.Db(), "sr_admin_" + shard, std::to_string(id)));
        return id;
    };
    // The answers written so far, once `servers` of them are in.
    const auto answered = [&](std::int64_t id, int servers) {
        std::string r;
        for (int i = 0; i < 2000; ++i) {
            pair.a.pump();
            r = t.Db().Query("SELECT coalesce(result, '') FROM sro_vt_account.sr_admin_commands WHERE id = $1", {id}).Text(0, 0);
            int n = 0;
            for (std::size_t at = r.find("server "); at != std::string::npos; at = r.find("server ", at + 1)) ++n;
            if (n >= servers) break;
        }
        return r;
    };

    // An announcement for every server: both players see the notice; each server answers.
    const std::int64_t everyone = command(0, "announce", "Maintenance at 18:00.");
    const proto::ChatLine toAnn = Next<proto::ChatLine>(ann), toBob = Next<proto::ChatLine>(bob);
    CHECK(toAnn.kind == proto::ChatKind::Notice);
    CHECK(toAnn.text == "Maintenance at 18:00.");
    CHECK(toBob.text == "Maintenance at 18:00.");
    const std::string both = answered(everyone, 2);
    CHECK(both.find("server 1: told to 1 players") != std::string::npos);
    CHECK(both.find("server 2: told to 1 players") != std::string::npos);
    CHECK(t.Db().Query("SELECT done_at IS NOT NULL, done_by IS NOT NULL FROM sro_vt_account.sr_admin_commands WHERE id = $1", {everyone}).Text(0, 0) == "t");

    // The console on one server: its GM commands, an unknown one, a line that is no command; the other never answers.
    CHECK(answered(command(1, "console", "/online"), 1) == "server 1: 1 online: Ann");
    {
        const std::string where = answered(command(2, "console", "/where Bob"), 1);
        CHECK(where.starts_with("server 2: Bob: ")); // REGION,X,Y,Z,HEADING (beta-helpers' form)
        CHECK(std::count(where.begin(), where.end(), ',') == 4);
    }
    CHECK(answered(command(1, "console", "/teleport Bob"), 1) == "server 1: unknown command /teleport (/help lists them)");
    CHECK(answered(command(1, "console", "rm -rf /"), 1) == "server 1: not a command (commands start with /)");
    // Refused: no such kind, not an account name.
    CHECK(answered(command(1, "shutdown", "now"), 1) == "server 1: refused: no such kind");
    CHECK(answered(command(1, "kick", "no such name!"), 1) == "server 1: refused: not an account name");

    // Who plays: the session's character (account migration 5), cleared in the lobby.
    const std::int64_t annAccount = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {ann.character}).Int(0, 0);
    CHECK(t.Db().Query("SELECT character_id FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {annAccount}).Int(0, 0) == ann.character);

    // A ban for an hour, as the site writes it (the login's block), then the kick: Ann is told why and until when and
    // goes; the other server does not have her.
    REQUIRE(server::SetBlock(t.Db(), "accAnn", "Cheating", 3600));
    const std::int64_t kick = command(0, "kick", "accAnn");
    const proto::Disconnected bye = Next<proto::Disconnected>(ann);
    CHECK(bye.reason == proto::DisconnectReason::Blocked);
    CHECK(bye.text == "Cheating");
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK(bye.until > now + 3500);
    CHECK(bye.until < now + 3700);
    const std::string kicked = answered(kick, 2);
    CHECK(kicked.find("server 1: kicked Ann") != std::string::npos);
    CHECK(kicked.find("server 2: not here") != std::string::npos);
    for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {annAccount}).Int(0, 0) > 0; ++i) pair.a.pump();
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {annAccount}).Int(0, 0) == 0);
}

TEST_CASE("Two game servers: a party formed through matching: both sides told, its chat, the requests of a member elsewhere") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Pair pair(t);
    Player ann = pair.a.Join("Ann"), cid = pair.a.Join("Cid"), bob = pair.b.Join("Bob"), dan = pair.b.Join("Dan");

    ann.c->Send(proto::PartyMatchRegister{Form("hunting")});
    const auto registered = Next<proto::PartyMatchAnswer>(ann);
    REQUIRE(registered.status == S::Ok);

    // Bob, on the other server, finds it and is taken; each side knows only its own side's entities.
    const std::uint32_t entry = FindListing(bob, "Ann", pair);
    CHECK(entry == registered.entry);
    JoinThrough(bob, ann, entry);
    const auto atAnn = Next<proto::PartyInfo>(ann);
    const auto atBob = Next<proto::PartyInfo>(bob);
    REQUIRE(atAnn.members.size() == 2);
    REQUIRE(atBob.members.size() == 2);
    CHECK(atAnn.party == atBob.party);
    CHECK(atAnn.leader == std::uint32_t(ann.character));
    const auto record = [](const proto::PartyInfo& info, const std::string& name) {
        return *std::find_if(info.members.begin(), info.members.end(), [&](const proto::PartyMemberRecord& r) { return r.name == name; });
    };
    CHECK(record(atAnn, "Ann").entity == ann.entity);
    CHECK(record(atAnn, "Bob").entity == 0u);
    CHECK(record(atBob, "Bob").entity == bob.entity);
    CHECK(record(atBob, "Ann").entity == 0u);

    // A third, on Bob's server: Ann and Bob hear of it, Dan gets the whole party.
    const std::uint32_t again = FindListing(dan, "Ann", pair);
    JoinThrough(dan, ann, again);
    CHECK(Next<proto::PartyMemberJoined>(ann).member.name == "Dan");
    CHECK(Next<proto::PartyMemberJoined>(bob).member.name == "Dan");
    CHECK(Next<proto::PartyInfo>(dan).members.size() == 3);

    // The party's chat crosses, from a member on the mirror's side.
    CHECK(Say(bob, proto::ChatKind::Party, 1, "", "over here").result == proto::ChatResult::Ok);
    CHECK(Next<proto::ChatLine>(ann).text == "over here");
    CHECK(Next<proto::ChatLine>(dan).text == "over here");

    // The master, on its home, puts out a member on the other server: everyone hears, it too.
    ann.c->Send(proto::PartyKick{std::uint32_t(dan.character)});
    CHECK(Next<proto::PartyMemberLeft>(ann).member == std::uint32_t(dan.character));
    CHECK(Next<proto::PartyMemberLeft>(bob).member == std::uint32_t(dan.character));
    CHECK(Next<proto::PartyMemberLeft>(dan).reason == proto::PartyLeaveReason::Kicked);

    // A member elsewhere tries the master's work, then leaves: asked at home, answered back.
    bob.c->Send(proto::PartySetSettings{game::kPartyExpShare});
    CHECK(Next<proto::PartyResult>(bob).status == S::NotLeader);
    bob.c->Send(proto::PartyLeave{});
    CHECK(Next<proto::PartyMemberLeft>(bob).member == std::uint32_t(bob.character));
    CHECK(Next<proto::PartyDismissed>(ann).reason == 2);
}

TEST_CASE("Two game servers: when one stops, its parties and its listings go with it") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Pair pair(t);
    Player ann = pair.a.Join("Ann"), bob = pair.b.Join("Bob");
    ann.c->Send(proto::PartyMatchRegister{Form("hunting")});
    const auto registered = Next<proto::PartyMatchAnswer>(ann);
    REQUIRE(registered.status == S::Ok);
    JoinThrough(bob, ann, FindListing(bob, "Ann", pair));
    Next<proto::PartyInfo>(ann);
    Next<proto::PartyInfo>(bob);

    // A listing on the server that stops: gone from the one that stays.
    bob.c->Send(proto::PartyLeave{});
    Next<proto::PartyDismissed>(ann);
    Next<proto::PartyMemberLeft>(bob);
    bob.c->Send(proto::PartyMatchRegister{Form("on the other side")});
    REQUIRE(Next<proto::PartyMatchAnswer>(bob).status == S::Ok);
    CHECK(FindListing(ann, "Bob", pair) != 0);
    pair.b.game->Stop();
    for (int i = 0; i < 300; ++i) pair.a.game->Poll(1ms);
    pair.a.pump = [&] { pair.a.game->Poll(1ms); };
    ann.c->Send(proto::PartyMatchList{});
    bool listed = true;
    proto::PartyMatchListing list;
    REQUIRE(ann.c->Expect(list));
    listed = std::any_of(list.entries.begin(), list.entries.end(), [](const proto::PartyMatchEntry& e) { return e.name == "Bob"; });
    CHECK_FALSE(listed);
}

// An exchange needs its two players within 320 of each other, so both are always on one server (nobody
// walks across a border); what can happen is one of them leaving for the other
// server's part while it is open: it ends for the one left behind, nothing moved.
TEST_CASE("A split world: an open exchange ends when its player leaves for the other server, its gold intact") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    Pair pair(t, &split);
    Player ann = pair.a.Join("Ann", 30, false);
    Player bob = pair.a.Join("Bob", 30, false);
    Stand(t, ann, kEast);
    Stand(t, bob, kEast);
    REQUIRE(t.Db().Query("UPDATE _char SET remaingold = 500 WHERE charid = $1", {ann.character}).Ok());
    pair.a.Enter(ann);
    pair.a.Enter(bob);
    ann.c->Send(proto::ExchangeAsk{bob.entity});
    CHECK(Next<proto::ExchangeAsked>(bob).name == "Ann");
    bob.c->Send(proto::ExchangeReply{1});
    CHECK(Next<proto::ExchangeOpened>(ann).peer == bob.entity);
    ann.c->Send(proto::ExchangeGold{300});
    CHECK(Next<proto::ExchangeSide>(bob).gold == 300);

    ann.c->Send(proto::DevGoto{2});
    const auto move = Next<proto::ServerMove>(ann);
    CHECK(Next<proto::ExchangeClosed>(bob).code == proto::ExchangeCode::Cancelled);
    Arrive(pair.b, ann, move);
    CHECK(Next<proto::InventoryList>(ann).gold == 500);
}

TEST_CASE("A split world: a stall on the other server is found through the network and bought from afar in one go") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    Pair pair(t, &split);
    Player sel = pair.b.Join("Sel", 30, false);
    Player buy = pair.a.Join("Buy", 30, false);
    Stand(t, sel, kWest);
    Stand(t, buy, kEast);
    // The potions given while Sel is not in the game (its session aside for a moment).
    const std::int64_t jid = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {sel.character}).Int(0, 0);
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {jid}).Ok());
    server::StoredItem potions;
    potions.refId = kStallPotion;
    potions.quantity = 5;
    potions.counted = true;
    std::string error;
    REQUIRE(server::GiveItems(t.Db(), 2, "Sel", {potions}, 0, error) == server::GiveResult::Done);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, 2)", {jid}).Ok());
    REQUIRE(t.Db().Query("UPDATE _char SET remaingold = 1000 WHERE charid = $1", {buy.character}).Ok());
    pair.b.Enter(sel);
    pair.a.Enter(buy);
    const auto bag = Next<proto::InventoryList>(sel);
    std::uint8_t place = 0;
    for (const proto::ItemRecord& i : bag.items) {
        if (i.item == kStallPotion) place = i.slot;
    }
    REQUIRE(place >= 13);

    sel.c->Send(proto::StallOpen{"West Wares"});
    Next<proto::StallView>(sel);
    sel.c->Send(proto::StallPut{0, place, 250});
    Next<proto::StallSlotChanged>(sel);
    sel.c->Send(proto::StallSetOpen{1, 1});
    CHECK(Next<proto::StallStateChanged>(sel).network == 1);

    // The row reaches server 1 over the bus.
    proto::NetworkPage page;
    for (int tries = 0; tries < 4 && page.rows.empty(); ++tries) {
        for (int i = 0; i < 200; ++i) pair.a.pump();
        if (tries > 0) std::this_thread::sleep_for(3100ms); // a new search at most once every 3 s
        buy.c->Send(proto::NetworkSearch{42, 0});
        page = Next<proto::NetworkPage>(buy);
    }
    REQUIRE(page.rows.size() == 1);
    CHECK(page.rows[0].owner == "Sel");
    CHECK(page.rows[0].town == 1); // Constantinople
    CHECK(page.rows[0].item.quantity == 5);

    const proto::NetworkRow row = page.rows[0];
    buy.c->Send(proto::NetworkBuy{row.listing, row.revision, row.price});
    const auto got = Next<proto::StallBought>(buy);
    REQUIRE(got.code == proto::StallCode::Ok);
    CHECK(got.network == 1);
    CHECK(got.item.item == kStallPotion);
    CHECK(got.item.quantity == 5);
    CHECK(got.gold == 750);
    const auto sold = Next<proto::StallSold>(sel);
    CHECK(sold.place == place);
    CHECK(sold.buyer == "Buy");
    CHECK(sold.commission == 2); // 1% of 250
    CHECK(sold.gold == 248);

    // One transaction: the potions in Buy's bag where it was told, gone from Sel's, the gold moved.
    const auto at = [&](std::int64_t character, std::uint8_t slot) {
        return t.Db()
            .Query("SELECT count(*) FROM _inventory i JOIN _items it ON it.id64 = i.itemid WHERE i.charid = $1 AND i.slot = $2 AND it.refitemid = $3",
                   {character, int(slot), std::int64_t(kStallPotion)})
            .Int(0, 0);
    };
    const auto gold = [&](std::int64_t character) { return t.Db().Query("SELECT remaingold FROM _char WHERE charid = $1", {character}).Int(0, 0); };
    for (int i = 0; i < 200 && gold(buy.character) != 750; ++i) pair.a.pump();
    CHECK(at(buy.character, got.item.slot) == 1);
    CHECK(at(sel.character, place) == 0);
    CHECK(gold(buy.character) == 750);
    CHECK(gold(sel.character) == 248);

    // Sold: the row is gone everywhere.
    buy.c->Send(proto::NetworkBuy{row.listing, row.revision, row.price});
    CHECK(Next<proto::StallBought>(buy).code == proto::StallCode::Changed);
}


TEST_CASE("A split world: a Forgotten World gap opened on one server; a party member from the other comes over, sees it and enters (game files, export)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    const std::filesystem::path exportDir = ExportDir();
    if (dir == nullptr || !std::filesystem::exists(dir) || !std::filesystem::exists(exportDir / "SRO_VT_SHARD" / "schema.tsv")) {
        MESSAGE("SR_GAME_DIR or SR_VSRO_EXPORT missing - skipped");
        return;
    }
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    // The original's tables (the Forgotten World's catalog), then the test's own split of the world.
    server::ImportOptions options;
    options.dir = exportDir;
    options.databases = {"SRO_VT_SHARD"};
    options.replace = true;
    server::ImportReport report;
    REQUIRE_MESSAGE(server::ImportDatabases(t.Db(), options, report, error), error);
    REQUIRE(t.Db().Query("DROP TABLE sro_vt_shard._refregion").Ok());
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    auto items = std::make_shared<assets::ItemTable>();
    auto skills = std::make_shared<assets::SkillTable>();
    REQUIRE_MESSAGE(characters->Load(fs, error), error);
    REQUIRE_MESSAGE(items->Load(fs, error), error);
    REQUIRE_MESSAGE(skills->Load(fs, error), error);
    const assets::ItemRow* hole = items->Find(std::string_view("ITEM_ETC_TELEPORT_HOLE_TOGUI_35_50_LEVEL_1"));
    REQUIRE(hole != nullptr);
    SplitWorld split(t);
    split.skills = skills;
    auto rules = Rules();
    rules->items = items;
    Pair pair(t, &split, [&](server::GameServerConfig& c) {
        c.files = &fs;
        c.characters = characters;
        c.rules = rules;
    });

    // Ann in Jangan (server 1's part, a town) with a hole; Bob in Constantinople (server 2's).
    Player ann = pair.a.Join("Ann", 40, false);
    Stand(t, ann, kEast);
    server::StoredItem item;
    item.refId = hole->refObjId;
    item.quantity = 1;
    item.counted = true;
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    REQUIRE(server::GiveItems(t.Db(), 1, "Ann", {item}, 0, pair.a.error) == server::GiveResult::Done);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) SELECT userjid, 1 FROM _user WHERE charid = $1", {ann.character}).Ok());
    pair.a.Enter(ann);
    const auto bag = Next<proto::InventoryList>(ann);
    const auto held = std::find_if(bag.items.begin(), bag.items.end(), [&](const proto::ItemRecord& i) { return i.item == hole->refObjId; });
    REQUIRE(held != bag.items.end());
    Player bob = pair.b.Join("Bob", 40, false);
    Stand(t, bob, kWest);
    pair.b.Enter(bob);
    Player cid = pair.b.Join("Cid", 40, false);
    Stand(t, cid, kWest);
    pair.b.Enter(cid);
    // One party across the servers (through matching), Ann its master.
    ann.c->Send(proto::PartyMatchRegister{Form("Into the Forgotten World")});
    REQUIRE(Next<proto::PartyMatchAnswer>(ann).status == S::Ok);
    const std::uint32_t listing = FindListing(bob, "Ann", pair);
    JoinThrough(bob, ann, listing);
    JoinThrough(cid, ann, listing);

    // Ann opens the hole in Jangan: the gap stands at her feet.
    ann.c->Send(proto::UseItem{held->slot});
    std::uint32_t gap = 0;
    for (int i = 0; i < 40 && gap == 0; ++i) {
        proto::EntityAppeared seen;
        if (ann.c->Expect(seen, 1s) && seen.character.model == 19556) gap = seen.entity; // INS_QUEST_TELEPORT
    }
    REQUIRE(gap != 0);

    // Bob comes over to Jangan (server 1): he sees the gap, its facts are his party's, and he enters.
    bob.c->Send(proto::DevGoto{1});
    Arrive(pair.a, bob, Next<proto::ServerMove>(bob));
    bool seesGap = false;
    for (int i = 0; i < 40 && !seesGap; ++i) {
        proto::EntityAppeared seen;
        seesGap = bob.c->Expect(seen, 1s) && seen.entity == gap;
    }
    REQUIRE(seesGap);
    bob.c->Send(proto::SelectGap{gap});
    const auto info = Next<proto::GapInfo>(bob);
    CHECK(info.world == 63);
    CHECK(info.master == "Ann");
    CHECK(info.own);
    bob.c->Send(proto::EnterGap{gap});
    proto::WorldEntered inside;
    REQUIRE(bob.c->Expect(inside, 20s));
    CHECK(inside.position.region == 32484);
    const auto state = Next<proto::InstanceState>(bob);
    CHECK(state.world == 63);
    CHECK(state.secondsLeft > 7000);
    // Ann follows: the same instance (they see each other there).
    ann.c->Send(proto::EnterGap{gap});
    proto::WorldEntered annInside;
    REQUIRE(ann.c->Expect(annInside, 20s));
    CHECK(annInside.position.region == 32484);
    bool together = false;
    std::uint32_t pillar = 0;
    proto::WorldPosition pillarAt;
    for (int i = 0; i < 60 && (!together || pillar == 0); ++i) {
        proto::EntityAppeared seen;
        if (!bob.c->Expect(seen, 1s)) continue;
        together = together || seen.entity == ann.entity;
        if (seen.character.model == 36033 && pillar == 0) { // NPC_FWORLD_RECALL_PARTY
            pillar = seen.entity;
            pillarAt = seen.position;
        }
    }
    CHECK(together);
    REQUIRE(pillar != 0);

    // Ann summons Cid from Constantinople (server 2) at the party recall pillar: asked there, he comes over
    // and straight into the instance; his way home stays his own town on server 2.
    ann.c->Send(proto::MoveTo{pillarAt});
    // Talked to there, the pillar's NPC window has its one row, "Summon Party member" (1.188's NPC function 0x27).
    proto::NpcMenu menu;
    for (int i = 0; i < 20 && menu.entity != pillar; ++i) {
        ann.c->Send(proto::NpcTalk{pillar});
        ann.c->Expect(menu, 1s);
    }
    CHECK(menu.entity == pillar);
    CHECK(menu.services == proto::kNpcPartySummon);
    proto::SummonList list;
    for (int i = 0; i < 20; ++i) {
        ann.c->Send(proto::OpenSummon{pillar});
        if (ann.c->Expect(list, 1s)) break;
    }
    const auto cidRow = std::find_if(list.rows.begin(), list.rows.end(), [&](const auto& r) { return r.name == "Cid"; });
    REQUIRE(cidRow != list.rows.end());
    CHECK(cidRow->state == 2);
    const auto bobRow = std::find_if(list.rows.begin(), list.rows.end(), [&](const auto& r) { return r.name == "Bob"; });
    REQUIRE(bobRow != list.rows.end());
    CHECK(bobRow->state == 1); // inside
    ann.c->Send(proto::SummonMember{cidRow->character});
    const auto asked = Next<proto::SummonAsked>(cid);
    CHECK(asked.master == "Ann");
    cid.c->Send(proto::SummonReply{true});
    proto::WorldEntered cidInside = Arrive(pair.a, cid, Next<proto::ServerMove>(cid)); // taken in on arrival: straight there, or a teleport after
    bool cidIn = cidInside.position.region == 32484;
    for (int i = 0; i < 4 && !cidIn; ++i) cidIn = cid.c->Expect(cidInside, 10s) && cidInside.position.region == 32484;
    REQUIRE(cidIn);
    CHECK(Next<proto::InstanceState>(cid).world == 63);
    // Out of the game inside: kept where he was summoned from, Constantinople.
    cid.c->Connection().Close();
    for (int i = 0; i < 300; ++i) {
        pair.a.pump();
        if (t.Db().Query("SELECT latestregion FROM _char WHERE charname16 = 'Cid'").Int(0, 0) == std::int16_t(kWest)) break;
        std::this_thread::sleep_for(10ms);
    }
    CHECK(std::uint16_t(t.Db().Query("SELECT latestregion FROM _char WHERE charname16 = 'Cid'").Int(0, 0)) == kWest);
}

// --- The fortress war across two game servers (step 1) ---

namespace {

// A character made before (its guild written since) back into the world on `on`, standing in `region`.
Player Again(Server& on, TestDatabase& t, const std::string& name, std::uint16_t region) {
    Player p;
    p.name = name;
    bool ok = false;
    const std::int64_t id = server::FindAccount(t.Db(), "acc" + name, ok)->id;
    p.character = t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {name}).Int(0, 0);
    Stand(t, p, region);
    p.c = std::make_unique<Client>(on.net, on.game->Port(), on.pump);
    REQUIRE(p.c->Greet());
    proto::LobbyEnter e;
    e.ticket = server::IssueTicket(t.Db(), id, on.config.id, 30, on.error);
    p.c->Send(e);
    REQUIRE(Next<proto::LobbyEntered>(p).status == proto::LobbyEnterStatus::Accepted);
    p.c->Send(proto::CharacterSelect{name});
    REQUIRE(Next<proto::LobbyResult>(p).status == proto::LobbyStatus::Ok);
    on.Enter(p);
    return p;
}

template <typename T, typename Pred>
T Until(Player& p, Pred pred, std::chrono::milliseconds timeout = 10s) {
    T m;
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
        if (p.c->Expect(m, 1s) && pred(m)) return m;
    }
    FAIL_CHECK(p.name << " never got the awaited " << typeid(T).name());
    return m;
}

} // namespace

TEST_CASE("A split world: the fortress war seen from the other server: the list, the war's word, the guild's refusals, Return to fortress, a rank told to the guild") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    constexpr std::uint32_t kOfficialRef = 19498, kGuildNpcRef = 19470;
    const auto fortress = [&](server::GameServerConfig& c) {
        // Jangan's fortress in server 1's part (East), its Gate of Resurrection there; server 2 (West) holds a mirror.
        server::FortressSpec f;
        f.id = 1;
        f.codename = "FORTRESS_JANGAN";
        f.nameKey = "SN_FORTRESS_JANGAN";
        f.official = "NPC_CH_FORTRESS_OFFICIAL";
        f.fee = 1000;
        f.regions = {kEast};
        f.revivalGate = "GATE_E";
        c.fortresses = {f};
        c.fortress.battleRanks = {{6, 1, 0}}; // ours, for the test: the last rank at the first kill
        c.fortress.warningMinutes.clear();
        c.npcs.push_back({kOfficialRef, "NPC_CH_FORTRESS_OFFICIAL", {kEast, 1000.0f, 0.0f, 1005.0f}, 0.0f});
        c.npcs.push_back({kGuildNpcRef, "NPC_WC_GUILD", {kWest, 1000.0f, 0.0f, 1005.0f}, 0.0f});
    };
    {
        // The characters first; then their guilds and the fortress's owner, read by the servers as they start.
        Pair first(t, &split, fortress);
        first.a.Join("Wolf", 30, false);
        first.b.Join("Wulf", 30, false);
        first.a.Join("Drag", 30, false);
    }
    const auto charOf = [&](const char* n) { return t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {std::string(n)}).Int(0, 0); };
    const auto guild = [&](int id, const char* name, const std::vector<std::pair<const char*, int>>& members, int extra) {
        REQUIRE(t.Db().Query("INSERT INTO _guild (id, name, lvl, gatheredsp, foundationdate, alliance, booty) OVERRIDING SYSTEM VALUE VALUES ($1, $2, 3, 0, now(), 0, 0)",
                             {id, std::string(name)}).Ok());
        const auto add = [&](std::int64_t character, const std::string& n, int memberClass) {
            REQUIRE(t.Db().Query("INSERT INTO _guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate, permission, siegeauthority) "
                                 "VALUES ($1, $2, $3, $4, 30, 0, now(), $5, $6)",
                                 {id, character, n, memberClass, memberClass == 0 ? -1 : 0, memberClass == 0 ? 1 : 0}).Ok());
            REQUIRE(t.Db().Query("UPDATE _char SET guildid = $1 WHERE charid = $2", {id, character}).Ok());
        };
        for (const auto& [n, cls] : members) add(charOf(n), n, cls);
        for (int i = 0; i < extra; ++i) add(9'000'000 + id * 100 + i, std::string(name) + std::to_string(i), 10);
    };
    guild(201, "Wolves", {{"Wulf", 0}, {"Wolf", 10}}, 5);
    guild(202, "Dragons", {{"Drag", 0}}, 6);
    REQUIRE(t.Db().Query("UPDATE _char SET remaingold = 1000000 WHERE charname16 = 'Drag'").Ok());
    REQUIRE(t.Db().Query("DELETE FROM _siegefortress WHERE fortressid = 1").Ok()); // made by the first start, owner none
    REQUIRE(t.Db().Query("INSERT INTO _siegefortress (fortressid, guildid) VALUES (1, 201)").Ok());

    Pair pair(t, &split, fortress);
    REQUIRE(pair.a.game->GetFortresses() != nullptr);
    REQUIRE(pair.b.game->GetFortresses() != nullptr); // the mirror
    Player wolf = Again(pair.a, t, "Wolf", kEast);
    Player drag = Again(pair.a, t, "Drag", kEast);
    Player wulf = Again(pair.b, t, "Wulf", kWest);
    // The West member is told the fortresses on entering, as the East ones are: Jangan, the Wolves'.
    const auto list = Until<proto::FortressList>(wulf, [](const proto::FortressList& l) { return !l.fortresses.empty(); });
    REQUIRE(list.fortresses.size() == 1);
    CHECK(list.fortresses[0].owner == 201);
    CHECK(list.fortresses[0].ownerName == "Wolves");

    // The application period, switched from West (the GM's STARTREQ goes to the server keeping the periods): heard there.
    wulf.c->Send(proto::FortressDev{proto::FortressDevAction::StartRequests});
    Until<proto::FortressNotice>(wulf, [](const proto::FortressNotice& n) { return n.kind == proto::FortressNoticeKind::RequestsOpen; });
    Until<proto::FortressNotice>(drag, [](const proto::FortressNotice& n) { return n.kind == proto::FortressNoticeKind::RequestsOpen; });
    // The Dragons apply on East; West's mirror has their application.
    const auto official = Until<proto::EntityAppeared>(drag, [](const proto::EntityAppeared& e) { return e.character.model == kOfficialRef; });
    drag.c->Send(proto::FortressApply{official.entity, 1, proto::FortressSide::Attack});
    proto::FortressAnswer applied = Until<proto::FortressAnswer>(drag, [](const proto::FortressAnswer& a) { return a.action == proto::FortressAction::Apply; });
    CHECK(applied.ok);
    bool mirrored = false;
    for (int i = 0; i < 400 && !mirrored; ++i) {
        pair.a.pump();
        mirrored = pair.b.game->GetFortresses()->GuildInWar(202);
    }
    CHECK(mirrored);

    // The war, switched from West, heard on both.
    wulf.c->Send(proto::FortressDev{proto::FortressDevAction::EndRequests});
    wulf.c->Send(proto::FortressDev{proto::FortressDevAction::StartWar});
    Until<proto::FortressNotice>(wulf, [](const proto::FortressNotice& n) { return n.kind == proto::FortressNoticeKind::WarBegin; });
    Until<proto::FortressNotice>(wolf, [](const proto::FortressNotice& n) { return n.kind == proto::FortressNoticeKind::WarBegin; });
    CHECK(pair.b.game->GetFortresses()->WarOn());

    // The Wolves' master on West: his guild holds Jangan, it may not disband (0x4C70); no position while the war runs (0x4C5D).
    const auto keeper = Until<proto::EntityAppeared>(wulf, [](const proto::EntityAppeared& e) { return e.character.model == kGuildNpcRef; });
    wulf.c->Send(proto::GuildDisband{keeper.entity});
    CHECK(Until<proto::GuildAnswer>(wulf, [](const proto::GuildAnswer& a) { return a.action == proto::GuildAction::Disband; }).error ==
          std::uint16_t(game::GuildError::DisbandFortress));
    wulf.c->Send(proto::GuildPositionGrant{std::uint32_t(wolf.character), game::kPositionDeputy});
    CHECK(Until<proto::GuildAnswer>(wulf, [](const proto::GuildAnswer& a) { return a.action == proto::GuildAction::Position; }).error ==
          std::uint16_t(game::GuildError::PositionInWar));

    // A Wolf kills a Dragon in the fortress's area on East: his rank told to his guild, its master on West hears it.
    wolf.c->Send(proto::DevStrike{drag.entity, 1'000'000});
    const auto rank = Until<proto::FortressNotice>(wulf, [](const proto::FortressNotice& n) { return n.kind == proto::FortressNoticeKind::RankUp; });
    CHECK(rank.value == 6);
    CHECK(rank.guild == "Wolf");

    // The war ends; Return to fortress from West: on to East, at Jangan's Gate of Resurrection.
    wulf.c->Send(proto::FortressDev{proto::FortressDevAction::EndWar});
    Until<proto::FortressNotice>(wulf, [](const proto::FortressNotice& n) { return n.kind == proto::FortressNoticeKind::WarEnd; });
    wulf.c->Send(proto::FortressReturn{1});
    const auto there = Arrive(pair.a, wulf, Next<proto::ServerMove>(wulf));
    CHECK(there.position.region == kEast);
}

TEST_CASE("A split world: a fortress's word to its guild and to everyone reaches the other server: hired NPCs (0x12) and a new tax rate (7)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    constexpr std::uint32_t kManagerRef = 19499;
    const auto fortress = [&](server::GameServerConfig& c) {
        // Jangan's fortress and its manager in server 1's part (East); server 2 (West) mirrors it.
        server::FortressSpec f;
        f.id = 1;
        f.codename = "FORTRESS_JANGAN";
        f.nameKey = "SN_FORTRESS_JANGAN";
        f.official = "NPC_CH_FORTRESS_OFFICIAL";
        f.regions = {kEast};
        c.fortresses = {f};
        c.fortress.warningMinutes.clear();
        c.npcs.push_back({kManagerRef, "NPC_CH_FORTRESS_MANAGER1", {kEast, 1000.0f, 0.0f, 1005.0f}, 0.0f});
    };
    {
        Pair first(t, &split, fortress);
        first.a.Join("Wolf", 30, false);
        first.b.Join("Wulf", 30, false);
        first.b.Join("Drag", 30, false);
    }
    const auto charOf = [&](const char* n) { return t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {std::string(n)}).Int(0, 0); };
    // The Wolves hold Jangan: Wolf, their master, on East; Wulf, a member, on West. Drag (no guild) on West.
    REQUIRE(t.Db().Query("INSERT INTO _guild (id, name, lvl, gatheredsp, foundationdate, alliance, booty) OVERRIDING SYSTEM VALUE VALUES (201, 'Wolves', 3, 10000, now(), 0, 0)").Ok());
    for (const auto& [n, cls] : std::vector<std::pair<const char*, int>>{{"Wolf", 0}, {"Wulf", 10}}) {
        REQUIRE(t.Db().Query("INSERT INTO _guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate, permission, siegeauthority) "
                             "VALUES (201, $1, $2, $3, 30, 0, now(), $4, $5)",
                             {charOf(n), std::string(n), cls, cls == 0 ? -1 : 0, cls == 0 ? 1 : 0}).Ok());
        REQUIRE(t.Db().Query("UPDATE _char SET guildid = 201 WHERE charid = $1", {charOf(n)}).Ok());
    }
    REQUIRE(t.Db().Query("UPDATE _char SET remaingold = 1000000 WHERE charname16 = 'Wolf'").Ok());
    REQUIRE(t.Db().Query("DELETE FROM _siegefortress WHERE fortressid = 1").Ok());
    REQUIRE(t.Db().Query("INSERT INTO _siegefortress (fortressid, guildid) VALUES (1, 201)").Ok());

    Pair pair(t, &split, fortress);
    Player wolf = Again(pair.a, t, "Wolf", kEast);
    Player wulf = Again(pair.b, t, "Wulf", kWest);
    Player drag = Again(pair.b, t, "Drag", kWest);
    const auto manager = Until<proto::EntityAppeared>(wolf, [](const proto::EntityAppeared& e) { return e.character.model == kManagerRef; });

    // The master hires the smith on East: the hired flags (0x385F 0x12) go to the holder's guild on every server.
    wolf.c->Send(proto::FortressManage{manager.entity, proto::FortressAction::Hire, 1, 2});
    CHECK(Until<proto::FortressAnswer>(wolf, [](const proto::FortressAnswer& a) { return a.action == proto::FortressAction::Hire; }).ok);
    // (the flags told on entering, none hired, came first)
    const auto hired = Until<proto::FortressNotice>(wulf, [](const proto::FortressNotice& n) { return n.kind == proto::FortressNoticeKind::Hired && n.value != 0; });
    CHECK(hired.fortress == 1);
    CHECK(hired.value == 2);

    // In the tax period the master changes the rate on East: kind 7 to every player on both servers,
    // a stranger on West too.
    wulf.c->Send(proto::FortressDev{proto::FortressDevAction::StartTax});
    Until<proto::FortressNotice>(wolf, [](const proto::FortressNotice& n) { return n.kind == proto::FortressNoticeKind::TaxOpen; });
    wolf.c->Send(proto::FortressTaxRate{manager.entity, 7});
    for (Player* p : {&wulf, &drag}) {
        const auto rate = Until<proto::FortressNotice>(*p, [](const proto::FortressNotice& n) { return n.kind == proto::FortressNoticeKind::TaxRate; });
        CHECK(rate.fortress == 1);
        CHECK(std::int16_t(rate.value & 0xFFFF) == 7);
    }
}

TEST_CASE("A split world: a murderer handed over keeps his counts and his red state on the other server (step 8)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._refregion VALUES ($1, 'CHINA')", {int(std::int16_t(assets::RegionId(170, 97)))}).Ok());
    Pair pair(t, &split);
    // In the field east of Jangan, East's ground (Jangan itself is a town: no PvP there, 0x3018).
    const std::uint16_t eastField = assets::RegionId(170, 97);
    Player ann = pair.a.Join("Ann", 60, false);
    Stand(t, ann, eastField);
    pair.a.Enter(ann);
    Player ben = pair.a.Join("Ben", 50, false);
    Stand(t, ben, eastField);
    pair.a.Enter(ben);
    Player bob = pair.b.Join("Bob", 60, false);
    Stand(t, bob, kWest);
    pair.b.Enter(bob);
    for (int i = 0; i < 100; ++i) pair.a.pump();
    // Ann murders Ben on the East server: TotalPK 1, 1,200 points, DailyPK 5; a murderer.
    ann.c->Send(proto::DevStrike{ben.entity, 10'000'000});
    proto::PkCounters counts;
    for (int i = 0; i < 40 && counts.points != 1'200; ++i) ann.c->Expect(counts, 100ms);
    CHECK(counts.points == 1'200);
    std::int64_t saved = 0;
    for (int i = 0; i < 200 && saved != 1'200; ++i) {
        pair.a.pump();
        saved = t.Db().Query("SELECT pkpenaltypoint FROM _char WHERE charid = $1", {ann.character}).Int(0, 0);
    }
    REQUIRE(saved == 1'200);
    // Over to the West server: her counts with her, told at the entry; Bob sees her red there.
    ann.c->Send(proto::DevGoto{2});
    const auto move = Next<proto::ServerMove>(ann);
    Arrive(pair.b, ann, move);
    proto::PkCounters there;
    for (int i = 0; i < 40 && there.points == 0; ++i) ann.c->Expect(there, 100ms);
    CHECK(there.points == 1'200);
    CHECK(there.total == 1);
    CHECK(there.daily == 5);
    bool red = false;
    for (int i = 0; i < 60 && !red; ++i) {
        proto::EntityPvpState s;
        if (bob.c->Expect(s, 100ms) && s.entity == ann.entity && s.state == 2) red = true;
    }
    CHECK(red);
}

TEST_CASE("Two game servers: silk is the account's: a grant from outside reaches its player on West at once; a purchase on East is the balance on West") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    // A one-good mall on both servers (the silk's bus is every shard's).
    auto mall = std::make_shared<assets::MallTable>();
    assets::MallShop shop;
    shop.code = "MALL_CONSUME";
    assets::MallTab tab;
    tab.id = 501;
    tab.code = "MALL_CONSUME_ETC";
    assets::MallGood good;
    good.slot = 0;
    good.packageId = 7001;
    good.package = "PACKAGE_ITEM_MALL_GLOBAL_CHATTING";
    good.itemCodename = "ITEM_MALL_GLOBAL_CHATTING";
    good.price = 130;
    good.data = 11;
    tab.goods.push_back(good);
    shop.tabs.push_back(tab);
    mall->Add(shop);
    Pair pair(t, nullptr, [mall](server::GameServerConfig& c) { c.mall = mall; });
    Player west = pair.b.Join("Westy"), east = pair.a.Join("Easty");
    CHECK(Next<proto::SilkBalanceMessage>(west).own == 0); // no starting silk
    CHECK(Next<proto::SilkBalanceMessage>(east).own == 0);
    // 10,000 each from the site first (the old starting silk).
    const std::int64_t westAccount = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {west.character}).Int(0, 0);
    server::SilkBalance after;
    for (Player* who : {&west, &east}) {
        const std::int64_t account = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {who->character}).Int(0, 0);
        REQUIRE(server::GrantSilk(t.Db(), account, 10000, server::SilkKind::Own, server::SilkReason::GiftReceived, "WEB-0-" + who->name, "the site", after,
                                  pair.a.error) == server::SilkGrant::Done);
        CHECK(Next<proto::SilkBalanceMessage>(*who).own == 10000);
        CHECK(Next<proto::SilkLine>(*who).value == 10000);
    }

    // The site (a test standing in for it) grants 100: the West player hears it within a second.
    const auto granted = std::chrono::steady_clock::now();
    REQUIRE(server::GrantSilk(t.Db(), westAccount, 100, server::SilkKind::Own, server::SilkReason::GiftReceived, "WEB-1", "the site", after, pair.a.error) ==
            server::SilkGrant::Done);
    CHECK(Next<proto::SilkBalanceMessage>(west).own == 10100);
    const auto line = Next<proto::SilkLine>(west);
    CHECK(std::chrono::steady_clock::now() - granted < 1s);
    CHECK(line.reason == 2); // "You have received [%d] silk as gift."
    CHECK(line.value == 100);

    // East buys; the account's balance is the same wherever its character goes next.
    east.c->Send(proto::MallBuy{501, 0, 7001, 1, 0});
    const auto bought = Next<proto::MallBought>(east);
    REQUIRE(bought.code == 0);
    CHECK(bought.items.at(0).quantity == 11);
    CHECK(Next<proto::SilkBalanceMessage>(east).own == 9870);
    east.c->Connection().Close();
    for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions").Int(0, 0) > 1; ++i) pair.a.pump();
    const std::int64_t eastAccount = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {east.character}).Int(0, 0);
    Player again;
    again.name = "Easty";
    again.character = east.character;
    again.c = std::make_unique<Client>(pair.b.net, pair.b.game->Port(), pair.b.pump);
    REQUIRE(again.c->Greet());
    proto::LobbyEnter enter;
    enter.ticket = server::IssueTicket(t.Db(), eastAccount, 2, 30, pair.b.error);
    again.c->Send(enter);
    REQUIRE(Next<proto::LobbyEntered>(again).status == proto::LobbyEnterStatus::Accepted);
    again.c->Send(proto::CharacterSelect{"Easty"});
    REQUIRE(Next<proto::LobbyResult>(again).status == proto::LobbyStatus::Ok);
    again.c->Send(proto::WorldEnter{});
    REQUIRE(Next<proto::WorldEntered>(again).status == proto::WorldStatus::Ok);
    CHECK(Next<proto::SilkBalanceMessage>(again).own == 9870);
    // A grant to East's account now reaches it on West; nobody else hears a line.
    REQUIRE(server::GrantSilk(t.Db(), eastAccount, 30, server::SilkKind::Own, server::SilkReason::GiftReceived, "", "the site", after, pair.a.error) ==
            server::SilkGrant::Done);
    CHECK(Next<proto::SilkBalanceMessage>(again).own == 9900);
    CHECK(Next<proto::SilkLine>(again).value == 30);
    proto::SilkLine none;
    CHECK_FALSE(west.c->Expect(none, 300ms));
}

TEST_CASE("Two game servers: a Magic POP win of an entry marked visible on East is announced on West") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    constexpr std::uint32_t kCard = 9238, kWin = 9239, kLose = 9240, kStone = 920003, kMachine = 9251;
    const auto extra = [&](server::GameServerConfig& c) {
        auto rules = std::make_shared<game::CharacterRules>(*c.rules);
        auto items = std::make_shared<assets::ItemTable>(*rules->items);
        const auto add = [&](std::uint32_t id, const char* code, std::array<int, 4> tid, int param1 = 0) {
            assets::ItemRow r;
            r.refObjId = id;
            r.codename = code;
            r.typeId = tid;
            r.inService = true;
            r.params[0] = param1;
            return r;
        };
        assets::ItemRow card = add(kCard, "ITEM_MALL_GACHA_CARD", {3, 3, 14, 1});
        card.paramTexts[0] = "ITEM_MALL_GACHA_CARD_LOSE";
        card.paramTexts[1] = "ITEM_MALL_GACHA_CARD_WIN";
        items->Add(card);
        items->Add(add(kWin, "ITEM_MALL_GACHA_CARD_WIN", {3, 3, 14, 2}, 1));
        items->Add(add(kLose, "ITEM_MALL_GACHA_CARD_LOSE", {3, 3, 14, 2}));
        items->Add(add(kStone, "ITEM_ETC_ARCHEMY_MAGICSTONE_SOLID_01", {3, 3, 11, 7}));
        rules->items = items;
        c.rules = rules;
        auto gacha = std::make_shared<assets::GachaTable>();
        gacha->Add(assets::GachaEntry{1, kStone, 10000, 1, 1, true, ""}); // visible: announced (none of 1.188's is)
        gacha->Add(assets::GachaEntry{2, kStone, 10000, 1, 2, false, ""});
        gacha->Add(assets::GachaMachine{kMachine, 1, 2});
        c.gacha = gacha;
        c.npcs.push_back({kMachine, "NPC_CH_GACHA_MACHINE", {assets::RegionId(168, 97), 1050.0f, 0.0f, 1000.0f}, 0.0f});
    };
    Pair pair(t, nullptr, extra);
    Player east = pair.a.Join("Lucky", 30, false), west = pair.b.Join("Watcher");
    Stand(t, east, assets::RegionId(168, 97));
    server::StoredItem card;
    card.refId = kCard;
    card.quantity = 1;
    card.counted = true;
    const std::int64_t account = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {east.character}).Int(0, 0);
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {account}).Ok());
    REQUIRE(server::GiveItems(t.Db(), 1, "Lucky", {card}, 0, pair.a.error) == server::GiveResult::Done);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, 1)", {account}).Ok());
    pair.a.Enter(east);
    std::uint32_t machine = 0;
    while (machine == 0) {
        const auto seen = Next<proto::EntityAppeared>(east);
        if (seen.character.model == kMachine) machine = seen.entity;
    }
    east.c->Send(proto::PopPlay{machine, 1, 13});
    const auto played = Next<proto::PopPlayed>(east);
    REQUIRE(played.code == 0);
    CHECK(played.won == 1);
    const auto heard = Next<proto::PopAnnounce>(west);
    CHECK(heard.item == kStone);
    CHECK(heard.count == 1);
    CHECK(heard.name == "Lucky");
    CHECK(Next<proto::PopAnnounce>(east).item == kStone); // its own server's players too
}

TEST_CASE("Two game servers: the premium's day's uses and a time ticket go along to the other server") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    // The premium package and its parts on both servers' item tables.
    constexpr std::uint32_t kPremiumItem = 930001, kTicket4W = 930002, kReturnHigh = 930003, kResurrect = 930004, kReverse = 930005;
    const auto mallRules = [&](server::GameServerConfig& c) {
        auto rules = std::make_shared<game::CharacterRules>(*Rules());
        auto items = std::make_shared<assets::ItemTable>(*rules->items);
        const auto row = [&](std::uint32_t id, const char* code, std::array<int, 4> tid) {
            assets::ItemRow r;
            r.refObjId = id;
            r.codename = code;
            r.typeId = tid;
            r.inService = true;
            r.maxStack = 10;
            return r;
        };
        assets::ItemRow premium = row(kPremiumItem, "ITEM_MALL_PREMIUM_GOLDTIME", {3, 3, 13, 14});
        premium.params[0] = 2419200;
        premium.paramTexts[1] = "[UIU1:ITEM_PRE_GOLTAM_4W],[UIL1:ITEM_MALL_RESURRECTION_100P_SCROLL, A, 86400, 1]";
        premium.paramTexts[2] = "[UIL1:ITEM_MALL_REVERSE_RETURN_SCROLL, A, 86400, 3],[UIL1:ITEM_MALL_RETURN_SCROLL_HIGH_SPEED, A, 86400, 3]";
        items->Add(premium);
        assets::ItemRow ticket = row(kTicket4W, "ITEM_PRE_GOLTAM_4W", {3, 3, 13, 4});
        ticket.params[0] = 2419200;
        ticket.params[2] = 10'800'000;
        ticket.params[3] = 100;
        ticket.params[4] = 100;
        items->Add(ticket);
        assets::ItemRow ret = row(kReturnHigh, "ITEM_MALL_RETURN_SCROLL_HIGH_SPEED", {3, 3, 3, 1});
        ret.params[0] = 1000;
        ret.paramTexts[2] = "RESURRECT";
        items->Add(ret);
        items->Add(row(kResurrect, "ITEM_MALL_RESURRECTION_100P_SCROLL", {3, 3, 13, 6}));
        items->Add(row(kReverse, "ITEM_MALL_REVERSE_RETURN_SCROLL", {3, 3, 3, 3}));
        rules->items = std::move(items);
        c.rules = std::move(rules);
        c.devCommands = true;
    };
    Pair pair(t, nullptr, mallRules);
    Player east = pair.a.Join("Premia");
    east.c->Send(proto::DevGive{"ITEM_MALL_PREMIUM_GOLDTIME", 1});
    proto::ItemMoved given;
    do {
        given = Next<proto::ItemMoved>(east);
    } while (given.changed.empty());
    east.c->Send(proto::UseItem{given.changed.front().slot});
    proto::TimedServices told;
    do {
        told = Next<proto::TimedServices>(east);
    } while (told.premium.empty());
    // One /Return used on East (cancelled before it lands).
    east.c->Send(proto::PremiumUse{kReturnHigh, 0, 0});
    REQUIRE(Next<proto::ServiceAnswer>(east).code == 0);
    east.c->Send(proto::CancelReturn{});
    for (int i = 0; i < 100; ++i) pair.a.pump();
    east.c->Connection().Close();
    for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions").Int(0, 0) > 0; ++i) pair.a.pump();
    // In again on West: the premium's uses left (2 returns), its gold time running.
    const std::int64_t account = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {east.character}).Int(0, 0);
    Player west;
    west.name = "Premia";
    west.character = east.character;
    west.c = std::make_unique<Client>(pair.b.net, pair.b.game->Port(), pair.b.pump);
    REQUIRE(west.c->Greet());
    proto::LobbyEnter enter;
    enter.ticket = server::IssueTicket(t.Db(), account, 2, 30, pair.b.error);
    west.c->Send(enter);
    REQUIRE(Next<proto::LobbyEntered>(west).status == proto::LobbyEnterStatus::Accepted);
    west.c->Send(proto::CharacterSelect{"Premia"});
    REQUIRE(Next<proto::LobbyResult>(west).status == proto::LobbyStatus::Ok);
    west.c->Send(proto::WorldEnter{});
    REQUIRE(Next<proto::WorldEntered>(west).status == proto::WorldStatus::Ok);
    const auto there = Next<proto::TimedServices>(west);
    bool gold = false, premium = false;
    for (const proto::TimedService& s : there.services) {
        gold |= s.category == 3 && s.item == kTicket4W;
        premium |= s.category == 10 && s.item == kPremiumItem;
    }
    CHECK(gold);
    CHECK(premium);
    REQUIRE(there.premium.size() == 3);
    CHECK(there.premium[2].item == kReturnHigh);
    CHECK(there.premium[2].left == 2);
    // Its uses go on there: two more, then refused.
    for (int k = 0; k < 2; ++k) {
        west.c->Send(proto::PremiumUse{kReturnHigh, 0, 0});
        REQUIRE(Next<proto::ServiceAnswer>(west).code == 0);
        west.c->Send(proto::CancelReturn{});
        for (int i = 0; i < 50; ++i) pair.b.pump();
    }
    west.c->Send(proto::PremiumUse{kReturnHigh, 0, 0});
    CHECK(Next<proto::ServiceAnswer>(west).code == 0x18C6);
}

TEST_CASE("Two game servers: a skin change on East is the character's look when it comes to West") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    constexpr std::uint32_t kScroll = 900301, kOtherFigure = 102;
    const auto extra = [](server::GameServerConfig& c) {
        auto rules = std::make_shared<game::CharacterRules>(*c.rules);
        rules->creation.AddFigure(kOtherFigure, Race::China, true);
        auto items = std::make_shared<assets::ItemTable>(*rules->items);
        assets::ItemRow scroll;
        scroll.refObjId = kScroll;
        scroll.codename = "ITEM_MALL_CHAR_SKIN_CHANGE_SCROLL";
        scroll.typeId = {3, 3, 13, 9};
        scroll.inService = true;
        items->Add(scroll);
        rules->items = items;
        c.rules = rules;
    };
    Pair pair(t, nullptr, extra);
    const std::uint16_t region = assets::RegionId(168, 97);
    // The shaper, made on East, gets its scroll while away and comes back.
    Player shaper = pair.a.Join("Shaper", 30, false);
    shaper.c->Connection().Close();
    for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions").Int(0, 0) > 0; ++i) pair.a.pump();
    server::StoredItem item;
    item.refId = kScroll;
    item.quantity = 1;
    item.counted = true;
    REQUIRE(server::GiveItems(t.Db(), 1, "Shaper", {item}, 0, pair.a.error) == server::GiveResult::Done);
    shaper = Again(pair.a, t, "Shaper", region);
    const std::uint8_t place = std::uint8_t(
        t.Db().Query("SELECT v.slot FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2", {shaper.character, std::int64_t(kScroll)}).Int(0, 0));
    shaper.c->Send(proto::SkinChange{place, kOtherFigure, 0x31});
    const auto answer = Until<proto::ServiceAnswer>(shaper, [](const proto::ServiceAnswer& a) { return a.request == proto::ServiceRequest::Skin; });
    REQUIRE(answer.code == 0);
    // West: a neighbour there sees it arrive in its new look.
    Player seer = pair.b.Join("Seer", 30, false);
    Stand(t, seer, region);
    pair.b.Enter(seer);
    shaper.c->Connection().Close();
    for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions").Int(0, 0) > 1; ++i) pair.a.pump();
    shaper = Again(pair.b, t, "Shaper", region);
    const auto seen = Until<proto::EntityAppeared>(seer, [&](const proto::EntityAppeared& e) { return e.entity == shaper.entity; });
    CHECK(seen.character.model == kOtherFigure);
    CHECK(seen.character.height == 0x31);
}

// --- Capture the Flag and Event So-Ok across two game servers ---------

namespace {

constexpr std::uint32_t kIceTrophy = 26500, kCard = 10559;

// The split world with the event map (region 0x8009) on East, the trophy, the card and the swap's prizes known.
void FlagWorldSettings(TestDatabase& t, server::GameServerConfig& c) {
    auto rules = Rules();
    auto items = std::make_shared<assets::ItemTable>(*rules->items);
    const auto add = [&](std::uint32_t id, std::string_view code, int t2, int t3, int t4, std::uint16_t stack) {
        assets::ItemRow row;
        row.refObjId = id;
        row.codename = std::string(code);
        row.typeId = {3, 3, decltype(row.typeId[0])(t2), decltype(row.typeId[0])(t3)};
        (void)t4;
        row.inService = true;
        row.maxStack = stack;
        items->Add(row);
    };
    add(kIceTrophy, game::flag::kTrophy, 3, 9, 0, 100);
    add(kCard, game::flag::kCard, 3, 9, 0, 10);
    std::uint32_t id = 26600;
    for (const game::SwapPrize& p : game::SwapIceTrophies().prizes) add(id++, p.item, 3, 3, 1, 50);
    rules->items = std::move(items);
    c.rules = rules;
    // Two players make a match here (the tries' ctf-min-players; the original's fewest is 4 at these levels).
    c.flagWorld.minPlayers = 2;
    c.arena.minPlayers = 2;
    (void)t;
}

proto::FlagWorldNotice NoticeOf(Player& p, bool personal, std::uint8_t code, std::chrono::milliseconds timeout = 10s) {
    return Until<proto::FlagWorldNotice>(p, [&](const proto::FlagWorldNotice& n) { return (n.personal != 0) == personal && n.code == code; }, timeout);
}

} // namespace

TEST_CASE("A split world: Capture the Flag: registered on both servers, one match on East with the West player come over, foes, a kill's trophy, home after the draw") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    // The event map's region is East's (with the rest of West_China).
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._refregion VALUES ($1, 'CHINA')", {int(std::int16_t(game::flag::kRegion))}).Ok());
    Pair pair(t, &split, [&](server::GameServerConfig& c) { FlagWorldSettings(t, c); });
    Player ann = pair.a.Join("Ann", 40, false);
    Stand(t, ann, kEast);
    pair.a.Enter(ann);
    Player bob = pair.b.Join("Bob", 41, false);
    Stand(t, bob, kWest);
    pair.b.Enter(bob);
    for (int i = 0; i < 100; ++i) pair.a.pump();

    // Outside its time So-Ok takes nobody (FF 04).
    ann.c->Send(proto::FlagWorldRegister{});
    CHECK(NoticeOf(ann, true, std::uint8_t(game::FlagCode::NotNow)).code == 4);
    // The developer opens the registration on East: both servers' players hear the 10 minutes' line (state 2).
    ann.c->Send(proto::FlagWorldDev{1});
    NoticeOf(ann, false, std::uint8_t(game::FlagState::Open));
    NoticeOf(bob, false, std::uint8_t(game::FlagState::Open));
    // Both register (FF 00); a second request is the cancel, a third registers again.
    ann.c->Send(proto::FlagWorldRegister{});
    NoticeOf(ann, true, 0);
    bob.c->Send(proto::FlagWorldRegister{});
    NoticeOf(bob, true, 0);
    bob.c->Send(proto::FlagWorldRegister{});
    NoticeOf(bob, true, 0);
    bob.c->Send(proto::FlagWorldRegister{});
    NoticeOf(bob, true, 0);
    // The shard's queue holds both, each with its server and its way home.
    std::vector<server::EventEntryRow> rows;
    for (int i = 0; i < 300 && rows.size() < 2; ++i) {
        pair.a.pump();
        rows.clear();
        std::string error;
        REQUIRE(server::LoadEventEntries(t.Db(), server::EventKind::FlagWorld, rows, error));
    }
    REQUIRE(rows.size() == 2);
    for (const auto& r : rows) {
        if (r.name == "Bob") {
            CHECK(r.server == 2);
            CHECK(r.region == kWest);
        } else {
            CHECK(r.server == 1);
            CHECK(r.region == kEast);
        }
    }

    // The play: East (the host) forms one match, takes Ann into its layer; Bob comes over from West to his camp.
    ann.c->Send(proto::FlagWorldDev{2});
    NoticeOf(bob, false, std::uint8_t(game::FlagState::Started));
    const proto::ServerMove move = Next<proto::ServerMove>(bob);
    proto::WorldEntered bobIn = Arrive(pair.a, bob, move);
    for (int i = 0; i < 4 && bobIn.position.region != game::flag::kRegion; ++i) REQUIRE(bob.c->Expect(bobIn, 10s));
    CHECK(bobIn.position.region == game::flag::kRegion);
    const auto annIn = Until<proto::WorldEntered>(ann, [](const proto::WorldEntered& w) { return w.position.region == game::flag::kRegion; });
    CHECK(annIn.position.region == game::flag::kRegion);
    CHECK(NoticeOf(bob, false, std::uint8_t(game::FlagState::MatchWorld)).value == game::flag::kWorld);
    // Bob (41) is the higher: red (team 0) at the red camp; Ann blue.
    CHECK(std::abs(bobIn.position.x - game::flag::kCamps[0].x) < 5.0f);
    CHECK(std::abs(annIn.position.x - game::flag::kCamps[1].x) < 5.0f);
    CHECK(t.Db().Query("SELECT count(*) FROM sr_event_entries").Int(0, 0) == 0);

    // Foes (relation 0x20): Ann strikes Bob dead after the warm-up's untouchable seconds; one Ice Trophy for her (41 - 40 <= 9),
    // and no murderer's state for anyone.
    std::this_thread::sleep_for(6500ms);
    // Bob walks 60 from his camp first (his death is away from it).
    proto::WorldPosition away = bobIn.position;
    away.x -= 60.0f;
    bob.c->Send(proto::MoveTo{away});
    std::this_thread::sleep_for(3000ms);
    ann.c->Send(proto::DevStrike{bob.entity, 10'000'000});
    CHECK(NoticeOf(bob, true, std::uint8_t(game::FlagCode::Dead)).code == 0x0D);
    std::int64_t trophies = 0;
    for (int i = 0; i < 300 && trophies == 0; ++i) {
        pair.a.pump();
        trophies = t.Db().Query("SELECT count(*) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2",
                                {ann.character, int(kIceTrophy)}).Int(0, 0);
    }
    CHECK(trophies == 1);
    CHECK(t.Db().Query("SELECT coalesce(pkpenaltypoint, 0) FROM _char WHERE charid = $1", {ann.character}).Int(0, 0) == 0);

    // Dead, the death box's camp button (Resurrect 1): 10 s on he stands up at his own camp, in the match's layer, whole
    // again (the user's report 2026-11-17: the killed one seemed to stand up where he fell).
    bob.c->Send(proto::Resurrect{1});
    const proto::WorldEntered bobUp =
        Until<proto::WorldEntered>(bob, [](const proto::WorldEntered& w) { return w.position.region == game::flag::kRegion; }, 20s);
    CHECK(std::abs(bobUp.position.x - game::flag::kCamps[0].x) < 5.0f);
    CHECK(std::abs(bobUp.position.z - game::flag::kCamps[0].z) < 5.0f);

    // The end (the developer's): the flag still on the white pole is a draw: both told, both home on their own servers.
    ann.c->Send(proto::FlagWorldDev{3});
    NoticeOf(ann, true, std::uint8_t(game::FlagCode::Draw));
    NoticeOf(bob, true, std::uint8_t(game::FlagCode::Draw));
    const auto annHome = Until<proto::WorldEntered>(ann, [](const proto::WorldEntered& w) { return w.position.region == kEast; });
    CHECK(annHome.position.region == kEast);
    const proto::WorldEntered bobHome = Arrive(pair.b, bob, Next<proto::ServerMove>(bob));
    CHECK(bobHome.position.region == kWest);
}

TEST_CASE("Two game servers: So-Ok's Subscription Card counts the account's uses; her swap takes 10 Ice Trophies for one prize") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    Pair pair(t, &split, [&](server::GameServerConfig& c) { FlagWorldSettings(t, c); });
    Player ann = pair.a.Join("Ann", 40, false);
    Stand(t, ann, kEast);
    std::string error;
    server::StoredItem card;
    card.refId = kCard;
    card.quantity = 2;
    card.counted = true;
    server::StoredItem trophies;
    trophies.refId = kIceTrophy;
    trophies.quantity = 19;
    trophies.counted = true;
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    REQUIRE(server::GiveItems(t.Db(), 1, "Ann", {card, trophies}, 0, error) == server::GiveResult::Done);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) SELECT userjid, 1 FROM _user WHERE charid = $1", {ann.character}).Ok());
    pair.a.Enter(ann);
    const auto bag = Next<proto::InventoryList>(ann);
    const auto held = std::find_if(bag.items.begin(), bag.items.end(), [&](const proto::ItemRecord& i) { return i.item == kCard; });
    REQUIRE(held != bag.items.end());
    const std::int64_t jid = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {ann.character}).Int(0, 0);
    for (int use = 0; use < 2; ++use) {
        ann.c->Send(proto::UseItem{held->slot});
        CHECK(NoticeOf(ann, true, std::uint8_t(game::FlagCode::Subscribed)).code == 0x0C);
    }
    int count = 0;
    for (int i = 0; i < 300 && count < 2; ++i) {
        pair.a.pump();
        count = server::FlagEventParticipantCount(t.Db(), jid);
    }
    CHECK(count == 2);
    CHECK(t.Db().Query("SELECT count(*) FROM _flagworld_eventparticipants").Int(0, 0) == 1);

    // The swap: talk A, then 10 of her 19 trophies for one of the 15 prizes (talk C); with 9 left: talk E, nothing taken.
    ann.c->Send(proto::EventTalk{0, 10145, 0xFF});
    const auto a = Next<proto::EventSays>(ann);
    CHECK(a.text == game::kSwapTalkA);
    REQUIRE(a.rows.size() == 2);
    ann.c->Send(proto::EventTalk{0, 10145, 0});
    CHECK(Until<proto::EventSays>(ann, [](const proto::EventSays&) { return true; }).text == game::kSwapTalkC);
    ann.c->Send(proto::EventTalk{0, 10145, 0});
    CHECK(Until<proto::EventSays>(ann, [](const proto::EventSays&) { return true; }).text == game::kSwapTalkE);
    std::int64_t left = -1;
    for (int i = 0; i < 300 && left != 9; ++i) {
        pair.a.pump();
        left = t.Db().Query("SELECT coalesce(sum(i.data), 0) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2",
                            {ann.character, int(kIceTrophy)}).Int(0, 0);
    }
    CHECK(left == 9);
}
TEST_CASE("A split world: the Battle Arena: random score registered on both servers, the match on West (the arena's maps), a kill's 6 points, the result's coins, home") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    constexpr std::uint32_t kCoin = 25834;
    Pair pair(t, &split, [&](server::GameServerConfig& c) {
        FlagWorldSettings(t, c);
        auto rules = std::make_shared<game::CharacterRules>(*c.rules);
        auto items = std::make_shared<assets::ItemTable>(*rules->items);
        assets::ItemRow coin;
        coin.refObjId = kCoin;
        coin.codename = std::string(game::arena::kCoin);
        coin.typeId = {3, 3, 3, 1};
        coin.inService = true;
        coin.maxStack = 1000;
        items->Add(coin);
        rules->items = std::move(items);
        c.rules = rules;
        // A score map of 8 in West's part (the arena's maps are West's: ARENA_*), its gates apart, no monsters.
        server::ArenaWorldSpec w;
        w.id = 48;
        w.codename = "INS_ARENA_SCORE_8";
        w.kind = game::arena::kScore;
        w.size = 8;
        w.gates[0].region = w.gates[1].region = kWest;
        w.gates[0].x = 400;
        w.gates[1].x = 1200;
        w.gates[0].z = w.gates[1].z = 100;
        w.reward[0] = 15;
        w.reward[1] = 3;
        w.reward[2] = 5;
        c.arena.worlds = {w};
        c.arena.devRegister = 600;
        c.arena.devPlay = 600;
    });
    Player ann = pair.a.Join("Ann", 40, false);
    Stand(t, ann, kEast);
    pair.a.Enter(ann);
    Player bob = pair.b.Join("Bob", 42, false);
    Stand(t, bob, kWest);
    pair.b.Enter(bob);
    for (int i = 0; i < 100; ++i) pair.a.pump();

    const auto notice = [](Player& p, bool personal, std::uint8_t code) {
        return Until<proto::ArenaNotice>(p, [&](const proto::ArenaNotice& n) { return (n.personal != 0) == personal && n.code == code; }, 20s);
    };
    // Closed: FF 04. Opened by the developer (random): both servers hear it with the kinds offered.
    ann.c->Send(proto::ArenaRegister{1, 0, 1, {}});
    notice(ann, true, std::uint8_t(game::ArenaCode::NotNow));
    ann.c->Send(proto::ArenaDev{1, 0});
    CHECK(notice(ann, false, std::uint8_t(game::ArenaState::Open)).kinds == (game::arena::kScoreBit | game::arena::kFlagBit));
    notice(bob, false, std::uint8_t(game::ArenaState::Open));
    ann.c->Send(proto::ArenaRegister{1, 0, 1, {}});
    CHECK(notice(ann, true, 0).value == 1);
    bob.c->Send(proto::ArenaRegister{1, 0, 1, {}});
    notice(bob, true, 0);
    bob.c->Send(proto::ArenaRegister{1, 0, 1, {}});
    notice(bob, true, std::uint8_t(game::ArenaCode::Already));

    // The play: West forms the match; Ann comes over to her gate in its layer.
    ann.c->Send(proto::ArenaDev{2, 0});
    const proto::WorldEntered annIn = Arrive(pair.b, ann, Next<proto::ServerMove>(ann));
    CHECK(annIn.position.region == kWest);
    const auto started = notice(bob, false, std::uint8_t(game::ArenaState::Started));
    CHECK(started.value == 600'000);
    notice(ann, false, std::uint8_t(game::ArenaState::Started)); // the play's start, past the untouchable seconds of the move in
    ann.c->Send(proto::DevStrike{bob.entity, 10'000'000});
    CHECK(notice(ann, true, std::uint8_t(game::ArenaCode::Points)).value == 6);
    CHECK(notice(bob, true, std::uint8_t(game::ArenaCode::Revival)).value == 10);
    const auto board = Until<proto::ArenaScores>(ann, [](const proto::ArenaScores& s) { return s.team0 + s.team1 == 6; }, 15s);
    REQUIRE(board.top.size() == 1);
    CHECK(board.top[0].name == "Ann");
    CHECK(board.top[0].points == 6);

    // The end: Ann's team won: 5 coins, the score map's 15 x her level's monster experience as skill experience.
    ann.c->Send(proto::ArenaDev{3, 0});
    const auto result = Until<proto::ArenaResult>(ann, [](const proto::ArenaResult&) { return true; });
    CHECK(result.outcome == 1);
    CHECK(result.coins == 5);
    CHECK(Until<proto::ArenaResult>(bob, [](const proto::ArenaResult&) { return true; }).outcome == 2);
    std::int64_t coins = 0;
    for (int i = 0; i < 300 && coins == 0; ++i) {
        pair.a.pump();
        coins = t.Db().Query("SELECT coalesce(sum(i.data), 0) FROM _inventory v JOIN _items i ON i.id64 = v.itemid WHERE v.charid = $1 AND i.refitemid = $2",
                             {ann.character, int(kCoin)}).Int(0, 0);
    }
    CHECK(coins == 5);
    // Everyone home at the stop: Ann to East.
    ann.c->Send(proto::ArenaDev{4, 0});
    const proto::WorldEntered home = Arrive(pair.a, ann, Next<proto::ServerMove>(ann));
    CHECK(home.position.region == kEast);
}
TEST_CASE("A split world: the guild recall: a master on East calls its members; East's is asked at once, West's over the bus, and its yes moves it to East") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    // Both regions towns (_RefRegion.IsBattleField 0).
    REQUIRE(t.Db().Query("ALTER TABLE sro_vt_shard._refregion ADD COLUMN isbattlefield int DEFAULT 0").Ok());
    constexpr std::uint32_t kRecallItem = 9267; // ITEM_ETC_GUILD_RECALL as the export has it
    const auto recall = [&](server::GameServerConfig& c) {
        auto rules = std::make_shared<game::CharacterRules>(*c.rules);
        auto items = std::make_shared<assets::ItemTable>(*rules->items);
        assets::ItemRow row;
        row.refObjId = kRecallItem;
        row.codename = "ITEM_ETC_GUILD_RECALL";
        row.typeId = {3, 3, 12, 3};
        row.inService = true;
        row.canUse = 1;
        row.requirementTypes = {-1, -1, -1, -1};
        row.params[0] = 3'600'000; // the wait, ms
        row.params[1] = 30'000;    // the members' time to answer, ms
        row.params[2] = 5;         // the guild's level
        items->Add(row);
        rules->items = items;
        c.rules = rules;
    };
    {
        Pair first(t, &split, recall);
        first.a.Join("Mast", 30, false);
        first.a.Join("Eve", 30, false);
        first.b.Join("Wanda", 30, false);
    }
    const auto charOf = [&](const char* n) { return t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {std::string(n)}).Int(0, 0); };
    REQUIRE(t.Db().Query("INSERT INTO _guild (id, name, lvl, gatheredsp, foundationdate, alliance, booty) OVERRIDING SYSTEM VALUE VALUES (401, 'Bells', 5, 0, now(), 0, 0)").Ok());
    for (const auto& [n, memberClass] : std::vector<std::pair<const char*, int>>{{"Mast", 0}, {"Eve", 10}, {"Wanda", 10}}) {
        REQUIRE(t.Db().Query("INSERT INTO _guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate, permission, siegeauthority) "
                             "VALUES (401, $1, $2, $3, 30, 0, now(), $4, 0)",
                             {charOf(n), std::string(n), memberClass, memberClass == 0 ? -1 : 0})
                    .Ok());
        REQUIRE(t.Db().Query("UPDATE _char SET guildid = 401 WHERE charid = $1", {charOf(n)}).Ok());
    }
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    server::StoredItem scroll;
    scroll.refId = kRecallItem;
    scroll.counted = true;
    std::string error;
    REQUIRE(server::GiveItems(t.Db(), 1, "Mast", {scroll}, 0, error) == server::GiveResult::Done);

    Pair pair(t, &split, recall);
    Player mast = Again(pair.a, t, "Mast", kEast);
    const auto bag = Next<proto::InventoryList>(mast);
    const auto it = std::find_if(bag.items.begin(), bag.items.end(), [&](const proto::ItemRecord& i) { return i.item == kRecallItem; });
    REQUIRE(it != bag.items.end());
    Player eve = Again(pair.a, t, "Eve", kEast);
    Player wanda = Again(pair.b, t, "Wanda", kWest);
    for (int i = 0; i < 100; ++i) pair.a.pump();

    // The master on East reads it in Jangan: Eve (East) is asked at once, Wanda (West) over the shard's bus.
    mast.c->Send(proto::UseItem{it->slot, 0, 0});
    const auto moved = Next<proto::ItemMoved>(mast);
    CHECK((moved.emptied.size() == 1 && moved.emptied[0] == it->slot));
    const auto forEve = Next<proto::GuildRecallAsk>(eve);
    CHECK(forEve.master == "Mast");
    CHECK(forEve.seconds == 30u);
    const auto forWanda = Next<proto::GuildRecallAsk>(wanda);
    CHECK(forWanda.master == "Mast");
    CHECK(forWanda.seconds == 30u);
    CHECK(pair.b.game->GetGuilds()->RecallAsked(wanda.character));
    CHECK_FALSE(pair.a.game->GetGuilds()->RecallAsked(wanda.character)); // asked where it plays

    // Wanda's yes on West: on its way to East, where the master stands.
    wanda.c->Send(proto::GuildRecallAnswer{1});
    const auto answered = Until<proto::GuildAnswer>(wanda, [](const proto::GuildAnswer& a) { return a.action == proto::GuildAction::RecallAnswer; });
    CHECK(answered.error == 0);
    const auto there = Arrive(pair.a, wanda, Next<proto::ServerMove>(wanda));
    CHECK(there.position.region == kEast);
    CHECK(there.position.x == doctest::Approx(1000.0f));
    CHECK(there.position.z == doctest::Approx(1000.0f));

    // Eve's no: nothing moves; the master's wait (_TimedJob 2 / 3) is written.
    eve.c->Send(proto::GuildRecallAnswer{0});
    for (int i = 0; i < 50; ++i) pair.a.pump();
    CHECK_FALSE(pair.a.game->GetGuilds()->RecallAsked(eve.character));
    CHECK(t.Db().Query("SELECT timetokeep FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 3", {mast.character}).Int(0, 0) == 3600);
}

TEST_CASE("A split world: a caught COS comes back beside its owner after a relog and after a move to the other server, its minutes kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    // A coded quest's trap by hand (its COS a made-up row): its record keeps the catch as the original's does, the minutes in
    // byte +4 (our word 2) and the mark 10 in word +0xA (our word 5).
    game::QuestDef quest;
    REQUIRE_MESSAGE(game::ParseQuest("quest QNO_T_XTRAP\nid 20330\nstart level 1-\ngiver NPC_T_NOBODY\n"
                                     "on use ITEM_T_XTRAP target MOB_T_XMOB capture MOB_T_XCOS minutes 20 roll 50 ran @SN_T_RAN\n"
                                     "goal capture MOB_T_XCOS at NPC_T_NOBODY\n",
                                     quest, error),
                    error);
    REQUIRE_MESSAGE(server::SaveQuest(t.Db(), quest, server::QuestOrigin::Ours, error), error);
    auto characters = std::make_shared<assets::CharacterTable>();
    assets::ObjectRow cos;
    cos.refObjId = 990001;
    cos.codename = "MOB_T_XCOS";
    cos.typeId = {1, 2, 3, 6};
    cos.level = 12;
    cos.maxHp = 359;
    characters->Add(cos);
    SplitWorld split(t);
    Pair pair(t, &split, [&](server::GameServerConfig& c) { c.characters = characters; });
    Player ann = pair.a.Join("Ann", 30, false);
    Stand(t, ann, kEast);
    REQUIRE(t.Db().Query("INSERT INTO _charquest (charid, questid, status, achievementcount, starttime, endtime, questdata1, questdata2) "
                         "VALUES ($1, 20330, 1, 0, now(), now(), $2, $3)",
                         {ann.character, std::int64_t(15) << 32, std::int64_t(server::Quests::kCaught) << 16})
                .Ok());
    const auto cosOf = [&](Player& p, std::chrono::milliseconds most) {
        const auto end = std::chrono::steady_clock::now() + most;
        while (std::chrono::steady_clock::now() < end) {
            proto::EntityAppeared a;
            if (p.c->Expect(a, 20ms) && a.kind == proto::EntityKind::Cos && a.character.model == cos.refObjId) return a.entity;
        }
        return std::uint32_t(0);
    };
    const auto minutesKept = [&] {
        const std::int64_t data1 = t.Db().Query("SELECT questdata1 FROM _charquest WHERE charid = $1 AND questid = 20330", {ann.character}).Int(0, 0);
        return int((std::uint64_t(data1) >> 32) & 0xFFFF);
    };

    // In the world on East: the caught COS beside it (on the world entry).
    pair.a.Enter(ann);
    REQUIRE(cosOf(ann, 5s) != 0);
    // A move to West (the other game server reads the same record): there with it.
    ann.c->Send(proto::DevGoto{2});
    Arrive(pair.b, ann, Next<proto::ServerMove>(ann));
    CHECK(cosOf(ann, 5s) != 0);
    CHECK(minutesKept() == 15);
    // Back to East: with it again; still 15 minutes (they run only a minute at a time in the world).
    ann.c->Send(proto::DevGoto{1});
    Arrive(pair.a, ann, Next<proto::ServerMove>(ann));
    CHECK(cosOf(ann, 5s) != 0);
    CHECK(minutesKept() == 15);
}

TEST_CASE("A split world: the Academy: founded at 60 for 100,000 gold, an apprentice invited and joined, its chat and presence across the servers, the 24 h and 7 day waits, leave, rejoin wait, disband") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    Pair pair(t, &split, [](server::GameServerConfig& c) { c.devCommands = true; });
    const auto make = [&](const char* name, int level, std::uint64_t gold) {
        Player p = pair.a.Join(name, level, false);
        Stand(t, p, kEast);
        REQUIRE(t.Db().Query("UPDATE _char SET maxlevel = curlevel, remaingold = $2 WHERE charid = $1", {p.character, std::int64_t(gold)}).Ok());
        pair.a.Enter(p);
        Until<proto::AcademyInfo>(p, [](const proto::AcademyInfo& i) { return i.id == 0; });
        return p;
    };
    Player guard = make("Guard", 60, 250'000);
    Player kid = make("Kid", 20, 0);
    Player young = make("Young", 59, 250'000);
    const auto refusal = [&](Player& p) { return Until<proto::AcademyRefusal>(p, [](const proto::AcademyRefusal&) { return true; }); };

    // Below level 60: refused (0x6809). Founded: the guardian alone, its grade 7; 100,000 gold paid.
    young.c->Send(proto::AcademyFound{});
    CHECK(refusal(young).code == game::academy::code::kLevel);
    guard.c->Send(proto::AcademyFound{});
    const auto founded = Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.id != 0; });
    REQUIRE(founded.members.size() == 1);
    CHECK(founded.members[0].memberClass == 0);
    CHECK(founded.members[0].grade == 7);
    for (int i = 0; i < 200 && t.Db().Query("SELECT remaingold FROM _char WHERE charid = $1", {guard.character}).Int(0, 0) != 150'000; ++i) pair.a.pump();
    CHECK(t.Db().Query("SELECT remaingold FROM _char WHERE charid = $1", {guard.character}).Int(0, 0) == 150'000);

    // The guardian invites the apprentice he sees; asked, it answers yes: two members, the kid an apprentice (class 2).
    guard.c->Send(proto::AcademyInvite{kid.entity});
    CHECK(Until<proto::AcademyAsk>(kid, [](const proto::AcademyAsk&) { return true; }).from == "Guard");
    kid.c->Send(proto::AcademyAnswer{1});
    const auto joined = Until<proto::AcademyInfo>(kid, [](const proto::AcademyInfo& i) { return i.members.size() == 2; });
    const auto kidRow = std::find_if(joined.members.begin(), joined.members.end(), [&](const proto::AcademyMember& m) { return m.name == "Kid"; });
    REQUIRE(kidRow != joined.members.end());
    CHECK(kidRow->memberClass == 2);
    CHECK(kidRow->grade == 2); // level 20: "Apprentice" (up to 20)
    Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.members.size() == 2; });

    // Within 24 hours of joining no leaving (0x6821); within 7 days of founding no disbanding (0x681D).
    kid.c->Send(proto::AcademyLeave{});
    CHECK(refusal(kid).code == game::academy::code::kLeaveWait);
    guard.c->Send(proto::AcademyDisband{});
    CHECK(refusal(guard).code == game::academy::code::kDisbandWait);

    // The kid goes to the West: its academy is there too, both see each other online; its chat reaches the guardian.
    kid.c->Send(proto::DevGoto{2});
    Arrive(pair.b, kid, Next<proto::ServerMove>(kid));
    Until<proto::AcademyInfo>(kid, [](const proto::AcademyInfo& i) { return i.members.size() == 2; });
    Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) {
        return std::any_of(i.members.begin(), i.members.end(), [](const proto::AcademyMember& m) { return m.name == "Kid" && m.online == 1; });
    });
    CHECK(Say(kid, proto::ChatKind::Academy, 1, {}, "hello guardian").result == proto::ChatResult::Ok);
    const auto heard = Until<proto::ChatLine>(guard, [](const proto::ChatLine& l) { return l.kind == proto::ChatKind::Academy; });
    CHECK(heard.text == "hello guardian");
    CHECK(heard.name == "Kid");

    // A day and a week later (the rows' dates moved back), on its way home it leaves: out, the guardian told; its rejoin
    // wait (24 h for an apprentice, timed job 2/10) refuses a new invitation (0x681E).
    REQUIRE(t.Db().Query("UPDATE _trainingcampmember SET joindate = localtimestamp - interval '2 days'").Ok());
    REQUIRE(t.Db().Query("UPDATE _trainingcamp SET creationdate = localtimestamp - interval '8 days'").Ok());
    kid.c->Send(proto::DevGoto{1});
    Arrive(pair.a, kid, Next<proto::ServerMove>(kid));
    Until<proto::AcademyInfo>(kid, [](const proto::AcademyInfo& i) { return i.members.size() == 2; });
    kid.c->Send(proto::AcademyLeave{});
    Until<proto::AcademyInfo>(kid, [](const proto::AcademyInfo& i) { return i.id == 0; });
    CHECK(Until<proto::AcademyNews>(guard, [](const proto::AcademyNews& n) { return n.kind == proto::AcademyNewsKind::Left; }).name == "Kid");
    Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.members.size() == 1; });
    for (int i = 0; i < 200 && t.Db().Query("SELECT count(*) FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 9", {kid.character}).Int(0, 0) == 0; ++i) {
        pair.a.pump();
    }
    CHECK(t.Db().Query("SELECT count(*) FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 9", {kid.character}).Int(0, 0) == 1);
    guard.c->Send(proto::AcademyInvite{kid.entity});
    CHECK(refusal(guard).code == game::academy::code::kRejoinWait);

    // Disbanded (the week over): gone, and a new founding waits 7 days (timed job 2/7: 0x681C, its seconds).
    guard.c->Send(proto::AcademyDisband{});
    Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.id == 0; });
    CHECK(t.Db().Query("SELECT count(*) FROM _trainingcamp").Int(0, 0) == 0);
    guard.c->Send(proto::AcademyFound{});
    const auto wait = refusal(guard);
    CHECK(wait.code == game::academy::code::kFoundWait);
    CHECK(wait.seconds > 6 * 24 * 3600);
}

namespace {

// An academy server without the rest of a game server: its database tasks run at once, its
// bus a list both read, its clock the test's.
struct BuffHost : server::AcademyHost {
    server::Database* db = nullptr;
    std::uint16_t id = 0;
    std::vector<std::string>* bus = nullptr;
    std::int64_t now = 1'800'000'000;
    std::vector<std::pair<std::uint64_t, std::pair<proto::MessageId, std::vector<std::byte>>>> sent;
    void AcademySend(std::uint64_t peer, proto::MessageId m, const std::vector<std::byte>& payload) override { sent.push_back({peer, {m, payload}}); }
    std::optional<server::AcademyActor> AcademyPlayerAt(std::uint32_t) const override { return std::nullopt; }
    bool AcademyTakeGold(std::uint64_t, std::uint64_t) override { return true; }
    void AcademyGiveGold(std::uint64_t, std::uint64_t) override {}
    void AcademySubmit(std::function<void(server::Database&)> task, std::function<void()> done) override {
        task(*db);
        done();
    }
    std::uint16_t AcademyServer() const override { return id; }
    bool AcademyOnBus() const override { return true; }
    void AcademyPublish(std::string text) override { bus->push_back(std::move(text)); }
    std::int64_t AcademyNow() const override { return now; }
    std::map<std::uint32_t, proto::WorldPosition> positions;
    std::optional<proto::WorldPosition> AcademyPositionOf(std::uint32_t entity) const override {
        const auto it = positions.find(entity);
        return it != positions.end() ? std::optional(it->second) : std::nullopt;
    }
    // The messages of a kind sent to `peer` since the last look, decoded.
    template <typename T>
    std::vector<T> Take(std::uint64_t peer) {
        std::vector<T> out;
        for (auto it = sent.begin(); it != sent.end();) {
            if (it->first != peer || it->second.first != T::kId) {
                ++it;
                continue;
            }
            T m;
            REQUIRE(proto::Decode(it->second.second, m));
            out.push_back(std::move(m));
            it = sent.erase(it);
        }
        return out;
    }
};

} // namespace

TEST_CASE("A split world: the Academy's experience buffs: the accumulation, the pass across the servers, the spending, the day's end, level 40") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const auto character = [&](const char* name, int level) {
        const server::QueryResult r = t.Db().Query("INSERT INTO sro_vt_shard._char (charname16, curlevel, maxlevel, refobjid, scale, posx, posy, posz, lastlogout) "
                                                   "VALUES ($1, $2, $2, 1907, 0, 0, 0, 0, now()) RETURNING charid",
                                                   {name, level});
        REQUIRE_MESSAGE(r.Ok(), r.Error());
        return r.Int(0, 0);
    };
    const std::int64_t guard = character("Guard", 60), kid = character("Kid", 20);
    std::uint32_t id = 0;
    std::string error;
    REQUIRE(server::FoundAcademy(t.Db(), guard, 60, 60, id, error) == server::AcademyWrite::Ok);
    REQUIRE(server::AddAcademyMember(t.Db(), id, kid, 20, 20, error) == server::AcademyWrite::Ok);
    // Its slots as the original's procedure makes them: the guardian 0..5, the apprentice 0 and 1, never begun.
    CHECK(t.Db().Query("SELECT count(*) FROM _trainingcampbuffstatus WHERE recipientcharid = $1", {guard}).Int(0, 0) == 6);
    CHECK(t.Db().Query("SELECT count(*) FROM _trainingcampbuffstatus WHERE recipientcharid = $1 AND startingtime = '2000-01-01'", {kid}).Int(0, 0) == 2);

    // The kid on server 1 (East), the guardian on server 2 (West); each told who plays on the other.
    std::vector<std::string> bus;
    BuffHost east, west;
    east.db = west.db = &t.Db();
    east.id = 1;
    west.id = 2;
    east.bus = west.bus = &bus;
    server::Academies a(east), b(west);
    const auto deliver = [&] {
        for (std::size_t i = 0; i < bus.size(); ++i) {
            a.BusMessage(bus[i]);
            b.BusMessage(bus[i]);
        }
        bus.clear();
    };
    const auto tick = [&](std::int64_t seconds) {
        east.now += seconds;
        west.now += seconds;
        a.Tick({});
        b.Tick({});
        deliver();
    };
    constexpr std::uint64_t kKidPeer = 11, kGuardPeer = 22;
    a.Entered({kKidPeer, 1001, kid, "Kid", 1907, 20, 20});
    b.Entered({kGuardPeer, 1002, guard, "Guard", 1907, 60, 60});
    deliver();
    tick(0);
    CHECK(east.Take<proto::AcademyReserve>(kKidPeer).size() == 1); // the entry's lists, empty
    CHECK(west.Take<proto::AcademyReserve>(kGuardPeer).size() == 1);
    CHECK(east.Take<proto::AcademyReserve>(kKidPeer).empty());

    // Where the kid is, told to the guardian on the other server (the world map's mark); not again until it moves 10.
    east.positions[1001] = {0x6A9F, 960.0f, 0.0f, 458.0f};
    east.now += 1;
    a.Tick(server::Academies::Clock::now());
    deliver();
    const auto placed = west.Take<proto::AcademyPlace>(kGuardPeer);
    REQUIRE(placed.size() == 1);
    CHECK(placed[0].character == std::uint32_t(kid));
    CHECK(placed[0].position.region == 0x6A9F);

    // Kills: the first begins the day (kind 0, 1440 minutes, 0 points); a whole kill 10 points, a big one at most 30.
    CHECK(a.Kill(kid, 1.0) == 0);
    auto began = east.Take<proto::AcademyAccumulation>(kKidPeer);
    REQUIRE(began.size() == 1);
    CHECK(began[0].kind == 0);
    CHECK(began[0].minutes == 1440);
    a.Kill(kid, 5.0);
    a.Kill(kid, 0.25); // 2.5: 2
    // An assistant's and the guardian's kills give nothing to accumulate.
    CHECK(b.Kill(guard, 1.0) == 0);

    // Passed before its hour: nothing. After it (ten minutes told at 1430 left): the guardian (West) a buff of 0.2 of
    // 42 = 8 from the kid, the kid one of 0.8 = 33 from the guardian; its accumulation over (kind 5).
    a.Handle({kKidPeer, 1001, kid, "Kid"}, proto::AcademyPassBuff::kId, {}, {});
    deliver();
    CHECK(west.Take<proto::AcademyReserve>(kGuardPeer).empty());
    for (int m = 0; m < 61; ++m) tick(60);
    const auto told = east.Take<proto::AcademyAccumulation>(kKidPeer);
    REQUIRE(!told.empty());
    CHECK(told.front().kind == 10);
    CHECK(told.front().minutes == 1430);
    a.Handle({kKidPeer, 1001, kid, "Kid"}, proto::AcademyPassBuff::kId, {}, {});
    deliver();
    const auto toGuard = west.Take<proto::AcademyReserve>(kGuardPeer);
    REQUIRE(toGuard.size() == 1);
    CHECK(toGuard[0].kind == 0);
    REQUIRE(toGuard[0].entries.size() == 1);
    CHECK(toGuard[0].entries[0].donor == std::uint32_t(kid));
    CHECK(toGuard[0].entries[0].points == 8);
    CHECK(toGuard[0].entries[0].minutes == 1440);
    const auto ended = east.Take<proto::AcademyAccumulation>(kKidPeer);
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].kind == 5);
    const auto toKid = east.Take<proto::AcademyReserve>(kKidPeer);
    REQUIRE(toKid.size() == 1);
    CHECK(toKid[0].entries[0].donor == std::uint32_t(guard));
    CHECK(toKid[0].entries[0].points == 33);
    CHECK(t.Db().Query("SELECT remainbuffpoint FROM _trainingcampbuffstatus WHERE recipientcharid = $1 AND bufftype = 4", {guard}).Int(0, 0) == 8);

    // Spent 10 a kill for 10 % more; the last 3 for 3 %, then gone (kind 5). The kid's next kill begins a new day.
    CHECK(a.Kill(kid, 1.0) == 10);
    CHECK(east.Take<proto::AcademyAccumulation>(kKidPeer).size() == 1);
    CHECK(a.Kill(kid, 1.0) == 10);
    CHECK(a.Kill(kid, 1.0) == 10);
    CHECK(a.Kill(kid, 1.0) == 3);
    const auto spent = east.Take<proto::AcademyReserve>(kKidPeer);
    REQUIRE(spent.size() == 1);
    CHECK(spent[0].kind == 5);
    CHECK(a.Kill(kid, 1.0) == 0);
    CHECK(b.Kill(guard, 1.0) == 8);

    // The new day runs out: kind 7, its points kept for the pass.
    for (int m = 0; m < 1440; ++m) tick(60);
    bool over = false;
    for (const auto& acc : east.Take<proto::AcademyAccumulation>(kKidPeer)) over = over || acc.kind == 7;
    CHECK(over);
    CHECK(t.Db().Query("SELECT bufftype, remainbuffpoint FROM _trainingcampbuffstatus WHERE recipientcharid = $1 AND buffslotidx = 0", {kid}).Int(0, 0) == 2);

    // The guardian off: the pass refused (kind 2, "only when Guardian is online").
    b.Left(guard);
    deliver();
    a.Handle({kKidPeer, 1001, kid, "Kid"}, proto::AcademyPassBuff::kId, {}, {});
    const auto refused = east.Take<proto::AcademyReserve>(kKidPeer);
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].kind == 2);

    // Level 40: the accumulation ends (kind 8).
    a.LevelChanged(kid, 40, 40);
    const auto forty = east.Take<proto::AcademyAccumulation>(kKidPeer);
    REQUIRE(!forty.empty());
    CHECK(forty.back().kind == 8);
    CHECK(a.Kill(kid, 1.0) == 0);
    CHECK(east.Take<proto::AcademyAccumulation>(kKidPeer).empty());
    CHECK(t.Db().Query("SELECT bufftype FROM _trainingcampbuffstatus WHERE recipientcharid = $1 AND buffslotidx = 0", {kid}).Int(0, 0) == 0);
}

TEST_CASE("A split world: the Academy's graduation, notice, ranking, honour buffs and matching") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    // The four honour buffs (_RefSkill 12000..12003: their own groups, no overlap).
    auto skills = std::make_shared<assets::SkillTable>();
    for (std::uint32_t i = 0; i < 4; ++i) {
        assets::SkillRow row;
        row.id = 12000 + i;
        row.group = 811 + i;
        row.codename = game::academy::kHonorBuffs[3 - i];
        row.level = 1;
        row.activity = 1;
        skills->Add(std::move(row));
    }
    split.skills = skills;
    Pair pair(t, &split, [](server::GameServerConfig& c) { c.devCommands = true; });
    const auto make = [&](const char* name, int level, std::uint64_t gold, bool west) {
        Player p = pair.a.Join(name, level, false);
        Stand(t, p, west ? kWest : kEast);
        REQUIRE(t.Db().Query("UPDATE _char SET maxlevel = curlevel, remaingold = $2 WHERE charid = $1", {p.character, std::int64_t(gold)}).Ok());
        if (west) {
            p.c->Send(proto::WorldEnter{});
            Arrive(pair.b, p, Next<proto::ServerMove>(p));
        } else {
            pair.a.Enter(p);
        }
        Until<proto::AcademyInfo>(p, [](const proto::AcademyInfo& i) { return i.id == 0; });
        return p;
    };
    Player guard = make("Guard", 60, 250'000, false);
    Player kid = make("Kid", 20, 0, false);
    const auto refusal = [&](Player& p) { return Until<proto::AcademyRefusal>(p, [](const proto::AcademyRefusal&) { return true; }); };
    guard.c->Send(proto::AcademyFound{});
    Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.id != 0; });
    guard.c->Send(proto::AcademyInvite{kid.entity});
    Until<proto::AcademyAsk>(kid, [](const proto::AcademyAsk&) { return true; });
    kid.c->Send(proto::AcademyAnswer{1});
    Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.members.size() == 2; });

    // The notice: the guardian's alone; the members told it changed.
    kid.c->Send(proto::AcademyNoticeWrite{"No", "way"});
    CHECK(refusal(kid).code == game::academy::code::kNotGuardian);
    guard.c->Send(proto::AcademyNoticeWrite{"Hunt", "Tonight at eight"});
    CHECK(Until<proto::AcademyInfo>(kid, [](const proto::AcademyInfo& i) { return i.noticeTitle == "Hunt"; }).notice == "Tonight at eight");
    Until<proto::AcademyNews>(kid, [](const proto::AcademyNews& n) { return n.kind == proto::AcademyNewsKind::NoticeChanged; });

    // Graduation: below level 40 refused (0x681A); at 40 and more, rated "very satisfied" (+5): out of the academy, its
    // 2,000,000 experience; the guardian 40 - 20 = 20 honour points, the academy a graduate and 5 points.
    kid.c->Send(proto::AcademyGraduate{4});
    CHECK(refusal(kid).code == game::academy::code::kGraduateLevel);
    // Its level 45 (written, then read as it enters the West: the graduation across the servers, the guardian East).
    REQUIRE(t.Db().Query("UPDATE _char SET curlevel = 45, maxlevel = 45 WHERE charid = $1", {kid.character}).Ok());
    kid.c->Send(proto::DevGoto{2});
    Arrive(pair.b, kid, Next<proto::ServerMove>(kid));
    Until<proto::AcademyInfo>(kid, [](const proto::AcademyInfo& i) { return i.members.size() == 2; });
    kid.c->Send(proto::AcademyGraduate{4});
    Until<proto::AcademyInfo>(kid, [](const proto::AcademyInfo& i) { return i.id == 0; });
    CHECK(Until<proto::ExperienceGained>(kid, [](const proto::ExperienceGained& e) { return e.from == 0 && e.experience >= 2'000'000; }).experience >= 2'000'000);
    CHECK(Until<proto::AcademyNews>(guard, [](const proto::AcademyNews& n) { return n.kind == proto::AcademyNewsKind::Graduated; }).name == "Kid");
    const auto after = Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.members.size() == 1; });
    CHECK(after.graduates == 1);
    CHECK(after.evaluation == 5);
    CHECK(after.members[0].honor == 20);

    // The daily ranking (evaluation 10 at least): ranked first, rank 1; the guardian wears every honour buff at its next
    // entry (copper up to crown, cumulative).
    REQUIRE(t.Db().Query("UPDATE _trainingcamp SET evaluationpoint = 12").Ok());
    std::string error;
    CHECK(server::UpdateAcademyHonorRanking(t.Db(), true, error));
    const auto ranking = server::LoadAcademyRanking(t.Db());
    REQUIRE(ranking.size() == 1);
    CHECK(ranking[0].place == 1);
    CHECK(ranking[0].rank == 1);
    CHECK(ranking[0].guardian == "Guard");
    guard.c->Send(proto::DevGoto{2});
    Arrive(pair.b, guard, Next<proto::ServerMove>(guard));
    CHECK(Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.id != 0; }).rank == 1);
    std::set<std::uint32_t> worn;
    for (int i = 0; i < 4; ++i) worn.insert(Until<proto::BuffAdded>(guard, [](const proto::BuffAdded& b) { return b.skill >= 12000 && b.skill <= 12003; }).skill);
    CHECK(worn.size() == 4);

    // Matching across the servers: the guardian (now West) lists its academy for apprentices; a level 50 player on the East is
    // refused its kind (0x682A); a level 15 one asks, the guardian says yes: joined.
    guard.c->Send(proto::AcademyMatchRegister{1, "Learn with us"});
    Until<proto::AcademyNews>(guard, [](const proto::AcademyNews& n) { return n.kind == proto::AcademyNewsKind::MatchEntered; });
    const auto listed = Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.matchEntry != 0; });
    Player big = make("Big", 50, 0, false);
    Player small = make("Small", 15, 0, false);
    small.c->Send(proto::AcademyMatchListAsk{});
    const auto page = Until<proto::AcademyMatchList>(small, [](const proto::AcademyMatchList& l) { return !l.rows.empty(); });
    REQUIRE(page.rows.size() == 1);
    CHECK(page.rows[0].entry == listed.matchEntry);
    CHECK(page.rows[0].guardian == "Guard");
    CHECK(page.rows[0].title == "Learn with us");
    CHECK(page.rows[0].rank == 1);
    big.c->Send(proto::AcademyMatchJoin{listed.matchEntry});
    CHECK(refusal(big).code == game::academy::code::kJoinKind);
    small.c->Send(proto::AcademyMatchJoin{listed.matchEntry});
    CHECK(Until<proto::AcademyMatchProgress>(small, [](const proto::AcademyMatchProgress& p) { return true; }).waiting == 1);
    const auto asked = Until<proto::AcademyAsk>(guard, [](const proto::AcademyAsk& a) { return a.join == 1; });
    CHECK(asked.from == "Small");
    guard.c->Send(proto::AcademyMatchReply{1});
    Until<proto::AcademyNews>(small, [](const proto::AcademyNews& n) { return n.kind == proto::AcademyNewsKind::JoinComplete; });
    CHECK(Until<proto::AcademyInfo>(small, [](const proto::AcademyInfo& i) { return i.members.size() == 2; }).id == listed.id);
    Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.members.size() == 2; });

    // Listed, the guardian bans nobody (0x6829); unlisted, the entry gone.
    guard.c->Send(proto::AcademyBan{"Small"});
    CHECK(refusal(guard).code == game::academy::code::kBanWhileListed);
    guard.c->Send(proto::AcademyMatchDelete{});
    Until<proto::AcademyNews>(guard, [](const proto::AcademyNews& n) { return n.kind == proto::AcademyNewsKind::MatchDeleted; });
    Until<proto::AcademyInfo>(guard, [](const proto::AcademyInfo& i) { return i.matchEntry == 0; });
    CHECK(t.Db().Query("SELECT count(*) FROM sr_academy_matching").Int(0, 0) == 0);
}

TEST_CASE("A split world: the Open Market: a listing made on one server is found, bought and settled from the other") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    namespace om = game::openmarket;
    // Juel at both servers' spots (the market's NPC; out of service in vSRO 1.188, ours in).
    Pair pair(t, &split, [](server::GameServerConfig& c) {
        c.npcs.push_back({om::kNpcRefObjId, om::kNpcCodename, {kEast, 1010.0f, 0.0f, 1000.0f}, 0.0f});
        c.npcs.push_back({om::kNpcRefObjId, om::kNpcCodename, {kWest, 1010.0f, 0.0f, 1000.0f}, 0.0f});
    });
    Player sel = pair.a.Join("Sel", 30, false);
    Player buy = pair.b.Join("Buy", 30, false);
    Stand(t, sel, kEast);
    Stand(t, buy, kWest);
    // Two stacks of potions for Sel, given while it is not in the game.
    const std::int64_t jid = t.Db().Query("SELECT userjid FROM _user WHERE charid = $1", {sel.character}).Int(0, 0);
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions WHERE jid = $1", {jid}).Ok());
    server::StoredItem potions;
    potions.refId = kStallPotion;
    potions.quantity = 5;
    potions.counted = true;
    std::string error;
    REQUIRE(server::GiveItems(t.Db(), 1, "Sel", {potions, potions}, 0, error) == server::GiveResult::Done);
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_account.sr_online_sessions (jid, shard_id) VALUES ($1, 1)", {jid}).Ok());
    REQUIRE(t.Db().Query("UPDATE _char SET remaingold = 1000000 WHERE charid = $1", {sel.character}).Ok());
    REQUIRE(t.Db().Query("UPDATE _char SET remaingold = 3000000 WHERE charid = $1", {buy.character}).Ok());
    pair.a.Enter(sel);
    pair.b.Enter(buy);
    std::vector<std::uint8_t> places;
    for (const proto::ItemRecord& i : Next<proto::InventoryList>(sel).items) {
        if (i.item == kStallPotion) places.push_back(i.slot);
    }
    REQUIRE(places.size() == 2);
    const auto npcOf = [](Player& p) {
        return Until<proto::EntityAppeared>(p, [](const proto::EntityAppeared& m) { return m.character.model == om::kNpcRefObjId; }).entity;
    };
    const std::uint32_t eastNpc = npcOf(sel), westNpc = npcOf(buy);
    const auto answer = [](Player& p, proto::MessageId request) {
        return Until<proto::OpenMarketAnswer>(p, [&](const proto::OpenMarketAnswer& a) { return a.request == request; }).code;
    };

    // Not open: refused (0x7009). Opened at Juel: the fees and the room.
    sel.c->Send(proto::OpenMarketMine{});
    CHECK(answer(sel, proto::OpenMarketMine::kId) == om::code::kNotOpen);
    sel.c->Send(proto::OpenMarketOpen{eastNpc});
    CHECK(answer(sel, proto::OpenMarketOpen::kId) == om::code::kOk);
    const auto state = Until<proto::OpenMarketState>(sel, [](const proto::OpenMarketState&) { return true; });
    CHECK(state.room == om::kListings);
    CHECK(state.listingFees == std::vector<std::uint8_t>{0, 5, 5, 5});

    // A stack only whole (0x7018); then the first stack for 2,000,000: its deposit 5 % (100,000), its sale fee 10 % (200,000).
    sel.c->Send(proto::OpenMarketRegister{{{places[0], 3, 2'000'000}}});
    CHECK(answer(sel, proto::OpenMarketRegister::kId) == om::code::kCount);
    sel.c->Send(proto::OpenMarketRegister{{{places[0], 5, 2'000'000}}});
    CHECK(answer(sel, proto::OpenMarketRegister::kId) == om::code::kOk);
    CHECK(Until<proto::GoldChanged>(sel, [](const proto::GoldChanged&) { return true; }).gold == 900'000);
    const auto listed = Until<proto::OpenMarketListings>(sel, [](const proto::OpenMarketListings& l) { return !l.listings.empty(); });
    REQUIRE(listed.listings.size() == 1);
    CHECK(listed.listings[0].status == std::uint8_t(om::Status::OnSale));
    CHECK(listed.listings[0].deposit == 100'000);
    CHECK(listed.listings[0].saleFee == 200'000);
    CHECK(listed.listings[0].item.quantity == 5);
    // The item held by the listing, out of the bag, never freed by a later save of the bag.
    CHECK(t.Db().Query("SELECT count(*) FROM _inventory WHERE charid = $1 AND slot = $2 AND itemid <> 0", {sel.character, int(places[0])}).Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM _openmarket om JOIN _items it ON it.id64 = om.itemid WHERE it.refitemid = $1", {std::int64_t(kStallPotion)})
              .Int(0, 0) == 1);

    // Found and bought from the other server.
    buy.c->Send(proto::OpenMarketOpen{westNpc});
    CHECK(answer(buy, proto::OpenMarketOpen::kId) == om::code::kOk);
    buy.c->Send(proto::OpenMarketSearch{42, 0, "S", 0});
    CHECK(answer(buy, proto::OpenMarketSearch::kId) == om::code::kShortText);
    buy.c->Send(proto::OpenMarketSearch{42, 0, "", 0});
    const auto found = Until<proto::OpenMarketResults>(buy, [](const proto::OpenMarketResults&) { return true; });
    REQUIRE(found.rows.size() == 1);
    CHECK(found.total == 1);
    CHECK(found.rows[0].seller == "Sel");
    CHECK(found.rows[0].price == 2'000'000);
    CHECK(found.rows[0].item.item == kStallPotion);
    buy.c->Send(proto::OpenMarketBuy{found.rows[0].account, found.rows[0].listing});
    CHECK(answer(buy, proto::OpenMarketBuy::kId) == om::code::kOk);
    CHECK(Until<proto::GoldChanged>(buy, [](const proto::GoldChanged&) { return true; }).gold == 1'000'000);
    const auto landed = Until<proto::ItemMoved>(buy, [](const proto::ItemMoved& m) { return !m.changed.empty(); });
    CHECK(landed.changed[0].item == kStallPotion);
    CHECK(landed.changed[0].quantity == 5);
    // The seller, on the other server, told of its sale; a second purchase refused (sold).
    CHECK(Until<proto::OpenMarketNews>(sel, [](const proto::OpenMarketNews& n) { return n.kind == proto::OpenMarketNewsKind::Sold; }).item == kStallPotion);
    buy.c->Send(proto::OpenMarketBuy{found.rows[0].account, found.rows[0].listing});
    CHECK(answer(buy, proto::OpenMarketBuy::kId) == om::code::kNotOnSale);

    // Settled: the price and the deposit less the sale fee (2,000,000 + 100,000 - 200,000).
    const auto sold = Until<proto::OpenMarketListings>(sel, [](const proto::OpenMarketListings& l) {
        return !l.listings.empty() && l.listings[0].status == std::uint8_t(om::Status::Sold);
    });
    sel.c->Send(proto::OpenMarketSettle{{sold.listings[0].listing}});
    CHECK(answer(sel, proto::OpenMarketSettle::kId) == om::code::kOk);
    CHECK(Until<proto::GoldChanged>(sel, [](const proto::GoldChanged&) { return true; }).gold == 900'000 + 1'900'000);
    sel.c->Send(proto::OpenMarketSettle{{sold.listings[0].listing}});
    CHECK(answer(sel, proto::OpenMarketSettle::kId) == om::code::kNotSold);

    // The second stack listed below a million (no deposit), then taken back into the bag.
    sel.c->Send(proto::OpenMarketRegister{{{places[1], 5, 500}}});
    CHECK(answer(sel, proto::OpenMarketRegister::kId) == om::code::kOk);
    const auto second = Until<proto::OpenMarketListings>(sel, [](const proto::OpenMarketListings& l) {
        return std::any_of(l.listings.begin(), l.listings.end(), [](const proto::OpenMarketListing& x) { return x.status == 0; });
    });
    std::uint32_t mine = 0;
    std::uint64_t deposit = 1;
    for (const proto::OpenMarketListing& l : second.listings) {
        if (l.status == 0) mine = l.listing, deposit = l.deposit;
    }
    CHECK(deposit == 0);
    sel.c->Send(proto::OpenMarketTakeBack{{mine}});
    CHECK(answer(sel, proto::OpenMarketTakeBack::kId) == om::code::kOk);
    const auto back = Until<proto::ItemMoved>(sel, [](const proto::ItemMoved& m) { return !m.changed.empty(); });
    CHECK(back.changed[0].item == kStallPotion);
    for (int i = 0; i < 100; ++i) {
        pair.a.pump();
        pair.b.pump();
    }
    CHECK(t.Db().Query("SELECT count(*) FROM _openmarket WHERE status <> 255").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM _inventory iv JOIN _items it ON it.id64 = iv.itemid WHERE it.refitemid = $1", {std::int64_t(kStallPotion)})
              .Int(0, 0) == 2);
}
