// The guild's mercenary scrolls and the guild recall: the original's rules on the data
// (the mercenary scrolls refused by the use's requirement gate, the recall's numbers, their paths in the item
// catalogue), and the recall on a game server: the master's checks and their codes, the item spent, its hour's wait kept
// in the database, the members asked, a yes brought to the master, a no, a late or a dead member's yes refused. The
// recall across the shard's two game servers is in CrossServerTests. Set SR_DB_URL (and SR_GAME_DIR for the game files'
// case); otherwise skipped.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Jobs.h"
#include "sr/game/GuildProtocol.h"
#include "sr/game/GuildRecall.h"
#include "sr/game/ItemCatalogue.h"
#include "sr/game/SpecialItems.h"
#include "sr/server/GameServer.h"
#include "sr/server/Guilds.h"
#include "sr/server/Store.h"
#include "sr/server/World.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
namespace proto = sr::game::proto;

namespace {

constexpr std::uint32_t kRecall = 940001, kSoldier = 940002;
const std::uint16_t kTown = assets::RegionId(168, 97), kField = assets::RegionId(170, 97);

// The 1.188 rows as the export has them (ITEM_ETC_GUILD_RECALL 9267, ITEM_ETC_GUILD_CH_SOLDIER1_SCROLL 9280); the
// recall's answer time cut to 3 s for the late answer.
assets::ItemRow RecallRow(int answerMs = 30'000) {
    assets::ItemRow r;
    r.refObjId = kRecall;
    r.codename = "ITEM_ETC_GUILD_RECALL";
    r.typeId = {3, 3, 12, 3};
    r.inService = true;
    r.canUse = 1;
    r.maxStack = 1;
    r.requirementTypes = {-1, -1, -1, -1};
    r.params[0] = 3'600'000;
    r.params[1] = answerMs;
    r.params[2] = 5;
    return r;
}
assets::ItemRow SoldierRow() {
    assets::ItemRow r;
    r.refObjId = kSoldier;
    r.codename = "ITEM_ETC_GUILD_CH_SOLDIER1_SCROLL";
    r.typeId = {3, 3, 12, 1};
    r.inService = true;
    r.canUse = 1;
    r.maxStack = 1;
    r.requirementTypes = {10, -1, -1, -1};
    r.requirementLevels = {3, 0, 0, 0};
    r.params[0] = 20;
    r.paramTexts[0] = "COS_GUILD_CH_SOLDIER1_001";
    return r;
}

} // namespace

TEST_CASE("Guild mercenaries and the recall: the original's rules on the data") {
    // The mercenary scrolls: requirement type 10, which the use's gate does not know: refused 2.
    const assets::ItemRow soldier = SoldierRow();
    CHECK_FALSE(game::RequirementTypeKnown(10));
    for (int known : {-1, 1, 2, 3, 4, 0x10, 0x201, 0x206, 0x301}) CHECK(game::RequirementTypeKnown(known));
    CHECK_FALSE(game::RequirementTypeKnown(0x207));
    CHECK(game::OriginalUseRefusal(soldier) == 2);
    const game::ItemRouting soldierRoute = game::ItemRouteOf(soldier, {});
    CHECK(soldierRoute.route == game::ItemRoute::OriginalRefuses);
    CHECK(soldierRoute.why.find("type 10") != std::string::npos);
    CHECK(game::ItemStateOf(soldierRoute) == game::ItemState::Implemented);

    // The recall: an hour's wait, 30 s to answer, guild level 5; its own path.
    const assets::ItemRow recall = RecallRow();
    CHECK(game::IsGuildRecall(recall));
    CHECK_FALSE(game::IsGuildRecall(soldier));
    const game::GuildRecallItem r = game::GuildRecallOf(recall);
    CHECK(r.waitSeconds == 3600);
    CHECK(r.answerSeconds == 30);
    CHECK(r.guildLevel == 5);
    CHECK(game::OriginalUseRefusal(recall) == 0);
    const game::ItemRouting recallRoute = game::ItemRouteOf(recall, {});
    CHECK(recallRoute.route == game::ItemRoute::GuildRecall);
    CHECK(std::string(game::ItemRouteName(recallRoute.route)) == "guild-recall");
    CHECK(game::ItemStateOf(recallRoute) == game::ItemState::Implemented);

    // The messages.
    proto::GuildRecallAsk ask{"Mast", 30}, back;
    REQUIRE(proto::Decode(proto::Encode(ask), back));
    CHECK(back.master == "Mast");
    CHECK(back.seconds == 30u);
    proto::GuildEvent event;
    REQUIRE(proto::DecodeGuildEvent(proto::GuildRecallAsk::kId, proto::Encode(ask), event));
    game::GuildView view;
    view.Apply(event);
    const auto asked = view.TakeRecallAsk();
    REQUIRE(asked.has_value());
    CHECK(asked->master == "Mast");
    CHECK_FALSE(view.TakeRecallAsk().has_value());
}

