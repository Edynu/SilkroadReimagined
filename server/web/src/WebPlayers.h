#pragma once

// The players on the website (W7; server/web/src only): signed in with their game account (checked as
// the login server checks it; a wrong password here never blocks the game account), a session in a cookie scripts cannot
// read (sro_player, 30 days, only its SHA-256 kept) and a second token on every change; their comments on the news,
// signed as one of their characters (an account's name is half its login, never shown).
//
//   POST /api/account/login {name, password}   GET /api/account/me   POST /api/account/logout
//   GET  /api/news/:id/comments                POST /api/news/:id/comments {text, character}
//   DELETE /api/comments/:id (one's own)

#include <memory>

#include "sr/server/WebAdmin.h"

namespace sr::server {

class WebPlayers;
std::shared_ptr<WebPlayers> RegisterPlayers(httplib::Server& http, const AdminHost& host);

} // namespace sr::server
