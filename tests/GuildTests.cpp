#include <doctest/doctest.h>

// Guilds and unions against the game server in-process: the original's rules over the wire, and what the original's
// tables keep.

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <memory>
#include <string>

#include "ServerTestSupport.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/Terrain.h"
#include "sr/assets/UiLayout2dt.h"
#include "sr/core/Path.h"
#include "sr/core/Jobs.h"
#include "sr/game/Guild.h"
#include "sr/game/WorldProtocol.h"
#include "sr/server/GameServer.h"
#include "sr/server/Guilds.h"
#include "sr/server/Store.h"

using namespace sr;
using namespace sr::test;
using namespace std::chrono_literals;
using Race = game::CreationRules::Race;
using game::GuildError;
using proto::GuildAction;

namespace {

constexpr std::uint32_t kKeeperModel = 2021; // NPC_CH_GENARAL_SP: Jangan's keeper of guilds

std::shared_ptr<game::CharacterRules> GuildRules() {
    auto rules = std::make_shared<game::CharacterRules>();
    for (auto [first, last] : {std::pair<char32_t, char32_t>{'0', '9'}, {'A', 'Z'}, {'a', 'z'}}) rules->names.Allow(first, last);
    rules->creation.AddFigure(100, Race::China, true);
    rules->creation.AddArmourSet(Race::China, true, 1, 2, 3);
    rules->creation.AddWeapon(Race::China, 10);
    return rules;
}

struct Player {
    std::unique_ptr<Client> c;
    std::string name;
    std::int64_t character = 0;
    std::uint32_t entity = 0;
    std::uint32_t keeper = 0; // the guild NPC's entity as this player sees it
};

// A game server with Jangan's storage keeper standing by the Chinese start place, and players.
struct Rig {
    TestDatabase& t;
    core::JobSystem jobs{2};
    std::unique_ptr<server::GameServer> game;
    server::GameServerConfig config;
    net::NetService net{1};
    std::function<void()> pump;
    std::string error;

