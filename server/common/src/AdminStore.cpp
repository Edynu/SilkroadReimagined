#include "sr/server/AdminStore.h"

#include <algorithm>
#include <format>

#include "sr/server/Database.h"

namespace sr::server {

namespace {

AdminCommand CommandOf(const QueryResult& r, int i) {
    AdminCommand c;
    c.id = r.Int(i, 0);
    c.shard = r.Text(i, 1);
    c.serverId = int(r.Int(i, 2));
    c.kind = r.Text(i, 3);
    c.text = r.Text(i, 4);
    c.createdBy = r.Text(i, 5);
    return c;
}

constexpr const char* kColumns = "id, shard, server_id, kind, text, created_by";

} // namespace

std::optional<AdminCommand> LoadAdminCommand(Database& db, std::int64_t id, std::string_view shard, bool& ok) {
    const QueryResult r = db.Query(std::format("SELECT {} FROM sro_vt_account.sr_admin_commands WHERE id = $1 AND shard = $2", kColumns),
                                   {id, std::string(shard)});
    ok = r.Ok();
    if (!ok || r.Rows() == 0) return std::nullopt;
    return CommandOf(r, 0);
}

std::vector<AdminCommand> RecentAdminCommands(Database& db, std::string_view shard, std::uint16_t server, int seconds, bool& ok) {
    const QueryResult r = db.Query(std::format("SELECT {} FROM sro_vt_account.sr_admin_commands WHERE shard = $1 AND done_at IS NULL "
                                               "AND server_id IN (0, $2) AND created_at > now() - make_interval(secs => $3) ORDER BY id",
                                               kColumns),
                                   {std::string(shard), int(server), seconds});
    ok = r.Ok();
    std::vector<AdminCommand> out;
    for (int i = 0; ok && i < r.Rows(); ++i) out.push_back(CommandOf(r, i));
    return out;
}

bool FinishAdminCommand(Database& db, std::int64_t id, std::uint16_t server, std::string_view outcome) {
    // At most 200 characters of it (whole UTF-8 sequences).
    std::string said(outcome.substr(0, std::min<std::size_t>(outcome.size(), 200)));
    while (!said.empty() && (static_cast<unsigned char>(said.back()) & 0xC0) == 0x80) said.pop_back();
    if (!said.empty() && static_cast<unsigned char>(said.back()) >= 0xC0) said.pop_back();
    return db.Query("UPDATE sro_vt_account.sr_admin_commands SET done_at = coalesce(done_at, now()), done_by = coalesce(done_by, $2::smallint), "
                    "result = concat_ws('; ', nullif(result, ''), $3::text) WHERE id = $1",
                    {id, int(server), std::format("server {}: {}", server, said)})
        .Ok();
}

bool SetSessionCharacter(Database& db, std::int64_t account, std::uint16_t server, std::int64_t character) {
    return db.Query("UPDATE sro_vt_account.sr_online_sessions SET character_id = $3 WHERE jid = $1 AND shard_id = $2",
                    {account, int(server), character != 0 ? Param(character) : Param(nullptr)})
        .Ok();
}

bool AccountSecurity(Database& db, std::int64_t account, std::string& name, int& secPrimary) {
    const QueryResult r = db.Query("SELECT struserid, sec_primary FROM sro_vt_account.tb_user WHERE jid = $1", {account});
    if (!r.Ok() || r.Rows() == 0) return false;
    name = r.Text(0, 0);
    secPrimary = r.IsNull(0, 1) ? 3 : int(r.Int(0, 1));
    return true;
}

} // namespace sr::server
