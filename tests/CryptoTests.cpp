#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "sr/server/Crypto.h"

using namespace sr::server;

namespace {

std::string Hex(const std::vector<std::uint8_t>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

} // namespace

TEST_CASE("Crypto: Argon2id matches RFC 9106's test vector") {
    // RFC 9106 section 5.3: password 32 x 0x01, salt 16 x 0x02, secret 8 x 0x03, associated data
    // 12 x 0x04; 3 passes, 32 KiB, 4 lanes; a 32-byte tag.
    const std::vector<std::uint8_t> password(32, 0x01), salt(16, 0x02), secret(8, 0x03), associated(12, 0x04);
    Argon2Cost cost;
    cost.memoryKiB = 32;
    cost.passes = 3;
    cost.lanes = 4;
    const std::vector<std::uint8_t> tag = Argon2id(password, salt, cost, 32, secret, associated);
    CHECK(Hex(tag) == "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659");
}

TEST_CASE("Crypto: password hashes verify, and only the right password") {
    const std::string phc = HashPassword("correct horse");
    REQUIRE(phc.starts_with("$argon2id$v=19$m=19456,t=2,p=1$"));
    CHECK(VerifyPassword("correct horse", phc));
    CHECK_FALSE(VerifyPassword("correct horse ", phc));
    CHECK_FALSE(VerifyPassword("", phc));
    // A new salt each time.
    CHECK(HashPassword("correct horse") != phc);

    // Another cost still verifies with its own parameters.
    Argon2Cost cheap;
    cheap.memoryKiB = 64;
    cheap.passes = 1;
    const std::string light = HashPassword("pw123456", cheap);
    CHECK(light.starts_with("$argon2id$v=19$m=64,t=1,p=1$"));
    CHECK(VerifyPassword("pw123456", light));
}

TEST_CASE("Crypto: malformed or tampered hashes never verify") {
    const std::string phc = HashPassword("secret!");
    CHECK_FALSE(VerifyPassword("secret!", ""));
    CHECK_FALSE(VerifyPassword("secret!", "plain"));
    CHECK_FALSE(VerifyPassword("secret!", "$argon2i$" + phc.substr(10)));
    // A changed character inside the hash.
    std::string flipped = phc;
    char& c = flipped[flipped.size() - 10];
    c = c == 'A' ? 'B' : 'A';
    CHECK_FALSE(VerifyPassword("secret!", flipped));
    // The last character carries 2 padding bits for a 32-byte hash: a spelling with them set is
    // refused, not read as the same hash.
    std::string padded = phc;
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto index = std::string(alphabet).find(padded.back());
    REQUIRE(index != std::string::npos);
    padded.back() = alphabet[index ^ 1];
    CHECK_FALSE(VerifyPassword("secret!", padded));
    // A cost no hash of ours has is refused before anything is allocated.
    std::string huge = phc;
    huge.replace(huge.find("m=19456"), 7, "m=99999999");
    CHECK_FALSE(VerifyPassword("secret!", huge));
}

TEST_CASE("Crypto: SHA-256 and random bytes") {
    const std::string abc = "abc";
    CHECK(Hex(Sha256({reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()})) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto a = RandomBytes(32), b = RandomBytes(32);
    CHECK(a.size() == 32);
    CHECK(a != b);
}
