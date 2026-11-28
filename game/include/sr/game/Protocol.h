#pragma once

// Our protocol's messages, shared by the client and the servers.
//
// A message is a struct with a `kId` and one `Visit` that lists its fields once, each with its
// name (and, for strings and lists, the most it may hold):
//
//     struct Hello {
//         static constexpr MessageId kId = 0x0001;
//         std::uint32_t protocol = kProtocolVersion;
//         std::string client;
//         template <typename V> void Visit(V& v) { v("protocol", protocol); v("client", client, 64); }
//     };
//
// Encode, Decode and Describe all walk that list, so a field is added in one place. The bytes
// are little-endian: integers and enums at their size, floats as IEEE bits, bool as one byte
// (0 or 1), strings as a u16 byte count and UTF-8, lists as a u16 count and the elements,
// nested structs inline. Decode refuses anything that is not exactly one well-formed message:
// short or trailing bytes, a string or list over its limit, a bool other than 0 or 1, a
// non-finite float. What the values mean is checked by whoever handles the message.
//
// Message ids by range: 0x0001-0x00FF the session (this file), 0x0100 the gateway, 0x0200 the
// login server, 0x0300 the game server's lobby, 0x1000 and up the world.

#include <algorithm>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sr::game::proto {

using MessageId = std::uint16_t;

// Raised whenever a message changes in a way an older peer cannot read.
// 2: the moves packed (EntityMoved's places, a tick's moves in one frame).
// 3: a ground item's owner in EntityAppeared.
// 4: the lobby's characters' academy role (CharacterSummary.academyRank).
// 5: a pet's experience lost by its death (CosExperience.lost).
// 6: a quest instance's members asked first (QuestEnterAsk, QuestEnterReply).
// 7: the casting gauge's kind (ReturnStarted.kind, .quest; CancelCasting).
// 8: the guild's mercenaries (GuildInfo.mercenaryAttr, .mercenaries; GuildMercenaryAbility).
// 9: an academy join request's highest level and race (AcademyAsk.maxLevel, .race).
// 10: a member's fortress war position to those around (EntityGuild.fortressRole).
// 11: a states-only skill's hit told (EntityDamaged with kDamageResist, damage 0).
// 12: TradeScale carries the cargo and its rule (goods, the trader's unit, capacity, the scale, the three-star cap).
// 13: the event guide's pages met (EventGuideState, EventGuideMet), kept by the server.
// 14: the skill refusals Level, MonsterClass and TargetLevel (SkillUseStatus 31..33).
inline constexpr std::uint32_t kProtocolVersion = 14;

// Limits when a field names none.
inline constexpr std::size_t kMaxString = 1024;
inline constexpr std::size_t kMaxList = 4096;

template <typename T>
concept Scalar = std::is_arithmetic_v<T> || std::is_enum_v<T>;

// The length in bytes of the well-formed UTF-8 sequence at `s[i]` (no overlong forms, no surrogates, at most
// U+10FFFF), 0 when there is none. Every string of our protocol is UTF-8: the decoder refuses anything else.
inline std::size_t Utf8SequenceAt(std::string_view s, std::size_t i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[i + k]); };
    const unsigned char c = b(0);
    if (c < 0x80) return 1;
    const std::size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (n == 0 || i + n > s.size()) return 0;
    for (std::size_t k = 1; k < n; ++k)
        if ((b(k) & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && b(1) < 0xA0) return 0; // overlong
    if (c == 0xED && b(1) > 0x9F) return 0; // a surrogate
    if (c == 0xF0 && b(1) < 0x90) return 0; // overlong
    if (c == 0xF4 && b(1) > 0x8F) return 0; // past U+10FFFF
    return n;
}

inline bool ValidUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = Utf8SequenceAt(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

// `s` as well-formed UTF-8 of at most `maxBytes`: a broken byte becomes '?', and a character that would not fit
// whole is left out (what the encoder sends, so that the other side's decoder always takes it).
inline std::string ToValidUtf8(std::string_view s, std::size_t maxBytes) {
    std::string out;
    out.reserve(std::min(s.size(), maxBytes));
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = Utf8SequenceAt(s, i);
        const std::size_t take = n == 0 ? 1 : n;
        if (out.size() + take > maxBytes) break;
        if (n == 0) {
            out += '?';
        } else {
            out.append(s.substr(i, n));
        }
        i += take;
    }
    return out;
}

