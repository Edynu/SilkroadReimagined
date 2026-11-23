// The game's logs (server/common GameLog.h): the original's log tables, partitioned by month; a batch written in one go;
// one the database refuses kept in the spill file and written with the next.

#include <doctest/doctest.h>

#include <filesystem>
#include <format>

#include "ServerTestSupport.h"
#include "sr/server/GameLog.h"

namespace server = sr::server;
using sr::test::TestDatabase;

TEST_CASE("Game logs: the original's log tables by month, a batch in one go, a refused one kept in the spill and written later") {
    TestDatabase t;
    SKIP_WITHOUT_DATABASE(t);
    const std::filesystem::path spill = std::filesystem::temp_directory_path() / std::format("sr_gamelog_test_{}.spill", SR_GETPID());
    std::filesystem::remove(spill);
    server::GameLog log(server::LogSchemaOf("sro_vt_shard"), spill);
    CHECK(log.Schema() == "sro_vt_log");
    CHECK(server::LogSchemaOf("sro_vt_shard2") == "sro_vt_shard2_log");
    // The event tables partitioned by month, each with its default partition.
    CHECK(t.Db().Query("SELECT count(*) FROM pg_partitioned_table WHERE partrelid::regclass::text LIKE 'sro_vt_log._log%'").Int(0, 0) == 4);

    log.Char({77, std::uint8_t(server::logs::CharEvent::Login), 0, 0, "25000(1,2,3)", {}});
    server::ItemLogRow bought;
    bought.character = 77;
    bought.refId = 5;
    bought.data = 1;
    bought.operation = std::uint8_t(server::logs::ItemOp::BuyCash);
    bought.serial = 9001;
    bought.to = 13;
    bought.desc = "a\ttab";
    log.Item(bought);
    CHECK(log.Write(t.Db(), log.Take()));
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_log._logeventchar WHERE charid = 77 AND eventid = 4 AND eventpos = '25000(1,2,3)' AND strdesc IS NULL").Int(0, 0) == 1);
    CHECK(t.Db().Query("SELECT strdesc FROM sro_vt_log._logeventitem WHERE serial64 = 9001 AND operation = 35 AND slot_to = 13 AND slot_from = 255").Text(0, 0) == "a\ttab");
    // A mall purchase: its _LogCashItem row too.
    CHECK(t.Db().Query("SELECT cnt FROM sro_vt_log._logcashitem WHERE serial64 = 9001 AND charid = 77").Int(0, 0) == 1);
    // This month's partition made beside the default one.
    CHECK(t.Db().Query("SELECT count(*) FROM pg_inherits WHERE inhparent = 'sro_vt_log._logeventitem'::regclass").Int(0, 0) == 2);
    CHECK(t.Db().Query("SELECT count(*) FROM sro_vt_log._logeventitem_default").Int(0, 0) == 0);

    // Refused (the table away a moment): kept in the spill; written first by the next batch that can.
    REQUIRE(t.Db().Query("ALTER TABLE sro_vt_log._logeventchar RENAME TO _logeventchar_away").Ok());
    log.Char({78, std::uint8_t(server::logs::CharEvent::Logout), 0, 0, {}, "line\nbreak"});
    CHECK_FALSE(log.Write(t.Db(), log.Take()));
    CHECK(log.Spilled());
    REQUIRE(t.Db().Query("ALTER TABLE sro_vt_log._logeventchar_away RENAME TO _logeventchar").Ok());
    log.Char({79, std::uint8_t(server::logs::CharEvent::LevelChanged), 9, 10, {}, {}});
    CHECK(log.Write(t.Db(), log.Take()));
    CHECK_FALSE(log.Spilled());
    CHECK(t.Db().Query("SELECT strdesc FROM sro_vt_log._logeventchar WHERE charid = 78 AND eventid = 6").Text(0, 0) == "line\nbreak");
    CHECK(t.Db().Query("SELECT data2 FROM sro_vt_log._logeventchar WHERE charid = 79 AND eventid = 22 AND data1 = 9").Int(0, 0) == 10);
    std::filesystem::remove(spill);
}
