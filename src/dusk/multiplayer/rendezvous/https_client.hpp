#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

namespace dusk::mp::rdv {

struct HttpResponse {
    /// HTTP status code, or 0 when no response was received (then @c error says why).
    int status = 0;
    std::string body;
    /// Empty on success. Set for network, TLS, certificate, timeout and malformed-response
    /// failures.
    std::string error;
};

/**
 * Minimal blocking HTTPS/1.1 request over mbedTLS, for the rendezvous client's worker thread.
 *
 * - @p url must be `https://host[:port]/path?query`; plain http is refused.
 * - The server certificate must chain to one of the embedded roots (ca_roots.hpp) and match the
 *   host name (SNI + name check). There is no "insecure" switch.
 * - @p jsonBody is sent with `Content-Type: application/json` when non-empty, or always for POST.
 * - Sends `Connection: close`. Handles Content-Length, chunked and read-to-EOF bodies, capped at
 *   64 KiB.
 * - @p timeoutMs bounds the TLS handshake and the whole response. It does NOT bound DNS or the TCP
 *   connect, which use the OS defaults (mbedtls_net_connect has no timeout); callers that must not
 *   hang therefore run this off the game thread.
 * - @p cancel, if given, is polled about every 250 ms; setting it aborts the request with
 *   error "cancelled".
 */
HttpResponse https_request(const std::string& method, const std::string& url,
    const std::string& jsonBody, int timeoutMs, const std::atomic<bool>* cancel = nullptr);

/// Splits `https://host[:port]/path?query` into its parts (@p target is path + query, "/" if
/// absent).
bool parse_https_url(const std::string& url, std::string* host, std::string* port,
    std::string* target, std::string* error);

/// Cryptographically random bytes from PSA. False if the TLS library failed to initialise.
bool random_bytes(unsigned char* out, std::size_t n);

/**
 * One verified TLS connection, for callers that keep a connection open (the host's WebSocket).
 * Same certificate rules as https_request.
 *
 * Thread-safety: one TlsStream belongs to one thread. Across threads, every mbedTLS call made by
 * any TlsStream is serialised behind one process-wide lock (mbedTLS is built without
 * MBEDTLS_THREADING_C and PSA crypto keeps global state). The lock is held per call, never for the
 * life of a connection, so an idle open socket does not block other requests.
 */
class TlsStream {
public:
    TlsStream();
    ~TlsStream();
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    /// TCP connect + TLS handshake. @p timeoutMs bounds the handshake (not DNS/connect, see above).
    bool connect(const std::string& host, const std::string& port, int timeoutMs,
        const std::atomic<bool>* cancel, std::string* error);
    bool write_all(const void* data, std::size_t size, int timeoutMs,
        const std::atomic<bool>* cancel, std::string* error);

    static constexpr int kReadTimedOut = 0;
    static constexpr int kReadEof = -1;
    static constexpr int kReadError = -2;
    /// Waits at most ~250 ms. Returns bytes read (>0), kReadTimedOut, kReadEof, or kReadError
    /// (with @p error set).
    int read_some(void* buf, std::size_t size, std::string* error);
    /// Sends close_notify (best effort) and closes the socket. Also done by the destructor.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace dusk::mp::rdv