    explicit Rig(TestDatabase& db, std::shared_ptr<const assets::ItemTable> items = nullptr, std::uint16_t id = 1, const SplitWorld* split = nullptr) : t(db) {
        config.id = id;
        if (split != nullptr) split->Apply(config);
        config.listen = Local();
        config.databaseUrl = t.Url();
        auto rules = GuildRules();
        rules->items = std::move(items);
        config.rules = rules;
        config.reportInterval = 50ms;
        config.jobs = &jobs; // no navigation: walks go straight, everyone stands where it enters
        config.npcs.push_back({kKeeperModel, "NPC_CH_GENARAL_SP", {assets::RegionId(168, 98), 1000.0f, 50.0f, 458.0f}, 0.0f});
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
    // Waits until the guilds' writes are in the database.
    void Settle() {
        for (int i = 0; i < 20 || (game->GetGuilds() && game->GetGuilds()->Writing()); ++i) pump();
    }

    // An account and a character of `level` with `gold` (and skill points), selected and in the world.
    Player Join(const std::string& name, int level = 30, std::int64_t gold = 1'000'000, std::int64_t skillPoints = 0) {
        Player p;
        p.name = name;
        const std::string account = "acc" + name;
        if (!server::FindAccount(t.Db(), account, ok_)) {
            REQUIRE(server::CreateAccount(t.Db(), account, "secret1", error) == server::CreateAccountResult::Created);
        }
        const std::int64_t id = server::FindAccount(t.Db(), account, ok_)->id;
        p.c = std::make_unique<Client>(net, game->Port(), pump);
        REQUIRE(p.c->Greet());
        proto::LobbyEnter e;
        e.ticket = server::IssueTicket(t.Db(), id, config.id, 30, error);
        p.c->Send(e);
        proto::LobbyEntered entered;
        REQUIRE(p.c->Expect(entered));
        REQUIRE(entered.status == proto::LobbyEnterStatus::Accepted);
        const server::QueryResult exists = t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {name});
        proto::LobbyResult r;
        if (exists.Rows() == 0) {
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
            REQUIRE(t.Db()
                        .Query("UPDATE _char SET curlevel = $2, remaingold = $3, remainskillpoint = $4 WHERE charname16 = $1",
                               {name, level, gold, skillPoints})
                        .Ok());
        }
        p.character = t.Db().Query("SELECT charid FROM _char WHERE charname16 = $1", {name}).Int(0, 0);
        p.c->Send(proto::CharacterSelect{name});
        REQUIRE(p.c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        p.c->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.c->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
        proto::EntityAppeared seen;
        while (p.keeper == 0 && p.c->Expect(seen)) {
            if (seen.character.model == kKeeperModel) p.keeper = seen.entity;
        }
        REQUIRE(p.keeper != 0);
        return p;
    }
    // Back into the world from the lobby (after Leave).
    void Reenter(Player& p) {
        p.c->Send(proto::CharacterSelect{p.name});
        proto::LobbyResult r;
        REQUIRE(p.c->Expect(r));
        REQUIRE(r.status == proto::LobbyStatus::Ok);
        p.c->Send(proto::WorldEnter{});
        proto::WorldEntered w;
        REQUIRE(p.c->Expect(w));
        REQUIRE(w.status == proto::WorldStatus::Ok);
        p.entity = w.entity;
    }
    void Leave(Player& p) {
        p.c->Send(proto::WorldLeave{});
        proto::WorldLeft left;
        REQUIRE(p.c->Expect(left));
    }

private:
    bool ok_ = false;
};

proto::GuildAnswer Answer(Player& p, GuildAction action) {
    proto::GuildAnswer a;
    REQUIRE(p.c->Expect(a));
    CHECK(a.action == action);
    return a;
}

GuildError Error(Player& p, GuildAction action) { return GuildError(Answer(p, action).error); }

template <typename T>
T Next(Player& p) {
    T m;
    REQUIRE(p.c->Expect(m));
    return m;
}

// The last of what arrived (and nothing more for a moment).
template <typename T>
T Last(Player& p) {
    T last, m;
    while (p.c->Expect(m, 300ms)) last = m;
    return last;
}

void Invite(Player& from, Player& to, bool accept) {
    from.c->Send(proto::GuildInvite{to.entity});
    const auto proposal = Next<proto::GuildProposal>(to);
    CHECK(proposal.kind == proto::kProposalGuild);
    CHECK(proposal.fromName == from.name);
    to.c->Send(proto::GuildProposalAnswer{proto::kProposalGuild, std::uint8_t(accept ? 1 : 0)});
}

proto::GuildEvent Event(const auto& m) { return proto::GuildEvent{m}; }

} // namespace

TEST_CASE("Guilds: fortress positions by the original's checks, and what the points went on") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig rig(t);
    Player lead = rig.Join("Lead", 40, 10'000'000, 10'000);
    std::vector<Player> members;
    for (const char* name : {"Ann", "Bob", "Cid", "Dan", "Eve"}) members.push_back(rig.Join(name));
    lead.c->Send(proto::GuildCreate{lead.keeper, "Keepers"});
    CHECK(Next<proto::GuildInfo>(lead).name == "Keepers");
    CHECK(Error(lead, GuildAction::Create) == GuildError::Ok);
    for (Player& m : members) {
        Invite(lead, m, true);
        CHECK(Error(lead, GuildAction::Invite) == GuildError::Ok);
        CHECK(Error(m, GuildAction::Join) == GuildError::Ok);
    }

    const auto grant = [&](Player& by, const Player& to, std::uint8_t position) {
        by.c->Send(proto::GuildPositionGrant{std::uint32_t(to.character), position});
        return Error(by, GuildAction::Position);
    };
    // Only the master, never the commander's place, the administrators only with a fortress.
    CHECK(grant(members[0], members[1], game::kPositionDeputy) == GuildError::Denied);
    CHECK(grant(lead, members[0], game::kPositionCommander) == GuildError::PositionMaster);
    CHECK(grant(lead, members[0], game::kPositionWar) == GuildError::PositionNoFortress);
    CHECK(grant(lead, members[0], game::kPositionNone) == GuildError::PositionNone);
    CHECK(grant(lead, lead, game::kPositionNone) == GuildError::PositionMaster);
    // Four deputies at most; one position each; taken away again.
    for (int i = 0; i < 4; ++i) CHECK(grant(lead, members[std::size_t(i)], game::kPositionDeputy) == GuildError::Ok);
    CHECK(grant(lead, members[0], game::kPositionEngineer) == GuildError::PositionHeld);
    CHECK(grant(lead, members[4], game::kPositionDeputy) == GuildError::PositionFull);
    CHECK(grant(lead, members[4], game::kPositionEngineer) == GuildError::Ok);
    CHECK(grant(lead, members[0], game::kPositionNone) == GuildError::Ok);
    rig.Settle();
    const auto position = [&](const char* name) { return t.Db().Query("SELECT siegeauthority FROM _guildmember WHERE charname = $1", {std::string(name)}).Int(0, 0); };
    CHECK(position("Lead") == game::kPositionCommander);
    CHECK(position("Ann") == game::kPositionNone);
    CHECK(position("Bob") == game::kPositionDeputy);
    CHECK(position("Eve") == game::kPositionEngineer);

    // A promotion is written down, the GP asked at the NPC; the donation answers with its amount.
    lead.c->Send(proto::GuildDonate{game::kGuildLevelUpPoints[1]});
    CHECK(Answer(lead, GuildAction::Donate).value == game::kGuildLevelUpPoints[1]);
    lead.c->Send(proto::GuildLevelUp{lead.keeper});
    CHECK(Error(lead, GuildAction::LevelUp) == GuildError::Ok);
    rig.Settle();
    lead.c->Send(proto::GuildGpHistoryOpen{lead.entity}); // not an NPC
    CHECK(Error(lead, GuildAction::GpHistory) == GuildError::Failed);
    lead.c->Send(proto::GuildGpHistoryOpen{lead.keeper});
    const auto history = Next<proto::GuildGpHistory>(lead);
    REQUIRE(history.rows.size() == 1);
    CHECK(history.rows[0].name == "Lead");
    CHECK(history.rows[0].points == game::kGuildLevelUpPoints[1]);
    CHECK(history.rows[0].reason == game::kGpReasonGrowth);
    CHECK(history.rows[0].year >= 2026);
}

TEST_CASE("Guilds: the NPCs that keep them are one in each town, never a storage keeper or a guild agent") {
    for (const char* keeper : {"NPC_CH_GENARAL_SP", "NPC_WC_GUILD", "NPC_KT_GUILD", "NPC_EU_GUILD", "NPC_CA_GUILD", "NPC_SD_M_AREA_GUILD", "NPC_SD_T_AREA_GUILD2"}) {
        CHECK(game::IsGuildNpcCodename(keeper));
    }
    for (const char* other : {"NPC_CH_WAREHOUSE_M", "NPC_CH_WAREHOUSE_W", "NPC_SD_GUILD_AGENT", "NPC_SD_GUILD_AGENT2", "NPC_CH_GENARAL_SW", "NPC_KT_WAREHOUSE"}) {
        CHECK_FALSE(game::IsGuildNpcCodename(other));
    }
}

TEST_CASE("Guilds: create, invite, ranks, notice, chat, kick, leave, disband, the names others see") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig rig(t);
    Player alpha = rig.Join("Alpha", 30, 2'000'000);
    Player low = rig.Join("Lowly", 19, 2'000'000);
    Player poor = rig.Join("Poorly", 30, 499'999);
    Player beta = rig.Join("Beta");
    Player gamma = rig.Join("Gamma");

    // Creating: level 20 and 500,000 gold at a guild NPC, a name of 2-12 bytes, unique.
    low.c->Send(proto::GuildCreate{low.keeper, "Lows"});
    CHECK(Error(low, GuildAction::Create) == GuildError::CreatorLevel);
    poor.c->Send(proto::GuildCreate{poor.keeper, "Poors"});
    CHECK(Error(poor, GuildAction::Create) == GuildError::NoGold);
    alpha.c->Send(proto::GuildCreate{alpha.entity, "Dragons"}); // not an NPC
    CHECK(Error(alpha, GuildAction::Create) == GuildError::Failed);
    alpha.c->Send(proto::GuildCreate{alpha.keeper, "D"});
    CHECK(Error(alpha, GuildAction::Create) == GuildError::NameLength);
    alpha.c->Send(proto::GuildCreate{alpha.keeper, "ThirteenChars"});
    CHECK(Error(alpha, GuildAction::Create) == GuildError::NameLength);
    alpha.c->Send(proto::GuildCreate{alpha.keeper, "Bad!Name"});
    CHECK(Error(alpha, GuildAction::Create) == GuildError::NameNotAllowed);
    alpha.c->Send(proto::GuildCreate{alpha.keeper, "Dragons"});
    const auto info = Next<proto::GuildInfo>(alpha);
    CHECK(info.name == "Dragons");
    CHECK(info.level == 1);
    REQUIRE(info.members.size() == 1);
    CHECK(info.members[0].memberClass == game::kGuildMaster);
    CHECK(info.members[0].permission == game::kGuildPermAll);
    const auto gold = Next<proto::GoldChanged>(alpha);
    CHECK(gold.gold == 2'000'000 - game::kGuildCreateGold);
    CHECK(Error(alpha, GuildAction::Create) == GuildError::Ok);
    // Those around see its guild over its head.
    proto::EntityGuild tag;
    do tag = Next<proto::EntityGuild>(beta);
    while (tag.entity != alpha.entity);
    CHECK(tag.name == "Dragons");
    CHECK(game::GuildView::HeadLine(tag.name, tag.nickname) == "[Dragons]");
    // The same name again, ignoring case.
    beta.c->Send(proto::GuildCreate{beta.keeper, "dragons"});
    CHECK(Error(beta, GuildAction::Create) == GuildError::NameTaken);
    alpha.c->Send(proto::GuildCreate{alpha.keeper, "Second"});
    CHECK(Error(alpha, GuildAction::Create) == GuildError::InAnotherGuild);

    // Inviting: declined, then accepted; the others told of the new member.
    Invite(alpha, beta, false);
    CHECK(Error(alpha, GuildAction::Invite) == GuildError::JoinRefused);
    CHECK(Error(beta, GuildAction::Join) == GuildError::JoinRefused);
    Invite(alpha, beta, true);
    const auto joined = Next<proto::GuildInfo>(beta);
    CHECK(joined.members.size() == 2);
    CHECK(Error(alpha, GuildAction::Invite) == GuildError::Ok);
    CHECK(Error(beta, GuildAction::Join) == GuildError::Ok);
    CHECK(Next<proto::GuildMemberJoined>(alpha).member.name == "Beta");
    // A member without the invite bit may not invite.
    beta.c->Send(proto::GuildInvite{gamma.entity});
    CHECK(Error(beta, GuildAction::Invite) == GuildError::Denied);
    Invite(alpha, gamma, true);
    CHECK(Next<proto::GuildInfo>(gamma).members.size() == 3);
    CHECK(Error(alpha, GuildAction::Invite) == GuildError::Ok);
    CHECK(Error(gamma, GuildAction::Join) == GuildError::Ok);
    alpha.c->Send(proto::GuildInvite{beta.entity});
    CHECK(Error(alpha, GuildAction::Invite) == GuildError::ExistingMember);

    // Authority: only the master gives it; at most 12 hold union chat.
    beta.c->Send(proto::GuildPermissions{{{std::uint32_t(gamma.character), game::kGuildPermKick}}});
    CHECK(Error(beta, GuildAction::Permissions) == GuildError::Denied);
    alpha.c->Send(proto::GuildPermissions{{{std::uint32_t(beta.character), game::kGuildPermInvite | game::kGuildPermKick | game::kGuildPermNotice}}});
    CHECK(Error(alpha, GuildAction::Permissions) == GuildError::Ok);
    CHECK(Last<proto::GuildMemberChanged>(beta).member.permission == (game::kGuildPermInvite | game::kGuildPermKick | game::kGuildPermNotice));

    // The notice: 1-128 and 1-2048 characters, by those allowed.
    gamma.c->Send(proto::GuildNoticeEdit{"Hi", "Text"});
    CHECK(Error(gamma, GuildAction::Notice) == GuildError::Denied);
    beta.c->Send(proto::GuildNoticeEdit{"", "Text"});
    CHECK(Error(beta, GuildAction::Notice) == GuildError::NoticeTitle);
    beta.c->Send(proto::GuildNoticeEdit{std::string(129, 'x'), "Text"});
    CHECK(Error(beta, GuildAction::Notice) == GuildError::NoticeTitle);
    beta.c->Send(proto::GuildNoticeEdit{"Hunt", ""});
    CHECK(Error(beta, GuildAction::Notice) == GuildError::NoticeBody);
    beta.c->Send(proto::GuildNoticeEdit{"Hunt", "Tonight at Donwhang"});
    CHECK(Error(beta, GuildAction::Notice) == GuildError::Ok);
    const auto notice = Next<proto::GuildNoticeChanged>(gamma);
    CHECK(notice.title == "Hunt");
    CHECK(notice.body == "Tonight at Donwhang");

    // The guild's chat reaches its members, not the others.
    alpha.c->Send(proto::ChatSend{proto::ChatKind::Guild, 1, "", "hello guild"});
    CHECK(Next<proto::ChatAnswer>(alpha).result == proto::ChatResult::Ok);
    const auto line = Next<proto::ChatLine>(gamma);
    CHECK(line.kind == proto::ChatKind::Guild);
    CHECK(line.name == "Alpha");
    CHECK(line.text == "hello guild");
    low.c->Send(proto::ChatSend{proto::ChatKind::Guild, 1, "", "me too"});
    CHECK(Next<proto::ChatAnswer>(low).result == proto::ChatResult::InvalidCommand);
    // The union's: not in a union.
    alpha.c->Send(proto::ChatSend{proto::ChatKind::Union, 2, "", "allies?"});
    CHECK(Next<proto::ChatAnswer>(alpha).result == proto::ChatResult::NotInUnion);

    // The master's place handed over (at a guild NPC), then back.
    alpha.c->Send(proto::GuildMasterTransfer{alpha.keeper, 999999});
    CHECK(Error(alpha, GuildAction::MasterTransfer) == GuildError::MemberNotFound);
    beta.c->Send(proto::GuildMasterTransfer{beta.keeper, std::uint32_t(gamma.character)});
    CHECK(Error(beta, GuildAction::MasterTransfer) == GuildError::Denied);
    alpha.c->Send(proto::GuildMasterTransfer{alpha.keeper, std::uint32_t(gamma.character)});
    CHECK(Error(alpha, GuildAction::MasterTransfer) == GuildError::Ok);
    {
        rig.Settle();
        const server::QueryResult r = t.Db().Query("SELECT charname, memberclass, permission FROM _guildmember WHERE guildid <> 0 ORDER BY charname");
        REQUIRE(r.Rows() == 3);
        CHECK(r.Text(0, 0) == "Alpha");
        CHECK(r.Int(0, 1) == game::kGuildMember);
        CHECK(r.Int(0, 2) == 0);
        CHECK(r.Text(2, 0) == "Gamma");
        CHECK(r.Int(2, 1) == game::kGuildMaster);
        CHECK(r.Int(2, 2) == -1);
        // The lobby's list knows each one's place (its warnings before a deletion): the master 2, a member 1, none 0.
        const auto roleOf = [&](const std::string& name) {
            bool ok = false;
            const auto account = server::FindAccount(t.Db(), "acc" + name, ok);
            REQUIRE(account.has_value());
            for (const server::CharacterRow& c : server::ListCharacters(t.Db(), account->id, rig.config.id, 0, ok)) {
                if (c.name == name) return int(c.guildRole);
            }
            return -1;
        };
        CHECK(roleOf("Gamma") == 2);
        CHECK(roleOf("Alpha") == 1);
        CHECK(roleOf("Lowly") == 0);
    }
    gamma.c->Send(proto::GuildMasterTransfer{gamma.keeper, std::uint32_t(alpha.character)});
    CHECK(Error(gamma, GuildAction::MasterTransfer) == GuildError::Ok);

    // Titles from guild level 4 only.
    alpha.c->Send(proto::GuildNickname{std::uint32_t(beta.character), "Scout"});
    CHECK(Error(alpha, GuildAction::Nickname) == GuildError::NicknameLevel);

    // Putting out: the kick bit; never the master, never oneself.
    gamma.c->Send(proto::GuildKick{"Beta"});
    CHECK(Error(gamma, GuildAction::Kick) == GuildError::Denied);
    beta.c->Send(proto::GuildKick{"alpha"});
    CHECK(Error(beta, GuildAction::Kick) == GuildError::MasterNotExpellable);
    beta.c->Send(proto::GuildKick{"Beta"});
    CHECK(Error(beta, GuildAction::Kick) == GuildError::KickSelf);
    beta.c->Send(proto::GuildKick{"gamma"});
    const auto removed = Next<proto::GuildMemberRemoved>(gamma);
    CHECK(removed.reason == 2);
    CHECK(removed.name == "Gamma");
    CHECK(Next<proto::GuildInfo>(gamma).id == 0);
    CHECK(Error(beta, GuildAction::Kick) == GuildError::Ok);
    CHECK(Next<proto::GuildMemberRemoved>(alpha).name == "Gamma");
    // The one put out waits three days to join or create again.
    alpha.c->Send(proto::GuildInvite{gamma.entity});
    CHECK(Error(alpha, GuildAction::Invite) == GuildError::JoinPenalty);
    gamma.c->Send(proto::GuildCreate{gamma.keeper, "Gammas"});
    const auto penalty = Answer(gamma, GuildAction::Create);
    CHECK(GuildError(penalty.error) == GuildError::CreatePenalty);
    CHECK(penalty.value > game::kGuildPenaltySeconds - 60);
    CHECK(penalty.value <= game::kGuildPenaltySeconds);
    // Over its head: no guild any more.
    do tag = Next<proto::EntityGuild>(alpha);
    while (tag.entity != gamma.entity || tag.guild != 0);

    // Leaving at an NPC; the master cannot leave.
    alpha.c->Send(proto::GuildLeave{alpha.keeper});
    alpha.c->Send(proto::GuildNoticeEdit{"Still", "here"}); // the leave was ignored: the master is still in
    CHECK(Error(alpha, GuildAction::Notice) == GuildError::Ok);
    beta.c->Send(proto::GuildLeave{beta.keeper});
    CHECK(Next<proto::GuildMemberRemoved>(alpha).reason == 1);
    CHECK(Error(beta, GuildAction::Leave) == GuildError::Ok);
    rig.Settle();
    CHECK(t.Db().Query("SELECT count(*) FROM _timedjob WHERE category = 2 AND jobid = 1 AND timetokeep = 259200").Int(0, 0) == 2);
    CHECK(t.Db().Query("SELECT guildid FROM _char WHERE charname16 = 'Beta'").Int(0, 0) == 0);

    // Disbanding: the master only, at a guild NPC; the guild's rows gone, the master's penalty.
    alpha.c->Send(proto::GuildDisband{alpha.keeper});
    CHECK(Next<proto::GuildDisbanded>(alpha).name == "Dragons");
    CHECK(Error(alpha, GuildAction::Disband) == GuildError::Ok);
    rig.Settle();
    CHECK(t.Db().Query("SELECT count(*) FROM _guild WHERE id <> 0").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM _guildmember").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM _guildchest").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM _timedjob WHERE category = 2 AND jobid = 1").Int(0, 0) == 3);
    rig.game->Stop();
}

TEST_CASE("Guilds: kept in the original's tables across a relog and a restart; newcomers see the guild over heads") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig rig(t);
    Player alpha = rig.Join("Alpha");
    Player beta = rig.Join("Beta");
    alpha.c->Send(proto::GuildCreate{alpha.keeper, "Keepers"});
    CHECK(Next<proto::GuildInfo>(alpha).name == "Keepers");
    CHECK(Error(alpha, GuildAction::Create) == GuildError::Ok);
    Invite(alpha, beta, true);
    CHECK(Error(beta, GuildAction::Join) == GuildError::Ok);
    CHECK(Error(alpha, GuildAction::Invite) == GuildError::Ok);
    alpha.c->Send(proto::GuildNoticeEdit{"Rules", "Be kind"});
    CHECK(Error(alpha, GuildAction::Notice) == GuildError::Ok);
    rig.Settle();
    {
        const server::QueryResult g = t.Db().Query("SELECT id, name, lvl, mastercommenttitle, mastercomment FROM _guild WHERE id <> 0");
        REQUIRE(g.Rows() == 1);
        CHECK(g.Text(0, 1) == "Keepers");
        CHECK(g.Int(0, 2) == 1);
        CHECK(g.Text(0, 3) == "Rules");
        CHECK(t.Db().Query("SELECT count(*) FROM _char WHERE guildid = $1", {g.Int(0, 0)}).Int(0, 0) == 2);
        CHECK(t.Db().Query("SELECT count(*) FROM _guildchest WHERE guildid = $1", {g.Int(0, 0)}).Int(0, 0) == 240);
    }

