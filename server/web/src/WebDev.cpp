#include "WebDev.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <regex>

#include "WebCommon.h"
#include "sr/core/Log.h"
#include "sr/server/Crypto.h"
#include "sr/server/WebAdmin.h"

namespace sr::server {

namespace {

using namespace web;

constexpr std::size_t kNoteMax = 8000, kReMax = 40000;
constexpr const char* kKeyPrefix = "srdev_";

std::string Hex(const std::vector<std::uint8_t>& bytes) {
    static const char* const digits = "0123456789abcdef";
    std::string s;
    for (const std::uint8_t b : bytes) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

// A key's account: "Authorization: Bearer srdev_<64 hex>", its hash in sr_web_dev_keys, not disabled.
std::optional<DevWho> KeyWho(DatabasePool& pool, const std::string& header) {
    if (!header.starts_with("Bearer ")) return std::nullopt;
    const std::string token = header.substr(7);
    if (!token.starts_with(kKeyPrefix)) return std::nullopt;
    const std::vector<std::uint8_t> raw = FromHex(token.substr(std::string_view(kKeyPrefix).size()));
    if (raw.size() != 32) return std::nullopt;
    const auto found = OnPool<std::optional<DevWho>>(pool, [hash = Sha256(raw)](Database& db) -> std::optional<DevWho> {
        const QueryResult r = db.Query("UPDATE sro_vt_account.sr_web_dev_keys SET last_used_at = now() WHERE key_hash = $1 AND NOT disabled RETURNING name",
                                       {Param::Bytes(hash)});
        if (!r.Ok() || r.Rows() != 1) return std::nullopt;
        DevWho who;
        who.name = r.Text(0, 0);
        who.role = "key";
        who.key = true;
        return who;
    });
    return found ? *found : std::optional<DevWho>{};
}

json ParseJson(std::string_view text, json fallback = json::object()) {
    json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? fallback : j;
}

class Dev {
public:
    explicit Dev(DevAccess access) : a_(std::move(access)) {}

    void Register(httplib::Server& http) {
        http.Get("/api/admin/dev/skills", [this](const httplib::Request& req, httplib::Response& res) { List(req, res); });
        http.Get(R"(/api/admin/dev/skills/([A-Z0-9_]+))", [this](const httplib::Request& req, httplib::Response& res) { Skill(req, res); });
        http.Post(R"(/api/admin/dev/skills/([A-Z0-9_]+)/notes)", [this](const httplib::Request& req, httplib::Response& res) { AddNote(req, res); });
        http.Delete(R"(/api/admin/dev/notes/(\d+))", [this](const httplib::Request& req, httplib::Response& res) { RemoveNote(req, res); });
        http.Put(R"(/api/admin/dev/skills/([A-Z0-9_]+)/re)", [this](const httplib::Request& req, httplib::Response& res) { PutRe(req, res); });
        http.Get("/api/admin/dev/tags", [this](const httplib::Request& req, httplib::Response& res) { Tags(req, res); });
        http.Put(R"(/api/admin/dev/tags/([a-zA-Z0-9]+))", [this](const httplib::Request& req, httplib::Response& res) { PutTag(req, res); });
        http.Get("/api/admin/dev/summary", [this](const httplib::Request& req, httplib::Response& res) { Summary(req, res); });
        http.Get("/api/admin/dev/runs", [this](const httplib::Request& req, httplib::Response& res) { Runs(req, res); });
        http.Post(R"(/api/admin/dev/skills/([A-Z0-9_]+)/videos)",
                  [this](const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) { AddVideo(req, res, reader); });
        http.Get(R"(/api/admin/dev/videos/(\d+))", [this](const httplib::Request& req, httplib::Response& res) { Video(req, res); });
        // The item lab's list (L12): its notes and RE notes in the skill lab's tables, by codename.
        http.Get("/api/admin/dev/items", [this](const httplib::Request& req, httplib::Response& res) { Items(req, res); });
        http.Get(R"(/api/admin/dev/items/([A-Z0-9_]+))", [this](const httplib::Request& req, httplib::Response& res) { Item(req, res); });
        http.Post(R"(/api/admin/dev/items/([A-Z0-9_]+)/notes)", [this](const httplib::Request& req, httplib::Response& res) { AddNote(req, res, kItems); });
        http.Put(R"(/api/admin/dev/items/([A-Z0-9_]+)/re)", [this](const httplib::Request& req, httplib::Response& res) { PutRe(req, res, kItems); });
        // A path's RE note (every item our server takes that way): sr_lab_re's side 'route', keyed by the route's name.
        http.Put(R"(/api/admin/dev/item-routes/([a-z\-]+)/re)", [this](const httplib::Request& req, httplib::Response& res) { PutRe(req, res, kItemRoutes); });
    }

private:
    // The asker: a key (no session, no second token) or a session.
    std::optional<DevWho> Who(const httplib::Request& req, httplib::Response& res, bool write) {
        if (const std::string auth = req.get_header_value("Authorization"); !auth.empty()) {
            if (auto who = KeyWho(*a_.pool, auth)) return who;
            Refuse(res, 401, "key", "That key is not known or is disabled.");
            return std::nullopt;
        }
        return a_.session(req, res, write);
    }
    void Audit(const DevWho& who, const httplib::Request& req, std::string action, std::string detail) {
        if (a_.audit) a_.audit(who, a_.visitor ? a_.visitor(req) : std::string(), std::move(action), std::move(detail));
    }
    // Which list a note's or an RE note's code is in: the skills', the items' or the items' paths (routes).
    enum LabList : std::uint8_t { kSkills, kItems, kItemRoutes };
    // The skill (item, path) named in the path, if its list holds it (else the 404 sent).
    bool Known(httplib::Response& res, const std::string& code, LabList list = kSkills) {
        const auto found = OnPool<bool>(*a_.pool, [code, list](Database& db) {
            const QueryResult r = db.Query(list == kItems        ? "SELECT 1 FROM sro_vt_account.sr_lab_items WHERE code = $1"
                                           : list == kItemRoutes ? "SELECT 1 FROM sro_vt_account.sr_lab_items WHERE route = $1 LIMIT 1"
                                                                 : "SELECT 1 FROM sro_vt_account.sr_lab_skills WHERE code = $1",
                                           {code});
            return r.Ok() && r.Rows() == 1;
        });
        if (!found) return Refuse(res, 503, "busy", "The server is busy; try again."), false;
        if (!*found) {
            return Refuse(res, 404, "unknown", list == kItems ? "No such item in the list." : list == kItemRoutes ? "No item takes that path." : "No such skill in the list."),
                   false;
        }
        return true;
    }

    void List(const httplib::Request& req, httplib::Response& res) {
        if (!Who(req, res, false)) return;
        const auto rows = OnPool<std::optional<json>>(*a_.pool, [](Database& db) -> std::optional<json> {
            const QueryResult r = db.Query(
                "SELECT s.code, s.group_id, s.race, s.mastery, s.mastery_name, s.board, s.name, s.kinds, s.route, s.motion, s.weapons, s.levels, s.icon, "
                "s.build, extract(epoch FROM s.updated_at)::bigint, "
                "(SELECT json_object_agg(x.k, x.status)::text FROM (SELECT DISTINCT ON (scenario, variant) scenario::text || ':' || variant AS k, status "
                "  FROM sro_vt_account.sr_lab_results r WHERE r.code = s.code AND r.run_id = (SELECT max(q.run_id) FROM sro_vt_account.sr_lab_results q WHERE q.code = r.code AND q.scenario = r.scenario) "
                "  ORDER BY scenario, variant, id DESC) x), "
                "(SELECT n.verdict FROM sro_vt_account.sr_lab_notes n WHERE n.code = s.code AND n.kind = 'verdict' AND NOT n.removed ORDER BY n.id DESC LIMIT 1), "
                "(SELECT count(*) FROM sro_vt_account.sr_lab_notes n WHERE n.code = s.code AND NOT n.removed), "
                "(SELECT e.status FROM sro_vt_account.sr_lab_re e WHERE e.code = s.code AND e.side = 'server'), "
                "(SELECT e.status FROM sro_vt_account.sr_lab_re e WHERE e.code = s.code AND e.side = 'client'), s.files, "
                "(SELECT count(*) FROM sro_vt_account.sr_lab_videos v WHERE v.code = s.code AND NOT v.removed) "
                "FROM sro_vt_account.sr_lab_skills s ORDER BY s.race, s.mastery, s.board, s.code");
            if (!r.Ok()) return std::nullopt;
            json out = json::array();
            for (int i = 0; i < r.Rows(); ++i) {
                out.push_back({{"code", r.Text(i, 0)},     {"group", r.Int(i, 1)},       {"race", r.Int(i, 2)},       {"mastery", r.Int(i, 3)},
                               {"masteryName", r.Text(i, 4)}, {"board", r.Int(i, 5)},    {"name", r.Text(i, 6)},      {"kinds", r.Text(i, 7)},
                               {"route", r.Text(i, 8)},    {"motion", r.Text(i, 9)},     {"weapons", r.Text(i, 10)},  {"levels", r.Int(i, 11)},
                               {"icon", r.Text(i, 12)},    {"build", r.Text(i, 13)},     {"updated", r.Int(i, 14)},
                               {"results", r.IsNull(i, 15) ? json::object() : ParseJson(r.View(i, 15))},
                               {"verdict", r.IsNull(i, 16) ? json(nullptr) : json(r.Text(i, 16))}, {"notes", r.Int(i, 17)},
                               {"re", r.IsNull(i, 18) ? json(nullptr) : json(r.Text(i, 18))},
                               {"reClient", r.IsNull(i, 19) ? json(nullptr) : json(r.Text(i, 19))}, {"files", r.Text(i, 20)},
                               {"videos", r.Int(i, 21)}});
            }
            return out;
        });
        if (!rows || !*rows) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"skills", std::move(**rows)}});
    }

