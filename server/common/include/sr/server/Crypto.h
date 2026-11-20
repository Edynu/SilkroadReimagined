#pragma once

// Passwords and tokens, on OpenSSL 3.
//
// Passwords are stored as Argon2id hashes in the PHC string form
// ("$argon2id$v=19$m=19456,t=2,p=1$<salt>$<hash>", base64 without padding), so the cost can rise
// later and old hashes still verify with their own parameters. The default cost is the OWASP
// minimum for Argon2id (19 MiB, 2 passes, 1 lane): tens of milliseconds, run as a job.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sr::server {

struct Argon2Cost {
    std::uint32_t memoryKiB = 19 * 1024;
    std::uint32_t passes = 2;
    std::uint32_t lanes = 1;
};

// Raw Argon2id (RFC 9106): `tagBytes` of output. Empty on failure.
std::vector<std::uint8_t> Argon2id(std::span<const std::uint8_t> password, std::span<const std::uint8_t> salt,
                                   const Argon2Cost& cost, std::size_t tagBytes,
                                   std::span<const std::uint8_t> secret = {}, std::span<const std::uint8_t> associated = {});

// A new hash of `password` with a random salt, in PHC form. Empty on failure.
std::string HashPassword(std::string_view password, const Argon2Cost& cost = {});
// Whether `password` matches `phc` (any cost the string names). Constant-time comparison.
bool VerifyPassword(std::string_view password, std::string_view phc);

// The original's password form: an MD5 of the password in lower-case hex, as
// vSRO's account tools write TB_User.password. Weak: only read, to let such an account log in once
// and get our hash; written only when the server is told to keep the original's tools working.
std::string LegacyPasswordHash(std::string_view password);
bool VerifyLegacyPassword(std::string_view password, std::string_view stored);

std::vector<std::uint8_t> RandomBytes(std::size_t count);
std::vector<std::uint8_t> Sha256(std::span<const std::uint8_t> data);
// Lower-case hex of bytes.
std::string Hex(std::span<const std::uint8_t> data);

// SHA-256 over data that comes in pieces (an upload written as it arrives).
class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;
    void Add(std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> Finish();

private:
    void* ctx_; // EVP_MD_CTX
};

// Equal without telling where they differ (secrets, tokens).
bool ConstantTimeEqual(std::string_view a, std::string_view b);

// Time-based one-time codes (RFC 6238: HMAC-SHA1, 30-second steps, 6 digits), the authenticator apps' codes,
// and the secret's Base32 form they are given (RFC 4648, no padding).
std::string Base32(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> FromBase32(std::string_view text); // empty when not Base32
std::vector<std::uint8_t> HmacSha1(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);
// The code of the step `unixTime` falls in, `digits` long (8 for RFC 6238's test vectors).
std::string TotpCode(std::span<const std::uint8_t> secret, std::int64_t unixTime, int digits = 6);
// Whether `code` is the code now or of the step before or after (a clock a little off); the step it matched
// in `step` (a code is good once: the caller keeps the last step used).
bool VerifyTotp(std::span<const std::uint8_t> secret, std::string_view code, std::int64_t unixTime, std::int64_t& step);

} // namespace sr::server
