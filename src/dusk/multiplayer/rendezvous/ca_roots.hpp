#pragma once

#include <span>

namespace dusk::mp::rdv {

/// One embedded trust anchor. @c pem is a NUL-terminated PEM block, ready for
/// mbedtls_x509_crt_parse(chain, pem, strlen(pem) + 1).
struct CaRoot {
    const char* name;
    const char* pem;
};

/// The root certificates the rendezvous HTTPS client trusts: the CAs Cloudflare's edge
/// certificates chain to. Nothing else is trusted; the OS store is deliberately not used, so the
/// behaviour is the same on all five platforms.
std::span<const CaRoot> ca_roots();

}  // namespace dusk::mp::rdv
