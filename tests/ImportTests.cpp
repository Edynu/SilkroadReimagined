// The original server's database in ours (phases D1-D4): the 1.188 export
// (SR_VSRO_EXPORT; skipped without it or without SR_DB_URL) imported whole, then
// its account logging in with the original's password and its character entering the world as the
// original's tables keep it.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

#include "ServerTestSupport.h"
#include "sr/core/Jobs.h"
#include "sr/core/Text.h"
#include "sr/game/CharacterRules.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/server/Crypto.h"
#include "sr/server/DatabaseText.h"
#include "sr/assets/Shop.h"
#include "sr/assets/Gacha.h"
#include "sr/server/Drops.h"
#include "sr/server/GameServer.h"
#include "sr/server/Import.h"
#include "sr/server/LoginServer.h"
#include "sr/server/ReferenceData.h"
#include "sr/server/Store.h"

using namespace std::chrono_literals;
using namespace sr;
using namespace sr::test;
namespace proto = sr::game::proto;

namespace {

// A records text's lines (comments left out), sorted: two sources agree when these are equal.
std::vector<std::string> SortedRecords(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream all;
    all << in.rdbuf();
    return all.str();
}

} // namespace

TEST_CASE("Reference data (D6): drops and nests from the database are the generators' records") {
    // The generators' output for the same export: SR_REFERENCE_DIR holds drops_py.tsv and
    // nests_py.tsv (python tools/scripts/monster_drops.py <dir>/drops_py.tsv, monster_nests.py ...).
    const char* reference = std::getenv("SR_REFERENCE_DIR");
    const char* gameDir = std::getenv("SR_GAME_DIR");
    const std::filesystem::path dir = ExportDir();
    if (!reference || !gameDir || !std::filesystem::exists(dir / "SRO_VT_SHARD" / "schema.tsv")) {
        MESSAGE("SR_REFERENCE_DIR, SR_GAME_DIR or SR_VSRO_EXPORT missing - skipped");
        return;
    }
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::ImportOptions options;
    options.dir = dir;
    options.databases = {"SRO_VT_SHARD"};
    options.replace = true;
    server::ImportReport report;
    REQUIRE_MESSAGE(server::ImportDatabases(t.Db(), options, report, error), error);
    REQUIRE(report.Problems() == 0);
    REQUIRE(server::HasReferenceTables(t.Db()));
    assets::FileSystem fs;
    REQUIRE_MESSAGE(assets::MountGameArchives(fs, gameDir, error), error);
    assets::ItemTable items;
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    REQUIRE_MESSAGE(characters.Load(fs, error), error);

    std::string drops, nests;
    server::ReferenceCounts dropCounts, nestCounts;
    REQUIRE_MESSAGE(server::DropRecords(t.Db(), items, characters, drops, dropCounts, error), error);
    REQUIRE_MESSAGE(server::NestRecords(t.Db(), nests, nestCounts, error), error);
    MESSAGE(dropCounts.records << " drop records, " << nestCounts.records << " nests");
    const std::vector<std::string> ours = SortedRecords(drops), theirs = SortedRecords(ReadAll(std::filesystem::path(reference) / "drops_py.tsv"));
    CHECK(ours.size() == theirs.size());
    std::vector<std::string> onlyOurs, onlyTheirs;
    std::set_difference(ours.begin(), ours.end(), theirs.begin(), theirs.end(), std::back_inserter(onlyOurs));
    std::set_difference(theirs.begin(), theirs.end(), ours.begin(), ours.end(), std::back_inserter(onlyTheirs));
    for (std::size_t i = 0; i < std::min<std::size_t>(onlyOurs.size(), 5); ++i) MESSAGE("only from the database: " << onlyOurs[i]);
    for (std::size_t i = 0; i < std::min<std::size_t>(onlyTheirs.size(), 5); ++i) MESSAGE("only from the generator: " << onlyTheirs[i]);
    CHECK(onlyOurs.empty());
    CHECK(onlyTheirs.empty());
    // The generator's twelve columns (ours go on with the nest's id, its hive and the hive's cap); the instance worlds' own points
    // ("!" lines, ours: no shared monster there) are the generator's rows we leave out.
    std::string nestsAsGenerated;
    std::vector<std::string> instanceOnly;
    for (std::size_t at = 0; at < nests.size();) {
        const std::size_t end = std::min(nests.find('\n', at), nests.size());
        const std::string line = nests.substr(at, end - at);
        if (line.starts_with("!")) {
            instanceOnly.push_back(line.substr(1) + "\t");
            at = end + 1;
            continue;
        }
        std::size_t cut = 0;
        for (int tabs = 0; cut != std::string::npos && tabs < 12; ++tabs) cut = line.find('\t', cut == 0 ? 0 : cut + 1);
        nestsAsGenerated += (line.starts_with("#") || cut == std::string::npos ? line : line.substr(0, cut)) + "\n";
        at = end + 1;
    }
    std::vector<std::string> generated = SortedRecords(ReadAll(std::filesystem::path(reference) / "nests_py.tsv"));
    std::erase_if(generated, [&](const std::string& row) {
        return std::any_of(instanceOnly.begin(), instanceOnly.end(), [&](const std::string& key) { return row.starts_with(key); });
    });
    CHECK(!instanceOnly.empty());
    CHECK(SortedRecords(nestsAsGenerated) == generated);
    // And the server's table takes them.
    server::DropTable table;
    CHECK_MESSAGE(table.LoadText(drops, items, error), error);
    CHECK_FALSE(table.Empty());
}