    // Beta leaves the world: Alpha sees it offline; back in, online again, and its guild comes with it.
    rig.Leave(beta);
    CHECK(Last<proto::GuildMemberChanged>(alpha).member.online == 0);
    rig.Reenter(beta);
    const auto info = Last<proto::GuildInfo>(beta);
    CHECK(info.name == "Keepers");
    CHECK(info.noticeTitle == "Rules");
    CHECK(info.members.size() == 2);
    const auto online = Last<proto::GuildMemberChanged>(alpha);
    CHECK(online.member.name == "Beta");
    CHECK(online.member.online == 1);

    // A restart: everything from the tables.
    rig.Restart();
    Player a2 = rig.Join("Alpha");
    const auto restored = Last<proto::GuildInfo>(a2);
    CHECK(restored.name == "Keepers");
    CHECK(restored.notice == "Be kind");
    REQUIRE(restored.members.size() == 2);
    // A newcomer sees the guild over the head of whoever is in it (told after its appearance).
    Player gamma = rig.Join("Gamma");
    proto::EntityGuild tag;
    do tag = Next<proto::EntityGuild>(gamma);
    while (tag.entity != a2.entity);
    CHECK(tag.name == "Keepers");
    game::GuildView view;
    CHECK(view.HeadColour(tag.guild, tag.unionId) == 0xFF66B9FF); // another's guild: light blue
    view.SetSelf("Alpha");
    view.Apply(Event(restored));
    CHECK(view.HeadColour(tag.guild, tag.unionId) == 0xFFFEAD2E); // one's own: orange
    rig.game->Stop();
}