TEST_CASE("Guild mercenaries and the recall: the 1.188 items (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    assets::ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    int soldiers = 0;
    for (const auto& [code, row] : items.rows()) {
        if (!row.inService || row.typeId != std::array<int, 4>{3, 3, 12, 1}) continue;
        ++soldiers;
        CHECK_MESSAGE(row.requirementTypes[0] == 10, code);
        CHECK_MESSAGE(game::OriginalUseRefusal(row) == 2, code);
        CHECK_MESSAGE(game::ItemRouteOf(row, {}).route == game::ItemRoute::OriginalRefuses, code);
    }
    CHECK(soldiers == 15);
    const assets::ItemRow* recall = items.Find(std::string_view("ITEM_ETC_GUILD_RECALL"));
    REQUIRE(recall != nullptr);
    const game::GuildRecallItem r = game::GuildRecallOf(*recall);
    CHECK(r.waitSeconds == 3600);
    CHECK(r.answerSeconds == 30);
    CHECK(r.guildLevel == 5);
    CHECK(game::ItemRouteOf(*recall, {}).route == game::ItemRoute::GuildRecall);
}

namespace {

struct RecallServer {
    TestDatabase& t;
    core::JobSystem jobs{2};
    server::GameServer game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::string error;
    explicit RecallServer(TestDatabase& db, int answerMs = 30'000) : t(db) {
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = std::make_shared<game::CharacterRules>();
        for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
        rules->creation.AddFigure(100, game::CreationRules::Race::China, true);
        rules->creation.AddArmourSet(game::CreationRules::Race::China, true, 1, 2, 3);
        rules->creation.AddWeapon(game::CreationRules::Race::China, 10);
        auto items = std::make_shared<assets::ItemTable>();
        items->Add(RecallRow(answerMs));
        items->Add(SoldierRow());
        rules->items = items;
        config.rules = rules;
        config.reportInterval = 50ms;
        config.jobs = &jobs;
        config.logoutCountdown = std::chrono::seconds(0);
        config.databaseThreads = 2;
        config.devCommands = true;
        // The developer's points: the town (1), a field beside it (2), the thieves' village (3).
        const auto row = [](std::uint16_t region, int id, const char* code) {
            return "1\t" + std::to_string(id) + "\t" + code + "\t0\tSN_" + code + "\t" + std::to_string(region) + "\t1000\t0\t1000\t0\t0\t0\t1\n";
        };
        auto teleports = std::make_shared<assets::TeleportTable>();
        teleports->Decode("", row(kTown, 1, "GATE_TOWN") + row(kField, 2, "GATE_FIELD") + row(game::kThievesVillageRegion, 3, "GATE_THIEF"), "");
        config.teleports = teleports;
    }
    void Start() { REQUIRE_MESSAGE(game.Start(config, error), error); }
    void Pump(int times = 50) {
        for (int i = 0; i < times; ++i) game.Poll(2ms);
    }
    struct Player {
        std::unique_ptr<Client> client;
        std::string name;
        std::int64_t account = 0, character = 0;
        std::uint32_t entity = 0;
        proto::InventoryList bag;
    };
    // Made (the first time) or entered.
    Player Enter(const std::string& name, bool create) {
        Player p;
        p.name = name;
        if (create) REQUIRE(server::CreateAccount(t.Db(), name + "acct", "secret1", error) == server::CreateAccountResult::Created);
        bool ok = false;
        p.account = server::FindAccount(t.Db(), name + "acct", ok)->id;
        p.client = std::make_unique<Client>(net, game.Port(), [this] { game.Poll(2ms); });
        REQUIRE(p.client->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), p.account, config.id, 30, error);
        p.client->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.client->Expect(entered));
        proto::LobbyResult r;
        if (create) {
            proto::CharacterCreate m;
            m.name = name;
            m.model = 100;
            m.chest = 1;
            m.legs = 2;
            m.shoes = 3;
            m.weapon = 10;
            p.client->Send(m);
            REQUIRE(p.client->Expect(r));
            REQUIRE(r.status == proto::LobbyStatus::Ok);
            p.character = server::ListCharacters(t.Db(), p.account, config.id, 0, ok)[0].id;
            REQUIRE(server::SaveProgress(t.Db(), {{p.character, 30, 1'000'000, 0, 0, 40, 40, 0}}));
            return p;
        }
        p.character = server::ListCharacters(t.Db(), p.account, config.id, 0, ok)[0].id;
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
    std::uint8_t PlaceOf(const Player& p, std::uint32_t refId) {
        for (const proto::ItemRecord& i : p.bag.items) {
            if (i.item == refId) return i.slot;
        }
        FAIL("no such item");
        return 0;
    }
    std::uint16_t RegionOf(const Player& p) {
        const auto at = game.GetWorld()->Where(p.entity);
        return at ? at->position.region : 0;
    }
};