    void Skill(const httplib::Request& req, httplib::Response& res) {
        if (!Who(req, res, false)) return;
        const std::string code = req.matches[1].str();
        const auto found = OnPool<std::optional<json>>(*a_.pool, [code](Database& db) -> std::optional<json> {
            const QueryResult s = db.Query("SELECT data::text, build, extract(epoch FROM updated_at)::bigint FROM sro_vt_account.sr_lab_skills WHERE code = $1", {code});
            if (!s.Ok() || s.Rows() != 1) return std::nullopt;
            json out = {{"skill", ParseJson(s.View(0, 0))}, {"build", s.Text(0, 1)}, {"updated", s.Int(0, 2)}};
            const QueryResult r = db.Query(
                "SELECT DISTINCT ON (scenario, variant) id, run_id, scenario, variant, status, summary, expected::text, observed::text, "
                "extract(epoch FROM at)::bigint, (SELECT build FROM sro_vt_account.sr_lab_runs u WHERE u.id = run_id) "
                "FROM sro_vt_account.sr_lab_results r WHERE code = $1 AND r.run_id = (SELECT max(q.run_id) FROM sro_vt_account.sr_lab_results q WHERE q.code = r.code AND q.scenario = r.scenario) "
                "ORDER BY scenario, variant, id DESC",
                {code});
            json results = json::array();
            for (int i = 0; r.Ok() && i < r.Rows(); ++i) {
                results.push_back({{"id", r.Int(i, 0)}, {"run", r.Int(i, 1)}, {"scenario", r.Int(i, 2)}, {"variant", r.Text(i, 3)}, {"status", r.Text(i, 4)},
                                   {"summary", r.Text(i, 5)}, {"expected", ParseJson(r.View(i, 6))}, {"observed", ParseJson(r.View(i, 7))},
                                   {"at", r.Int(i, 8)}, {"build", r.Text(i, 9)}});
            }
            out["results"] = std::move(results);
            const QueryResult n = db.Query("SELECT id, kind, verdict, scenario, body, author, source, extract(epoch FROM created_at)::bigint, aspect "
                                           "FROM sro_vt_account.sr_lab_notes WHERE code = $1 AND NOT removed ORDER BY id DESC LIMIT 200",
                                           {code});
            json notes = json::array();
            for (int i = 0; n.Ok() && i < n.Rows(); ++i) {
                notes.push_back({{"id", n.Int(i, 0)}, {"kind", n.Text(i, 1)}, {"verdict", n.IsNull(i, 2) ? json(nullptr) : json(n.Text(i, 2))},
                                 {"scenario", n.IsNull(i, 3) ? json(nullptr) : json(n.Int(i, 3))}, {"body", n.Text(i, 4)}, {"author", n.Text(i, 5)},
                                 {"source", n.Text(i, 6)}, {"at", n.Int(i, 7)}, {"aspect", n.Text(i, 8)}});
            }
            out["notes"] = std::move(notes);
            const QueryResult e = db.Query("SELECT status, original, differs, sources, author, extract(epoch FROM updated_at)::bigint, side "
                                           "FROM sro_vt_account.sr_lab_re WHERE code = $1",
                                           {code});
            const QueryResult v = db.Query("SELECT id, label, bytes, build, extract(epoch FROM uploaded_at)::bigint, uploaded_by "
                                           "FROM sro_vt_account.sr_lab_videos WHERE code = $1 AND NOT removed ORDER BY id DESC LIMIT 20",
                                           {code});
            json videos = json::array();
            for (int i = 0; v.Ok() && i < v.Rows(); ++i) {
                videos.push_back({{"id", v.Int(i, 0)}, {"label", v.Text(i, 1)}, {"bytes", v.Int(i, 2)}, {"build", v.Text(i, 3)}, {"at", v.Int(i, 4)},
                                  {"by", v.Text(i, 5)}, {"url", std::format("/api/admin/dev/videos/{}", v.Int(i, 0))}});
            }
            out["videos"] = std::move(videos);
            out["re"] = json(nullptr);
            out["reClient"] = json(nullptr);
            for (int i = 0; e.Ok() && i < e.Rows(); ++i) {
                out[e.Text(i, 6) == "client" ? "reClient" : "re"] = json{{"status", e.Text(i, 0)}, {"original", e.Text(i, 1)}, {"differs", e.Text(i, 2)},
                                                                        {"sources", e.Text(i, 3)}, {"author", e.Text(i, 4)}, {"at", e.Int(i, 5)}};
            }
            return out;
        });
        if (!found) return Refuse(res, 503, "busy", "The server is busy; try again.");
        if (!*found) return Refuse(res, 404, "unknown", "No such skill in the list.");
        Answer(res, 200, **found);
    }

