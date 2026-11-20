#include "sr/server/Crypto.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <charconv>
#include <cstring>
#include <format>

namespace sr::server {

namespace {

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kTagBytes = 32;

std::string Base64(std::span<const std::uint8_t> data) {
    std::string out;
    std::uint32_t bits = 0;
    int count = 0;
    for (std::uint8_t b : data) {
        bits = bits << 8 | b;
        count += 8;
        while (count >= 6) {
            count -= 6;
            out += kBase64[(bits >> count) & 63];
        }
    }
    if (count > 0) out += kBase64[(bits << (6 - count)) & 63];
    return out; // no padding (PHC)
}

bool FromBase64(std::string_view text, std::vector<std::uint8_t>& out) {
    out.clear();
    std::uint32_t bits = 0;
    int count = 0;
    for (char c : text) {
        const char* p = std::char_traits<char>::find(kBase64, 64, c);
        if (!p) return false;
        bits = bits << 6 | std::uint32_t(p - kBase64);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(std::uint8_t((bits >> count) & 0xFF));
        }
    }
    // Canonical only: the bits left over must be zero (one hash, one spelling) and no lone char.
    return count < 6 && (bits & ((1u << count) - 1)) == 0;
}

bool ParseU32(std::string_view text, std::uint32_t& out) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = text.find(separator, start);
        parts.push_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) return parts;
        start = at + 1;
    }
}

std::span<const std::uint8_t> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

} // namespace

std::vector<std::uint8_t> Argon2id(std::span<const std::uint8_t> password, std::span<const std::uint8_t> salt,
                                   const Argon2Cost& cost, std::size_t tagBytes, std::span<const std::uint8_t> secret,
                                   std::span<const std::uint8_t> associated) {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (!kdf) return {};
    EVP_KDF_CTX* context = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!context) return {};

    std::uint32_t passes = cost.passes, memory = cost.memoryKiB, lanes = cost.lanes, threads = 1;
    std::uint32_t version = 0x13;
    OSSL_PARAM params[10]; // 7 settings, the secret, the associated data, the end
    int n = 0;
    params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, const_cast<std::uint8_t*>(password.data()), password.size());
    params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<std::uint8_t*>(salt.data()), salt.size());
    params[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &passes);
    params[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory);
    params[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes);
    params[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads);
    params[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_VERSION, &version);
    if (!secret.empty()) {
        params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET, const_cast<std::uint8_t*>(secret.data()), secret.size());
    }
    if (!associated.empty()) {
        params[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_ARGON2_AD, const_cast<std::uint8_t*>(associated.data()), associated.size());
    }
    std::vector<std::uint8_t> tag(tagBytes);
    params[n] = OSSL_PARAM_construct_end();
    const bool ok = EVP_KDF_derive(context, tag.data(), tag.size(), params) == 1;
    EVP_KDF_CTX_free(context);
    if (!ok) return {};
    return tag;
}

std::string HashPassword(std::string_view password, const Argon2Cost& cost) {
    const std::vector<std::uint8_t> salt = RandomBytes(kSaltBytes);
    if (salt.empty()) return {};
    const std::vector<std::uint8_t> tag = Argon2id(Bytes(password), salt, cost, kTagBytes);
    if (tag.empty()) return {};
    return std::format("$argon2id$v=19$m={},t={},p={}${}${}", cost.memoryKiB, cost.passes, cost.lanes, Base64(salt), Base64(tag));
}

bool VerifyPassword(std::string_view password, std::string_view phc) {
    // "" "argon2id" "v=19" "m=..,t=..,p=.." salt tag
    const std::vector<std::string_view> parts = Split(phc, '$');
    if (parts.size() != 6 || !parts[0].empty() || parts[1] != "argon2id" || parts[2] != "v=19") return false;
    Argon2Cost cost;
    const std::vector<std::string_view> settings = Split(parts[3], ',');
    if (settings.size() != 3 || !settings[0].starts_with("m=") || !settings[1].starts_with("t=") || !settings[2].starts_with("p=") ||
        !ParseU32(settings[0].substr(2), cost.memoryKiB) || !ParseU32(settings[1].substr(2), cost.passes) ||
        !ParseU32(settings[2].substr(2), cost.lanes)) {
        return false;
    }
    // Refuse costs no hash of ours would have (a tampered row must not make us allocate gigabytes).
    if (cost.memoryKiB < 8 || cost.memoryKiB > 1024 * 1024 || cost.passes < 1 || cost.passes > 16 || cost.lanes < 1 || cost.lanes > 16) {
        return false;
    }
    std::vector<std::uint8_t> salt, expected;
    if (!FromBase64(parts[4], salt) || !FromBase64(parts[5], expected) || salt.size() < 8 || expected.size() < 16) return false;
    const std::vector<std::uint8_t> tag = Argon2id(Bytes(password), salt, cost, expected.size());
    return tag.size() == expected.size() && CRYPTO_memcmp(tag.data(), expected.data(), tag.size()) == 0;
}