// The use's refusal, by its code (none within the wait: 0).
std::uint16_t Refused(RecallServer::Player& p, std::chrono::milliseconds wait = 3s) {
    proto::ItemRefused refused;
    return p.client->Expect(refused, wait) ? refused.code : 0;
}

proto::GuildAnswer RecallAnswered(RecallServer::Player& p) {
    proto::GuildAnswer a;
    for (int i = 0; i < 20; ++i) {
        REQUIRE(p.client->Expect(a));
        if (a.action == proto::GuildAction::RecallAnswer) return a;
    }
    FAIL("no recall answer");
    return a;
}

} // namespace

TEST_CASE("Guild recall: the master's checks and codes, the item spent, its hour's wait, the members asked, a yes brought over, a no, a late and a dead yes refused") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    // The regions: the town (IsBattleField 0), a field (1), the thieves' village (0, a town to the data).
    REQUIRE(t.Db().Query("CREATE TABLE sro_vt_shard._refregion (wregionid smallint, isbattlefield int, continentname varchar(32))").Ok());
    REQUIRE(t.Db().Query("INSERT INTO sro_vt_shard._refregion VALUES ($1, 0, 'CHINA'), ($2, 1, 'CHINA'), ($3, 0, 'Thief Village')",
                         {int(std::int16_t(kTown)), int(std::int16_t(kField)), int(std::int16_t(game::kThievesVillageRegion))})
                .Ok());
    {
        RecallServer first(t);
        first.Start();
        for (const char* name : {"Mast", "Memb", "Far", "Lone", "Low"}) first.Enter(name, true);
    }
    const auto charOf = [&](const char* n) { return t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {std::string(n)}).Int(0, 0); };
    const auto guild = [&](int id, const char* name, int level, const std::vector<std::pair<const char*, int>>& members) {
        REQUIRE(t.Db().Query("INSERT INTO _guild (id, name, lvl, gatheredsp, foundationdate, alliance, booty) OVERRIDING SYSTEM VALUE VALUES ($1, $2, $3, 0, now(), 0, 0)",
                             {id, std::string(name), level})
                    .Ok());
        for (const auto& [n, memberClass] : members) {
            REQUIRE(t.Db().Query("INSERT INTO _guildmember (guildid, charid, charname, memberclass, charlevel, gp_donation, joindate, permission, siegeauthority) "
                                 "VALUES ($1, $2, $3, $4, 30, 0, now(), $5, 0)",
                                 {id, charOf(n), std::string(n), memberClass, memberClass == 0 ? -1 : 0})
                        .Ok());
            REQUIRE(t.Db().Query("UPDATE _char SET guildid = $1 WHERE charid = $2", {id, charOf(n)}).Ok());
        }
    };
    guild(301, "Callers", 5, {{"Mast", 0}, {"Memb", 10}, {"Far", 10}});
    guild(302, "Smalls", 4, {{"Low", 0}});
    REQUIRE(t.Db().Query("DELETE FROM sro_vt_account.sr_online_sessions").Ok());
    std::string error;
    const auto give = [&](const char* name, std::uint32_t refId, std::uint16_t count) {
        server::StoredItem i;
        i.refId = refId;
        i.quantity = 1;
        i.counted = true;
        const std::vector<server::StoredItem> items(count, i); // a place each (MaxStack 1)
        REQUIRE(server::GiveItems(t.Db(), 1, name, items, 0, error) == server::GiveResult::Done);
    };
    give("Mast", kRecall, 4);
    give("Mast", kSoldier, 1);
    give("Memb", kRecall, 1);
    give("Lone", kRecall, 1);
    give("Low", kRecall, 1);
    const auto stand = [&](const char* name, std::uint16_t region) {
        server::CharacterRow::Place here;
        here.region = region;
        here.x = 1000;
        here.z = 1000;
        REQUIRE(server::SavePlaces(t.Db(), {{charOf(name), here}}));
    };
    stand("Mast", kTown);
    stand("Memb", kField);
    stand("Far", kField);
    stand("Lone", kTown);
    stand("Low", kTown);

    RecallServer s(t, 3'000);
    s.Start();
    auto mast = s.Enter("Mast", false), memb = s.Enter("Memb", false), far = s.Enter("Far", false), lone = s.Enter("Lone", false),
         low = s.Enter("Low", false);
    s.Pump();
    const auto use = [&](RecallServer::Player& p, std::uint32_t refId) { p.client->Send(proto::UseItem{s.PlaceOf(p, refId), 0, 0}); };
    // A use that went through: the item's place emptied (ItemMoved), out of the bag we keep.
    const auto spent = [&](RecallServer::Player& p, std::uint32_t refId) {
        const std::uint8_t slot = s.PlaceOf(p, refId);
        proto::ItemMoved moved;
        REQUIRE(p.client->Expect(moved));
        CHECK(moved.from == slot);
        CHECK((moved.emptied.size() == 1 && moved.emptied[0] == slot));
        std::erase_if(p.bag.items, [&](const proto::ItemRecord& i) { return i.slot == slot; });
    };

    // The mercenary scroll: the requirement gate's 2, even for a master of a level-5 guild in town.
    use(mast, kSoldier);
    CHECK(Refused(mast) == 2);
    // Not in a guild, or not its master: the guild-points item's 0x18CB.
    use(lone, kRecall);
    CHECK(Refused(lone) == game::kRecallNotMaster);
    use(memb, kRecall);
    CHECK(Refused(memb) == game::kRecallNotMaster);
    // A level-4 guild's master: 0x18BC.
    use(low, kRecall);
    CHECK(Refused(low) == game::kRecallGuildLevel);
    // Out of town: 0x18C2; in the thieves' village: 0x18C7.
    mast.client->Send(proto::DevGoto{2});
    s.Pump();
    REQUIRE(s.RegionOf(mast) == kField);
    use(mast, kRecall);
    CHECK(Refused(mast) == game::kRecallNotTown);
    mast.client->Send(proto::DevGoto{3});
    s.Pump();
    REQUIRE(s.RegionOf(mast) == game::kThievesVillageRegion);
    use(mast, kRecall);
    CHECK(Refused(mast) == game::kRecallThievesTown);
    mast.client->Send(proto::DevGoto{1});
    s.Pump();
    REQUIRE(s.RegionOf(mast) == kTown);
    // Nothing was spent by the refusals, nobody asked.
    CHECK_FALSE(s.game.GetGuilds()->RecallAsked(memb.character));

    // In town, the master of a level-5 guild: the item spent, its wait an hour in _TimedJob 2 / 3, the members asked
    // (not the master, not one of another guild).
    use(mast, kRecall);
    spent(mast, kRecall);
    proto::GuildRecallAsk ask;
    REQUIRE(memb.client->Expect(ask));
    CHECK(ask.master == "Mast");
    CHECK(ask.seconds == 3u);
    REQUIRE(far.client->Expect(ask));
    CHECK_FALSE(mast.client->Expect(ask, 300ms));
    CHECK_FALSE(lone.client->Expect(ask, 300ms));
    const auto wait = t.Db().Query("SELECT timetokeep FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 3", {mast.character});
    REQUIRE(wait.Rows() == 1);
    CHECK(wait.Int(0, 0) == 3600);
    CHECK(s.game.GetGuilds()->RecallAsked(memb.character));

    const auto pollFor = [&](std::chrono::milliseconds span) {
        const auto end = std::chrono::steady_clock::now() + span;
        while (std::chrono::steady_clock::now() < end) s.game.Poll(5ms);
    };
    // A yes: the recall's gauge for the ask's seconds (the casting gauge's kind 2), then on its way to where the master
    // stands.
    REQUIRE(s.RegionOf(memb) == kField);
    memb.client->Send(proto::GuildRecallAnswer{1});
    CHECK(RecallAnswered(memb).error == 0);
    proto::ReturnStarted gauge;
    REQUIRE(memb.client->Expect(gauge));
    CHECK(gauge.kind == proto::CastingKind::GuildRecall);
    CHECK(gauge.ms == 3000u);
    s.Pump();
    CHECK(s.RegionOf(memb) == kField); // on its gauge still
    pollFor(3500ms);
    CHECK(s.RegionOf(memb) == kTown);
    // A no: the ask gone, no answer; a yes after it refused.
    far.client->Send(proto::GuildRecallAnswer{0});
    s.Pump();
    CHECK_FALSE(s.game.GetGuilds()->RecallAsked(far.character));
    far.client->Send(proto::GuildRecallAnswer{1});
    CHECK(RecallAnswered(far).error == game::kRecallWorld);
    CHECK(s.RegionOf(far) == kField);

    // Its wait: the next use refused 0x185B.
    use(mast, kRecall);
    CHECK(Refused(mast) == game::kRecallWaiting);

    // The wait gone (as an hour on): asked again; a yes after the answer's time (3 s here) refused 0x18C1.
    REQUIRE(t.Db().Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 3", {mast.character}).Ok());
    use(mast, kRecall);
    spent(mast, kRecall);
    REQUIRE(far.client->Expect(ask));
    const auto late = std::chrono::steady_clock::now() + 3500ms;
    while (std::chrono::steady_clock::now() < late) s.game.Poll(5ms);
    far.client->Send(proto::GuildRecallAnswer{1});
    CHECK(RecallAnswered(far).error == game::kRecallWorld);
    CHECK(s.RegionOf(far) == kField);

    // A yes whose gauge is cancelled (the teleport's cancel, as a return scroll's): told, and it stays.
    REQUIRE(t.Db().Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 3", {mast.character}).Ok());
    use(mast, kRecall);
    spent(mast, kRecall);
    REQUIRE(far.client->Expect(ask));
    far.client->Send(proto::GuildRecallAnswer{1});
    CHECK(RecallAnswered(far).error == 0);
    REQUIRE(far.client->Expect(gauge));
    CHECK(gauge.kind == proto::CastingKind::GuildRecall);
    far.client->Send(proto::CancelReturn{});
    proto::ReturnCancelled cancelled;
    CHECK(far.client->Expect(cancelled));
    pollFor(3500ms);
    CHECK(s.RegionOf(far) == kField);

    // A dead member is asked but its yes refused.
    REQUIRE(t.Db().Query("DELETE FROM _timedjob WHERE charid = $1 AND category = 2 AND jobid = 3", {mast.character}).Ok());
    use(mast, kRecall);
    spent(mast, kRecall);
    REQUIRE(far.client->Expect(ask));
    far.client->Send(proto::DevDie{});
    s.Pump();
    far.client->Send(proto::GuildRecallAnswer{1});
    CHECK(RecallAnswered(far).error == game::kRecallWorld);
    CHECK(s.RegionOf(far) == kField);
}
