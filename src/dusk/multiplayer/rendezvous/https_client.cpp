#include "https_client.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>

#include <fmt/format.h>

#include "ca_roots.hpp"
#include "dusk/logging.h"

namespace dusk::mp::rdv {
namespace {

constexpr borealis::Log Log{"dusk::mp::rdv"};

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 64 * 1024;
/// Raw (still chunk-encoded) body allowance on top of kMaxBodyBytes for chunk-size lines.
constexpr std::size_t kMaxRawBodyBytes = kMaxBodyBytes + 8 * 1024;
/// Granularity of the read timeout, i.e. how quickly a cancel or the deadline is noticed.
constexpr std::uint32_t kReadSliceMs = 250;

using Clock = std::chrono::steady_clock;

/**
 * Process-wide TLS state, created once and deliberately never destroyed: a worker thread that was
 * detached at shutdown may still be inside a request while static destructors run, so nothing it
 * touches may be a static with a destructor.
 */
struct TlsGlobals {
    std::mutex requestLock;
    mbedtls_x509_crt roots;
    bool ok = false;
    std::string initError;
};

TlsGlobals& globals() {
    static TlsGlobals* g = [] {
        auto* t = new TlsGlobals();
        mbedtls_x509_crt_init(&t->roots);
        const psa_status_t ps = psa_crypto_init();
        if (ps != PSA_SUCCESS) {
            t->initError = "psa_crypto_init failed (" + std::to_string(static_cast<int>(ps)) + ")";
            Log.error("[mp-rdv] {}", t->initError);
            return t;
        }
        int parsed = 0;
        for (const CaRoot& root : ca_roots()) {
            const int rc = mbedtls_x509_crt_parse(&t->roots,
                reinterpret_cast<const unsigned char*>(root.pem), std::strlen(root.pem) + 1);
            if (rc != 0) {
                Log.warn("[mp-rdv] embedded root '{}' failed to parse (-0x{:04X})", root.name, -rc);
            } else {
                ++parsed;
            }
        }
        if (parsed == 0) {
            t->initError = "no embedded root certificate parsed";
            Log.error("[mp-rdv] {}", t->initError);
            return t;
        }
        Log.debug("[mp-rdv] TLS ready, {} trusted roots", parsed);
        t->ok = true;
        return t;
    }();
    return *g;
}

std::string mbed_error(const char* what, int rc) {
    char buf[160] = {};
    mbedtls_strerror(rc, buf, sizeof(buf));
    return std::string(what) + ": " + buf + " (-0x" + fmt::format("{:04X}", -rc) + ")";
}

struct ParsedUrl {
    std::string host;
    std::string port = "443";
    std::string target = "/";  // path + query
};

bool parse_url(const std::string& url, ParsedUrl* out, std::string* error) {
    constexpr std::string_view kScheme = "https://";
    if (url.size() <= kScheme.size() || url.compare(0, kScheme.size(), kScheme) != 0) {
        *error = "URL must start with https://";
        return false;
    }
    const std::size_t hostBegin = kScheme.size();
    const std::size_t hostEnd = url.find_first_of(":/?#", hostBegin);
    out->host = url.substr(
        hostBegin, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostBegin);
    if (out->host.empty() || out->host.find('@') != std::string::npos) {
        *error = "URL has no usable host";
        return false;
    }
    std::size_t pos = hostEnd;
    if (pos != std::string::npos && url[pos] == ':') {
        const std::size_t portEnd = url.find_first_of("/?#", pos + 1);
        out->port = url.substr(
            pos + 1, portEnd == std::string::npos ? std::string::npos : portEnd - pos - 1);
        if (out->port.empty() || out->port.size() > 5 ||
            !std::all_of(out->port.begin(), out->port.end(),
                [](unsigned char c) { return std::isdigit(c); }) ||
            std::stoi(out->port) == 0 || std::stoi(out->port) > 65535)
        {
            *error = "URL has an invalid port";
            return false;
        }
        pos = portEnd;
    }
    if (pos != std::string::npos) {
        std::string rest = url.substr(pos);
        const std::size_t hash = rest.find('#');
        if (hash != std::string::npos) {
            rest.resize(hash);
        }
        if (!rest.empty() && rest[0] == '?') {
            rest.insert(rest.begin(), '/');
        }
        if (!rest.empty()) {
            out->target = rest;
        }
    }
    for (const char c : out->target) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) {
            *error = "URL contains a space or control character";
            return false;
        }
    }
    return true;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

struct ResponseHead {
    int status = 0;
    long long contentLength = -1;
    bool chunked = false;
};

/// Parses the status line and the two headers we care about. @p head is everything before the
/// blank line, without it.
bool parse_head(std::string_view head, ResponseHead* out) {
    const std::size_t lineEnd = head.find("\r\n");
    const std::string_view statusLine = head.substr(0, lineEnd);
    if (statusLine.size() < 12 || statusLine.substr(0, 7) != "HTTP/1.") {
        return false;
    }
    const std::string_view code = statusLine.substr(9, 3);
    if (!std::all_of(code.begin(), code.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return false;
    }
    out->status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
    std::size_t pos = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        std::size_t end = head.find("\r\n", pos);
        if (end == std::string_view::npos) {
            end = head.size();
        }
        const std::string_view line = head.substr(pos, end - pos);
        pos = end + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view name = trim(line.substr(0, colon));
        const std::string_view value = trim(line.substr(colon + 1));
        if (iequals(name, "Content-Length")) {
            if (value.empty() || value.size() > 12 ||
                !std::all_of(
                    value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }))
            {
                return false;
            }
            out->contentLength = std::stoll(std::string(value));
        } else if (iequals(name, "Transfer-Encoding")) {
            std::string lower(value);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            out->chunked = lower.find("chunked") != std::string::npos;
        }
    }
    return true;
}