TEST_CASE("Reference data (D6): the client's tables rebuilt from the database load as the client's do") {
    const char* gameDir = std::getenv("SR_GAME_DIR");
    const std::filesystem::path dir = ExportDir();
    if (!gameDir || !std::filesystem::exists(dir / "SRO_VT_SHARD" / "schema.tsv")) {
        MESSAGE("SR_GAME_DIR or SR_VSRO_EXPORT missing - skipped");
        return;
    }
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;
    server::ImportOptions options;
    options.dir = dir;
    options.databases = {"SRO_VT_SHARD"};
    options.replace = true;
    server::ImportReport report;
    REQUIRE_MESSAGE(server::ImportDatabases(t.Db(), options, report, error), error);
    REQUIRE(report.Problems() == 0);

    assets::FileSystem client, database;
    REQUIRE_MESSAGE(assets::MountGameArchives(client, gameDir, error), error);
    REQUIRE_MESSAGE(assets::MountGameArchives(database, gameDir, error), error);
    server::DatabaseTextCounts counts;
    auto text = server::MakeDatabaseText(t.Db(), database, counts, error);
    REQUIRE_MESSAGE(text, error);
    database.Mount(std::move(text), assets::FileSystem::kOverridePriority);
    MESSAGE(counts.files << " files, " << counts.rows << " rows, " << counts.changed << " values differ, " << counts.databaseOnly
                         << " database rows the client lacks, " << counts.clientOnly << " client rows the database lacks");
    for (const auto& [column, n] : counts.byColumn) MESSAGE(column << ": " << n);
    // Measured on the 1.188 export: the database differs from the client only in skill values, all 203 of them in the
    // client's plain skill shards, an older copy (its encrypted shards, the ones the 1.188 client reads, equal _RefSkill
    // row for row); the database's taken. The client's drawing paths and the export's
    // lost text are not taken. Later content stays out.
    CHECK(counts.changed == 203);
    for (const std::string& d : counts.samples) CHECK(d.starts_with("skilldata "));
    CHECK(counts.databaseOnly > 10000);
    CHECK(counts.clientOnly <= 10); // the 3 ITEM_PRE_MALL_PET2_SYS_* the database lacks, and the like
    CHECK(counts.rows > 50000);

    const auto same = [&](auto load) {
        auto a = load(client), b = load(database);
        CHECK(a == b);
    };
    same([&](const assets::FileSystem& fs) {
        assets::ItemTable items;
        REQUIRE_MESSAGE(items.Load(fs, error), error);
        return items.size();
    });
    same([&](const assets::FileSystem& fs) {
        assets::CharacterTable characters;
        REQUIRE_MESSAGE(characters.Load(fs, error), error);
        return characters.size();
    });
    // The skills: the same from both (the socket stones' from the encrypted shards, rebuilt from the database).
    {
        assets::SkillTable a, b;
        REQUIRE_MESSAGE(a.Load(client, error), error);
        REQUIRE_MESSAGE(b.Load(database, error), error);
        CHECK(b.size() == a.size());
        REQUIRE(b.Find(std::uint32_t(31208)) != nullptr);
        CHECK(b.Find(std::uint32_t(31208))->codename == "SKILL_ITEM_ETC_SOCKET_STONE_GOLD_E_03");
        CHECK(b.Find(std::uint32_t(31232))->activity == 1);
    }
    // The item mall's catalogue: the server's, from the database, the client's good for good.
    {
        assets::MallTable a, b;
        REQUIRE_MESSAGE(a.Load(client, error), error);
        REQUIRE_MESSAGE(b.Load(database, error), error);
        CHECK(a.Goods() == 235);
        CHECK(b.Goods() == a.Goods());
        REQUIRE(b.shops().size() == a.shops().size());
        for (std::size_t s = 0; s < a.shops().size(); ++s) {
            REQUIRE(b.shops()[s].tabs.size() == a.shops()[s].tabs.size());
            for (std::size_t k = 0; k < a.shops()[s].tabs.size(); ++k) {
                const assets::MallTab &x = a.shops()[s].tabs[k], &y = b.shops()[s].tabs[k];
                CHECK(x.id == y.id);
                REQUIRE(x.goods.size() == y.goods.size());
                for (std::size_t g = 0; g < x.goods.size(); ++g) {
                    const assets::MallGood &u = x.goods[g], &v = y.goods[g];
                    CHECK_MESSAGE((u.slot == v.slot && u.packageId == v.packageId && u.price == v.price && u.itemCodename == v.itemCodename &&
                                   u.data == v.data && u.saleTag == v.saleTag),
                                  u.package);
                }
            }
        }
    }
    // Magic POP's sets and machine: the database's rows the client's.
    {
        assets::GachaTable a, b;
        REQUIRE_MESSAGE(a.Load(client, error), error);
        REQUIRE_MESSAGE(b.Load(database, error, false), error);
        REQUIRE(b.entries().size() == a.entries().size());
        for (std::size_t k = 0; k < a.entries().size(); ++k) {
            const assets::GachaEntry &x = a.entries()[k], &y = b.entries()[k];
            CHECK((x.id == y.id && x.item == y.item && x.ratio == y.ratio && x.count == y.count && x.set == y.set && x.options == y.options));
        }
        CHECK(b.machines().size() == 1);
    }
    // Each item, as the server reads it, the same from both.
    assets::ItemTable fromClient, fromDatabase;
    REQUIRE(fromClient.Load(client, error));
    REQUIRE(fromDatabase.Load(database, error));
    int differ = 0;
    for (const auto& [code, row] : fromClient.rows()) {
        const assets::ItemRow* other = fromDatabase.Find(std::string_view(code));
        if (other == nullptr || other->refObjId != row.refObjId || other->requiredLevel != row.requiredLevel || other->nameKey != row.nameKey ||
            other->maxStack != row.maxStack || other->typeId != row.typeId) {
            ++differ;
        }
    }
    CHECK(differ == 0);

    // The drops' blues: the magic options and the lists by kind the
    // server reads (the database's) are the client's, row for row.
    const auto magicOptions = [&](const assets::FileSystem& fs) {
        std::vector<assets::MagicOptionRow> rows;
        const auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoption.txt");
        REQUIRE(file);
        REQUIRE(assets::DecodeMagicOptions(core::DecodeTextFile(file->Bytes()), rows, error));
        return rows;
    };
    const auto blueLists = [&](const assets::FileSystem& fs) {
        std::vector<assets::MagicOptionAssignRow> rows;
        const auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoptionassign.txt");
        REQUIRE(file);
        REQUIRE(assets::DecodeMagicOptionAssign(core::DecodeTextFile(file->Bytes()), rows, error));
        return rows;
    };
    const std::vector<assets::MagicOptionRow> optionsA = magicOptions(client), optionsB = magicOptions(database);
    REQUIRE(optionsB.size() == optionsA.size());
    for (std::size_t i = 0; i < optionsA.size(); ++i) {
        const assets::MagicOptionRow &x = optionsA[i], &y = optionsB[i];
        CHECK((x.paramId == y.paramId && x.optionName == y.optionName && x.degree == y.degree && x.prob == y.prob && x.attribute == y.attribute &&
               x.rangeWords == y.rangeWords && x.availGroups == y.availGroups && x.reqClasses == y.reqClasses));
    }
    const std::vector<assets::MagicOptionAssignRow> listsA = blueLists(client), listsB = blueLists(database);
    CHECK(listsA.size() == 74);
    REQUIRE(listsB.size() == listsA.size());
    for (std::size_t i = 0; i < listsA.size(); ++i) {
        CHECK((listsA[i].race == listsB[i].race && listsA[i].tid3 == listsB[i].tid3 && listsA[i].tid4 == listsB[i].tid4 &&
               listsA[i].options == listsB[i].options));
    }
    // The drop groups' records: ROC's group 1 of Overlap 1 (all different), its members with RefMagicGroupID 1 and
    // MAGIC_NOR_ROCSET_001's five MagicType 1 rows; the tokens' group 2 of Overlap 0; no MagicType 2 row written.
    assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(database, error), error);
    std::string drops;
    server::ReferenceCounts dropCounts;
    REQUIRE_MESSAGE(server::DropRecords(t.Db(), fromDatabase, characters, drops, dropCounts, error), error);
    const std::vector<std::string> records = SortedRecords(drops);
    const auto has = [&](const std::string& line) { return std::binary_search(records.begin(), records.end(), line); };
    CHECK(has("assigned-group\tMOB_RM_ROC\t1\t1\t3\t3\t1"));
    int rocMembers = 0, magicRows = 0, tokenGroups = 0;
    for (const std::string& line : records) {
        std::vector<std::string> cells;
        for (std::size_t at = 0;;) {
            const std::size_t tab = line.find('\t', at);
            cells.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos) break;
            at = tab + 1;
        }
        if (cells[0] == "group-item" && cells[1] == "1") {
            ++rocMembers;
            CHECK(cells.size() == 5);
            CHECK(cells.back() == "1");
        }
        if (cells[0] == "magic-group") {
            ++magicRows;
            CHECK(cells[1] == "1");
        }
        if (cells[0] == "assigned-group" && cells.size() == 7 && cells[2] == "2") {
            ++tokenGroups;
            CHECK(cells[6] == "0");
        }
    }
    CHECK(rocMembers == 72);
    CHECK(magicRows == 5);
    CHECK(tokenGroups > 0);
    for (const char* row : {"magic-group\t1\t357\t6", "magic-group\t1\t358\t6", "magic-group\t1\t359\t60", "magic-group\t1\t362\t160", "magic-group\t1\t363\t6"}) {
        CHECK_MESSAGE(has(row), row);
    }
    // And the server's table drops ROC's three different set pieces, each with the five options.
    server::DropTable table;
    REQUIRE_MESSAGE(table.LoadText(drops, fromDatabase, error), error);
    table.SetMagicOptions(std::make_shared<const std::vector<assets::MagicOptionRow>>(optionsB), listsB);
    const assets::ObjectRow* roc = characters.Find(std::string_view("MOB_RM_ROC"));
    REQUIRE(roc != nullptr);
    std::mt19937 random(3877);
    std::set<std::uint32_t> pieces;
    int withSet = 0;
    for (const auto& d : table.Roll(*roc, 3, roc->level, server::GameRates{}, random)) {
        const assets::ItemRow* row = fromDatabase.Find(d.item.refId);
        if (row == nullptr || !row->codename.starts_with("ITEM_ROC_")) continue;
        pieces.insert(d.item.refId);
        withSet += d.item.options.size() == 5 && d.item.options[2].id == 359 && d.item.options[2].value == 60 ? 1 : 0;
    }
    CHECK(pieces.size() == 3);
    CHECK(withSet == 3);
}

