#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/**
 * Big-endian (network byte order) wire serialization for the multiplayer protocol.
 *
 * Deliberately NOT built on include/dusk/endian.h: that header targets big-endian *asset*
 * reading and is shared with decomp code, so extending it for wire formats would couple two
 * unrelated concerns. This is self-contained and has zero engine dependencies, which keeps the
 * protocol unit-testable headless (see the layering rule in 04-architecture.md).
 *
 * Reader is bounds-checked and never throws: every read returns false and sets a sticky failure
 * flag on overrun, so a malformed or truncated packet from the network can't run off the end.
 */

namespace dusk::mp {

class Writer {
public:
    void write_u8(std::uint8_t v) { mData.push_back(v); }

    void write_u16(std::uint16_t v) {
        mData.push_back(static_cast<std::uint8_t>(v >> 8));
        mData.push_back(static_cast<std::uint8_t>(v));
    }

    void write_u32(std::uint32_t v) {
        mData.push_back(static_cast<std::uint8_t>(v >> 24));
        mData.push_back(static_cast<std::uint8_t>(v >> 16));
        mData.push_back(static_cast<std::uint8_t>(v >> 8));
        mData.push_back(static_cast<std::uint8_t>(v));
    }

    void write_u64(std::uint64_t v) {
        write_u32(static_cast<std::uint32_t>(v >> 32));
        write_u32(static_cast<std::uint32_t>(v));
    }

    void write_s16(std::int16_t v) { write_u16(static_cast<std::uint16_t>(v)); }

    void write_s32(std::int32_t v) { write_u32(static_cast<std::uint32_t>(v)); }

    void write_f32(float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        write_u32(bits);
    }

    /// Length-prefixed (u16) UTF-8 string.
    void write_string(const std::string& v) {
        const std::size_t len = v.size() > 0xFFFF ? 0xFFFF : v.size();
        write_u16(static_cast<std::uint16_t>(len));
        mData.insert(mData.end(), v.begin(), v.begin() + static_cast<std::ptrdiff_t>(len));
    }

    const std::vector<std::uint8_t>& data() const { return mData; }
    std::size_t size() const { return mData.size(); }
    void clear() { mData.clear(); }

private:
    std::vector<std::uint8_t> mData;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : mData(data), mSize(size) {}

    bool read_u8(std::uint8_t& out) {
        if (!require(1)) {
            return false;
        }
        out = mData[mPos++];
        return true;
    }

    bool read_u16(std::uint16_t& out) {
        if (!require(2)) {
            return false;
        }
        out = static_cast<std::uint16_t>(mData[mPos] << 8 | mData[mPos + 1]);
        mPos += 2;
        return true;
    }

    bool read_u32(std::uint32_t& out) {
        if (!require(4)) {
            return false;
        }
        out = static_cast<std::uint32_t>(mData[mPos]) << 24 |
              static_cast<std::uint32_t>(mData[mPos + 1]) << 16 |
              static_cast<std::uint32_t>(mData[mPos + 2]) << 8 |
              static_cast<std::uint32_t>(mData[mPos + 3]);
        mPos += 4;
        return true;
    }

    bool read_u64(std::uint64_t& out) {
        std::uint32_t hi = 0;
        std::uint32_t lo = 0;
        if (!read_u32(hi) || !read_u32(lo)) {
            return false;
        }
        out = static_cast<std::uint64_t>(hi) << 32 | lo;
        return true;
    }

    bool read_s16(std::int16_t& out) {
        std::uint16_t v = 0;
        if (!read_u16(v)) {
            return false;
        }
        out = static_cast<std::int16_t>(v);
        return true;
    }

    bool read_s32(std::int32_t& out) {
        std::uint32_t v = 0;
        if (!read_u32(v)) {
            return false;
        }
        out = static_cast<std::int32_t>(v);
        return true;
    }

    bool read_f32(float& out) {
        std::uint32_t bits = 0;
        if (!read_u32(bits)) {
            return false;
        }
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    bool read_string(std::string& out) {
        std::uint16_t len = 0;
        if (!read_u16(len) || !require(len)) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(mData + mPos), len);
        mPos += len;
        return true;
    }

    /// True if every read so far succeeded. Check this once after parsing a whole packet.
    bool ok() const { return mOk; }
    std::size_t remaining() const { return mSize - mPos; }

private:
    bool require(std::size_t n) {
        if (!mOk || mSize - mPos < n) {
            mOk = false;
            return false;
        }
        return true;
    }

    const std::uint8_t* mData = nullptr;
    std::size_t mSize = 0;
    std::size_t mPos = 0;
    bool mOk = true;
};

}  // namespace dusk::mp
