#pragma once

// The website's commands for the game servers (SroWeb writes them into
// sro_vt_account.sr_admin_commands and NOTIFYs sr_admin_<the shard's schema> with the row's id; each game server
// takes the ones for it, acts and writes its outcome), and the online sessions' character (the site's "Online now").

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sr::server {

class Database;

struct AdminCommand {
    std::int64_t id = 0;
    std::string shard;
    int serverId = 0; // 0: every game server of the shard
    std::string kind; // announce, kick, console
    std::string text;
    std::string createdBy;
};

// The row of that id for this shard (nothing: no such row, or another shard's); `ok` is false when the query failed.
std::optional<AdminCommand> LoadAdminCommand(Database& db, std::int64_t id, std::string_view shard, bool& ok);
// The shard's rows for this server (or every one) not done yet and younger than `seconds`: what came while a
// game server was starting, never older ones.
std::vector<AdminCommand> RecentAdminCommands(Database& db, std::string_view shard, std::uint16_t server, int seconds, bool& ok);
// Done by this server: the first answer's time and server kept, its outcome (cut to 200 characters) added to the
// others' (a command for every server is answered by each).
bool FinishAdminCommand(Database& db, std::int64_t id, std::uint16_t server, std::string_view outcome);

// The character an online session plays in the world (0: none, it is in the lobby).
bool SetSessionCharacter(Database& db, std::int64_t account, std::uint16_t server, std::int64_t character);
// The account's name and the original's security level (tb_user.sec_primary: 1 a game master, 3 a player).
bool AccountSecurity(Database& db, std::int64_t account, std::string& name, int& secPrimary);

} // namespace sr::server