TEST_CASE("Guilds: points from skill points, the level up, then unions: formed, joined, chat, put out, left, dissolved") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig rig(t);
    // Three masters with gold for the level and skill points to give.
    std::map<std::string, Player> m;
    for (const char* name : {"Alpha", "Delta", "Echo"}) m[name] = rig.Join(name, 40, 10'000'000, 10'000);
    Player member = rig.Join("Member");
    for (const auto& [name, guild] : std::map<std::string, std::string>{{"Alpha", "Lions"}, {"Delta", "Wolves"}, {"Echo", "Eagles"}}) {
        Player& p = m[name];
        p.c->Send(proto::GuildCreate{p.keeper, guild});
        CHECK(Next<proto::GuildInfo>(p).name == guild);
        CHECK(Error(p, GuildAction::Create) == GuildError::Ok);
    }
    Player& alpha = m["Alpha"];
    Player& delta = m["Delta"];
    Player& echo = m["Echo"];

    // A union needs guilds of level 2.
    alpha.c->Send(proto::UnionInvite{delta.entity});
    CHECK(Error(alpha, GuildAction::UnionInvite) == GuildError::UnionLevel);
    // The level: 5,400 points and 3,000,000 gold; points one for one from skill points.
    alpha.c->Send(proto::GuildLevelUp{alpha.keeper});
    CHECK(Error(alpha, GuildAction::LevelUp) == GuildError::LevelUpPoints);
    alpha.c->Send(proto::GuildDonate{20'000});
    CHECK(Error(alpha, GuildAction::Donate) == GuildError::LevelUpPoints);
    for (const char* name : {"Alpha", "Delta", "Echo"}) {
        Player& p = m[name];
        p.c->Send(proto::GuildDonate{game::kGuildLevelUpPoints[1]});
        CHECK(Next<proto::GuildPoints>(p).points == game::kGuildLevelUpPoints[1]);
        CHECK(Error(p, GuildAction::Donate) == GuildError::Ok);
        p.c->Send(proto::GuildLevelUp{p.keeper});
        const auto points = Next<proto::GuildPoints>(p);
        CHECK(points.level == 2);
        CHECK(points.points == 0);
        CHECK(Error(p, GuildAction::LevelUp) == GuildError::Ok);
    }
    rig.Settle();
    CHECK(t.Db().Query("SELECT count(*) FROM _guild WHERE lvl = 2").Int(0, 0) == 3);
    CHECK(t.Db().Query("SELECT remaingold FROM _char WHERE charname16 = 'Alpha'").Int(0, 0) == 10'000'000 - 500'000 - 3'000'000);
    CHECK(t.Db().Query("SELECT gp_donation FROM _guildmember WHERE charname = 'Alpha'").Int(0, 0) == 5'400);

    // Only a guild's master may answer for it; a guild without a guild master in reach is refused.
    alpha.c->Send(proto::UnionInvite{member.entity});
    CHECK(Error(alpha, GuildAction::UnionInvite) == GuildError::NoTargetGuild);

    // Formed: Alpha's guild leads, Delta's joins.
    alpha.c->Send(proto::UnionInvite{delta.entity});
    const auto proposal = Next<proto::GuildProposal>(delta);
    CHECK(proposal.kind == proto::kProposalUnion);
    CHECK(proposal.guildName == "Lions");
    delta.c->Send(proto::GuildProposalAnswer{proto::kProposalUnion, 1});
    auto u = Next<proto::UnionInfo>(delta);
    REQUIRE(u.guilds.size() == 2);
    CHECK(u.guilds[0].name == "Lions");
    CHECK(u.guilds[0].master == "Alpha");
    CHECK(u.guilds[1].name == "Wolves");
    CHECK(Next<proto::UnionChanged>(alpha).event == 1);
    CHECK(Error(alpha, GuildAction::UnionInvite) == GuildError::Ok);
    CHECK(Error(delta, GuildAction::UnionJoin) == GuildError::Ok);
    // Only the leading guild's master invites.
    delta.c->Send(proto::UnionInvite{echo.entity});
    CHECK(Error(delta, GuildAction::UnionInvite) == GuildError::Denied);
    alpha.c->Send(proto::UnionInvite{echo.entity});
    Next<proto::GuildProposal>(echo);
    echo.c->Send(proto::GuildProposalAnswer{proto::kProposalUnion, 1});
    CHECK(Last<proto::UnionInfo>(echo).guilds.size() == 3);
    rig.Settle();
    {
        const server::QueryResult a = t.Db().Query("SELECT id, ally1, ally2, ally3 FROM _alliedclans WHERE id <> 0");
        REQUIRE(a.Rows() == 1);
        CHECK(t.Db().Query("SELECT count(*) FROM _guild WHERE alliance = $1", {a.Int(0, 0)}).Int(0, 0) == 3);
        CHECK(a.Int(0, 1) == t.Db().Query("SELECT id FROM _guild WHERE name = 'Lions'").Int(0, 0));
    }

    // The union's chat: masters always; it reaches the other guilds.
    (void)Last<proto::GuildAnswer>(alpha);
    (void)Last<proto::GuildAnswer>(echo);
    alpha.c->Send(proto::ChatSend{proto::ChatKind::Union, 3, "", "together"});
    CHECK(Next<proto::ChatAnswer>(alpha).result == proto::ChatResult::Ok);
    const auto line = Next<proto::ChatLine>(echo);
    CHECK(line.kind == proto::ChatKind::Union);
    CHECK(line.text == "together");
    // A member without the union chat bit may not.
    alpha.c->Send(proto::GuildInvite{member.entity});
    Next<proto::GuildProposal>(member);
    member.c->Send(proto::GuildProposalAnswer{proto::kProposalGuild, 1});
    CHECK(Next<proto::GuildInfo>(member).unionId != 0);
    member.c->Send(proto::ChatSend{proto::ChatKind::Union, 1, "", "me?"});
    CHECK(Next<proto::ChatAnswer>(member).result == proto::ChatResult::NoUnionRight);
    alpha.c->Send(proto::GuildPermissions{{{std::uint32_t(member.character), game::kGuildPermUnionChat}}});
    (void)Last<proto::GuildAnswer>(alpha);
    member.c->Send(proto::ChatSend{proto::ChatKind::Union, 2, "", "me!"});
    CHECK(Next<proto::ChatAnswer>(member).result == proto::ChatResult::Ok);

    // Put out by the leader: two guilds remain.
    delta.c->Send(proto::UnionKick{u.guilds[1].id});
    CHECK(Error(delta, GuildAction::UnionKick) == GuildError::Denied);
    const std::uint32_t eagles = std::uint32_t(t.Db().Query("SELECT id FROM _guild WHERE name = 'Eagles'").Int(0, 0));
    alpha.c->Send(proto::UnionKick{eagles});
    CHECK(Last<proto::UnionInfo>(echo).id == 0);
    CHECK(Last<proto::UnionInfo>(delta).guilds.size() == 2);
    // Delta's guild leaves: fewer than two remain, the union is gone.
    delta.c->Send(proto::UnionLeave{});
    CHECK(Last<proto::UnionInfo>(alpha).id == 0);
    rig.Settle();
    CHECK(t.Db().Query("SELECT count(*) FROM _alliedclans WHERE id <> 0").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT count(*) FROM _guild WHERE alliance <> 0").Int(0, 0) == 0);

    // Formed again; the leading guild leaves: dissolved.
    (void)Last<proto::GuildAnswer>(alpha);
    alpha.c->Send(proto::UnionInvite{delta.entity});
    Next<proto::GuildProposal>(delta);
    delta.c->Send(proto::GuildProposalAnswer{proto::kProposalUnion, 1});
    CHECK(Last<proto::UnionInfo>(delta).guilds.size() == 2);
    alpha.c->Send(proto::UnionLeave{});
    CHECK(Last<proto::UnionChanged>(delta).event == 4);
    rig.game->Stop();
}

TEST_CASE("Guilds: the guild's storage: level 2 and authority asked, one member at a time, its size by level, its gold kept") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig rig(t);
    {
        Player master = rig.Join("Keeper", 30, 5'000'000);
        Player helper = rig.Join("Helper");
        master.c->Send(proto::GuildCreate{master.keeper, "Vault"});
        Next<proto::GuildInfo>(master);
        Invite(master, helper, true);
        Next<proto::GuildInfo>(helper);
        // Level 1: no storage yet (0x4C4A, "Guild level must be 2 or higher to use guild storage.").
        master.c->Send(proto::StorageOpen{master.keeper, 1});
        CHECK(Next<proto::StorageRefused>(master).code == 0x4C4A);
        rig.Settle();
    }
    // The guild at level 3 (the original's tables, read at the start).
    REQUIRE(t.Db().Query("UPDATE _guild SET lvl = 3 WHERE name = 'Vault'").Ok());
    rig.Restart();
    Player master = rig.Join("Keeper");
    Player helper = rig.Join("Helper");
    // Not in a guild: refused (0x4C0D).
    Player outsider = rig.Join("Outsider");
    outsider.c->Send(proto::StorageOpen{outsider.keeper, 1});
    CHECK(Next<proto::StorageRefused>(outsider).code == 0x4C0D);
    // A member without the storage's bit: refused (0x4C1E).
    helper.c->Send(proto::StorageOpen{helper.keeper, 1});
    CHECK(Next<proto::StorageRefused>(helper).code == 0x4C1E);
    // Not at an NPC that keeps guilds: refused as a gate is.
    master.c->Send(proto::StorageOpen{helper.entity, 1});
    CHECK(Next<proto::GateRefused>(master).status == proto::GateStatus::NoSuchGate);
    // The master opens it: 60 places at level 3 (a page of 30 a level from 2), no gold yet.
    master.c->Send(proto::StorageOpen{master.keeper, 1});
    const auto list = Next<proto::StorageList>(master);
    CHECK(list.guild == 1);
    CHECK(list.size == 60);
    CHECK(list.gold == 0);
    // The helper given the bit; while the master holds it, refused with its name.
    master.c->Send(proto::GuildPermissions{{{std::uint32_t(helper.character), game::kGuildPermStorage}}});
    rig.Settle();
    helper.c->Send(proto::StorageOpen{helper.keeper, 1});
    const auto busy = Next<proto::StorageRefused>(helper);
    CHECK(busy.code == 1);
    CHECK(busy.name == "Keeper");
    // Gold into it.
    master.c->Send(proto::StorageGold{true, 1000, 1});
    const auto stored = Next<proto::StorageGoldChanged>(master);
    CHECK(stored.guild == 1);
    CHECK(stored.gold == 1000);
    CHECK(Next<proto::GoldChanged>(master).gold == 5'000'000 - game::kGuildCreateGold - 1000);
    // Put away: the helper opens it, the gold there.
    master.c->Send(proto::StorageClose{1});
    rig.Settle();
    helper.c->Send(proto::StorageOpen{helper.keeper, 1});
    const auto seen = Next<proto::StorageList>(helper);
    CHECK(seen.guild == 1);
    CHECK(seen.gold == 1000);
    // While the helper uses it: his storage right is not taken (0x4C4C), he is not put out and the guild is not disbanded
    // (0x4C48), as the original keeps its storage's user.
    const auto refusal = [&](GuildAction action) {
        for (int i = 0; i < 20; ++i) {
            const auto answer = Next<proto::GuildAnswer>(master);
            if (answer.action == action && GuildError(answer.error) != GuildError::Ok) return GuildError(answer.error);
        }
        return GuildError::Ok;
    };
    master.c->Send(proto::GuildPermissions{{{std::uint32_t(helper.character), 0}}});
    CHECK(refusal(GuildAction::Permissions) == GuildError::StorageRightInUse);
    master.c->Send(proto::GuildKick{"Helper"});
    CHECK(refusal(GuildAction::Kick) == GuildError::StorageInUse);
    master.c->Send(proto::GuildDisband{master.keeper});
    CHECK(refusal(GuildAction::Disband) == GuildError::StorageInUse);
    // Kept in the guild's row.
    for (int i = 0; i < 50 && t.Db().Query("SELECT gold FROM _guild WHERE name = 'Vault'").Int(0, 0) != 1000; ++i) rig.pump();
    CHECK(t.Db().Query("SELECT gold FROM _guild WHERE name = 'Vault'").Int(0, 0) == 1000);
    // The size by level.
    CHECK(game::GuildStorageSize(1) == 0);
    CHECK(game::GuildStorageSize(2) == 30);
    CHECK(game::GuildStorageSize(4) == 90);
    CHECK(game::GuildStorageSize(5) == 120);
}

namespace {

constexpr std::uint32_t kCrestUser = 9268, kCrestDelete = 9269, kUnionCrestUser = 9270;

// The emblem items as itemdata has them (9268-9271, TID 3/3/12/2).
std::shared_ptr<const assets::ItemTable> CrestItems() {
    auto items = std::make_shared<assets::ItemTable>();
    for (auto [id, code] : {std::pair<std::uint32_t, const char*>{9268, "ITEM_ETC_GUILD_CREST_USER"}, {9269, "ITEM_ETC_GUILD_CREST_DELETE"},
                            {9270, "ITEM_ETC_UNION_CREST_USER"}, {9271, "ITEM_ETC_UNION_CREST_DELETE"}}) {
        assets::ItemRow row;
        row.refObjId = id;
        row.codename = code;
        row.typeId = {3, 3, 12, 2};
        row.inService = true;
        items->Add(row);
    }
    return items;
}

std::vector<std::uint8_t> TestCrest(std::uint8_t seed) {
    std::vector<std::uint8_t> pixels(proto::kCrestBytes);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = std::uint8_t(i * 7 + seed);
    return pixels;
}

// The bag place a character keeps an item in (the first; -1: none).
int PlaceOf(TestDatabase& t, std::int64_t character, std::uint32_t refId) {
    const server::QueryResult r = t.Db().Query("SELECT i.slot FROM _inventory i JOIN _items it ON it.id64 = i.itemid "
                                               "WHERE i.charid = $1 AND it.refitemid = $2 ORDER BY i.slot LIMIT 1",
                                               {character, std::int64_t(refId)});
    return r.Rows() > 0 ? int(r.Int(0, 0)) : -1;
}

void Give(TestDatabase& t, Rig& rig, const std::string& name, const std::vector<std::pair<std::uint32_t, int>>& items) {
    std::vector<server::StoredItem> stored;
    for (auto [id, count] : items) {
        server::StoredItem s;
        s.refId = id;
        s.quantity = std::uint16_t(count);
        s.counted = true;
        stored.push_back(s);
    }
    REQUIRE(server::GiveItems(t.Db(), rig.config.id, name, stored, 0, rig.error) == server::GiveResult::Done);
}

// Stops the server (its players gone), runs `between`, starts it again.
void Restart(Rig& rig, TestDatabase& t, const std::function<void()>& between) {
    rig.Settle();
    rig.game->Stop();
    server::ReleaseServerSessions(t.Db(), rig.config.id);
    between();
    rig.Start();
}

// The first message of its kind that `wanted` takes (those before it passed over).
template <typename T, typename F>
T Until(Player& p, F&& wanted) {
    T m;
    while (p.c->Expect(m)) {
        if (wanted(m)) return m;
    }
    FAIL("not what was waited for");
    return m;
}

// The answer to `action` (those to other requests passed over).
GuildError ErrorOf(Player& p, GuildAction action) {
    return GuildError(Until<proto::GuildAnswer>(p, [&](const proto::GuildAnswer& a) { return a.action == action; }).error);
}

// `entity`'s guild as `seer` is told it, its emblem at `crest`.
proto::EntityGuild TagOf(Player& seer, std::uint32_t entity, std::uint32_t crest) {
    return Until<proto::EntityGuild>(seer, [&](const proto::EntityGuild& t) { return t.entity == entity && t.crest == crest; });
}
std::uint32_t CrestInfo(Player& p, std::uint32_t crest) {
    return Until<proto::GuildInfo>(p, [&](const proto::GuildInfo& g) { return g.crest == crest; }).crest;
}

} // namespace

TEST_CASE("Guilds: emblems: level 4 and the master asked, the item used up, one at a time, kept, sent to those who ask; a union's by its leading guild") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig rig(t, CrestItems());
    {
        Player herald = rig.Join("Herald", 30, 5'000'000);
        Player squire = rig.Join("Squire");
        Player ensign = rig.Join("Ensign", 30, 5'000'000);
        herald.c->Send(proto::GuildCreate{herald.keeper, "Banner"});
        Next<proto::GuildInfo>(herald);
        Invite(herald, squire, true);
        Next<proto::GuildInfo>(squire);
        ensign.c->Send(proto::GuildCreate{ensign.keeper, "Pennant"});
        Next<proto::GuildInfo>(ensign);
    }
    // Banner at level 4, Pennant at 5; the items in the bags.
    Restart(rig, t, [&] {
        REQUIRE(t.Db().Query("UPDATE _guild SET lvl = 4 WHERE name = 'Banner'").Ok());
        REQUIRE(t.Db().Query("UPDATE _guild SET lvl = 5 WHERE name = 'Pennant'").Ok());
        Give(t, rig, "Herald", {{kCrestUser, 2}, {kCrestDelete, 1}, {kUnionCrestUser, 1}});
        Give(t, rig, "Squire", {{kCrestUser, 1}});
        Give(t, rig, "Ensign", {{kUnionCrestUser, 1}});
    });
    {
        Player herald = rig.Join("Herald");
        Player squire = rig.Join("Squire");
        Player ensign = rig.Join("Ensign");
        const std::uint8_t user = std::uint8_t(PlaceOf(t, herald.character, kCrestUser));
        const std::uint8_t remover = std::uint8_t(PlaceOf(t, herald.character, kCrestDelete));
        const std::uint8_t unionUser = std::uint8_t(PlaceOf(t, herald.character, kUnionCrestUser));
        const auto banner = Next<proto::GuildInfo>(herald);
        CHECK(banner.crest == 0);
        // Not the master (0x4C1E); the wrong size (3); a union's at level 4 (0x4C4A); nothing to
        // delete (0x4C4F); a place without the right item (3).
        squire.c->Send(proto::GuildCrestRegister{proto::kCrestGuild, std::uint8_t(PlaceOf(t, squire.character, kCrestUser)), TestCrest(1)});
        CHECK(Error(squire, GuildAction::CrestRegister) == GuildError::Denied);
        herald.c->Send(proto::GuildCrestRegister{proto::kCrestGuild, user, std::vector<std::uint8_t>(10)});
        CHECK(Error(herald, GuildAction::CrestRegister) == GuildError::Failed);
        herald.c->Send(proto::GuildCrestRegister{proto::kCrestUnion, unionUser, TestCrest(1)});
        CHECK(Error(herald, GuildAction::UnionCrestRegister) == GuildError::LevelTooLow);
        herald.c->Send(proto::GuildCrestDelete{proto::kCrestGuild, remover});
        CHECK(Error(herald, GuildAction::CrestDelete) == GuildError::NoCrest);
        herald.c->Send(proto::GuildCrestRegister{proto::kCrestGuild, remover, TestCrest(1)});
        CHECK(Error(herald, GuildAction::CrestRegister) == GuildError::Failed);
        // Not in a union (0x4C2F).
        ensign.c->Send(proto::GuildCrestRegister{proto::kCrestUnion, std::uint8_t(PlaceOf(t, ensign.character, kUnionCrestUser)), TestCrest(1)});
        CHECK(Error(ensign, GuildAction::UnionCrestRegister) == GuildError::NotInUnion);

        // Registered: one of the two items used, revision 1 to the members and to those who see them.
        herald.c->Send(proto::GuildCrestRegister{proto::kCrestGuild, user, TestCrest(1)});
        const auto used = Next<proto::ItemMoved>(herald);
        REQUIRE(used.changed.size() == 1);
        CHECK(used.changed[0].quantity == 1);
        CHECK(Error(herald, GuildAction::CrestRegister) == GuildError::Ok);
        CHECK(CrestInfo(herald, 1) == 1);
        CHECK(CrestInfo(squire, 1) == 1);
        const auto seen = TagOf(ensign, herald.entity, 1);
        CHECK(seen.guild == banner.id);
        // Its pixels to whoever asks the current revision; nothing for another.
        ensign.c->Send(proto::GuildCrestRequest{proto::kCrestGuild, banner.id, 2});
        ensign.c->Send(proto::GuildCrestRequest{proto::kCrestGuild, banner.id, 1});
        const auto crest = Next<proto::GuildCrest>(ensign);
        CHECK(crest.revision == 1);
        CHECK(crest.pixels == TestCrest(1));
        proto::GuildCrest more;
        CHECK_FALSE(ensign.c->Expect(more, 300ms));
        // One at a time (0x4C4E).
        herald.c->Send(proto::GuildCrestRegister{proto::kCrestGuild, user, TestCrest(2)});
        CHECK(Error(herald, GuildAction::CrestRegister) == GuildError::CrestExists);
        rig.Settle();
        CHECK(t.Db().Query("SELECT curcrestrev FROM _guild WHERE name = 'Banner'").Int(0, 0) == 1);
        CHECK(t.Db().Query("SELECT pixels FROM sr_crests WHERE kind = 0 AND id = $1", {std::int64_t(banner.id)}).Bytes(0, 0) == TestCrest(1));

        // Deleted with its item, then registered again: revision 2.
        herald.c->Send(proto::GuildCrestDelete{proto::kCrestGuild, remover});
        CHECK(Next<proto::ItemMoved>(herald).emptied == std::vector<std::uint8_t>{remover});
        CHECK(Error(herald, GuildAction::CrestDelete) == GuildError::Ok);
        CHECK(CrestInfo(herald, 0) == 0);
        CHECK(TagOf(ensign, herald.entity, 0).guild == banner.id);
        herald.c->Send(proto::GuildCrestRegister{proto::kCrestGuild, user, TestCrest(3)});
        CHECK(Error(herald, GuildAction::CrestRegister) == GuildError::Ok);
        CHECK(CrestInfo(herald, 2) == 2);
        rig.Settle();
        CHECK(t.Db().Query("SELECT lastcrestrev, curcrestrev FROM _guild WHERE name = 'Banner'").Int(0, 1) == 2);
    }
    // Kept across a restart; Banner at level 5 for a union.
    Restart(rig, t, [&] { REQUIRE(t.Db().Query("UPDATE _guild SET lvl = 5 WHERE name = 'Banner'").Ok()); });
    Player herald = rig.Join("Herald");
    Player ensign = rig.Join("Ensign");
    const auto banner = Next<proto::GuildInfo>(herald);
    CHECK(banner.crest == 2);
    CHECK(TagOf(ensign, herald.entity, 2).guild == banner.id);
    ensign.c->Send(proto::GuildCrestRequest{proto::kCrestGuild, banner.id, 2});
    CHECK(Next<proto::GuildCrest>(ensign).pixels == TestCrest(3));

    // A union, Banner leading: the other guild's master may not register its emblem (0x4C1E).
    herald.c->Send(proto::UnionInvite{ensign.entity});
    CHECK(Next<proto::GuildProposal>(ensign).kind == proto::kProposalUnion);
    ensign.c->Send(proto::GuildProposalAnswer{proto::kProposalUnion, 1});
    const auto formed = Next<proto::UnionInfo>(herald);
    REQUIRE(formed.id != 0);
    CHECK(formed.crest == 0);
    CHECK(Error(ensign, GuildAction::UnionJoin) == GuildError::Ok);
    CHECK(Error(herald, GuildAction::UnionInvite) == GuildError::Ok);
    ensign.c->Send(proto::GuildCrestRegister{proto::kCrestUnion, std::uint8_t(PlaceOf(t, ensign.character, kUnionCrestUser)), TestCrest(4)});
    CHECK(Error(ensign, GuildAction::UnionCrestRegister) == GuildError::Denied);
    herald.c->Send(proto::GuildCrestRegister{proto::kCrestUnion, std::uint8_t(PlaceOf(t, herald.character, kUnionCrestUser)), TestCrest(4)});
    CHECK(Error(herald, GuildAction::UnionCrestRegister) == GuildError::Ok);
    CHECK(Until<proto::UnionInfo>(ensign, [](const proto::UnionInfo& u) { return u.crest == 1; }).id == formed.id);
    const auto tag = Until<proto::EntityGuild>(ensign, [&](const proto::EntityGuild& t) { return t.entity == herald.entity && t.unionCrest == 1; });
    CHECK(tag.unionId == formed.id);
    CHECK(tag.crest == 2);
    ensign.c->Send(proto::GuildCrestRequest{proto::kCrestUnion, formed.id, 1});
    CHECK(Next<proto::GuildCrest>(ensign).pixels == TestCrest(4));
    rig.Settle();
    CHECK(t.Db().Query("SELECT curcrestrev FROM _alliedclans WHERE id = $1", {std::int64_t(formed.id)}).Int(0, 0) == 1);
}

TEST_CASE("Guilds: the master's election: called when the master is away 45 days, candidates and voters by their days away, cancelled at its time without 60 %, won at the ballot reaching it") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig rig(t);
    rig.config.guildElectionSeconds = 4;
    {
        Player absent = rig.Join("Absent", 30, 5'000'000);
        Player first = rig.Join("First");
        Player second = rig.Join("Second");
        Player third = rig.Join("Third");
        absent.c->Send(proto::GuildCreate{absent.keeper, "Chair"});
        Next<proto::GuildInfo>(absent);
        Invite(absent, first, true);
        Next<proto::GuildInfo>(first);
        Invite(absent, second, true);
        Next<proto::GuildInfo>(second);
        Invite(absent, third, true);
        Next<proto::GuildInfo>(third);
        // The master here: no election (0x4C33); the master itself may not call one either.
        first.c->Send(proto::GuildElectionStart{first.keeper});
        CHECK(ErrorOf(first, GuildAction::ElectionStart) == GuildError::NoElection);
        absent.c->Send(proto::GuildElectionStart{absent.keeper});
        CHECK(ErrorOf(absent, GuildAction::ElectionStart) == GuildError::NoElection);
        // Away from the NPC: refused as any NPC request is.
        first.c->Send(proto::GuildElectionStart{second.entity});
        CHECK(ErrorOf(first, GuildAction::ElectionStart) == GuildError::Failed);
    }
    // The master away 50 days, Second 5 (a candidate, not a voter), First and Third on yesterday.
    Restart(rig, t, [&] {
        REQUIRE(t.Db().Query("UPDATE _char SET lastlogout = localtimestamp - interval '1 day' WHERE charname16 = 'Third'").Ok());
        REQUIRE(t.Db().Query("UPDATE _char SET lastlogout = localtimestamp - interval '50 days' WHERE charname16 = 'Absent'").Ok());
        REQUIRE(t.Db().Query("UPDATE _char SET lastlogout = localtimestamp - interval '5 days' WHERE charname16 = 'Second'").Ok());
        REQUIRE(t.Db().Query("UPDATE _char SET lastlogout = localtimestamp - interval '1 day' WHERE charname16 = 'First'").Ok());
    });
    Player first = rig.Join("First");
    first.c->Send(proto::GuildElectionStart{first.keeper});
    CHECK(ErrorOf(first, GuildAction::ElectionStart) == GuildError::Ok);
    const auto running = Next<proto::GuildElection>(first);
    CHECK(running.active == 1);
    CHECK(running.mayVote == 1);
    CHECK(running.secondsLeft <= 4);
    REQUIRE(running.candidates.size() == 3);
    CHECK(running.candidates[0].name == "First");
    CHECK(running.candidates[1].name == "Second");
    CHECK(running.candidates[2].name == "Third");
    // One at a time (0x4C37); a voter may not leave (0x4C38).
    first.c->Send(proto::GuildElectionStart{first.keeper});
    CHECK(ErrorOf(first, GuildAction::ElectionStart) == GuildError::ElectionOpen);
    first.c->Send(proto::GuildLeave{first.keeper});
    CHECK(ErrorOf(first, GuildAction::Leave) == GuildError::ElectionNoLeave);
    // Second comes back: away more than three days when it began, no vote (0x5008).
    Player second = rig.Join("Second");
    second.c->Send(proto::GuildElectionVote{second.keeper, std::uint32_t(second.character)});
    CHECK(ErrorOf(second, GuildAction::ElectionVote) == GuildError::NoVotingRight);
    // The window: the candidates, shown.
    first.c->Send(proto::GuildElectionOpen{first.keeper});
    CHECK(Until<proto::GuildElection>(first, [](const proto::GuildElection& e) { return e.show == 1; }).candidates.size() == 3);
    // First's ballot for Second (1 of 3 candidates, 33 %); told to every member.
    first.c->Send(proto::GuildElectionVote{first.keeper, std::uint32_t(second.character)});
    CHECK(ErrorOf(first, GuildAction::ElectionVote) == GuildError::Ok);
    const auto counted = Until<proto::GuildElection>(second, [](const proto::GuildElection& e) { return e.candidates.size() == 3 && e.candidates[1].votes == 1; });
    CHECK(counted.mayVote == 0);
    CHECK(Until<proto::GuildElection>(first, [](const proto::GuildElection& e) { return e.ballot != 0; }).ballot == std::uint32_t(second.character));
    // Time up with nobody at 60 %: cancelled (the shard manager's), the master unchanged.
    const auto cancelled = Next<proto::GuildElectionEnded>(first);
    CHECK(cancelled.guild == "Chair");
    CHECK(cancelled.winner.empty());
    rig.Settle();
    CHECK(t.Db().Query("SELECT memberclass FROM _guildmember WHERE charname = 'Absent'").Int(0, 0) == game::kGuildMaster);
    // Called again: First and Third for Second, 2 of 3 (67 %): won at that ballot, before its time.
    Player third = rig.Join("Third");
    first.c->Send(proto::GuildElectionStart{first.keeper});
    CHECK(ErrorOf(first, GuildAction::ElectionStart) == GuildError::Ok);
    first.c->Send(proto::GuildElectionVote{first.keeper, std::uint32_t(second.character)});
    CHECK(ErrorOf(first, GuildAction::ElectionVote) == GuildError::Ok);
    third.c->Send(proto::GuildElectionVote{third.keeper, std::uint32_t(second.character)});
    CHECK(ErrorOf(third, GuildAction::ElectionVote) == GuildError::Ok);
    const auto ended = Next<proto::GuildElectionEnded>(first);
    CHECK(ended.guild == "Chair");
    CHECK(ended.winner == "Second");
    CHECK(Until<proto::GuildMemberChanged>(second, [&](const proto::GuildMemberChanged& c) { return c.member.name == "Second"; }).member.memberClass == game::kGuildMaster);
    rig.Settle();
    CHECK(t.Db().Query("SELECT memberclass FROM _guildmember WHERE charname = 'Second'").Int(0, 0) == game::kGuildMaster);
    CHECK(t.Db().Query("SELECT memberclass FROM _guildmember WHERE charname = 'Absent'").Int(0, 0) == game::kGuildMember);
    // The master's place left by logging out is written (the rules read it).
    rig.Leave(first);
    rig.Settle();
    for (int i = 0; i < 50 && t.Db().Query("SELECT lastlogout > localtimestamp - interval '1 minute' FROM _char WHERE charname16 = 'First'").Bool(0, 0) == false; ++i) rig.pump();
    CHECK(t.Db().Query("SELECT lastlogout > localtimestamp - interval '1 minute' FROM _char WHERE charname16 = 'First'").Bool(0, 0));
}

TEST_CASE("Guilds: a member's skill points earned by play are its guild's points too, by ten, the rest on leaving") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig rig(t);
    rig.config.devCommands = true;
    rig.Restart();
    Player master = rig.Join("Hunter", 30, 5'000'000);
    Player member = rig.Join("Farmer");
    master.c->Send(proto::GuildCreate{master.keeper, "Harvest"});
    Next<proto::GuildInfo>(master);
    Invite(master, member, true);
    Next<proto::GuildInfo>(member);
    // Nine skill points: kept for the guild until there are ten.
    member.c->Send(proto::DevExperience{0, 9 * 400});
    proto::GuildPoints early;
    CHECK_FALSE(master.c->Expect(early, 500ms));
    // Three more: twelve given at once; the member keeps its skill points.
    member.c->Send(proto::DevExperience{0, 3 * 400 + 150});
    CHECK(Until<proto::GuildPoints>(master, [](const proto::GuildPoints& p) { return p.points > 0; }).points == 12);
    CHECK(Until<proto::GuildMemberChanged>(master, [](const proto::GuildMemberChanged& c) { return c.member.name == "Farmer"; }).member.donation == 12);
    // Five more, then leaving the world: the five given then.
    member.c->Send(proto::DevExperience{0, 5 * 400});
    rig.Leave(member);
    CHECK(Until<proto::GuildPoints>(master, [](const proto::GuildPoints& p) { return p.points > 12; }).points == 17);
    rig.Settle();
    for (int i = 0; i < 50 && t.Db().Query("SELECT remainskillpoint FROM _char WHERE charname16 = 'Farmer'").Int(0, 0) != 17; ++i) rig.pump();
    CHECK(t.Db().Query("SELECT gatheredsp FROM _guild WHERE name = 'Harvest'").Int(0, 0) == 17);
    CHECK(t.Db().Query("SELECT gp_donation FROM _guildmember WHERE charname = 'Farmer'").Int(0, 0) == 17);
    CHECK(t.Db().Query("SELECT remainskillpoint FROM _char WHERE charname16 = 'Farmer'").Int(0, 0) == 17);
}

TEST_CASE("Guilds, a split world: a member changing game server stays online for its guild; one who never arrives is logged off") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    SplitWorld split(t);
    Rig a(t, nullptr, 1, &split);
    Rig b(t, nullptr, 2, &split);
    a.pump = [&] {
        a.game->Poll(1ms);
        b.game->Poll(1ms);
    };
    b.pump = a.pump;
    for (int i = 0; i < 200; ++i) a.pump();
    Player lead = a.Join("Lead", 30, 10'000'000, 10'000);
    Player roam = a.Join("Roam");
    Player gone = a.Join("Gone");
    lead.c->Send(proto::GuildCreate{lead.keeper, "Twin"});
    Next<proto::GuildInfo>(lead);
    Invite(lead, roam, true);
    Next<proto::GuildInfo>(roam);
    Invite(lead, gone, true);
    Next<proto::GuildInfo>(gone);
    // What the guild's master hears of a member until nothing more comes, for longer than a move is
    // waited for (moveWait: 1 s here), so a false "has logged off" would be caught.
    const auto drain = [&](const char* name, bool& wentOffline) {
        proto::GuildMemberChanged c;
        while (lead.c->Expect(c, 1500ms)) {
            if (c.member.name == name && c.member.online == 0) wentOffline = true;
        }
    };
    bool offline = false;
    drain("Roam", offline);
    CHECK_FALSE(offline);

    roam.c->Send(proto::DevGoto{2});
    proto::ServerMove move;
    REQUIRE(roam.c->Expect(move));
    roam.c->Connection().Close();
    roam.c = std::make_unique<Client>(b.net, b.game->Port(), b.pump);
    REQUIRE(roam.c->Greet());
    proto::LobbyEnter e;
    e.ticket = move.ticket;
    roam.c->Send(e);
    proto::LobbyEntered lobby;
    REQUIRE(roam.c->Expect(lobby));
    REQUIRE(lobby.status == proto::LobbyEnterStatus::Accepted);
    const auto info = Next<proto::GuildInfo>(roam);
    CHECK(info.name == "Twin");
    CHECK(std::count_if(info.members.begin(), info.members.end(), [](const auto& m) { return m.name == "Lead" && m.online == 1; }) == 1);
    drain("Roam", offline);
    CHECK_FALSE(offline); // no "has logged off"

    // Guild chat still reaches it, over the bus now.
    lead.c->Send(proto::ChatSend{proto::ChatKind::Guild, 1, "", "still here?"});
    CHECK(Next<proto::ChatLine>(roam).text == "still here?");

    // One that sets off and never arrives (moveWait: 1 s here) has logged off, once.
    gone.c->Send(proto::DevGoto{2});
    REQUIRE(gone.c->Expect(move));
    gone.c->Connection().Close();
    CHECK(Until<proto::GuildMemberChanged>(lead, [](const proto::GuildMemberChanged& c) { return c.member.name == "Gone" && c.member.online == 0; }).member.online == 0);
}

TEST_CASE("Guilds across a shard's two game servers: a member on each, changes, lamps, chat, the storage's lock, a kick (S6)") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig a(t);
    Rig b(t, nullptr, 2);
    // Both servers turn while either's clients wait (the bus's notifications go between them).
    a.pump = [&] {
        a.game->Poll(1ms);
        b.game->Poll(1ms);
    };
    b.pump = a.pump;
    const auto settle = [&] {
        for (int i = 0; i < 30; ++i) a.pump();
        a.Settle();
        b.Settle();
    };
    Player lead = a.Join("Lead", 30, 10'000'000, 10'000);
    {
        Player roam = a.Join("Roam");
        lead.c->Send(proto::GuildCreate{lead.keeper, "Twin"});
        Next<proto::GuildInfo>(lead);
        Invite(lead, roam, true);
        Next<proto::GuildInfo>(roam);
        roam.c->Connection().Close();
    }
    settle();
    // Roam's session on A ended (one account in one place at a time).
    for (int i = 0; i < 500 && t.Db().Query("SELECT count(*) FROM sro_vt_account.sr_online_sessions").Int(0, 0) > 1; ++i) a.pump();
    // Roam plays on B: B loaded the guild again when A changed it.
    Player roam = b.Join("Roam");
    const auto twin = Next<proto::GuildInfo>(roam);
    CHECK(twin.name == "Twin");
    REQUIRE(twin.members.size() == 2);
    // Lead (on A) is online as B tells it, Roam's lamp lit on A.
    CHECK(std::count_if(twin.members.begin(), twin.members.end(), [](const auto& m) { return m.name == "Lead" && m.online == 1; }) == 1);
    CHECK(Until<proto::GuildMemberChanged>(lead, [](const proto::GuildMemberChanged& c) { return c.member.name == "Roam" && c.member.online == 1; }).member.online == 1);
    // Guild chat both ways.
    lead.c->Send(proto::ChatSend{proto::ChatKind::Guild, 1, "", "from A"});
    CHECK(Next<proto::ChatLine>(roam).text == "from A");
    roam.c->Send(proto::ChatSend{proto::ChatKind::Guild, 2, "", "from B"});
    CHECK(Next<proto::ChatLine>(lead).text == "from B");
    // A notice set on A: its line and the guild's data on B.
    lead.c->Send(proto::GuildNoticeEdit{"Plan", "Meet at noon"});
    CHECK(Next<proto::GuildNoticeChanged>(roam).title == "Plan");
    CHECK(Until<proto::GuildInfo>(roam, [](const proto::GuildInfo& g) { return g.noticeTitle == "Plan"; }).notice == "Meet at noon");
    // Points and a level up on A, the storage's right given to Roam: B's rules see them.
    lead.c->Send(proto::GuildDonate{game::kGuildLevelUpPoints[1]});
    CHECK(ErrorOf(lead, GuildAction::Donate) == GuildError::Ok);
    lead.c->Send(proto::GuildLevelUp{lead.keeper});
    CHECK(ErrorOf(lead, GuildAction::LevelUp) == GuildError::Ok);
    lead.c->Send(proto::GuildPermissions{{{std::uint32_t(roam.character), game::kGuildPermStorage}}});
    settle();
    CHECK(Until<proto::GuildInfo>(roam, [](const proto::GuildInfo& g) { return g.level == 2; }).level == 2);
    // The storage: Lead holds it on A, Roam on B is refused with Lead's name; put away with gold in
    // it, B opens it as the database has it.
    lead.c->Send(proto::StorageOpen{lead.keeper, 1});
    CHECK(Next<proto::StorageList>(lead).guild == 1);
    roam.c->Send(proto::StorageOpen{roam.keeper, 1});
    const auto refused = Next<proto::StorageRefused>(roam);
    CHECK(refused.code == 1);
    CHECK(refused.name == "Lead");
    lead.c->Send(proto::StorageGold{true, 1000, 1});
    CHECK(Next<proto::StorageGoldChanged>(lead).gold == 1000);
    lead.c->Send(proto::StorageClose{1});
    settle();
    roam.c->Send(proto::StorageOpen{roam.keeper, 1});
    const auto seen = Next<proto::StorageList>(roam);
    CHECK(seen.guild == 1);
    CHECK(seen.gold == 1000);
    roam.c->Send(proto::StorageClose{1});
    settle();
    // Put out on A: Roam on B told and out of the guild there too (B's rules: no guild, 0x4C0D).
    lead.c->Send(proto::GuildKick{"Roam"});
    CHECK(ErrorOf(lead, GuildAction::Kick) == GuildError::Ok);
    CHECK(Next<proto::GuildMemberRemoved>(roam).name == "Roam");
    CHECK(Until<proto::GuildInfo>(roam, [](const proto::GuildInfo& g) { return g.id == 0; }).id == 0);
    settle();
    roam.c->Send(proto::StorageOpen{roam.keeper, 1});
    CHECK(Next<proto::StorageRefused>(roam).code == 0x4C0D);
}

TEST_CASE("Guilds: wars: the original's refusals, answered, the stakes paid, kills counted by the levels, given up, the reward collected, even at the time's end goes on") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    Rig rig(t);
    rig.config.devCommands = true;
    rig.config.guildWarEndSeconds = 1;
    rig.Restart();
    const auto gold = [&](const char* name) { return t.Db().Query("SELECT remaingold FROM _char WHERE charname16 = $1", {std::string(name)}).Int(0, 0); };
    {
        Player ares = rig.Join("Ares", 30, 600'000'000);
        Player bane = rig.Join("Bane", 30, 10'000'000);
        Player cid = rig.Join("Cid", 30, 1'000);
        ares.c->Send(proto::GuildCreate{ares.keeper, "Iron"});
        Next<proto::GuildInfo>(ares);
        bane.c->Send(proto::GuildCreate{bane.keeper, "Stone"});
        Next<proto::GuildInfo>(bane);
        Invite(ares, cid, true);
        Next<proto::GuildInfo>(cid);
        proto::GuildWarTerms terms;
        terms.unlimited = 1;
        terms.wager = 1'000'000;
        // The original's refusals, in its order.
        ares.c->Send(proto::GuildWarDeclare{"Nobody", terms});
        CHECK(ErrorOf(ares, GuildAction::WarDeclare) == GuildError::NoTargetGuild);
        cid.c->Send(proto::GuildWarDeclare{"Stone", terms});
        CHECK(ErrorOf(cid, GuildAction::WarDeclare) == GuildError::Denied);
        ares.c->Send(proto::GuildWarDeclare{"Iron", terms});
        CHECK(ErrorOf(ares, GuildAction::WarDeclare) == GuildError::Failed); // its own
        proto::GuildWarTerms bad = terms;
        bad.unlimited = 0;
        bad.hours = 24;
        ares.c->Send(proto::GuildWarDeclare{"Stone", bad});
        CHECK(ErrorOf(ares, GuildAction::WarDeclare) == GuildError::Failed);
        proto::GuildWarTerms dear = terms;
        dear.wager = 500'000'001;
        ares.c->Send(proto::GuildWarDeclare{"Stone", dear});
        CHECK(ErrorOf(ares, GuildAction::WarDeclare) == GuildError::WagerLimit);
        dear.wager = 700'000'000;
        ares.c->Send(proto::GuildWarDeclare{"Stone", dear});
        CHECK(ErrorOf(ares, GuildAction::WarDeclare) == GuildError::WarGold);

        // Refused: "[Stone]guild refused to participate in the war." to Ares.
        ares.c->Send(proto::GuildWarDeclare{"stone", terms});
        const auto offer = Next<proto::GuildWarProposal>(bane);
        CHECK(offer.fromName == "Ares");
        CHECK(offer.guildName == "Iron");
        CHECK(offer.terms.wager == 1'000'000);
        bane.c->Send(proto::GuildProposalAnswer{proto::kProposalWar, 0});
        const auto refusal = Next<proto::GuildWarNews>(ares);
        CHECK(refusal.kind == 2);
        CHECK(refusal.a == "Stone");
        CHECK(ErrorOf(ares, GuildAction::WarDeclare) == GuildError::JoinRefused);
        CHECK(ErrorOf(bane, GuildAction::WarAnswer) == GuildError::JoinRefused);

        // Accepted: both stakes paid, the war to all of both guilds.
        ares.c->Send(proto::GuildWarDeclare{"Stone", terms});
        Next<proto::GuildWarProposal>(bane);
        bane.c->Send(proto::GuildProposalAnswer{proto::kProposalWar, 1});
        CHECK(ErrorOf(ares, GuildAction::WarDeclare) == GuildError::Ok);
        CHECK(ErrorOf(bane, GuildAction::WarAnswer) == GuildError::Ok);
        CHECK(Until<proto::GuildWarNews>(cid, [](const proto::GuildWarNews& n) { return n.kind == 1; }).b == "Stone");
        const auto wars = Until<proto::GuildWars>(cid, [](const proto::GuildWars& w) { return !w.wars.empty(); });
        REQUIRE(wars.wars.size() == 1);
        CHECK(wars.wars[0].name == "Stone");
        CHECK(wars.wars[0].master == "Bane");
        CHECK(wars.wars[0].secondsLeft == 0xFFFFFFFFu);
        rig.Settle();
        CHECK(gold("Ares") == 600'000'000 - std::int64_t(game::kGuildCreateGold) - 1'000'000);
        CHECK(gold("Bane") == 10'000'000 - std::int64_t(game::kGuildCreateGold) - 1'000'000);
        CHECK(t.Db().Query("SELECT lodgedgold FROM _guildwar").Int(0, 0) == 2'000'000);
        // At war already (0x4C3A); a guild at war is not disbanded (0x4C46).
        ares.c->Send(proto::GuildWarDeclare{"Stone", terms});
        CHECK(ErrorOf(ares, GuildAction::WarDeclare) == GuildError::AlreadyEnemies);
        bane.c->Send(proto::GuildDisband{bane.keeper});
        CHECK(ErrorOf(bane, GuildAction::Disband) == GuildError::DisbandWar);

        // Cid kills Bane, both level 30: 100 points (15 x 0 + 100), the line to both guilds; the counts kept.
        cid.c->Send(proto::DevStrike{bane.entity, 1'000'000});
        const auto kill = Until<proto::GuildWarNews>(bane, [](const proto::GuildWarNews& n) { return n.kind == 3; });
        CHECK(kill.a == "Cid");
        CHECK(kill.b == "Bane");
        CHECK(kill.c == "Stone");
        CHECK(Until<proto::GuildWars>(ares, [](const proto::GuildWars& w) { return !w.wars.empty() && w.wars[0].ours > 0; }).wars[0].ours == 100);
        // Not an enemy: Cid on its own master, refused.
        cid.c->Send(proto::DevStrike{ares.entity, 10});
        rig.Settle();
        CHECK(t.Db().Query("SELECT guildwarkill, contribution FROM _guildmember WHERE charname = 'Cid'").Int(0, 0) == 1);
        CHECK(t.Db().Query("SELECT contribution FROM _guildmember WHERE charname = 'Cid'").Int(0, 0) == 100);
        CHECK(t.Db().Query("SELECT guildwarkilled FROM _guildmember WHERE charname = 'Bane'").Int(0, 0) == 1);
        CHECK(t.Db().Query("SELECT pointgain1 FROM _guildwar").Int(0, 0) == 100);

        // Stone gives up: "is to close in a minute" (here 1 s), then Iron wins both stakes.
        bane.c->Send(proto::GuildWarEnd{std::uint32_t(t.Db().Query("SELECT id FROM _guild WHERE name = 'Iron'").Int(0, 0))});
        CHECK(ErrorOf(bane, GuildAction::WarEnd) == GuildError::Ok);
        CHECK(Until<proto::GuildWarNews>(ares, [](const proto::GuildWarNews& n) { return n.kind == 4; }).a == "Iron");
        CHECK(Until<proto::GuildWarNews>(ares, [](const proto::GuildWarNews& n) { return n.kind == 6; }).a == "Stone");
        CHECK(Until<proto::GuildWarNews>(bane, [](const proto::GuildWarNews& n) { return n.kind == 7; }).a == "Iron");
        CHECK(Until<proto::GuildWars>(cid, [](const proto::GuildWars& w) { return w.wars.empty(); }).wars.empty());
        rig.Settle();
        CHECK(t.Db().Query("SELECT count(*) FROM _guildwar").Int(0, 0) == 0);
        CHECK(t.Db().Query("SELECT booty FROM _guild WHERE name = 'Iron'").Int(0, 0) == 2'000'000);

        // The reward: asked by a member, taken by the master only, then none.
        cid.c->Send(proto::GuildWarRewardQuery{cid.keeper});
        CHECK(Next<proto::GuildWarReward>(cid).gold == 2'000'000);
        cid.c->Send(proto::GuildWarRewardClaim{cid.keeper});
        CHECK(ErrorOf(cid, GuildAction::WarClaim) == GuildError::Denied);
        ares.c->Send(proto::GuildWarRewardClaim{ares.keeper});
        CHECK(ErrorOf(ares, GuildAction::WarClaim) == GuildError::Ok);
        cid.c->Send(proto::GuildWarRewardQuery{cid.keeper});
        CHECK(ErrorOf(cid, GuildAction::WarReward) == GuildError::NoWarReward);
        rig.Settle();
        // Paid with the booty taken, in one transaction: the guild's row empty, the master's gold up.
        CHECK(t.Db().Query("SELECT booty FROM _guild WHERE name = 'Iron'").Int(0, 0) == 0);
        CHECK(gold("Ares") == 600'000'000 - std::int64_t(game::kGuildCreateGold) - 1'000'000 + 2'000'000);

        // A war of a day, its end passed while the server is down, even: it goes on (the shard manager's timer skips it).
        proto::GuildWarTerms day;
        day.days = 1;
        day.wager = 500'000;
        ares.c->Send(proto::GuildWarDeclare{"Stone", day});
        Next<proto::GuildWarProposal>(bane);
        bane.c->Send(proto::GuildProposalAnswer{proto::kProposalWar, 1});
        CHECK(ErrorOf(bane, GuildAction::WarAnswer) == GuildError::Ok);
        CHECK(Until<proto::GuildWars>(ares, [](const proto::GuildWars& w) { return !w.wars.empty(); }).wars[0].secondsLeft > 86'000u);
        rig.Settle();
    }
    Restart(rig, t, [&] { REQUIRE(t.Db().Query("UPDATE _guildwar SET warendtime = localtimestamp - interval '1 minute'").Ok()); });
    for (int i = 0; i < 50; ++i) rig.pump();
    rig.Settle();
    CHECK(t.Db().Query("SELECT count(*) FROM _guildwar").Int(0, 0) == 1);
    CHECK(t.Db().Query("SELECT booty FROM _guild WHERE name = 'Iron'").Int(0, 0) == 0);
    // Iron ahead (its points as a kill leaves them): over at the next tick, Iron the winner of the whole stake.
    Restart(rig, t, [&] { REQUIRE(t.Db().Query("UPDATE _guildwar SET pointgain1 = 100").Ok()); });
    for (int i = 0; i < 100 && t.Db().Query("SELECT count(*) FROM _guildwar").Int(0, 0) > 0; ++i) rig.pump();
    rig.Settle();
    CHECK(t.Db().Query("SELECT count(*) FROM _guildwar").Int(0, 0) == 0);
    CHECK(t.Db().Query("SELECT booty FROM _guild WHERE name = 'Iron'").Int(0, 0) == 1'000'000);
    CHECK(t.Db().Query("SELECT booty FROM _guild WHERE name = 'Stone'").Int(0, 0) == 0);
}

TEST_CASE("GuildView: the election's lines and window") {
    game::GuildView view;
    proto::GuildElection e;
    e.active = 1;
    e.candidates = {{1, "First", 0}};
    auto lines = view.Apply(Event(e));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].key == "UIIT_MSG_MRELEASE_VOTING");
    CHECK_FALSE(view.TakeElectionShow());
    e.show = 1;
    CHECK(view.Apply(Event(e)).empty()); // running already: no second line
    CHECK(view.TakeElectionShow());
    CHECK_FALSE(view.TakeElectionShow());
    CHECK(view.Election().candidates.size() == 1);
    lines = view.Apply(Event(proto::GuildElectionEnded{"Chair", "First"}));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].key == "UIIT_MSG_MRELEASE_BEELETED");
    CHECK(lines[0].argument == "Chair");
    CHECK(lines[0].second == "First");
    CHECK(view.Election().active == 0);
    CHECK(view.Apply(Event(proto::GuildElectionEnded{"Chair", ""})).front().key == "UIIT_MSG_MRELEASE_BROKEN");
    CHECK(view.Apply(Event(proto::GuildAnswer{GuildAction::MasterTransfer, 0, 0})).front().key == "UIIT_MSG_MLEAVE_SUCCESS");
}

