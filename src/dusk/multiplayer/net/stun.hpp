#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Minimal STUN client codec (RFC 5389, Binding method only), used to learn this machine's public
 * IPv4 endpoint as seen from outside its NAT.
 *
 * Pure functions, no sockets: EnetTransport sends the request from ENet's own socket and hands
 * the reply back here from its intercept callback, so the NAT mapping STUN reports is the very
 * one ENet's traffic uses. See .claude/future-plans/mp-internet-play.md, option (B).
 */

namespace dusk::mp::stun {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::uint32_t kMagicCookie = 0x2112A442u;

using TransactionId = std::array<std::uint8_t, 12>;

/// A fresh random 96-bit transaction id. Retransmits of one request must reuse the same id.
TransactionId make_transaction_id();

/// The 20-byte Binding Request for @p tid (no attributes).
std::vector<std::uint8_t> build_binding_request(const TransactionId& tid);

/// Cheap test for "this datagram is a well-formed STUN message": top two bits zero, the magic
/// cookie at offset 4, and a length field that matches the datagram. ENet's own packets cannot
/// pass this by accident in practice, so the transport may consume anything it accepts.
bool looks_like_stun(const std::uint8_t* data, std::size_t size);

/**
 * Parse a Binding Success Response. Returns true only if @p data is a success response carrying
 * @p tid and an IPv4 XOR-MAPPED-ADDRESS (or, from older servers, MAPPED-ADDRESS). On success
 * @p outIp is the IPv4 address in HOST byte order and @p outPort the port.
 */
bool parse_binding_response(const std::uint8_t* data, std::size_t size, const TransactionId& tid,
    std::uint32_t* outIp, std::uint16_t* outPort);

}  // namespace dusk::mp::stun
