// SroSkillLab.exe - the skill lab. The game files are SR_GAME_DIR (or --game-dir DIR), the
// database --db URL or SR_DB_URL (where SroWeb's /dev reads).
//
//   --catalogue        the list of every board skill group, with all its data, into sr_lab_skills (replacing it)
//   --items            the list of every item in service, its kind, its data and the path our server takes for it, into
//                      sr_lab_items (replacing it; L12); with --dump FILE an item a line
//   [--build ID]       the build named on what it writes (default: the repository's HEAD commit, read from .git)
//   [--dump FILE]      the list also written as JSON lines (a group a line) to FILE
//   --exam the exam (L3..): our game server in-process on a database of its own (made
//                      through the database's role, CREATEDB, and dropped at the end; --keep-db leaves it), a character of
//                      each mastery, its skills cast one by one, each result into sr_lab_results for the site's /dev
//     [--mastery LIST]   ids or names, commas between ("Spear,Bow", "258"); default every mastery
//     [--race china|europe] [--skill CODE] (that group alone) [--scenarios 1,2,3,4] (1 still, 2 fleeing, 3 special cases, 4 against a player)
//     [--workers N]      N exams at once (1..64), each its own process, game server and database, every Nth group of each
//                        mastery its share; one run on /dev, each worker's log in --log-dir DIR (default lab-logs), the
//                        results summed at the end. (--part K/N --run ID: a worker's own options, given by the parent.)
//   --item-exam the item exam (L13): our game server in-process on a database of its own,
//                      players of each race and sex, each item of the paths it knows given and used or worn through the
//                      protocol, each result into sr_lab_results (the item's codename) for /dev's Items tab (Tested)
//     [--routes LIST]    the paths, commas between (recovery, return-scroll, reverse-return, thief-den, buff-scroll,
//                        equip, avatar, ammunition); default all of them
//     [--item CODE]      that item alone; [--limit N] at most N items of each path; [--keep-db]
//     [--workers N]      N worker processes (1..32) as players on the one game server, each a share of the items, their
//                        logs in --log-dir DIR (default lab-logs). (--part K/N --run ID --worker-port P --pem FILE
//                        --exam-db NAME: a worker's own, given by the exam's process.)

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <regex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "sr/assets/FileSystem.h"
#include "sr/core/Config.h"
#include "sr/core/Log.h"
#include "sr/server/Database.h"
#include "sr/server/ItemExam.h"
#include "sr/server/ItemLab.h"
#include "sr/server/SkillExam.h"
#include "sr/server/SkillLab.h"