enum class ChunkResult { NeedMore, Complete, Malformed, TooLarge };

/// Decodes a complete or partial chunked body from scratch. Re-run after every read; the body is
/// capped at 64 KiB, so re-decoding is cheap.
ChunkResult decode_chunked(std::string_view raw, std::string* out) {
    out->clear();
    std::size_t pos = 0;
    for (;;) {
        const std::size_t lineEnd = raw.find("\r\n", pos);
        if (lineEnd == std::string_view::npos) {
            return raw.size() - pos > 64 ? ChunkResult::Malformed : ChunkResult::NeedMore;
        }
        std::string_view sizeField = raw.substr(pos, lineEnd - pos);
        const std::size_t semi = sizeField.find(';');
        if (semi != std::string_view::npos) {
            sizeField = sizeField.substr(0, semi);
        }
        sizeField = trim(sizeField);
        if (sizeField.empty() || sizeField.size() > 8) {
            return ChunkResult::Malformed;
        }
        std::size_t size = 0;
        for (const char c : sizeField) {
            const int v = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' :
                          (c >= 'a' && c <= 'f')                      ? c - 'a' + 10 :
                          (c >= 'A' && c <= 'F')                      ? c - 'A' + 10 :
                                                                        -1;
            if (v < 0) {
                return ChunkResult::Malformed;
            }
            size = size * 16 + static_cast<std::size_t>(v);
        }
        pos = lineEnd + 2;
        if (size == 0) {
            // Optional trailers, then the final blank line.
            if (raw.size() - pos >= 2 && raw.substr(pos, 2) == "\r\n") {
                return ChunkResult::Complete;
            }
            return raw.find("\r\n\r\n", pos) != std::string_view::npos ? ChunkResult::Complete :
                                                                         ChunkResult::NeedMore;
        }
        if (out->size() + size > kMaxBodyBytes) {
            return ChunkResult::TooLarge;
        }
        if (raw.size() - pos < size + 2) {
            return ChunkResult::NeedMore;
        }
        if (raw.substr(pos + size, 2) != "\r\n") {
            return ChunkResult::Malformed;
        }
        out->append(raw.substr(pos, size));
        pos += size + 2;
    }
}

/// In stream mode MBEDTLS_ERR_SSL_TIMEOUT comes straight from mbedtls_net_recv_timeout and leaves
/// the record state untouched (ssl_msg.c, mbedtls_ssl_fetch_input), so it is resumable exactly like
/// WANT_READ. That is what lets every read wait in 250 ms slices and still notice a cancel.
bool is_retryable(int rc) {
    return rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
           rc == MBEDTLS_ERR_SSL_TIMEOUT || rc == MBEDTLS_ERR_SSL_ASYNC_IN_PROGRESS ||
           rc == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
           || rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
#endif
        ;
}

}  // namespace

bool parse_https_url(const std::string& url, std::string* host, std::string* port,
    std::string* target, std::string* error) {
    ParsedUrl u;
    if (!parse_url(url, &u, error)) {
        return false;
    }
    *host = std::move(u.host);
    *port = std::move(u.port);
    *target = std::move(u.target);
    return true;
}

bool random_bytes(unsigned char* out, std::size_t n) {
    TlsGlobals& g = globals();
    if (!g.ok) {
        return false;
    }
    std::lock_guard lock(g.requestLock);
    return psa_generate_random(out, n) == PSA_SUCCESS;
}

struct TlsStream::Impl {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool open = false;