    // A video of the skill (the skill bench's, frame/VideoWriter.h): an MP4 as the body; ?label= its kind (the newest of a
    // label replaces the ones before: their files go), ?build= the client's build.
    void AddVideo(const httplib::Request& req, httplib::Response& res, const httplib::ContentReader& reader) {
        const auto who = Who(req, res, true);
        if (!who) return;
        const std::string code = req.matches[1].str();
        std::string label = req.get_param_value("label");
        if (label.empty()) label = "bench";
        static const std::regex kLabel("[A-Za-z0-9_.-]{1,64}");
        if (!std::regex_match(label, kLabel)) return Refuse(res, 400, "label", "A label is 1 to 64 letters, digits, '_', '-' or '.'.");
        const std::string build = req.get_param_value("build").substr(0, 64);
        const std::string lengthHeader = req.get_header_value("Content-Length");
        const std::int64_t length = lengthHeader.empty() ? -1 : std::atoll(lengthHeader.c_str());
        if (length <= 0) return Refuse(res, 411, "length", "The upload must say its size.");
        if (length > a_.videoMaxBytes) return Refuse(res, 413, "too-big", std::format("A video is at most {} MB.", a_.videoMaxBytes >> 20));
        if (a_.videos.empty()) return Refuse(res, 500, "disk", "No folder for the videos.");
        if (!Known(res, code)) return;
        std::error_code ec;
        std::filesystem::create_directories(a_.videos, ec);
        const std::string stored = Hex(RandomBytes(16)) + ".mp4";
        const std::filesystem::path part = a_.videos / (stored + ".part"), file = a_.videos / stored;
        std::ofstream out(part, std::ios::binary);
        if (!out) return Refuse(res, 500, "disk", "The videos' folder cannot be written.");
        Sha256Stream hash;
        std::int64_t got = 0;
        std::string head;
        const bool read = reader([&](const char* data, std::size_t size) {
            got += std::int64_t(size);
            if (got > a_.videoMaxBytes) return false;
            if (head.size() < 12) head.append(data, std::min<std::size_t>(size, 12 - head.size()));
            out.write(data, std::streamsize(size));
            hash.Add({reinterpret_cast<const std::uint8_t*>(data), size});
            return bool(out);
        });
        out.close();
        const auto fail = [&](int status, const char* what, const std::string& message) {
            std::filesystem::remove(part, ec);
            Refuse(res, status, what, message);
        };
        if (!read || got != length) return fail(400, "incomplete", "The upload did not arrive whole.");
        if (head.size() < 8 || head.compare(4, 4, "ftyp") != 0) return fail(400, "type", "A video is an MP4 file.");
        std::filesystem::rename(part, file, ec);
        if (ec) return fail(500, "disk", "The video could not be stored.");
        const std::string sha = Hex(hash.Finish());
        const std::string by = who->key ? who->Source() : who->name;
        using Stored = std::pair<std::int64_t, std::vector<std::string>>;
        const auto row = OnPool<Stored>(*a_.pool, [=](Database& db) -> Stored {
            const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_lab_videos (code, label, stored, bytes, sha256, build, uploaded_by) "
                                           "VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING id",
                                           {code, label, stored, got, sha, build, by});
            if (!r.Ok() || r.Rows() != 1) return {0, {}};
            const std::int64_t id = r.Int(0, 0);
            std::vector<std::string> gone;
            const QueryResult old = db.Query("UPDATE sro_vt_account.sr_lab_videos SET removed = true "
                                             "WHERE code = $1 AND label = $2 AND id <> $3 AND NOT removed RETURNING stored",
                                             {code, label, id});
            for (int i = 0; old.Ok() && i < old.Rows(); ++i) gone.push_back(old.Text(i, 0));
            return {id, gone};
        });
        if (!row || row->first == 0) {
            std::filesystem::remove(file, ec);
            return Refuse(res, 500, "server", "The video could not be recorded.");
        }
        for (const std::string& old : row->second) std::filesystem::remove(a_.videos / old, ec);
        Audit(*who, req, "lab-video", std::format("{} {} ({} bytes, build {})", code, label, got, build));
        Answer(res, 201, {{"id", row->first}, {"url", std::format("/api/admin/dev/videos/{}", row->first)}, {"bytes", got}, {"sha256", sha}});
    }

    // A video's file (ranges answered: a page seeks in it).
    void Video(const httplib::Request& req, httplib::Response& res) {
        if (!Who(req, res, false)) return;
        const std::int64_t id = std::atoll(req.matches[1].str().c_str());
        const auto stored = OnPool<std::string>(*a_.pool, [id](Database& db) {
            const QueryResult r = db.Query("SELECT stored FROM sro_vt_account.sr_lab_videos WHERE id = $1 AND NOT removed", {id});
            return r.Ok() && r.Rows() == 1 ? r.Text(0, 0) : std::string();
        });
        if (!stored) return Refuse(res, 503, "busy", "The server is busy; try again.");
        std::error_code ec;
        const std::filesystem::path file = a_.videos / *stored;
        if (stored->empty() || !std::filesystem::is_regular_file(file, ec)) return Refuse(res, 404, "unknown", "No such video.");
        res.set_header("Cache-Control", "private, max-age=86400");
        res.set_file_content(file.string(), "video/mp4");
    }

    void AddNote(const httplib::Request& req, httplib::Response& res, LabList list = kSkills) {
        const auto who = Who(req, res, true);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::string code = req.matches[1].str();
        const std::string kind = Field(*body, "kind"), verdict = Field(*body, "verdict"), text = Field(*body, "body");
        std::string aspect = Field(*body, "aspect");
        if (aspect.empty()) aspect = "play";
        if (aspect != "play" && aspect != "animation" && aspect != "effects" && aspect != "sound" && aspect != "client") {
            return Refuse(res, 400, "aspect", "A note is about play, animation, effects, sound or the client.");
        }
        if (kind != "verdict" && kind != "note") return Refuse(res, 400, "kind", "A note's kind is verdict or note.");
        if (kind == "verdict" && verdict != "works" && verdict != "broken" && verdict != "partly") {
            return Refuse(res, 400, "verdict", "A verdict is works, broken or partly.");
        }
        if (kind == "note" && text.empty()) return Refuse(res, 400, "body", "Write something.");
        if (text.size() > kNoteMax) return Refuse(res, 400, "body", "A note is at most 8,000 characters.");
        int scenario = 0;
        if (const auto it = body->find("scenario"); it != body->end() && it->is_number_integer()) scenario = it->get<int>();
        if (scenario < 0 || scenario > 5) return Refuse(res, 400, "scenario", "The scenario is 1 to 5 (or none).");
        if (!Known(res, code, list)) return;
        const auto id = OnPool<std::int64_t>(*a_.pool, [=, author = who->name, source = who->Source()](Database& db) -> std::int64_t {
            const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_lab_notes (code, kind, verdict, scenario, body, author, source, aspect) "
                                           "VALUES ($1, $2, $3, $4, $5, $6, $7, $8) RETURNING id",
                                           {code, kind, kind == "verdict" ? Param(verdict) : Param(nullptr), scenario > 0 ? Param(scenario) : Param(nullptr), text,
                                            author, source, aspect});
            return r.Ok() && r.Rows() == 1 ? r.Int(0, 0) : 0;
        });
        if (!id || *id == 0) return Refuse(res, 503, "busy", "The note was not kept; try again.");
        Audit(*who, req, "lab-note", std::format("{} {} {} {}", code, kind, verdict, aspect));
        Answer(res, 200, {{"id", *id}});
    }

    void RemoveNote(const httplib::Request& req, httplib::Response& res) {
        const auto who = Who(req, res, true);
        if (!who) return;
        const std::int64_t id = std::stoll(req.matches[1].str());
        // Its author's own, or any for an admin.
        const auto removed = OnPool<int>(*a_.pool, [id, author = who->name, admin = who->role == "admin"](Database& db) {
            const QueryResult r = db.Query("UPDATE sro_vt_account.sr_lab_notes SET removed = true WHERE id = $1 AND NOT removed AND ($2 OR author = $3)",
                                           {id, admin ? "true" : "false", author});
            return r.Ok() ? r.Affected() : -1;
        });
        if (!removed || *removed < 0) return Refuse(res, 503, "busy", "The server is busy; try again.");
        if (*removed == 0) return Refuse(res, 404, "unknown", "No such note of yours.");
        Audit(*who, req, "lab-note-remove", std::to_string(id));
        Answer(res, 200, {{"ok", true}});
    }

    void PutRe(const httplib::Request& req, httplib::Response& res, LabList list = kSkills) {
        const auto who = Who(req, res, true);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::string code = req.matches[1].str();
        const std::string status = Field(*body, "status"), original = Field(*body, "original"), differs = Field(*body, "differs"), sources = Field(*body, "sources");
        // The server's (the original's game server against ours) or the client's (its animations, effects and sounds); a
        // path's note is the server's for every item of it (side 'route').
        std::string side = Field(*body, "side");
        if (side.empty()) side = "server";
        if (list == kItemRoutes) side = "route";
        else if (side != "server" && side != "client") return Refuse(res, 400, "side", "The side is server or client.");
        if (status != "same" && status != "differs" && status != "partly" && status != "unknown") {
            return Refuse(res, 400, "status", "The status is same, differs, partly or unknown.");
        }
        if (original.size() + differs.size() + sources.size() > kReMax) return Refuse(res, 400, "body", "At most 40,000 characters together.");
        if (!Known(res, code, list)) return;
        const auto ok = OnPool<bool>(*a_.pool, [=, author = who->name](Database& db) {
            return db.Query("INSERT INTO sro_vt_account.sr_lab_re (code, side, status, original, differs, sources, author, updated_at) "
                            "VALUES ($1, $2, $3, $4, $5, $6, $7, now()) "
                            "ON CONFLICT (code, side) DO UPDATE SET status = EXCLUDED.status, original = EXCLUDED.original, differs = EXCLUDED.differs, "
                            "sources = EXCLUDED.sources, author = EXCLUDED.author, updated_at = now()",
                            {code, side, status, original, differs, sources, author})
                .Ok();
        });
        if (!ok || !*ok) return Refuse(res, 503, "busy", "The note was not kept; try again.");
        Audit(*who, req, "lab-re", code + " " + side + " " + status);
        Answer(res, 200, {{"ok", true}});
    }

    void Tags(const httplib::Request& req, httplib::Response& res) {
        if (!Who(req, res, false)) return;
        const auto rows = OnPool<std::optional<json>>(*a_.pool, [](Database& db) -> std::optional<json> {
            // Every tag the listed skills' ranks carry, with how many groups carry it, and its note.
            const QueryResult r = db.Query(
                "WITH used AS (SELECT t->>'tag' AS tag, count(DISTINCT s.code) AS groups, (array_agg(DISTINCT s.code))[1:12] AS examples "
                "  FROM sro_vt_account.sr_lab_skills s, jsonb_array_elements(s.data->'ranks') k, jsonb_array_elements(k->'tags') t GROUP BY 1) "
                "SELECT coalesce(u.tag, g.tag), coalesce(u.groups, 0), coalesce(array_to_json(u.examples)::text, '[]'), g.meaning, g.args, g.original, g.sources, "
                "g.author, extract(epoch FROM g.updated_at)::bigint "
                "FROM used u FULL JOIN sro_vt_account.sr_lab_tags g ON g.tag = u.tag ORDER BY 2 DESC, 1");
            if (!r.Ok()) return std::nullopt;
            json out = json::array();
            for (int i = 0; i < r.Rows(); ++i) {
                json t = {{"tag", r.Text(i, 0)}, {"groups", r.Int(i, 1)}, {"examples", ParseJson(r.View(i, 2), json::array())}};
                if (!r.IsNull(i, 3)) {
                    t["note"] = {{"meaning", r.Text(i, 3)}, {"args", r.Text(i, 4)}, {"original", r.Text(i, 5)}, {"sources", r.Text(i, 6)},
                                 {"author", r.Text(i, 7)}, {"at", r.Int(i, 8)}};
                }
                out.push_back(std::move(t));
            }
            return out;
        });
        if (!rows || !*rows) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"tags", std::move(**rows)}});
    }

    void PutTag(const httplib::Request& req, httplib::Response& res) {
        const auto who = Who(req, res, true);
        if (!who) return;
        const auto body = Body(req, res);
        if (!body) return;
        const std::string tag = req.matches[1].str();
        if (tag.size() > 4) return Refuse(res, 400, "tag", "A tag is up to four letters.");
        const std::string meaning = Field(*body, "meaning"), args = Field(*body, "args"), original = Field(*body, "original"), sources = Field(*body, "sources");
        if (meaning.empty()) return Refuse(res, 400, "meaning", "Say what the tag does.");
        if (meaning.size() + args.size() + original.size() + sources.size() > kReMax) return Refuse(res, 400, "body", "At most 40,000 characters together.");
        const auto ok = OnPool<bool>(*a_.pool, [=, author = who->name](Database& db) {
            return db.Query("INSERT INTO sro_vt_account.sr_lab_tags (tag, meaning, args, original, sources, author, updated_at) VALUES ($1, $2, $3, $4, $5, $6, now()) "
                            "ON CONFLICT (tag) DO UPDATE SET meaning = EXCLUDED.meaning, args = EXCLUDED.args, original = EXCLUDED.original, "
                            "sources = EXCLUDED.sources, author = EXCLUDED.author, updated_at = now()",
                            {tag, meaning, args, original, sources, author})
                .Ok();
        });
        if (!ok || !*ok) return Refuse(res, 503, "busy", "The note was not kept; try again.");
        Audit(*who, req, "lab-tag", tag);
        Answer(res, 200, {{"ok", true}});
    }

    void Summary(const httplib::Request& req, httplib::Response& res) {
        if (!Who(req, res, false)) return;
        const auto rows = OnPool<std::optional<json>>(*a_.pool, [](Database& db) -> std::optional<json> {
            // By mastery: its groups, its routes, the latest result of each group and scenario by status, the latest verdicts.
            const QueryResult r = db.Query(
                "WITH latest AS (SELECT DISTINCT ON (code, scenario, variant) code, scenario, status FROM sro_vt_account.sr_lab_results r "
                "  WHERE r.run_id = (SELECT max(q.run_id) FROM sro_vt_account.sr_lab_results q WHERE q.code = r.code AND q.scenario = r.scenario) ORDER BY code, scenario, variant, id DESC), "
                "verdicts AS (SELECT DISTINCT ON (code) code, verdict FROM sro_vt_account.sr_lab_notes WHERE kind = 'verdict' AND NOT removed ORDER BY code, id DESC) "
                "SELECT s.race, s.mastery, s.mastery_name, count(*), "
                "  (SELECT json_object_agg(route, n)::text FROM (SELECT route, count(*) n FROM sro_vt_account.sr_lab_skills x WHERE x.mastery = s.mastery GROUP BY route) q), "
                "  (SELECT json_object_agg(k, n)::text FROM (SELECT l.scenario::text || ':' || l.status AS k, count(*) n FROM latest l "
                "     JOIN sro_vt_account.sr_lab_skills x ON x.code = l.code WHERE x.mastery = s.mastery GROUP BY 1) q), "
                "  (SELECT json_object_agg(verdict, n)::text FROM (SELECT v.verdict, count(*) n FROM verdicts v "
                "     JOIN sro_vt_account.sr_lab_skills x ON x.code = v.code WHERE x.mastery = s.mastery GROUP BY 1) q) "
                "FROM sro_vt_account.sr_lab_skills s GROUP BY s.race, s.mastery, s.mastery_name ORDER BY s.race, s.mastery");
            if (!r.Ok()) return std::nullopt;
            json out = json::array();
            for (int i = 0; i < r.Rows(); ++i) {
                out.push_back({{"race", r.Int(i, 0)}, {"mastery", r.Int(i, 1)}, {"masteryName", r.Text(i, 2)}, {"groups", r.Int(i, 3)},
                               {"routes", r.IsNull(i, 4) ? json::object() : ParseJson(r.View(i, 4))},
                               {"results", r.IsNull(i, 5) ? json::object() : ParseJson(r.View(i, 5))},
                               {"verdicts", r.IsNull(i, 6) ? json::object() : ParseJson(r.View(i, 6))}});
            }
            return out;
        });
        if (!rows || !*rows) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"masteries", std::move(**rows)}});
    }

    // The item list: its columns, its state (game::ItemStateOf), the item exam's latest results, the latest verdict, the
    // notes' count and the RE note's status (the data stays on the item's page); and each path's RE status.
    void Items(const httplib::Request& req, httplib::Response& res) {
        if (!Who(req, res, false)) return;
        const auto rows = OnPool<std::optional<json>>(*a_.pool, [](Database& db) -> std::optional<json> {
            const QueryResult r = db.Query(
                "SELECT i.code, i.item_id, i.name, i.type_ids, i.item_group, i.kind, i.route, i.source, i.race, i.sex, i.level, i.degree, i.rarity, "
                "i.usable, i.icon, i.build, extract(epoch FROM i.updated_at)::bigint, "
                "(SELECT n.verdict FROM sro_vt_account.sr_lab_notes n WHERE n.code = i.code AND n.kind = 'verdict' AND NOT n.removed AND n.aspect <> 'client' ORDER BY n.id DESC LIMIT 1), "
                "(SELECT count(*) FROM sro_vt_account.sr_lab_notes n WHERE n.code = i.code AND NOT n.removed), "
                "(SELECT e.status FROM sro_vt_account.sr_lab_re e WHERE e.code = i.code AND e.side = 'server'), i.data->'route'->>'why', "
                "coalesce(i.data->'route'->>'state', ''), "
                "(SELECT json_object_agg(x.k, x.status)::text FROM (SELECT DISTINCT ON (scenario, variant) scenario::text || ':' || variant AS k, status "
                "  FROM sro_vt_account.sr_lab_results r WHERE r.code = i.code AND r.run_id = (SELECT max(q.run_id) FROM sro_vt_account.sr_lab_results q WHERE q.code = r.code AND q.scenario = r.scenario) "
                "  ORDER BY scenario, variant, id DESC) x), "
                // The client's verdict: the latest verdict note about the client (its windows and play in our client).
                "(SELECT n.verdict FROM sro_vt_account.sr_lab_notes n WHERE n.code = i.code AND n.kind = 'verdict' AND NOT n.removed AND n.aspect = 'client' ORDER BY n.id DESC LIMIT 1) "
                "FROM sro_vt_account.sr_lab_items i ORDER BY i.item_group, i.kind, i.item_id");
            if (!r.Ok()) return std::nullopt;
            json items = json::array();
            for (int i = 0; i < r.Rows(); ++i) {
                items.push_back({{"code", r.Text(i, 0)},     {"id", r.Int(i, 1)},       {"name", r.Text(i, 2)},    {"typeIds", r.Text(i, 3)},
                                 {"group", r.Text(i, 4)},    {"kind", r.Text(i, 5)},    {"route", r.Text(i, 6)},   {"source", r.Text(i, 7)},
                                 {"race", r.Int(i, 8)},      {"sex", r.Int(i, 9)},      {"level", r.Int(i, 10)},   {"degree", r.Int(i, 11)},
                                 {"rarity", r.Int(i, 12)},   {"usable", r.Int(i, 13)},  {"icon", r.Text(i, 14)},   {"build", r.Text(i, 15)},
                                 {"updated", r.Int(i, 16)},  {"verdict", r.IsNull(i, 17) ? json(nullptr) : json(r.Text(i, 17))},
                                 {"notes", r.Int(i, 18)},    {"re", r.IsNull(i, 19) ? json(nullptr) : json(r.Text(i, 19))},
                                 {"why", r.IsNull(i, 20) ? std::string() : r.Text(i, 20)}, {"state", r.Text(i, 21)},
                                 {"results", r.IsNull(i, 22) ? json::object() : ParseJson(r.View(i, 22))},
                                 {"client", r.IsNull(i, 23) ? json(nullptr) : json(r.Text(i, 23))}});
            }
            // The paths' RE notes: their status, for the list's RE column where an item has none of its own.
            json routes = json::object();
            const QueryResult e = db.Query("SELECT code, status FROM sro_vt_account.sr_lab_re WHERE side = 'route'");
            for (int i = 0; e.Ok() && i < e.Rows(); ++i) routes[e.Text(i, 0)] = e.Text(i, 1);
            return json{{"items", std::move(items)}, {"routes", std::move(routes)}};
        });
        if (!rows || !*rows) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, **rows);
    }

    // An item's page: all its data, its notes and verdicts, its RE note.
    void Item(const httplib::Request& req, httplib::Response& res) {
        if (!Who(req, res, false)) return;
        const std::string code = req.matches[1].str();
        const auto found = OnPool<std::optional<json>>(*a_.pool, [code](Database& db) -> std::optional<json> {
            const QueryResult s = db.Query("SELECT data::text, build, extract(epoch FROM updated_at)::bigint FROM sro_vt_account.sr_lab_items WHERE code = $1", {code});
            if (!s.Ok() || s.Rows() != 1) return std::nullopt;
            json out = {{"item", ParseJson(s.View(0, 0))}, {"build", s.Text(0, 1)}, {"updated", s.Int(0, 2)}};
            const QueryResult n = db.Query("SELECT id, kind, verdict, body, author, source, extract(epoch FROM created_at)::bigint, aspect "
                                           "FROM sro_vt_account.sr_lab_notes WHERE code = $1 AND NOT removed ORDER BY id DESC LIMIT 200",
                                           {code});
            json notes = json::array();
            for (int i = 0; n.Ok() && i < n.Rows(); ++i) {
                notes.push_back({{"id", n.Int(i, 0)}, {"kind", n.Text(i, 1)}, {"verdict", n.IsNull(i, 2) ? json(nullptr) : json(n.Text(i, 2))},
                                 {"scenario", nullptr}, {"body", n.Text(i, 3)}, {"author", n.Text(i, 4)}, {"source", n.Text(i, 5)},
                                 {"at", n.Int(i, 6)}, {"aspect", n.Text(i, 7)}});
            }
            out["notes"] = std::move(notes);
            out["re"] = json(nullptr);
            const QueryResult e = db.Query("SELECT status, original, differs, sources, author, extract(epoch FROM updated_at)::bigint "
                                           "FROM sro_vt_account.sr_lab_re WHERE code = $1 AND side = 'server'",
                                           {code});
            if (e.Ok() && e.Rows() == 1) {
                out["re"] = json{{"status", e.Text(0, 0)},  {"original", e.Text(0, 1)}, {"differs", e.Text(0, 2)},
                                 {"sources", e.Text(0, 3)}, {"author", e.Text(0, 4)},   {"at", e.Int(0, 5)}};
            }
            // Its path's RE note (every item of the path) and how many items take the path.
            const std::string route = out["item"].value("route", json::object()).value("name", std::string());
            out["routeRe"] = json(nullptr);
            out["routeItems"] = 0;
            const QueryResult p = db.Query("SELECT status, original, differs, sources, author, extract(epoch FROM updated_at)::bigint, "
                                           "(SELECT count(*) FROM sro_vt_account.sr_lab_items WHERE route = $1) "
                                           "FROM (SELECT 1) one LEFT JOIN sro_vt_account.sr_lab_re ON code = $1 AND side = 'route'",
                                           {route});
            if (p.Ok() && p.Rows() == 1) {
                out["routeItems"] = p.Int(0, 6);
                if (!p.IsNull(0, 0)) {
                    out["routeRe"] = json{{"status", p.Text(0, 0)},  {"original", p.Text(0, 1)}, {"differs", p.Text(0, 2)},
                                          {"sources", p.Text(0, 3)}, {"author", p.Text(0, 4)},   {"at", p.Int(0, 5)}};
                }
            }
            // The item exam's latest results (its scenarios' variants), as the skills'.
            const QueryResult x = db.Query(
                "SELECT DISTINCT ON (scenario, variant) id, run_id, scenario, variant, status, summary, expected::text, observed::text, "
                "extract(epoch FROM at)::bigint, (SELECT build FROM sro_vt_account.sr_lab_runs u WHERE u.id = run_id) "
                "FROM sro_vt_account.sr_lab_results r WHERE code = $1 AND r.run_id = (SELECT max(q.run_id) FROM sro_vt_account.sr_lab_results q WHERE q.code = r.code AND q.scenario = r.scenario) "
                "ORDER BY scenario, variant, id DESC",
                {code});
            json results = json::array();
            for (int i = 0; x.Ok() && i < x.Rows(); ++i) {
                results.push_back({{"id", x.Int(i, 0)}, {"run", x.Int(i, 1)}, {"scenario", x.Int(i, 2)}, {"variant", x.Text(i, 3)}, {"status", x.Text(i, 4)},
                                   {"summary", x.Text(i, 5)}, {"expected", ParseJson(x.View(i, 6))}, {"observed", ParseJson(x.View(i, 7))},
                                   {"at", x.Int(i, 8)}, {"build", x.IsNull(i, 9) ? std::string() : x.Text(i, 9)}});
            }
            out["results"] = std::move(results);
            return out;
        });
        if (!found) return Refuse(res, 503, "busy", "The server is busy; try again.");
        if (!*found) return Refuse(res, 404, "unknown", "No such item in the list.");
        Answer(res, 200, **found);
    }

    void Runs(const httplib::Request& req, httplib::Response& res) {
        if (!Who(req, res, false)) return;
        const auto rows = OnPool<std::optional<json>>(*a_.pool, [](Database& db) -> std::optional<json> {
            const QueryResult r = db.Query("SELECT id, build, filter, scenarios, extract(epoch FROM started_at)::bigint, coalesce(extract(epoch FROM finished_at)::bigint, 0), "
                                           "passed, failed, skipped FROM sro_vt_account.sr_lab_runs ORDER BY id DESC LIMIT 50");
            if (!r.Ok()) return std::nullopt;
            json out = json::array();
            for (int i = 0; i < r.Rows(); ++i) {
                out.push_back({{"id", r.Int(i, 0)}, {"build", r.Text(i, 1)}, {"filter", r.Text(i, 2)}, {"scenarios", r.Text(i, 3)}, {"started", r.Int(i, 4)},
                               {"finished", r.Int(i, 5)}, {"passed", r.Int(i, 6)}, {"failed", r.Int(i, 7)}, {"skipped", r.Int(i, 8)}});
            }
            return out;
        });
        if (!rows || !*rows) return Refuse(res, 503, "busy", "The server is busy; try again.");
        Answer(res, 200, {{"runs", std::move(**rows)}});
    }

    DevAccess a_;
};

} // namespace

