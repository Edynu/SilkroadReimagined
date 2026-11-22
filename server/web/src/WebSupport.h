#pragma once

// Support: the players' messages to the team (contact) and their bug reports, kept as tickets (sr_support_tickets) with
// their conversation (sr_support_messages: the player's, the staff's answers, the staff's own notes the player never
// sees) and their attachments (sr_support_files: screenshots and videos, kept in SroWeb's web-support folder, never
// served to the public: the administration reads them through its own guarded route).
//
// A ticket is sent first (JSON), its files after it one by one with the upload token its answer carries (good for
// kSupportUploadHours, kSupportFilesMax files). The website sends them; the beta client will send its bug reports the
// same way (source "client", its version and system in clientInfo).
//
//   POST /api/support {kind, category, subject, text, name, email, clientInfo, source}  -> {id, uploadToken}
//   POST /api/support/:id/files?token=T&name=N  (the file's bytes)                       -> {id, kind}
//   GET  /api/account/tickets                    a signed-in player's own, with the staff's answers
//   POST /api/account/tickets/:id/reply {text}
// The administration's: /api/admin/tickets (list, one, answer or note, status, delete, a file).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sr/server/Database.h"
#include "sr/server/SupportStore.h"

namespace sr::server::web {

constexpr int kSupportFilesMax = 5;
constexpr std::int64_t kSupportImageMax = std::int64_t(8) << 20;  // a screenshot
constexpr std::int64_t kSupportVideoMax = std::int64_t(95) << 20; // a video (Cloudflare's free plan takes 100 MB a request)
constexpr int kSupportUploadHours = 2;

// POST /api/support/<id>/files: a body larger than the others' (the server's size guard lets it through).
bool SupportUploadPath(const std::string& method, const std::string& path);

// Made here and by the game servers (sr/server/SupportStore.h).
using sr::server::CheckTicket;
using sr::server::CreateTicket;
using sr::server::NewTicket;
using sr::server::SupportCategories;
using sr::server::SupportFileExtension;
using sr::server::SupportFileType;

// A player's own tickets: their messages (not the staff's notes) and their files' names.
nlohmann::json PlayerTickets(Database& db, std::int64_t jid);
// The player's answer on its own open ticket (it opens again); false: not its or closed.
bool PlayerReply(Database& db, std::int64_t jid, std::int64_t ticket, const std::string& name, const std::string& text);

// The administration's.
nlohmann::json TicketList(Database& db, const std::string& status, const std::string& kind, const std::string& search);
nlohmann::json TicketDetail(Database& db, std::int64_t id); // null: none
bool StaffMessage(Database& db, std::int64_t id, const std::string& name, const std::string& text, bool note);
bool SetTicketStatus(Database& db, std::int64_t id, const std::string& status);
// The ticket gone; its files' stored names (removed from the folder by the caller).
std::optional<std::vector<std::string>> DeleteTicket(Database& db, std::int64_t id);
nlohmann::json SupportCounts(Database& db); // open, answered, closed by kind

} // namespace sr::server::web
