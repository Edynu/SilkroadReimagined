#pragma once

// The support tickets (sr_support_tickets, sr_support_messages, sr_support_files; docs/DATABASE.md): made by the website
// (SroWeb's WebSupport.h, a player's message or bug report from the site) and by the game servers (a bug report from the
// game, SupportProtocol.h: source "client", the account known, its screenshot kept in the database).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sr/server/Database.h"

namespace sr::server {

struct NewTicket {
    std::string kind;     // "contact" or "bug"
    std::string category; // SupportCategories(kind)
    std::string subject, body;
    std::string name, email;
    std::string source = "site"; // "site" or "client"
    std::string clientInfo;      // the client's version and system (a bug from the game)
    std::int64_t jid = 0;        // the signed-in player's account; 0: none
    std::string address;
};

const std::vector<std::string>& SupportCategories(const std::string& kind);
// Trims and checks it; the refusal's words, or none.
std::optional<std::string> CheckTicket(NewTicket& t);
// The ticket made: its id and the upload token (shown once; the database keeps its SHA-256).
std::optional<std::pair<std::int64_t, std::string>> CreateTicket(Database& db, const NewTicket& t);

// An attachment's kind by its first bytes: "image/jpeg", "image/png", "image/webp", "video/mp4", "video/webm"; empty: refused.
std::string SupportFileType(std::string_view head);
std::string SupportFileExtension(const std::string& mime);

// A file kept in the database itself (sr_support_files.data; a game server's screenshot: the website may run elsewhere).
// Its stored name is "db:" and its SHA-256. False: not saved.
bool AddTicketFileData(Database& db, std::int64_t ticket, const std::string& original, const std::string& mime, std::span<const std::uint8_t> data);

} // namespace sr::server