namespace detail {

template <typename T>
using Raw = std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>, std::type_identity<T>>::type;

class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) : out_(out) {}

    template <Scalar T>
    void operator()(const char*, const T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            out_.push_back(std::byte(value ? 1 : 0));
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            Put(std::bit_cast<Bits>(value));
        } else {
            Put(static_cast<Raw<T>>(value));
        }
    }
    // A string is sent as the decoder takes it: well-formed UTF-8 within its field's limit (anything else is mended,
    // never sent broken: a broken one would make the whole message refused, or its size overflow its 16 bits).
    void operator()(const char*, const std::string& value, std::size_t max = kMaxString) {
        const std::size_t most = std::min<std::size_t>(max, 0xFFFF);
        if (value.size() <= most && ValidUtf8(value)) return PutBytes(value);
        PutBytes(ToValidUtf8(value, most));
    }
    template <typename T>
    void operator()(const char* name, const std::vector<T>& list, std::size_t max = kMaxList) {
        // At most its limit (more would have the whole message refused); the rest is not sent.
        const std::size_t count = std::min<std::size_t>({list.size(), max, 0xFFFF});
        Put(std::uint16_t(count));
        for (std::size_t i = 0; i < count; ++i) (*this)(name, list[i]);
    }
    template <typename T>
        requires(!Scalar<T>)
    void operator()(const char*, const T& nested) {
        const_cast<T&>(nested).Visit(*this); // Visit only reads through a Writer
    }

private:
    void PutBytes(std::string_view value) {
        Put(std::uint16_t(value.size()));
        const auto* p = reinterpret_cast<const std::byte*>(value.data());
        out_.insert(out_.end(), p, p + value.size());
    }
    template <std::integral T>
    void Put(T value) {
        using U = std::make_unsigned_t<T>;
        const U u = static_cast<U>(value);
        for (std::size_t i = 0; i < sizeof(T); ++i) out_.push_back(std::byte((u >> (8 * i)) & 0xFF));
    }
    std::vector<std::byte>& out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> in) : in_(in) {}

    bool Ok() const { return ok_; }
    bool AtEnd() const { return at_ == in_.size(); }

    template <Scalar T>
    void operator()(const char*, T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            std::uint8_t b = 0;
            Get(b);
            if (b > 1) ok_ = false;
            value = b == 1;
        } else if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            Bits bits = 0;
            Get(bits);
            value = std::bit_cast<T>(bits);
            if (!std::isfinite(value)) ok_ = false;
        } else {
            Raw<T> raw{};
            Get(raw);
            value = static_cast<T>(raw);
        }
    }
    void operator()(const char*, std::string& value, std::size_t max = kMaxString) {
        std::uint16_t size = 0;
        Get(size);
        if (!ok_ || size > max || size > in_.size() - at_) {
            ok_ = false;
            return;
        }
        value.assign(reinterpret_cast<const char*>(in_.data() + at_), size);
        at_ += size;
        if (!ValidUtf8(value)) ok_ = false; // every string of ours is UTF-8
    }
    template <typename T>
    void operator()(const char* name, std::vector<T>& list, std::size_t max = kMaxList) {
        std::uint16_t count = 0;
        Get(count);
        // Every element takes at least one byte: a count the rest cannot hold is refused before
        // anything is allocated.
        if (!ok_ || count > max || count > in_.size() - at_) {
            ok_ = false;
            return;
        }
        list.assign(count, T{});
        for (T& item : list) {
            (*this)(name, item);
            if (!ok_) return;
        }
    }
    template <typename T>
        requires(!Scalar<T>)
    void operator()(const char*, T& nested) {
        nested.Visit(*this);
    }

private:
    template <std::integral T>
    void Get(T& value) {
        if (!ok_ || in_.size() - at_ < sizeof(T)) {
            ok_ = false;
            value = 0;
            return;
        }
        using U = std::make_unsigned_t<T>;
        U u = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) u |= U(U(in_[at_ + i]) << (8 * i));
        value = static_cast<T>(u);
        at_ += sizeof(T);
    }
    std::span<const std::byte> in_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

