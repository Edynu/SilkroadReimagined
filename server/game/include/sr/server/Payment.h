#pragma once

// A payment with its database work in one transaction (the guilds', the leagues', the academies' fees and rewards): the
// game server runs it in the paying character's order of saves (GameServer's character writer). At its turn the fee is
// taken from the character's gold (too little, or the character gone: it fails and nothing runs); `PaymentWork` runs in
// the transaction and returns what it credits to the gold (a reward the database held, 0 for none), or nothing to roll
// the whole back (the fee given back); the gold column is written in the same transaction, and a credit is given to the
// player only once that has committed. The original runs each such operation as one procedure, or charges only after
// its job succeeded; ours never takes a fee for work not done, nor pays a reward twice.

#include <cstdint>
#include <functional>
#include <optional>

namespace sr::server {

class Database;

// In the payment's transaction: the credit (0: none), or nothing to roll back.
using PaymentWork = std::function<std::optional<std::uint64_t>(Database& tx)>;
// Its end on the server's thread: whether it stood (false also when the fee could not be taken), what it credited.
using PaymentDone = std::function<void(bool ok, std::uint64_t credit)>;

} // namespace sr::server
