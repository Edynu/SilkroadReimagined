// SroSetup: a server's one-time setup (docs/SERVER_SETUP.md 5). From the shard's settings file:
//
//   SroSetup.exe --config shard.cfg [--reimport]
//
// 1. The files: data/SRO_VT_SHARD.bak and data/Quest.sct (the setting `data`, default "data"; or `shard-backup` and
//    `quest-script` by name), the vSRO 1.188 client folder (`game`, or SR_GAME_DIR); the backup's items must be the
//    client's (the same version).
// 2. The database (db-host, db-port, db-name, db-user, db-password or SR_DB_PASSWORD; or db / SR_DB_URL): the original's
//    shard tables made in the shard's schema (`shard-schema`, default sro_vt_shard), with the game's content only
//    (ImportBackup: no accounts, characters, items or logs), then our layout on top (the migrations).
// 3. The quests: Quest.sct read as data (never run) into sr_quests.
// 4. The certificate: dev/server.pem and dev/server.key for `tls-name` (`certificate`, `key`), made when missing.
//
// Run again, it changes nothing that is there already but brings the layout up to date; --reimport reads the game's
// data and the quests again (players' accounts and characters stay). Exit 0 when the server is ready to start.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Log.h"
#include "sr/server/Database.h"
#include "sr/server/Import.h"
#include "sr/server/Program.h"
#include "sr/server/QuestStore.h"
#include "sr/server/Service.h"
#include "sr/server/SqlServerBackup.h"

namespace {

using namespace sr;

int Fail(const std::string& what) {
    core::LogError("SroSetup: {}", what);
    core::LogError("SroSetup: not done; see docs/SERVER_SETUP.md");
    return 1;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = line.find('\t', start);
        out.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) return out;
        start = end + 1;
    }
}

// The backup's items in service against the client's: the same ids with the same codenames when both are 1.188.
bool SameVersion(const server::SqlServerBackup& backup, const assets::ItemTable& items, std::string& why) {
    const server::BackupTable* common = backup.Find("_RefObjCommon");
    if (common == nullptr) {
        why = "the backup has no _RefObjCommon: not a vSRO shard database (SRO_VT_SHARD)";
        return false;
    }
    int idAt = -1, codeAt = -1, serviceAt = -1, typeAt = -1;
    for (std::size_t i = 0; i < common->columns.size(); ++i) {
        const std::string& n = common->columns[i].name;
        if (n == "ID") idAt = int(i);
        if (n == "CodeName128") codeAt = int(i);
        if (n == "Service") serviceAt = int(i);
        if (n == "TypeID1") typeAt = int(i);
    }
    if (idAt < 0 || codeAt < 0 || serviceAt < 0 || typeAt < 0) {
        why = "the backup's _RefObjCommon lacks ID, CodeName128, Service or TypeID1";
        return false;
    }
    int checked = 0, same = 0;
    std::string example;
    std::string error;
    const bool read = backup.ReadRows(*common, [&](std::string&& line) {
        const std::vector<std::string> cells = SplitTabs(line);
        if (cells.size() <= std::size_t(std::max({idAt, codeAt, serviceAt, typeAt}))) return;
        if (cells[std::size_t(serviceAt)] != "1" || cells[std::size_t(typeAt)] != "3") return; // items in service
        ++checked;
        const assets::ItemRow* row = items.Find(std::uint32_t(std::strtoul(cells[std::size_t(idAt)].c_str(), nullptr, 10)));
        if (row != nullptr && row->codename == cells[std::size_t(codeAt)]) {
            ++same;
        } else if (example.empty()) {
            example = cells[std::size_t(idAt)] + " " + cells[std::size_t(codeAt)];
        }
    }, error);
    if (!read) {
        why = "the backup's _RefObjCommon cannot be read: " + error;
        return false;
    }
    core::LogInfo("SroSetup: the backup's {} items in service: {} the client has, by id and codename", checked, same);
    // The original's database lists a few rows its client lacks (later content); far more is another version.
    if (checked == 0 || same < checked * 9 / 10) {
        why = std::format("the backup's items are not the client's ({} of {} match; e.g. {}): the backup and the client must both be vSRO 1.188",
                          same, checked, example.empty() ? "none" : example);
        return false;
    }
    return true;
}