TEST_CASE("GuildView: the guild's wars, their lines, the enemies' red over heads") {
    game::GuildView view;
    proto::GuildWars wars;
    wars.wars.push_back({});
    wars.wars[0].guild = 7;
    wars.wars[0].name = "Ravens";
    view.Apply(Event(wars));
    CHECK(view.AtWarWith(7));
    CHECK_FALSE(view.AtWarWith(8));
    CHECK(view.HeadColour(7, 0) == 0xFFFF0000u);
    CHECK(view.HeadColour(8, 0) == 0xFF66B9FFu);
    const auto lines = view.Apply(Event(proto::GuildWarNews{3, "Cid", "Bane", "Stone"}));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].key == "UIIT_MSG_GUILDWAR_KILLNEWS");
    CHECK(lines[0].argument == "Cid");
    CHECK(lines[0].second == "Bane");
    CHECK(lines[0].third == "Stone");
    CHECK(view.Apply(Event(proto::GuildWarNews{6, "Stone", {}, {}})).front().key == "UIIT_MSG_GUILDWAR_WINERGUILD");
    proto::GuildWarProposal offer{"Ares", "Iron", {}};
    view.Apply(Event(offer));
    REQUIRE(view.WarProposal() != nullptr);
    CHECK(view.WarProposal()->guildName == "Iron");
    view.ClearWarProposal();
    view.Apply(Event(proto::GuildWarReward{2'000'000}));
    CHECK(view.TakeWarReward() == std::optional<std::uint64_t>(2'000'000));
    CHECK_FALSE(view.TakeWarReward().has_value());
    // The terms the original takes: under 8 victories, at most 30 days 23 hours 50 minutes.
    proto::GuildWarTerms t;
    t.days = 30;
    t.hours = 23;
    t.minutes = 50;
    CHECK(game::GuildWarTermsValid(t));
    CHECK(game::GuildWarSeconds(t) == 30 * 86'400 + 23 * 3'600 + 50 * 60);
    t.days = 31;
    CHECK_FALSE(game::GuildWarTermsValid(t));
    t = {};
    CHECK_FALSE(game::GuildWarTermsValid(t)); // nothing to end it
    t.victory = 8;
    CHECK_FALSE(game::GuildWarTermsValid(t));
}