TEST_CASE("Import: the original's types and defaults") {
    CHECK(server::ImportedType("tinyint", 1, 3, 0) == "smallint");
    CHECK(server::ImportedType("bit", 1, 1, 0) == "smallint");
    CHECK(server::ImportedType("int", 4, 10, 0) == "integer");
    CHECK(server::ImportedType("nvarchar", 100, 0, 0) == "varchar(100)");
    CHECK(server::ImportedType("varchar", -1, 0, 0) == "text");
    CHECK(server::ImportedType("smalldatetime", 4, 16, 0) == "timestamp");
    CHECK(server::ImportedType("numeric", 9, 18, 0) == "numeric(18,0)");
    CHECK(server::ImportedType("image", 16, 0, 0) == "bytea");
    CHECK(server::ImportedType("hierarchyid", 0, 0, 0).empty());
    CHECK(server::ImportedDefault("((0))") == "0");
    CHECK(server::ImportedDefault("((-1))") == "-1");
    CHECK(server::ImportedDefault("('xxx')") == "'xxx'");
    CHECK(server::ImportedDefault("(N'abc')") == "'abc'");
    CHECK(server::ImportedDefault("(getdate())") == "localtimestamp");
    CHECK(server::ImportedDefault("(newid())").empty());
    CHECK(server::ImportedDefault("").empty());
}