bool ImportQuests(const std::string& url, const std::string& shard, const std::filesystem::path& file) {
    std::string error;
    auto db = server::Database::Connect(server::WithSearchPath(url, shard + ", " + server::kAccountSchema), error);
    if (!db) {
        core::LogError("SroSetup: {}", error);
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    server::QuestImportReport report;
    if (bytes.empty() || !server::ImportQuestScript(*db, bytes, report, error)) {
        core::LogError("SroSetup: the quests ({}): {}", file.string(), bytes.empty() ? "cannot be read" : error);
        return false;
    }
    core::LogInfo("SroSetup: quests: {} in the script, {} the game server codes, {} imported ({} the database does not list left out)",
                  report.decoded, report.coded, report.imported, report.unknown);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    server::ProgramOptions options;
    std::string error;
    if (!server::ReadProgramOptions(argc, argv, "SroSetup", 0, options, error)) return Fail(error);
    const core::Config& c = options.config;
    const bool reimport = c.Has("reimport");
    const std::string shard = c.GetString("shard-schema", server::kDefaultShardSchema);

    // 1. The files.
    const std::filesystem::path data = c.GetString("data", "data");
    const std::filesystem::path backupFile = c.GetString("shard-backup", (data / "SRO_VT_SHARD.bak").string());
    const std::filesystem::path questFile = c.GetString("quest-script", (data / "Quest.sct").string());
    std::string game = c.GetString("game");
    if (game.empty()) {
        if (const char* env = std::getenv("SR_GAME_DIR")) game = env;
    }
    std::vector<std::string> missing;
    if (!std::filesystem::exists(backupFile)) missing.push_back(backupFile.string() + " (the vSRO 1.188 server's shard database backup)");
    if (!std::filesystem::exists(questFile)) missing.push_back(questFile.string() + " (the vSRO 1.188 server's Script\\VIETNAM\\Quest.sct)");
    if (game.empty()) {
        missing.push_back("the client folder: set game = <your vSRO 1.188 client folder> in the settings");
    } else if (!std::filesystem::exists(std::filesystem::path(game) / "Media.pk2")) {
        missing.push_back(game + "\\Media.pk2 (game = must name a vSRO 1.188 client folder)");
    }
    if (!missing.empty()) {
        for (const std::string& m : missing) core::LogError("SroSetup: not found: {}", m);
        return Fail("your own original files are missing (docs/SERVER_SETUP.md 1, 2)");
    }
    core::LogInfo("SroSetup: reading {}", backupFile.string());
    server::SqlServerBackup backup;
    if (!backup.Open(backupFile, error)) return Fail(error);
    core::LogInfo("SroSetup: the backup: {} pages, {} tables", backup.Pages(), backup.Tables().size());
    assets::FileSystem fs;
    if (!assets::MountGameArchives(fs, game, error)) return Fail("the client folder: " + error);
    assets::ItemTable items;
    if (!items.Load(fs, error)) return Fail("the client's items: " + error);
    std::string why;
    if (!SameVersion(backup, items, why)) return Fail(why);

    // 2. The database.
    auto db = server::Database::Connect(options.databaseUrl, error);
    if (!db) return Fail("cannot connect to the database: " + error + " (check db-host, db-port, db-name, db-user and the password)");
    const bool there = server::HasImportedShard(*db, shard);
    if (there && !reimport) {
        core::LogInfo("SroSetup: {} holds the game's data already (--reimport reads it again)", shard);
    } else {
        server::BackupImportOptions importOptions;
        importOptions.shardSchema = shard;
        importOptions.replace = reimport;
        server::ImportReport report;
        if (!server::ImportBackup(*db, backup, importOptions, report, error)) return Fail("the game's data: " + error);
        std::int64_t rows = 0;
        int filled = 0;
        for (const server::ImportReport::Table& t : report.tables) {
            rows += t.loaded;
            filled += t.loaded > 0;
            if (!t.problem.empty()) core::LogWarning("SroSetup: {}.{}: {}", t.schema, t.name, t.problem);
        }
        core::LogInfo("SroSetup: the game's data: {} tables made, {} with rows, {} rows", report.tables.size(), filled, rows);
        if (report.Problems() > 0) return Fail(std::format("{} tables were not loaded whole (above)", report.Problems()));
    }
    if (!server::MigrateSchema(*db, shard, error)) return Fail("our layout: " + error);
    core::LogInfo("SroSetup: our layout is on it (account schema version {}, shard version {})", server::SchemaVersion(*db),
                  server::ShardSchemaVersion(*db, shard));

    // 3. The quests.
    bool questsThere = false;
    if (auto check = server::Database::Connect(server::WithSearchPath(options.databaseUrl, shard + ", " + server::kAccountSchema), error)) {
        questsThere = server::HasScriptQuests(*check);
    }
    if (!questsThere || reimport) {
        if (!ImportQuests(options.databaseUrl, shard, questFile)) return Fail("the quests were not imported");
    } else {
        core::LogInfo("SroSetup: the quests are there already");
    }

    // 4. The certificate.
    const std::string certificate = c.GetString("certificate", "dev/server.pem"), key = c.GetString("key", "dev/server.key");
    const bool made = !std::filesystem::exists(certificate);
    if (!server::LoadCertificate(certificate, key, true, c.GetString("tls-name", "localhost"), options.listen.tls, error)) return Fail(error);
    core::LogInfo("SroSetup: the certificate: {} ({}; players need it, the launcher brings it)", certificate, made ? "made" : "there already");

    core::LogInfo("SroSetup: done. Start SroControl and the game servers (docs/SERVER_SETUP.md 6).");
    return 0;
}