TEST_CASE("Emblems: guildmark.bmp as the 1.188 client reads it, the items, the client's requests and lines") {
    // A 16 x 16 24-bit bitmap, bottom-up: the first row of the file is the picture's last.
    std::vector<std::uint8_t> bmp(54 + proto::kCrestBytes);
    const auto put = [&](std::size_t at, std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) bmp[at + std::size_t(i)] = std::uint8_t(v >> (8 * i));
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put(2, std::uint32_t(bmp.size()), 4);
    put(10, 54, 4);
    put(14, 40, 4);
    put(18, 16, 4);
    put(22, 16, 4);
    put(26, 1, 2);
    put(28, 24, 2);
    for (std::size_t i = 0; i < proto::kCrestBytes; ++i) bmp[54 + i] = std::uint8_t(i / 48); // each row its number
    std::vector<std::uint8_t> pixels;
    REQUIRE(game::ReadCrestBitmap(bmp, pixels) == game::CrestFile::Ok);
    REQUIRE(pixels.size() == proto::kCrestBytes);
    CHECK(pixels[0] == 15);   // the top row: the file's last
    CHECK(pixels[767] == 0);  // the bottom row: the file's first
    auto wrong = bmp;
    put(28, 32, 2);
    std::swap(wrong, bmp);
    CHECK(game::ReadCrestBitmap(wrong, pixels) == game::CrestFile::Format); // 32 bits a pixel
    put(28, 24, 2);
    put(18, 32, 4);
    CHECK(game::ReadCrestBitmap(bmp, pixels) == game::CrestFile::Format); // 32 wide
    put(18, 16, 4);
    bmp[0] = 'X';
    CHECK(game::ReadCrestBitmap(bmp, pixels) == game::CrestFile::Format);
    bmp[0] = 'B';
    CHECK(game::ReadCrestBitmap(std::span(bmp).first(100), pixels) == game::CrestFile::Format); // cut short
    CHECK(game::ReadCrestBitmap(bmp, pixels) == game::CrestFile::Ok);

    CHECK(game::CrestItemOf("ITEM_ETC_UNION_CREST_DELETE")->kind == proto::kCrestUnion);
    CHECK(game::CrestItemOf("ITEM_ETC_UNION_CREST_DELETE")->remove);
    CHECK_FALSE(game::CrestItemOf("ITEM_ETC_GUILD_CREST_USER")->remove);
    CHECK_FALSE(game::CrestItemOf("ITEM_ETC_HP_POTION_01").has_value());

    // The client asks once for an emblem it sees, keeps it when it comes.
    game::GuildView view;
    proto::EntityGuild tag{7, 3, "Banner", "", 9, 2, 1};
    view.Apply(Event(tag));
    view.Apply(Event(tag));
    const auto asked = view.TakeCrestRequests();
    REQUIRE(asked.size() == 2);
    CHECK(asked[0].kind == proto::kCrestGuild);
    CHECK(asked[0].id == 3);
    CHECK(asked[0].revision == 2);
    CHECK(asked[1].kind == proto::kCrestUnion);
    CHECK(asked[1].id == 9);
    CHECK(view.Crest(proto::kCrestGuild, 3, 2) == nullptr);
    view.Apply(Event(proto::GuildCrest{proto::kCrestGuild, 3, 2, TestCrest(5)}));
    REQUIRE(view.Crest(proto::kCrestGuild, 3, 2) != nullptr);
    CHECK(*view.Crest(proto::kCrestGuild, 3, 2) == TestCrest(5));
    CHECK(view.Crest(proto::kCrestGuild, 3, 1) == nullptr);
    // Registered: its line and the four lines of the notice; deleted: its line.
    const auto lines = view.Apply(Event(proto::GuildAnswer{GuildAction::CrestRegister, 0, 0}));
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].key == "UIIT_MSG_GUILD_CREST_REGISTER_COMPLETE");
    CHECK(lines[4].key == "UIIT_MSG_CREST_REGISTER_INFORMATION_04");
    CHECK(view.Apply(Event(proto::GuildAnswer{GuildAction::UnionCrestDelete, 0, 0})).front().key == "UIIT_MSG_UNION_CREST_DELETE_COMPLETE");
    CHECK(view.Apply(Event(proto::GuildAnswer{GuildAction::CrestRegister, 0x4C4E, 0})).empty());
}

