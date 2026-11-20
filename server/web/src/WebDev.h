#pragma once

// The site's developer pages' API (website/src/dev): the skill lab's list, its exam's results, the
// testers' verdicts and notes, the RE notes by skill and by tag, at /api/admin/dev/... (under the administration's
// path: its session cookie goes there). Who may: an administration session of any role (admin, moderator, tester; a
// change with the session's second token, as the administration's), or a script or an agent with a key
// (Authorization: Bearer srdev_..., SroWeb --make-dev-key NAME). Every change is in the audit log.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace httplib {
class Server;
struct Request;
struct Response;
} // namespace httplib

namespace sr::server {

class DatabasePool;

namespace web {

// Who asks: a session's account (by name and role) or a key's name.
struct DevWho {
    int adminId = 0;
    std::string name, role;
    bool key = false;
    std::string Source() const { return key ? "key:" + name : "web"; }
};

struct DevAccess {
    DatabasePool* pool = nullptr;
    // A session's account (refusing the request when there is none); `write` checks its origin and second token.
    std::function<std::optional<DevWho>(const httplib::Request&, httplib::Response&, bool write)> session;
    std::function<std::string(const httplib::Request&)> visitor;
    std::function<void(const DevWho&, const std::string& address, std::string action, std::string detail)> audit;
    // Where the skill bench's videos are kept (POST .../skills/CODE/videos), and the largest taken.
    std::filesystem::path videos;
    std::int64_t videoMaxBytes = std::int64_t(64) << 20;
};

// An upload the body limit lets through (a video's).
bool DevUploadPath(const std::string& method, const std::string& path);

// The routes on `http`; what it returns lives as long as they are served.
std::shared_ptr<void> RegisterDev(httplib::Server& http, DevAccess access);

} // namespace web
} // namespace sr::server