TEST_CASE("Import: the 1.188 export loads whole; its account logs in with the original's password, its character enters the world") {
    const std::filesystem::path dir = ExportDir();
    if (!std::filesystem::exists(dir / "SRO_VT_SHARD" / "schema.tsv")) {
        MESSAGE("SR_VSRO_EXPORT not set - skipped");
        return;
    }
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    std::string error;

    // D1: every table of every database, each with all its file's rows. The test database has our
    // layout already (its placeholder rows): the import replaces.
    server::ImportOptions options;
    options.dir = dir;
    options.replace = true;
    server::ImportReport report;
    REQUIRE_MESSAGE(server::ImportDatabases(t.Db(), options, report, error), error);
    std::int64_t rows = 0;
    for (const server::ImportReport::Table& table : report.tables) {
        CAPTURE(table.schema + "." + table.name);
        CHECK_MESSAGE(table.problem.empty(), table.problem);
        CHECK(table.loaded == table.expected);
        rows += table.loaded;
    }
    MESSAGE(report.tables.size() << " tables, " << rows << " rows");
    CHECK(report.tables.size() >= 267); // the account's 50, the log's 7, the shard's 210 (SRO_CERTIFICATION left out)
    CHECK_FALSE(t.Db().Query("SELECT EXISTS (SELECT 1 FROM pg_namespace WHERE nspname = 'sro_certification')").Bool(0, 0));
    CHECK(rows > 190000);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_shard._refobjcommon").Int(0, 0) > 25000);
    REQUIRE_MESSAGE(server::MigrateSchema(t.Db(), error), error); // our layout on top again: a no-op but the indexes

    // A second import without --replace is refused, table by table, and changes nothing.
    server::ImportReport again;
    options.replace = false;
    options.databases = {"SRO_VT_SHARD"};
    REQUIRE(server::ImportDatabases(t.Db(), options, again, error));
    CHECK(again.Problems() > 100);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_shard._char").Int(0, 0) == 2);

    // D2: the export's account 1 (it holds no account rows: one is made with the original's MD5)
    // logs in through the login server; our hash is stored and the MD5 kept (the default).
    REQUIRE(t.Db()
                .Query("INSERT INTO sro_vt_account.tb_user (jid, struserid, password, sec_primary, sec_content) VALUES (1, 'admin', $1, 1, 1)",
                       {server::LegacyPasswordHash("secret1")})
                .Ok());
    server::GameServerInfo shard;
    shard.id = 1;
    shard.name = "Silkroad";
    shard.host = "127.0.0.1";
    shard.port = 16001;
    shard.capacity = 100;
    REQUIRE(server::ReportGameServer(t.Db(), shard, error));
    core::JobSystem jobs(2);
    {
        server::LoginServer login;
        server::LoginConfig config;
        config.listen = Local();
        config.databaseUrl = t.Url();
        config.jobs = &jobs;
        REQUIRE_MESSAGE(login.Start(config, error), error);
        net::NetService net(1);
        const auto attempt = [&](const std::string& password) {
            Client c(net, login.Port(), [&] { login.Poll(2ms); });
            REQUIRE(c.Greet());
            proto::LoginRequest request;
            request.account = "admin";
            request.password = password;
            request.server = 1;
            c.Send(request);
            proto::LoginResult result;
            REQUIRE(c.Expect(result));
            return result.status;
        };
        CHECK(attempt("wrong!!") == proto::LoginStatus::WrongPassword);
        CHECK(attempt("secret1") == proto::LoginStatus::Accepted);
        for (int i = 0; i < 100; ++i) login.Poll(2ms); // our hash written on a database thread
        bool ok = false;
        const auto account = server::FindAccount(t.Db(), "admin", ok);
        REQUIRE(account);
        CHECK(account->passwordHash.starts_with("$argon2id$"));
        CHECK(account->legacyHash == server::LegacyPasswordHash("secret1"));
        CHECK(attempt("secret1") == proto::LoginStatus::Accepted); // now against ours
        login.Stop();
    }

    // D3: a new character gets what the original's creation gives it from the imported tables: every
    // mastery of its race at level 0, the race's basic skills (punch, the weapons' base attacks), its
    // first quests (a Chinese figure: model 1907).
    {
        server::NewCharacter n;
        n.name = "Novice";
        n.model = 1907;
        n.height = 34;
        n.strength = n.intelligence = 20;
        n.hp = n.mp = 200;
        REQUIRE(server::CreateCharacter(t.Db(), 1, 1, n, 4, error) == server::CreateCharacterResult::Created);
        const auto ids = [&](const std::string& sql) {
            std::set<std::int64_t> out;
            server::QueryResult r = t.Db().Query(sql);
            for (int i = 0; r.Ok() && i < r.Rows(); ++i) out.insert(r.Int(i, 0));
            return out;
        };
        const std::string novice = "(SELECT charid FROM sro_vt_shard._char WHERE charname16 = 'Novice')";
        CHECK(ids("SELECT masteryid FROM sro_vt_shard._charskillmastery WHERE level = 0 AND charid = " + novice) ==
              std::set<std::int64_t>{257, 258, 259, 273, 274, 275, 276});
        CHECK(ids("SELECT skillid FROM sro_vt_shard._charskill WHERE charid = " + novice) == std::set<std::int64_t>{1, 2, 40, 70});
        CHECK(ids("SELECT questid FROM sro_vt_shard._charquest WHERE charid = " + novice).size() ==
              std::size_t(t.Db().Query("SELECT count(*) FROM sro_vt_shard._refchardefault_quest WHERE requiredlevel = 1 AND service = 1 "
                                       "AND race IN (0, 3)").Int(0, 0)));
        bool ok = false;
        REQUIRE(server::ListCharacters(t.Db(), 1, 1, 3600, ok).size() == 2);
        REQUIRE(server::DeleteCharacter(t.Db(), 1, 1, "Novice", 3600) == server::CharacterChange::Done);
        t.Db().Query("UPDATE sro_vt_shard._deletedchar SET deleteddate = localtimestamp - interval '2 hours'");
        CHECK(server::ListCharacters(t.Db(), 1, 1, 3600, ok).size() == 1); // gone, every row of it
        CHECK(ids("SELECT questid FROM sro_vt_shard._charquest WHERE charid NOT IN (SELECT charid FROM sro_vt_shard._char)").empty());
    }

    // D3, D4: its character [GM] Admin lists and enters the world where _char puts it (region
    // 25000, Jangan), wearing what _inventory holds (places 1, 4, 5, 6, 7) with the bag's 13-15.
    server::GameServer game;
    server::GameServerConfig config;
    config.listen = Local();
    config.databaseUrl = t.Url();
    config.rules = std::make_shared<game::CharacterRules>();
    config.reportInterval = 50ms;
    config.jobs = &jobs;
    REQUIRE_MESSAGE(game.Start(config, error), error);
    net::NetService net(1);
    Client c(net, game.Port(), [&] { game.Poll(2ms); });
    REQUIRE(c.Greet());
    proto::LobbyEnter enter;
    enter.ticket = server::IssueTicket(t.Db(), 1, config.id, 30, error);
    c.Send(enter);
    proto::LobbyEntered entered;
    REQUIRE(c.Expect(entered));
    REQUIRE(entered.status == proto::LobbyEnterStatus::Accepted);
    c.Send(proto::CharacterListRequest{});
    proto::CharacterList list;
    REQUIRE(c.Expect(list));
    REQUIRE(list.characters.size() == 1);
    CHECK(list.characters[0].name == "[GM] Admin");
    CHECK(list.characters[0].model == 1924);
    CHECK(list.characters[0].level == 101);
    CHECK(list.characters[0].equipment.size() == 5);
    c.Send(proto::CharacterSelect{"[GM] Admin"});
    proto::LobbyResult r;
    REQUIRE(c.Expect(r));
    REQUIRE(r.status == proto::LobbyStatus::Ok);
    c.Send(proto::WorldEnter{});
    proto::WorldEntered world;
    REQUIRE(c.Expect(world));
    REQUIRE(world.status == proto::WorldStatus::Ok);
    CHECK(world.position.region == 25000);
    CHECK(world.position.x == doctest::Approx(986.0173));
    CHECK(world.position.z == doctest::Approx(1375.869));
    proto::InventoryList inventory;
    REQUIRE(c.Expect(inventory));
    std::set<int> places;
    for (const auto& item : inventory.items) places.insert(item.slot);
    CHECK(places == std::set<int>{1, 4, 5, 6, 7, 13, 14, 15});
    game.Stop();
}
