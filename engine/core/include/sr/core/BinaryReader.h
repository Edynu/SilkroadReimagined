#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace sr::core {

// Little-endian reader over a byte span. Reading past the end does not throw:
// it returns zeros and sets a sticky failure flag, so parsers can read a whole
// record and check Ok() once.
class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> data, std::size_t offset = 0) : data_(data), offset_(offset) {
        if (offset_ > data_.size()) Fail();
    }

    template <typename T>
    T Read() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        if (Require(sizeof(T))) {
            std::memcpy(&value, data_.data() + offset_, sizeof(T));
            offset_ += sizeof(T);
        }
        return value;
    }

    std::uint8_t U8() { return Read<std::uint8_t>(); }
    std::uint16_t U16() { return Read<std::uint16_t>(); }
    std::uint32_t U32() { return Read<std::uint32_t>(); }
    float F32() { return Read<float>(); }

    // JMX counted string: u32 byte length, then the bytes (no terminator).
    std::string CountedString(std::size_t maxLength = 4096) {
        const std::uint32_t length = U32();
        if (length > maxLength || !Require(length)) {
            Fail();
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data_.data() + offset_), length);
        offset_ += length;
        return s;
    }

    std::span<const std::byte> Bytes(std::size_t count) {
        if (!Require(count)) return {};
        auto out = data_.subspan(offset_, count);
        offset_ += count;
        return out;
    }

    void Skip(std::size_t count) {
        if (Require(count)) offset_ += count;
    }
    void Seek(std::size_t offset) {
        if (offset > data_.size()) {
            Fail();
            return;
        }
        offset_ = offset;
    }
    bool Matches(std::string_view magic) const {
        return offset_ + magic.size() <= data_.size() &&
               std::memcmp(data_.data() + offset_, magic.data(), magic.size()) == 0;
    }

    std::size_t Offset() const { return offset_; }
    std::size_t Size() const { return data_.size(); }
    std::size_t Remaining() const { return ok_ ? data_.size() - offset_ : 0; }
    bool Ok() const { return ok_; }
    void Fail() { ok_ = false; }

private:
    bool Require(std::size_t count) {
        if (!ok_ || count > data_.size() - offset_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    std::span<const std::byte> data_;
    std::size_t offset_ = 0;
    bool ok_ = true;
};

} // namespace sr::core