namespace web {

std::shared_ptr<void> RegisterDev(httplib::Server& http, DevAccess access) {
    auto dev = std::make_shared<Dev>(std::move(access));
    dev->Register(http);
    return dev;
}

bool DevUploadPath(const std::string& method, const std::string& path) {
    static const std::regex kVideos(R"(/api/admin/dev/skills/[A-Z0-9_]+/videos)");
    return method == "POST" && std::regex_match(path, kVideos);
}

} // namespace web

bool MakeDevKey(Database& db, std::string_view name, std::string& key, std::string& error) {
    if (!ValidAdminName(name)) return (error = "a key's name is 3 to 32 letters, digits or _"), false;
    const std::vector<std::uint8_t> raw = RandomBytes(32);
    if (raw.size() != 32) return (error = "no random bytes"), false;
    const QueryResult r = db.Query("INSERT INTO sro_vt_account.sr_web_dev_keys (name, key_hash) VALUES ($1, $2)", {name, Param::Bytes(Sha256(raw))});
    if (!r.Ok()) return (error = r.Error()), false;
    key = kKeyPrefix + Hex(raw);
    return true;
}

bool SetDevKeyDisabled(Database& db, std::string_view name, bool disabled, std::string& error) {
    const QueryResult r = db.Query("UPDATE sro_vt_account.sr_web_dev_keys SET disabled = $2 WHERE lower(name) = lower($1)", {name, disabled ? "true" : "false"});
    if (!r.Ok()) return (error = r.Error()), false;
    if (r.Affected() == 0) return (error = "no such key"), false;
    return true;
}

} // namespace sr::server