TEST_CASE("GuildView: the client's picture of its guild and the lines it prints") {
    game::GuildView view;
    view.SetSelf("Alpha");
    proto::GuildInfo info;
    info.id = 7;
    info.name = "Dragons";
    info.members = {{1, "Alpha", game::kGuildMaster, 30, 0, game::kGuildPermAll, "", 100, 1}, {2, "Beta", game::kGuildMember, 25, 0, 0, "", 100, 0}};
    CHECK(view.Apply(Event(info)).empty());
    CHECK(view.InGuild());
    CHECK(view.Online() == 1);
    CHECK(view.May(game::kGuildPermKick));
    CHECK(view.Master()->name == "Alpha");
    auto lines = view.Apply(Event(proto::GuildMemberChanged{{2, "Beta", game::kGuildMember, 25, 0, 0, "", 100, 1}}));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].key == "UIIT_MSG_GUILD_CONNECT");
    // A title granted (Grant name): the master is told "You have designated [Beta] as [Scout]."
    lines = view.Apply(Event(proto::GuildMemberChanged{{2, "Beta", game::kGuildMember, 25, 0, 0, "Scout", 100, 1}}));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].key == "UIIT_MSG_GUILD_NAME_GRANT_RESULT");
    CHECK(lines[0].argument == "Beta");
    CHECK(lines[0].second == "Scout");
    {
        game::GuildView member;
        member.SetSelf("Beta");
        member.Apply(Event(info));
        CHECK(member.Apply(Event(proto::GuildMemberChanged{{2, "Beta", game::kGuildMember, 25, 0, 0, "Scout", 100, 1}})).empty()); // not the master
    }
    lines = view.Apply(Event(proto::GuildMemberRemoved{2, 2, "Beta"}));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].key == "UIIT_MSG_GUILD_EXPULSION_RESULT");
    CHECK(view.Guild().members.size() == 1);
    CHECK(game::GuildView::HeadLine("Dragons", "Scout") == "[Dragons * Scout]");
    view.Apply(Event(proto::EntityGuild{55, 9, "Wolves", "", 3}));
    CHECK(view.EntityTag(55)->name == "Wolves");
    view.Apply(Event(proto::EntityGuild{55, 0, "", "", 0}));
    CHECK(view.EntityTag(55) == nullptr);
    lines = view.Apply(Event(proto::GuildDisbanded{"Dragons"}));
    CHECK(lines[0].key == "UIIT_MSG_GUILD_BREAK_RESULT");
    CHECK(!view.InGuild());
}