std::vector<std::uint8_t> RandomBytes(std::size_t count) {
    std::vector<std::uint8_t> out(count);
    if (RAND_bytes(out.data(), int(count)) != 1) return {};
    return out;
}

std::vector<std::uint8_t> Sha256(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out(32);
    unsigned size = 0;
    if (EVP_Digest(data.data(), data.size(), out.data(), &size, EVP_sha256(), nullptr) != 1) return {};
    out.resize(size);
    return out;
}

std::string Hex(std::span<const std::uint8_t> data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t b : data) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

Sha256Stream::Sha256Stream() : ctx_(EVP_MD_CTX_new()) {
    if (ctx_ != nullptr) EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_), EVP_sha256(), nullptr);
}
Sha256Stream::~Sha256Stream() { EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_)); }
void Sha256Stream::Add(std::span<const std::uint8_t> data) {
    if (ctx_ != nullptr && !data.empty()) EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), data.data(), data.size());
}
std::vector<std::uint8_t> Sha256Stream::Finish() {
    std::vector<std::uint8_t> out(32);
    unsigned size = 0;
    if (ctx_ == nullptr || EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx_), out.data(), &size) != 1) return {};
    out.resize(size);
    return out;
}

bool ConstantTimeEqual(std::string_view a, std::string_view b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

namespace {
constexpr char kBase32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
}

std::string Base32(std::span<const std::uint8_t> data) {
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const std::uint8_t b : data) {
        buffer = (buffer << 8) | b;
        bits += 8;
        while (bits >= 5) {
            out += kBase32[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) out += kBase32[(buffer << (5 - bits)) & 31];
    return out;
}

std::vector<std::uint8_t> FromBase32(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char raw : text) {
        if (raw == ' ' || raw == '-' || raw == '=') continue;
        const char c = char(raw >= 'a' && raw <= 'z' ? raw - 32 : raw);
        const char* at = std::strchr(kBase32, c);
        if (c == 0 || at == nullptr) return {};
        buffer = (buffer << 5) | std::uint32_t(at - kBase32);
        bits += 5;
        if (bits >= 8) {
            out.push_back(std::uint8_t(buffer >> (bits - 8)));
            bits -= 8;
        }
    }
    return out;
}

std::vector<std::uint8_t> HmacSha1(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out(20);
    std::size_t size = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA1", nullptr, key.data(), key.size(), data.data(), data.size(), out.data(), out.size(), &size) == nullptr)
        return {};
    out.resize(size);
    return out;
}

std::string TotpCode(std::span<const std::uint8_t> secret, std::int64_t unixTime, int digits) {
    const std::uint64_t step = std::uint64_t(unixTime / 30);
    std::uint8_t counter[8];
    for (int i = 0; i < 8; ++i) counter[i] = std::uint8_t(step >> (56 - 8 * i));
    const std::vector<std::uint8_t> mac = HmacSha1(secret, counter);
    if (mac.size() != 20) return {};
    const int offset = mac[19] & 15; // RFC 4226's dynamic truncation
    const std::uint32_t value = (std::uint32_t(mac[std::size_t(offset)] & 0x7F) << 24) | (std::uint32_t(mac[std::size_t(offset) + 1]) << 16) |
                                (std::uint32_t(mac[std::size_t(offset) + 2]) << 8) | std::uint32_t(mac[std::size_t(offset) + 3]);
    std::uint32_t mod = 1;
    for (int i = 0; i < digits; ++i) mod *= 10;
    std::string code = std::to_string(value % mod);
    return std::string(std::size_t(digits) - code.size(), '0') + code;
}

bool VerifyTotp(std::span<const std::uint8_t> secret, std::string_view code, std::int64_t unixTime, std::int64_t& step) {
    if (code.size() != 6 || secret.empty()) return false;
    bool match = false;
    for (int d = -1; d <= 1; ++d) { // every step compared, so the time taken does not tell which
        const std::int64_t at = unixTime + 30 * d;
        if (ConstantTimeEqual(TotpCode(secret, at), code) && !match) {
            match = true;
            step = at / 30;
        }
    }
    return match;
}

std::string LegacyPasswordHash(std::string_view password) {
    unsigned char digest[16];
    unsigned size = 0;
    if (EVP_Digest(password.data(), password.size(), digest, &size, EVP_md5(), nullptr) != 1 || size != 16) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < size; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 15];
    }
    return out;
}

bool VerifyLegacyPassword(std::string_view password, std::string_view stored) {
    const std::string mine = LegacyPasswordHash(password);
    if (mine.empty() || stored.size() != mine.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < mine.size(); ++i) {
        const char c = stored[i] >= 'A' && stored[i] <= 'F' ? char(stored[i] - 'A' + 'a') : stored[i]; // either case
        diff |= static_cast<unsigned char>(c ^ mine[i]);
    }
    return diff == 0;
}

} // namespace sr::server
