#pragma once

// What SroWeb's request handlers share (server/web/src only): database work waited for, JSON answers and refusals,
// the cookie and body readers.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "sr/server/Database.h"

namespace sr::server::web {

using json = nlohmann::json;

inline std::int64_t UnixNow() { return std::int64_t(std::time(nullptr)); }

// `fn` on the database pool, waited for (a request's worker blocks on it). None when the pool is too busy.
template <typename T>
std::optional<T> OnPool(DatabasePool& pool, std::function<T(Database&)> fn) {
    auto promise = std::make_shared<std::promise<T>>();
    std::future<T> future = promise->get_future();
    pool.Submit([promise, fn = std::move(fn)](Database& db) { promise->set_value(fn(db)); });
    if (future.wait_for(std::chrono::seconds(20)) != std::future_status::ready) return std::nullopt;
    return future.get();
}

inline void Answer(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body.dump(), "application/json");
}
inline void Refuse(httplib::Response& res, int status, const char* code, const std::string& message) {
    Answer(res, status, {{"error", code}, {"message", message}});
}

inline std::string CookieValue(const httplib::Request& req, std::string_view name) {
    const std::string header = req.get_header_value("Cookie");
    for (std::size_t at = 0; at < header.size();) {
        std::size_t end = header.find(';', at);
        if (end == std::string::npos) end = header.size();
        std::string_view part = std::string_view(header).substr(at, end - at);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        if (part.starts_with(name) && part.size() > name.size() && part[name.size()] == '=') return std::string(part.substr(name.size() + 1));
        at = end + 1;
    }
    return {};
}

inline std::vector<std::uint8_t> FromHex(std::string_view s) {
    std::vector<std::uint8_t> out;
    if (s.size() % 2 != 0) return {};
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        const int hi = nibble(s[i]), lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(std::uint8_t(hi * 16 + lo));
    }
    return out;
}

inline std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

inline std::string Field(const json& body, const char* key) {
    const auto it = body.find(key);
    return it != body.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A JSON body, or the answer refusing it.
inline std::optional<json> Body(const httplib::Request& req, httplib::Response& res) {
    if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
        Refuse(res, 415, "bad-request", "The request must be JSON.");
        return std::nullopt;
    }
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        Refuse(res, 400, "bad-request", "The request is not valid JSON.");
        return std::nullopt;
    }
    return body;
}

// A change's Origin (browsers send it): this site's (its Host) or one listed. None: not a browser's cross-site request.
inline bool SameOrigin(const httplib::Request& req, const std::vector<std::string>& origins) {
    const std::string origin = req.get_header_value("Origin");
    if (origin.empty()) return true;
    const std::string host = req.get_header_value("Host");
    if (origin == "http://" + host || origin == "https://" + host) return true;
    return std::find(origins.begin(), origins.end(), origin) != origins.end();
}

} // namespace sr::server::web
