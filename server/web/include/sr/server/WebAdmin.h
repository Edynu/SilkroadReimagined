#pragma once

// The website's administration (W6; server/web/src/WebAdmin.cpp). Its accounts are its own
// (sr_web_admins), never the game's: made on the server's command line (SroWeb --make-admin), each with a password
// (Argon2id, 12 characters or more) and an authenticator app's secret: a login takes both, and a code is good once.
//
// Safety, in short:
//   - a session is a random token in a cookie scripts cannot read (HttpOnly), sent only to /api/admin and never by
//     another site (SameSite=Strict; Secure behind HTTPS); the database keeps only its SHA-256; it ends after 12 hours
//     or an hour unused;
//   - every change also carries the session's second token in a header (X-CSRF-Token), and its Origin must be this site;
//   - logins are limited per address (10 in 15 minutes) and per account (5 wrong in a row lock it for 15 minutes);
//   - the admin API can be held to listed addresses (web-admin-allow);
//   - roles: "admin" does everything, "moderator" only the players (the list, bans) and the announcements, "tester"
//     only the developer pages (/dev: the skill lab) and its own password;
//   - everything done is written to sr_web_audit, with who and from where;
//   - uploads: images only, as JPEG, PNG or WebP by their first bytes (8 MB), stored under random names. The client is
//     not uploaded here: SroDeploy makes its zip when a patch is published (web-patch-dir);
//   - the site's texts are data: the page shows them as text, and links must be http(s) or the site's own paths.
//
// The public side reads it all at /api/site (the texts, the visible news and images, the client's download).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sr/server/Database.h"
#include "sr/server/Store.h"

namespace httplib {
class Server;
struct Request;
}

namespace sr::server {

struct WebConfig;

// What the administration needs of the web server.
struct AdminHost {
    const WebConfig* config = nullptr;
    DatabasePool* pool = nullptr;
    std::function<std::string(const httplib::Request&)> visitor; // the visitor's address
    std::function<void()> siteChanged;                            // /api/site read again
};

class WebAdmin;
// The admin routes on `http`; what it returns lives as long as they are served.
std::shared_ptr<WebAdmin> RegisterAdmin(httplib::Server& http, const AdminHost& host);

// /api/site's answer: the texts (sr_site_content "site"), the visible news and images, and "client": the players' zip
// from SroDeploy's patch folder (null without one): its version, address, size, SHA-256 and VirusTotal link.
std::optional<std::string> SiteJson(Database& db, const WebConfig& config);

// Whether `address` is one `allow` lists (empty: any): exact addresses and CIDR ranges, IPv4 and IPv6; an IPv4 address
// seen as ::ffff:a.b.c.d is that address; 127.0.0.1, ::1 and "localhost" each stand for this machine.
bool AddressAllowed(const std::vector<std::string>& allow, const std::string& address);

// The command line's (SroWeb --make-admin ...): an admin account and its authenticator secret (Base32, shown once).
bool ValidAdminName(std::string_view name);
bool MakeAdmin(Database& db, std::string_view name, std::string_view role, std::string_view password, std::string& secretBase32, std::string& error);
bool ResetAdminSecret(Database& db, std::string_view name, std::string& secretBase32, std::string& error);
bool SetAdminPassword(Database& db, std::string_view name, std::string_view password, std::string& error);
bool SetAdminDisabled(Database& db, std::string_view name, bool disabled, std::string& error);
// The link an authenticator app reads (as a QR code or typed): otpauth://totp/...
std::string AuthenticatorUri(std::string_view name, std::string_view secretBase32);
// The developer pages' keys (SroWeb --make-dev-key NAME; server/web/src/WebDev.h): a script's or an agent's way into
// /api/admin/dev, "Authorization: Bearer srdev_..."; the key is shown once, the database keeps its SHA-256.
bool MakeDevKey(Database& db, std::string_view name, std::string& key, std::string& error);
bool SetDevKeyDisabled(Database& db, std::string_view name, bool disabled, std::string& error);

} // namespace sr::server