class Describer {
public:
    explicit Describer(std::string& out) : out_(out) {}

    template <Scalar T>
    void operator()(const char* name, const T& value) {
        Name(name);
        if constexpr (std::is_same_v<T, bool>) {
            out_ += value ? "true" : "false";
        } else if constexpr (std::is_enum_v<T>) {
            out_ += std::to_string(static_cast<Raw<T>>(value));
        } else if constexpr (sizeof(T) == 1) {
            out_ += std::to_string(int(value));
        } else {
            out_ += std::format("{}", value);
        }
    }
    void operator()(const char* name, const std::string& value, std::size_t = kMaxString) {
        Name(name);
        out_ += '"';
        out_ += value;
        out_ += '"';
    }
    template <typename T>
    void operator()(const char* name, const std::vector<T>& list, std::size_t = kMaxList) {
        Name(name);
        out_ += '[';
        Describer inner(out_);
        for (const T& item : list) inner(nullptr, item);
        out_ += ']';
    }
    template <typename T>
        requires(!Scalar<T>)
    void operator()(const char* name, const T& nested) {
        Name(name);
        out_ += '{';
        Describer inner(out_);
        const_cast<T&>(nested).Visit(inner);
        out_ += '}';
    }

private:
    void Name(const char* name) {
        if (!first_) out_ += ", ";
        first_ = false;
        if (name) {
            out_ += name;
            out_ += '=';
        }
    }
    std::string& out_;
    bool first_ = true;
};

} // namespace detail

template <typename T>
std::vector<std::byte> Encode(const T& message) {
    std::vector<std::byte> out;
    detail::Writer writer(out);
    const_cast<T&>(message).Visit(writer); // Visit only reads through a Writer
    return out;
}

// True only if `payload` is exactly one well-formed T.
template <typename T>
bool Decode(std::span<const std::byte> payload, T& message) {
    T fresh{};
    detail::Reader reader(payload);
    fresh.Visit(reader);
    if (!reader.Ok() || !reader.AtEnd()) return false;
    message = std::move(fresh);
    return true;
}

// "Name{field=value, ...}" for logs; `name` is the message's name.
template <typename T>
std::string Describe(const char* name, const T& message) {
    std::string out = name;
    out += '{';
    detail::Describer describer(out);
    const_cast<T&>(message).Visit(describer);
    out += '}';
    return out;
}

// --- The session (0x0001-0x00FF) ------------------------------------------------------------

// Client -> server, first: who is talking.
struct Hello {
    static constexpr MessageId kId = 0x0001;
    std::uint32_t protocol = kProtocolVersion;
    std::string client; // the program and its version, for logs
    template <typename V>
    void Visit(V& v) {
        v("protocol", protocol);
        v("client", client, 64);
    }
};

enum class WelcomeResult : std::uint8_t {
    Accepted = 0,
    ProtocolMismatch = 1, // the client must be updated (or the server)
    ServerFull = 2,
    Refused = 3, // see `reason`
};

// Server -> client, the answer to Hello. Anything but Accepted is followed by the server closing.
struct Welcome {
    static constexpr MessageId kId = 0x0002;
    WelcomeResult result = WelcomeResult::Accepted;
    std::uint32_t protocol = kProtocolVersion; // the server's
    std::string server;                        // its name and version, for logs
    std::string reason;
    template <typename V>
    void Visit(V& v) {
        v("result", result);
        v("protocol", protocol);
        v("server", server, 64);
        v("reason", reason, 256);
    }
};

// Client -> server, while it has nothing else to say (the original sends its heartbeat the same
// way): keeps a quiet connection from closing for its silence. Every server takes it and does
// nothing with it.
struct KeepAlive {
    static constexpr MessageId kId = 0x0003;
    template <typename V>
    void Visit(V&) {}
};

// The server's answer to a client's Hello (the version check every server runs first).
Welcome AnswerHello(const Hello& hello, std::string_view serverName);

} // namespace sr::game::proto