    Impl() {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~Impl() {
        std::lock_guard lock(globals().requestLock);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        mbedtls_net_free(&net);
    }
};

TlsStream::TlsStream() : m(std::make_unique<Impl>()) {}

TlsStream::~TlsStream() {
    close();
}

bool TlsStream::connect(const std::string& host, const std::string& port, int timeoutMs,
    const std::atomic<bool>* cancel, std::string* error) {
    TlsGlobals& g = globals();
    if (!g.ok) {
        *error = g.initError;
        return false;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
    const auto cancelled = [&] {
        return cancel != nullptr && cancel->load(std::memory_order_relaxed);
    };

    // DNS + TCP connect touch no mbedTLS global state, so they run outside the lock.
    int rc = mbedtls_net_connect(&m->net, host.c_str(), port.c_str(), MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        *error = mbed_error("connect failed", rc);
        return false;
    }
    if (cancelled()) {
        *error = "cancelled";
        return false;
    }

    {
        std::lock_guard lock(g.requestLock);
        static constexpr char kPers[] = "dusk-mp-rdv";
        rc = mbedtls_ctr_drbg_seed(&m->drbg, mbedtls_entropy_func, &m->entropy,
            reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers) - 1);
        if (rc != 0) {
            *error = mbed_error("RNG seed failed", rc);
            return false;
        }
        rc = mbedtls_ssl_config_defaults(&m->conf, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) {
            *error = mbed_error("TLS config failed", rc);
            return false;
        }
        mbedtls_ssl_conf_authmode(&m->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&m->conf, &g.roots, nullptr);
        mbedtls_ssl_conf_rng(&m->conf, mbedtls_ctr_drbg_random, &m->drbg);
        mbedtls_ssl_conf_read_timeout(&m->conf, kReadSliceMs);
        rc = mbedtls_ssl_setup(&m->ssl, &m->conf);
        if (rc == 0) {
            rc = mbedtls_ssl_set_hostname(&m->ssl, host.c_str());
        }
        if (rc != 0) {
            *error = mbed_error("TLS setup failed", rc);
            return false;
        }
        mbedtls_ssl_set_bio(&m->ssl, &m->net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
    }

    for (;;) {
        {
            std::lock_guard lock(g.requestLock);
            rc = mbedtls_ssl_handshake(&m->ssl);
            if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                char info[256] = {};
                mbedtls_x509_crt_verify_info(
                    info, sizeof(info), "", mbedtls_ssl_get_verify_result(&m->ssl));
                std::string why(info);
                std::replace(why.begin(), why.end(), '\n', ' ');
                *error = "certificate rejected: " + std::string(trim(why));
                return false;
            }
        }
        if (rc == 0) {
            break;
        }
        if (!is_retryable(rc)) {
            *error = mbed_error("TLS handshake failed", rc);
            return false;
        }
        if (cancelled()) {
            *error = "cancelled";
            return false;
        }
        if (Clock::now() >= deadline) {
            *error = "timed out during TLS handshake";
            return false;
        }
    }
    m->open = true;
    return true;
}

bool TlsStream::write_all(const void* data, std::size_t size, int timeoutMs,
    const std::atomic<bool>* cancel, std::string* error) {
    if (!m->open) {
        *error = "not connected";
        return false;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
    const auto* p = static_cast<const unsigned char*>(data);
    std::size_t sent = 0;
    while (sent < size) {
        int rc;
        {
            std::lock_guard lock(globals().requestLock);
            rc = mbedtls_ssl_write(&m->ssl, p + sent, size - sent);
        }
        if (rc > 0) {
            sent += static_cast<std::size_t>(rc);
            continue;
        }
        if (!is_retryable(rc)) {
            *error = mbed_error("send failed", rc);
            return false;
        }
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            *error = "cancelled";
            return false;
        }
        if (Clock::now() >= deadline) {
            *error = "timed out sending";
            return false;
        }
    }
    return true;
}

int TlsStream::read_some(void* buf, std::size_t size, std::string* error) {
    if (!m->open) {
        *error = "not connected";
        return kReadError;
    }
    int rc;
    {
        std::lock_guard lock(globals().requestLock);
        rc = mbedtls_ssl_read(&m->ssl, static_cast<unsigned char*>(buf), size);
    }
    if (rc > 0) {
        return rc;
    }
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF ||
        rc == MBEDTLS_ERR_NET_CONN_RESET)
    {
        return kReadEof;
    }
    if (is_retryable(rc)) {
        return kReadTimedOut;
    }
    *error = mbed_error("receive failed", rc);
    return kReadError;
}

void TlsStream::close() {
    if (m->open) {
        std::lock_guard lock(globals().requestLock);
        mbedtls_ssl_close_notify(&m->ssl);
        m->open = false;
    }
    mbedtls_net_free(&m->net);
}