TEST_CASE("Guild window layouts: every res_ui .2dt of the 1.188 files decodes, each parent before its children (game files)") {
    const char* dir = std::getenv("SR_GAME_DIR");
    if (dir == nullptr || *dir == 0) return;
    assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, dir, error), error);
    int files = 0;
    std::size_t records = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = core::NormalizeAssetPath(path);
        if (!key.ends_with(".2dt") || !key.starts_with("res_ui\\")) continue;
        const auto file = fs.Read(key);
        REQUIRE(file.has_value());
        std::vector<assets::Ui2dtRecord> list;
        CHECK_MESSAGE(assets::DecodeUi2dt(file->Bytes(), list, error), key, ": ", error);
        std::set<std::pair<int, int>> made;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& r = list[i];
            if (i > 0 && r.parents[0] != 0) CHECK_MESSAGE(made.contains({r.parents[0], r.parents[1]}), key, " record ", i);
            made.insert({r.id, r.parents[0]});
        }
        ++files;
        records += list.size();
    }
    CHECK(files == 42);
    CHECK(records == 2312);
    // The guild window: its root and its six member rows.
    const auto guild = fs.Read("res_ui\\guild.2dt");
    REQUIRE(guild.has_value());
    std::vector<assets::Ui2dtRecord> list;
    REQUIRE(assets::DecodeUi2dt(guild->Bytes(), list, error));
    CHECK(list.front().className == "CNIFGuildWnd");
    CHECK(list.front().kind == 0);
    CHECK(list.front().rect.w == 621.0f);
    CHECK(std::count_if(list.begin(), list.end(), [](const auto& r) { return r.className == "CNIFGuildUserSlot"; }) == 6);
}