namespace {

#ifdef _WIN32
// The parallel exam (--workers N): this program again, N times, each a worker of one run (--part K/N --run ID), its
// output in a log of its own; their results summed as they end.
int RunWorkers(int workers, const std::vector<std::string>& passed, const std::string& resultsUrl, const std::string& build,
               const std::string& filter, const std::string& scenarios, const std::filesystem::path& logDir) {
    using namespace sr;
    std::string error;
    // As many as the database's connections allow: each exam holds about 4 (its results, its own database, its game
    // server's pool and listener); 6 kept for others.
    if (auto db = server::Database::Connect(resultsUrl, error)) {
        const server::QueryResult most = db->Query("SELECT current_setting('max_connections')::int, (SELECT count(*) FROM pg_stat_activity)::int", {});
        if (most.Ok() && most.Rows() == 1) {
            const int room = std::max(1, (int(most.Int(0, 0)) - int(most.Int(0, 1)) - 6) / 4);
            if (workers > room) {
                core::LogWarning("SroSkillLab: {} workers asked, the database has room for {} ({} connections, {} in use): {} it is", workers, room,
                                 most.Int(0, 0), most.Int(0, 1), room);
                workers = room;
            }
        }
    }
    const std::int64_t run = server::NewExamRun(resultsUrl, build, filter, scenarios, error);
    if (run == 0) {
        core::LogError("SroSkillLab: the run: {}", error);
        return 1;
    }
    std::filesystem::create_directories(logDir);
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    struct Worker {
        PROCESS_INFORMATION process{};
        std::filesystem::path log;
        bool done = false;
    };
    std::vector<Worker> list(static_cast<std::size_t>(workers));
    const auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
    for (int k = 0; k < workers; ++k) {
        Worker& w = list[std::size_t(k)];
        w.log = logDir / std::format("exam-{}-{:02}.log", run, k);
        std::wstring line = L"\"" + std::wstring(self) + L"\" --exam";
        for (const std::string& a : passed) line += L" \"" + widen(a) + L"\"";
        line += widen(std::format(" --part {}/{} --run {}", k, workers, run));
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE out = CreateFileW(w.log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW start{};
        start.cb = sizeof(start);
        start.dwFlags = STARTF_USESTDHANDLES;
        start.hStdOutput = out;
        start.hStdError = out;
        start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &w.process)) {
            core::LogError("SroSkillLab: worker {} could not start (error {})", k, GetLastError());
            w.done = true;
        }
        CloseHandle(out);
    }
    core::LogInfo("SroSkillLab: exam run {}: {} workers, their logs in {}", run, workers, logDir.string());
    int passedCount = 0, failedCount = 0, skippedCount = 0, broken = 0;
    std::vector<std::string> failures;
    const std::regex summary(R"((\d+) passed, (\d+) failed, (\d+) skipped)");
    for (int left = int(std::count_if(list.begin(), list.end(), [](const Worker& w) { return !w.done; })); left > 0;) {
        std::vector<HANDLE> handles;
        std::vector<std::size_t> which;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (!list[i].done) {
                handles.push_back(list[i].process.hProcess);
                which.push_back(i);
            }
        }
        const DWORD r = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, INFINITE);
        if (r < WAIT_OBJECT_0 || r >= WAIT_OBJECT_0 + handles.size()) break;
        Worker& w = list[which[r - WAIT_OBJECT_0]];
        w.done = true;
        --left;
        DWORD code = 0;
        GetExitCodeProcess(w.process.hProcess, &code);
        CloseHandle(w.process.hProcess);
        CloseHandle(w.process.hThread);
        // Its counts (its last line) and its failures.
        std::ifstream in(w.log);
        std::string text;
        bool counted = false;
        while (std::getline(in, text)) {
            if (text.find("): fail") != std::string::npos || text.find("): error") != std::string::npos) failures.push_back(text);
            std::smatch m;
            if (text.find("SroSkillLab: exam run") != std::string::npos && std::regex_search(text, m, summary)) {
                passedCount += std::stoi(m[1]), failedCount += std::stoi(m[2]), skippedCount += std::stoi(m[3]);
                counted = true;
            }
        }
        if (code != 0 || !counted) ++broken;
        core::LogInfo("SroSkillLab: worker {} done (exit {}){}; {} left", w.log.filename().string(), code, counted ? "" : ", no result line", left);
    }
    for (const std::string& f : failures) core::LogInfo("{}", f);
    core::LogInfo("SroSkillLab: exam run {}: {} passed, {} failed, {} skipped ({} workers{}; build {})", run, passedCount, failedCount, skippedCount, workers,
                  broken > 0 ? std::format(", {} of them broke off", broken) : std::string(), build);
    return broken > 0 ? 1 : 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    using namespace sr;
    core::Config c;
    std::string error;
    if (!c.ParseCommandLine(argc, argv, error)) {
        core::LogError("SroSkillLab: {}", error);
        return 1;
    }
    if (!c.Has("catalogue") && !c.Has("exam") && !c.Has("items") && !c.Has("item-exam")) {
        core::LogError("SroSkillLab: say what to do: --catalogue, --items, --exam or --item-exam");
        return 1;
    }
    std::string gameDir = c.GetString("game-dir");
    if (gameDir.empty()) {
        if (const char* env = std::getenv("SR_GAME_DIR")) gameDir = env;
    }
    if (gameDir.empty()) {
        core::LogError("SroSkillLab: the game files: SR_GAME_DIR or --game-dir DIR");
        return 1;
    }
    std::string url = c.GetString("db");
    if (url.empty()) {
        if (const char* env = std::getenv("SR_DB_URL")) url = env;
    }
    if (url.empty()) {
        core::LogError("SroSkillLab: the database: --db URL or SR_DB_URL");
        return 1;
    }
    if (c.Has("item-exam")) {
        server::ItemExamOptions o;
        o.resultsUrl = url;
        o.gameDir = gameDir;
        o.build = c.GetString("build", server::BuildCommit());
        o.item = c.GetString("item", "");
        o.limit = std::max(0, std::atoi(c.GetString("limit", "0").c_str()));
        o.keepDatabase = c.Has("keep-db");
        o.workers = std::clamp(std::atoi(c.GetString("workers", "1").c_str()), 1, 32);
        o.logDir = c.GetString("log-dir", "lab-logs");
        const std::string routes = c.GetString("routes", "");
        for (std::size_t at = 0; at < routes.size();) {
            std::size_t end = routes.find(',', at);
            if (end == std::string::npos) end = routes.size();
            if (end > at) o.routes.push_back(routes.substr(at, end - at));
            at = end + 1;
        }
        for (const std::string& r : o.routes) {
            const std::vector<std::string> known = server::ItemExamRoutes();
            if (std::find(known.begin(), known.end(), r) == known.end()) {
                core::LogError("SroSkillLab: the item exam knows no path {} (it knows {})", r, [&] {
                    std::string list;
                    for (const std::string& k : known) list += (list.empty() ? "" : ", ") + k;
                    return list;
                }());
                return 1;
            }
        }
        if (const std::string p = c.GetString("part", ""); !p.empty()) {
            const std::size_t slash = p.find('/');
            o.part = std::atoi(p.substr(0, slash).c_str());
            o.parts = slash != std::string::npos ? std::max(1, std::atoi(p.substr(slash + 1).c_str())) : 1;
        }
        o.run = std::atoll(c.GetString("run", "0").c_str());
        o.port = std::uint16_t(std::atoi(c.GetString("worker-port", "0").c_str()));
        o.pemFile = c.GetString("pem", "");
        o.examDatabase = c.GetString("exam-db", "");
        server::ItemExamReport report;
        if (!server::RunItemExam(o, report, error)) {
            core::LogError("SroSkillLab: the item exam: {}", error);
            return 1;
        }
        core::LogInfo("SroSkillLab: item exam {} {}: {} passed, {} failed, {} skipped (build {})", o.port != 0 ? "worker" : "run",
                      o.port != 0 ? std::to_string(o.part) : std::to_string(report.run), report.passed, report.failed, report.skipped, o.build);
        return 0;
    }
    if (c.Has("exam")) {
        server::ExamOptions o;
        o.resultsUrl = url;
        o.gameDir = gameDir;
        o.build = c.GetString("build", server::BuildCommit());
        o.skill = c.GetString("skill", "");
        o.keepDatabase = c.Has("keep-db");
        const std::string race = c.GetString("race", "");
        o.race = race == "china" ? 0 : race == "europe" ? 1 : -1;
        const auto list = [](const std::string& text) {
            std::vector<std::string> out;
            for (std::size_t at = 0; at <= text.size();) {
                std::size_t end = text.find(',', at);
                if (end == std::string::npos) end = text.size();
                if (end > at) out.push_back(text.substr(at, end - at));
                at = end + 1;
            }
            return out;
        };
        o.masteries = list(c.GetString("mastery", ""));
        if (const std::string s = c.GetString("scenarios", ""); !s.empty()) {
            o.scenarios.clear();
            for (const std::string& n : list(s)) o.scenarios.push_back(std::atoi(n.c_str()));
        }
        if (const std::string p = c.GetString("part", ""); !p.empty()) {
            const std::size_t slash = p.find('/');
            o.part = std::atoi(p.substr(0, slash).c_str());
            o.parts = slash != std::string::npos ? std::max(1, std::atoi(p.substr(slash + 1).c_str())) : 1;
        }
        o.run = std::atoll(c.GetString("run", "0").c_str());
        if (const int workers = std::clamp(std::atoi(c.GetString("workers", "1").c_str()), 1, 64); workers > 1) {
#ifdef _WIN32
            // The workers get this exam's own options again (the database and the game files through the environment).
            std::vector<std::string> passed;
            for (const char* name : {"mastery", "race", "skill", "scenarios", "game-dir", "db"}) {
                if (const std::string v = c.GetString(name, ""); !v.empty()) {
                    passed.push_back(std::format("--{}", name));
                    passed.push_back(v);
                }
            }
            passed.push_back("--build");
            passed.push_back(o.build);
            if (o.keepDatabase) passed.push_back("--keep-db");
            std::string filter = o.skill;
            for (const std::string& m : o.masteries) filter += (filter.empty() ? "" : ",") + m;
            std::string scenarios;
            for (const int s : o.scenarios) scenarios += (scenarios.empty() ? "" : ",") + std::to_string(s);
            return RunWorkers(workers, passed, url, o.build, filter, scenarios, c.GetString("log-dir", "lab-logs"));
#else
            core::LogError("SroSkillLab: --workers runs on Windows only (start the workers with --part K/N --run ID)");
            return 1;
#endif
        }
        server::ExamReport report;
        if (!server::RunExam(o, report, error)) {
            core::LogError("SroSkillLab: the exam: {}", error);
            return 1;
        }
        core::LogInfo("SroSkillLab: exam run {}: {} passed, {} failed, {} skipped (build {})", report.run, report.passed, report.failed, report.skipped, o.build);
        return 0;
    }
    assets::FileSystem files;
    if (!assets::MountGameArchives(files, gameDir, error)) {
        core::LogError("SroSkillLab: {}", error);
        return 1;
    }
    if (c.Has("items")) {
        server::ItemLabData items;
        if (!server::LoadItemLabData(files, items, error)) {
            core::LogError("SroSkillLab: the game data: {}", error);
            return 1;
        }
        const std::vector<server::LabItemRow> rows = server::MakeLabItemRows(items);
        if (const std::string dump = c.GetString("dump"); !dump.empty()) {
            std::ofstream out(dump, std::ios::binary);
            for (const server::LabItemRow& r : rows) out << r.data << '\n';
            core::LogInfo("SroSkillLab: the list written to {}", dump);
        }
        auto db = server::Database::Connect(url, error);
        if (!db) {
            core::LogError("SroSkillLab: {}", error);
            return 1;
        }
        if (!server::MigrateSchema(*db, c.GetString("shard-schema", "sro_vt_shard"), error)) {
            core::LogError("SroSkillLab: the layout: {}", error);
            return 1;
        }
        const std::string build = c.GetString("build", server::BuildCommit());
        if (!server::SaveLabItems(*db, rows, build, error)) {
            core::LogError("SroSkillLab: the items: {}", error);
            return 1;
        }
        std::map<std::string, int> kinds, routes, states;
        for (const server::LabItemRow& r : rows) {
            ++kinds[r.group + "/" + r.kind];
            ++routes[r.route];
            ++states[r.state];
        }
        const auto line = [](const std::map<std::string, int>& counts) {
            std::string s;
            for (const auto& [name, n] : counts) s += std::format("{}{} {}", s.empty() ? "" : ", ", n, name);
            return s;
        };
        core::LogInfo("SroSkillLab: {} items listed (build {})", rows.size(), build);
        core::LogInfo("SroSkillLab: by kind: {}", line(kinds));
        core::LogInfo("SroSkillLab: by route: {}", line(routes));
        core::LogInfo("SroSkillLab: by state: {}", line(states));
        return 0;
    }
    server::LabData data;
    if (!server::LoadLabData(files, data, error)) {
        core::LogError("SroSkillLab: the game data: {}", error);
        return 1;
    }
    const std::vector<server::LabSkillRow> rows = server::MakeLabSkillRows(data);
    if (const std::string dump = c.GetString("dump"); !dump.empty()) {
        std::ofstream out(dump, std::ios::binary);
        for (const server::LabSkillRow& r : rows) out << r.data << '\n';
        core::LogInfo("SroSkillLab: the list written to {}", dump);
    }
    auto db = server::Database::Connect(url, error);
    if (!db) {
        core::LogError("SroSkillLab: {}", error);
        return 1;
    }
    if (!server::MigrateSchema(*db, c.GetString("shard-schema", "sro_vt_shard"), error)) {
        core::LogError("SroSkillLab: the layout: {}", error);
        return 1;
    }
    const std::string build = c.GetString("build", server::BuildCommit());
    if (!server::SaveLabSkills(*db, rows, build, error)) {
        core::LogError("SroSkillLab: the list: {}", error);
        return 1;
    }
    std::map<std::string, int> routes;
    for (const server::LabSkillRow& r : rows) ++routes[r.route];
    std::string counts;
    for (const auto& [route, n] : routes) counts += std::format("{}{} {}", counts.empty() ? "" : ", ", n, route);
    core::LogInfo("SroSkillLab: {} skill groups listed (build {}): {}", rows.size(), build, counts);
    return 0;
}
