#include "stun.hpp"

#include <chrono>
#include <cstring>
#include <random>

namespace dusk::mp::stun {
namespace {

constexpr std::uint16_t kBindingRequest = 0x0001;
constexpr std::uint16_t kBindingSuccess = 0x0101;

constexpr std::uint16_t kAttrMappedAddress = 0x0001;
constexpr std::uint16_t kAttrXorMappedAddress = 0x0020;
/// Pre-RFC 5389 servers (draft-ietf-behave-rfc3489bis) sent XOR-MAPPED-ADDRESS under this type.
constexpr std::uint16_t kAttrXorMappedAddressOld = 0x8020;

constexpr std::uint8_t kFamilyIPv4 = 0x01;

std::uint16_t read_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t read_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void write_be16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

void write_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

}  // namespace

TransactionId make_transaction_id() {
    // random_device alone is deterministic on some old toolchains, so the clock is mixed in. The
    // id only has to be unguessable enough that a stray datagram cannot pose as our reply.
    std::random_device rd;
    const auto now =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::seed_seq seed{
        rd(), rd(), rd(), static_cast<unsigned>(now), static_cast<unsigned>(now >> 32)};
    std::mt19937 gen(seed);

    TransactionId tid{};
    for (std::size_t i = 0; i < tid.size(); i += 4) {
        const std::uint32_t r = gen();
        std::memcpy(tid.data() + i, &r, 4);
    }
    return tid;
}

std::vector<std::uint8_t> build_binding_request(const TransactionId& tid) {
    std::vector<std::uint8_t> out(kHeaderSize, 0);
    write_be16(out.data(), kBindingRequest);
    write_be16(out.data() + 2, 0);  // message length: no attributes
    write_be32(out.data() + 4, kMagicCookie);
    std::memcpy(out.data() + 8, tid.data(), tid.size());
    return out;
}

bool looks_like_stun(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < kHeaderSize) {
        return false;
    }
    if ((data[0] & 0xC0) != 0) {
        return false;
    }
    if (read_be32(data + 4) != kMagicCookie) {
        return false;
    }
    const std::uint16_t length = read_be16(data + 2);
    return (length % 4) == 0 && kHeaderSize + length == size;
}

bool parse_binding_response(const std::uint8_t* data, std::size_t size, const TransactionId& tid,
    std::uint32_t* outIp, std::uint16_t* outPort) {
    if (!looks_like_stun(data, size)) {
        return false;
    }
    if (read_be16(data) != kBindingSuccess) {
        return false;
    }
    if (std::memcmp(data + 8, tid.data(), tid.size()) != 0) {
        return false;
    }

    bool haveXor = false;
    bool havePlain = false;
    std::uint32_t xorIp = 0, plainIp = 0;
    std::uint16_t xorPort = 0, plainPort = 0;

    const std::size_t end = size;  // looks_like_stun checked the length field against this
    std::size_t pos = kHeaderSize;
    while (pos + 4 <= end) {
        const std::uint16_t type = read_be16(data + pos);
        const std::uint16_t length = read_be16(data + pos + 2);
        const std::size_t valueAt = pos + 4;
        if (valueAt + length > end) {
            break;  // truncated attribute: ignore the rest
        }
        const std::uint8_t* value = data + valueAt;

        // Both address attributes: 1 reserved byte, family, port, then a 4-byte IPv4 address.
        const bool ipv4 = length >= 8 && value[1] == kFamilyIPv4;
        const bool isXor = type == kAttrXorMappedAddress || type == kAttrXorMappedAddressOld;
        if (ipv4 && isXor && !haveXor) {
            xorPort = static_cast<std::uint16_t>(read_be16(value + 2) ^ (kMagicCookie >> 16));
            xorIp = read_be32(value + 4) ^ kMagicCookie;
            haveXor = true;
        } else if (ipv4 && type == kAttrMappedAddress && !havePlain) {
            plainPort = read_be16(value + 2);
            plainIp = read_be32(value + 4);
            havePlain = true;
        }

        // Attribute values are padded to a 4-byte boundary.
        pos = valueAt + ((static_cast<std::size_t>(length) + 3) & ~static_cast<std::size_t>(3));
    }

    if (haveXor) {
        *outIp = xorIp;
        *outPort = xorPort;
        return true;
    }
    if (havePlain) {
        *outIp = plainIp;
        *outPort = plainPort;
        return true;
    }
    return false;
}

}  // namespace dusk::mp::stun