HttpResponse https_request(const std::string& method, const std::string& url,
    const std::string& jsonBody, int timeoutMs, const std::atomic<bool>* cancel) {
    HttpResponse res;
    ParsedUrl u;
    if (!parse_url(url, &u, &res.error)) {
        return res;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
    const auto remainingMs = [&] {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
    };
    const auto expired = [&] { return Clock::now() >= deadline; };
    const auto cancelled = [&] {
        return cancel != nullptr && cancel->load(std::memory_order_relaxed);
    };

    TlsStream stream;
    if (!stream.connect(u.host, u.port, timeoutMs, cancel, &res.error)) {
        return res;
    }

    // Request. Host carries the port only when it isn't the default.
    std::string req;
    req.reserve(256 + jsonBody.size());
    req += method + " " + u.target + " HTTP/1.1\r\n";
    req += "Host: " + u.host + (u.port == "443" ? "" : ":" + u.port) + "\r\n";
    req += "User-Agent: Dusklight-rdv/1\r\n";
    req += "Accept: application/json\r\n";
    req += "Connection: close\r\n";
    if (!jsonBody.empty() || method == "POST") {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n";
    }
    req += "\r\n";
    req += jsonBody;

    if (!stream.write_all(req.data(), req.size(), std::max(remainingMs(), 1), cancel, &res.error)) {
        return res;
    }

    // Response.
    std::string raw;
    std::size_t headEnd = std::string::npos;
    ResponseHead head;
    bool done = false;
    bool eof = false;
    const bool noBody = method == "HEAD";
    unsigned char buf[4096];

    const auto check_complete = [&]() -> bool {
        // Returns true when finished (success or failure recorded in res).
        if (headEnd == std::string::npos) {
            for (;;) {
                const std::size_t end = raw.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (raw.size() > kMaxHeaderBytes) {
                        res.error = "response headers too large";
                        return true;
                    }
                    return false;
                }
                head = ResponseHead{};
                if (!parse_head(std::string_view(raw).substr(0, end), &head)) {
                    res.error = "malformed HTTP response";
                    return true;
                }
                if (head.status >= 100 && head.status < 200) {
                    raw.erase(0, end + 4);  // interim response; the real one follows
                    continue;
                }
                headEnd = end + 4;
                break;
            }
        }
        const std::string_view bodyRaw = std::string_view(raw).substr(headEnd);
        if (noBody || head.status == 204 || head.status == 304) {
            res.status = head.status;
            return true;
        }
        if (head.chunked) {
            switch (decode_chunked(bodyRaw, &res.body)) {
            case ChunkResult::Complete:
                res.status = head.status;
                return true;
            case ChunkResult::Malformed:
                res.error = "malformed chunked body";
                return true;
            case ChunkResult::TooLarge:
                res.error = "response body over 64 KiB";
                return true;
            case ChunkResult::NeedMore:
                if (eof) {
                    res.error = "connection closed mid-body";
                    return true;
                }
                return false;
            }
        }
        if (head.contentLength >= 0) {
            if (static_cast<unsigned long long>(head.contentLength) > kMaxBodyBytes) {
                res.error = "response body over 64 KiB";
                return true;
            }
            if (bodyRaw.size() >= static_cast<std::size_t>(head.contentLength)) {
                res.body.assign(bodyRaw.substr(0, static_cast<std::size_t>(head.contentLength)));
                res.status = head.status;
                return true;
            }
            if (eof) {
                res.error = "connection closed mid-body";
                return true;
            }
            return false;
        }
        // Neither length nor chunked: the body runs to EOF.
        if (bodyRaw.size() > kMaxBodyBytes) {
            res.error = "response body over 64 KiB";
            return true;
        }
        if (eof) {
            res.body.assign(bodyRaw);
            res.status = head.status;
            return true;
        }
        return false;
    };

    while (!done) {
        std::string readError;
        const int n = stream.read_some(buf, sizeof(buf), &readError);
        if (n > 0) {
            raw.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
            if (raw.size() > kMaxHeaderBytes + kMaxRawBodyBytes) {
                res.error = "response too large";
                break;
            }
            done = check_complete();
            continue;
        }
        if (n == TlsStream::kReadEof) {
            eof = true;
            if (!check_complete()) {
                res.error = "connection closed before the response";
            }
            break;
        }
        if (n == TlsStream::kReadError) {
            res.error = readError;
            break;
        }
        if (cancelled()) {
            res.error = "cancelled";
            break;
        }
        if (expired()) {
            res.error = "timed out waiting for the response";
            break;
        }
    }

    if (!res.error.empty()) {
        res.status = 0;
        res.body.clear();
    }
    return res;
}

}  // namespace dusk::mp::rdv
