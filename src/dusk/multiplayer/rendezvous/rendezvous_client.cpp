#include "rendezvous_client.hpp"

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>

#include "dusk/logging.h"
#include "https_client.hpp"

namespace dusk::mp::rdv {
namespace {

constexpr borealis::Log Log{"dusk::mp::rdv"};

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

constexpr int kRequestTimeoutMs = 10000;
constexpr int kCloseTimeoutMs = 5000;
/// Destructor's best-effort DELETE budget, and how long the destructor waits for the worker.
constexpr int kShutdownDeleteMs = 2000;
constexpr auto kShutdownJoinWait = std::chrono::milliseconds(3000);
/// Host socket: client ping cadence, and the silence after which the socket is presumed dead. The
/// server's runtime answers every ping, so 35 s without a byte means the path is gone.
constexpr auto kWsPingInterval = std::chrono::seconds(30);
constexpr auto kWsSilenceLimit = std::chrono::seconds(35);
constexpr int kWsBackoffFirstS = 2;
constexpr int kWsBackoffCapS = 30;
constexpr std::size_t kWsMaxMessage = 64 * 1024;
constexpr std::size_t kMaxCandidates = 16;
constexpr std::size_t kMaxGuestsPerMessage = 16;
constexpr std::size_t kMaxNameBytes = 32;
constexpr std::string_view kCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// First 4 characters of a secret, for logs.
std::string redact(const std::string& s) {
    return s.substr(0, 4) + "...";
}

bool is_hex(const std::string& s, std::size_t len) {
    return s.size() == len &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string jstr(const json& o, const char* key) {
    if (!o.is_object()) {
        return {};
    }
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/// Strips control bytes and caps the length without splitting a UTF-8 sequence. Names come from
/// the other player via the server, so they are untrusted display text.
std::string sanitize_name(const std::string& in) {
    std::string out;
    for (const char c : in) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u != 0x7F) {
            out += c;
        }
    }
    if (out.size() > kMaxNameBytes) {
        std::size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
    }
    return out.empty() ? std::string("Player") : out;
}

bool plausible_ip(const std::string& ip) {
    return !ip.empty() && ip.size() <= 45 && std::all_of(ip.begin(), ip.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0 || c == '.' || c == ':';
    });
}

json encode_candidates(const std::vector<Candidate>& cands) {
    json arr = json::array();
    for (const Candidate& c : cands) {
        arr.push_back(json{{"ip", c.ip}, {"port", c.port}, {"lan", c.lan}});
    }
    return arr;
}

/// Tolerant: skips malformed entries and the server's port-0 "seen" hint (diagnostic only, it
/// cannot be connected to).
std::vector<Candidate> decode_candidates(const json& arr) {
    std::vector<Candidate> out;
    if (!arr.is_array()) {
        return out;
    }
    for (const json& e : arr) {
        if (out.size() >= kMaxCandidates) {
            break;
        }
        if (!e.is_object()) {
            continue;
        }
        Candidate c;
        c.ip = jstr(e, "ip");
        const auto port = e.find("port");
        const auto lan = e.find("lan");
        if (port == e.end() || !port->is_number_integer() || !plausible_ip(c.ip)) {
            continue;
        }
        const auto p = port->get<std::int64_t>();
        if (p <= 0 || p > 65535) {
            if (p == 0) {
                Log.debug("[mp-rdv] hint candidate {} (port unknown, not used)", c.ip);
            }
            continue;
        }
        c.port = static_cast<std::uint16_t>(p);
        c.lan = lan != e.end() && lan->is_boolean() && lan->get<bool>();
        out.push_back(std::move(c));
    }
    return out;
}

void log_candidates(const char* who, const std::vector<Candidate>& cands) {
    for (const Candidate& c : cands) {
        Log.debug("[mp-rdv]   {} candidate {}:{}{}", who, c.ip, c.port, c.lan ? " (lan)" : "");
    }
}

std::string dump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

Status map_http(const HttpResponse& r) {
    if (r.status == 0) {
        return Status::NetworkError;
    }
    if (r.status >= 200 && r.status < 300) {
        return Status::Ok;
    }
    switch (r.status) {
    case 404:
        return Status::NoRoom;
    case 409:
        return Status::Full;
    case 429:
        return Status::RateLimited;
    default:
        return Status::ServerError;
    }
}

std::string describe(const HttpResponse& r) {
    return r.status == 0 ? r.error : "HTTP " + std::to_string(r.status);
}

struct Job {
    enum class Kind { Create, Join, Close } kind;
    std::uint64_t gen = 0;
    std::vector<Candidate> cands;
    int maxPlayers = 0;
    std::string code, name, secret;
};

enum class WsOutcome { Interrupted, Retry, RoomGone, Replaced };

}  // namespace

struct Client::Shared {
    std::string baseUrl;
    std::mutex mu;
    std::condition_variable cv;
    std::condition_variable doneCv;
    std::atomic<bool> stop{false};
    bool done = false;
    std::thread thread;
    std::deque<Job> jobs;

    // Host. hostGen invalidates in-flight work when the room is closed or replaced.
    std::uint64_t hostGen = 0;
    Status createStatus = Status::NoRoom;
    bool hosting = false;
    std::string code, secret;
    std::vector<JoinRequest> joins;
    std::set<std::string> seenJoinIds;
    Clock::time_point wsRetryAt{};
    int wsBackoffS = 0;

    // Guest.
    std::uint64_t joinGen = 0;
    Status joinStatus = Status::NoRoom;
    std::vector<Candidate> hostCands;
    std::string token;

    void worker();
    void run_job(const Job& job);
    WsOutcome host_socket(const std::string& code, const std::string& secret, std::uint64_t gen);
    void accept_guests(const json& msg, std::uint64_t gen);
    bool still_hosting(std::uint64_t gen) {
        std::lock_guard lk(mu);
        return !stop && hosting && hostGen == gen;
    }
    bool pop_job(Job* out) {
        std::lock_guard lk(mu);
        if (jobs.empty()) {
            return false;
        }
        *out = std::move(jobs.front());
        jobs.pop_front();
        return true;
    }
    std::string room_url(const std::string& c) const { return baseUrl + "/v1/rooms/" + c; }
    void delete_room(const std::string& c, const std::string& s, int timeoutMs,
        const std::atomic<bool>* cancel) {
        const HttpResponse r =
            https_request("DELETE", room_url(c) + "?secret=" + s, "", timeoutMs, cancel);
        if (r.status == 204 || r.status == 200 || r.status == 404) {
            Log.info("[mp-rdv] room {} deleted", c);
        } else {
            Log.warn("[mp-rdv] deleting room {} failed: {}", c, describe(r));
        }
    }
};

const char* status_name(Status s) {
    switch (s) {
    case Status::Pending:
        return "Pending";
    case Status::Ok:
        return "Ok";
    case Status::NoRoom:
        return "NoRoom";
    case Status::Full:
        return "Full";
    case Status::RateLimited:
        return "RateLimited";
    case Status::NetworkError:
        return "NetworkError";
    case Status::ServerError:
        return "ServerError";
    }
    return "?";
}

std::string normalize_code(std::string typed) {
    std::string out;
    for (const char ch : typed) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (c == '-' || c == ' ') {
            continue;
        }
        if (c == 'O') {
            c = '0';
        } else if (c == 'I' || c == 'L') {
            c = '1';
        }
        if (kCodeAlphabet.find(c) == std::string_view::npos) {
            return {};
        }
        out += c;
    }
    return out.size() == 6 ? out : std::string();
}

// ---------------------------------------------------------------------------------------------
// Public API (any thread)

Client::Client(std::string baseUrl) : m(std::make_shared<Shared>()) {
    while (!baseUrl.empty() && baseUrl.back() == '/') {
        baseUrl.pop_back();
    }
    std::string host, port, target, error;
    if (!parse_https_url(baseUrl + "/", &host, &port, &target, &error)) {
        Log.error("[mp-rdv] rendezvous URL '{}' is unusable: {}", baseUrl, error);
    }
    m->baseUrl = std::move(baseUrl);
    m->thread = std::thread([s = m] { s->worker(); });
}

Client::~Client() {
    {
        std::lock_guard lk(m->mu);
        m->stop = true;
    }
    m->cv.notify_all();
    std::unique_lock lk(m->mu);
    const bool finished = m->doneCv.wait_for(lk, kShutdownJoinWait, [this] { return m->done; });
    lk.unlock();
    if (finished) {
        m->thread.join();
    } else {
        // Stuck in DNS or a TCP connect, which have no timeout. The thread owns a reference to
        // the shared state, so letting it finish on its own is safe.
        Log.warn("[mp-rdv] worker still busy at shutdown; detaching it");
        m->thread.detach();
    }
}

void Client::create_room(std::vector<Candidate> c, int maxPlayers) {
    {
        std::lock_guard lk(m->mu);
        ++m->hostGen;
        if (!m->code.empty() && !m->secret.empty()) {
            Job close{Job::Kind::Close};
            close.code = m->code;
            close.secret = m->secret;
            m->jobs.push_back(std::move(close));
        }
        m->hosting = false;
        m->code.clear();
        m->secret.clear();
        m->joins.clear();
        m->seenJoinIds.clear();
        m->createStatus = Status::Pending;
        Job job{Job::Kind::Create};
        job.gen = m->hostGen;
        job.cands = std::move(c);
        job.maxPlayers = maxPlayers;
        m->jobs.push_back(std::move(job));
    }
    m->cv.notify_all();
}

Status Client::create_status(std::string* code) const {
    std::lock_guard lk(m->mu);
    if (code != nullptr && m->createStatus == Status::Ok) {
        *code = m->code;
    }
    return m->createStatus;
}

std::vector<JoinRequest> Client::take_joins() {
    std::lock_guard lk(m->mu);
    std::vector<JoinRequest> out;
    out.swap(m->joins);
    return out;
}

void Client::join(std::string code, std::string name, std::vector<Candidate> c) {
    {
        std::lock_guard lk(m->mu);
        ++m->joinGen;
        m->hostCands.clear();
        m->token.clear();
        std::string norm = normalize_code(std::move(code));
        if (norm.empty()) {
            m->joinStatus = Status::NoRoom;
            Log.info("[mp-rdv] join code is not a valid code; not asking the server");
            return;
        }
        m->joinStatus = Status::Pending;
        Job job{Job::Kind::Join};
        job.gen = m->joinGen;
        job.code = std::move(norm);
        job.name = std::move(name);
        job.cands = std::move(c);
        m->jobs.push_back(std::move(job));
    }
    m->cv.notify_all();
}

Status Client::join_status(std::vector<Candidate>* host, std::string* token) const {
    std::lock_guard lk(m->mu);
    if (m->joinStatus == Status::Ok) {
        if (host != nullptr) {
            *host = m->hostCands;
        }
        if (token != nullptr) {
            *token = m->token;
        }
    }
    return m->joinStatus;
}

void Client::close_room() {
    {
        std::lock_guard lk(m->mu);
        ++m->hostGen;
        if (!m->code.empty() && !m->secret.empty()) {
            Job close{Job::Kind::Close};
            close.code = m->code;
            close.secret = m->secret;
            m->jobs.push_back(std::move(close));
        }
        m->hosting = false;
        m->code.clear();
        m->secret.clear();
        m->joins.clear();
        m->createStatus = Status::NoRoom;
    }
    m->cv.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Worker thread

void Client::Shared::worker() {
    for (;;) {
        std::unique_lock lk(mu);
        if (stop) {
            break;
        }
        if (!jobs.empty()) {
            Job job = std::move(jobs.front());
            jobs.pop_front();
            lk.unlock();
            run_job(job);
            continue;
        }
        if (hosting && Clock::now() >= wsRetryAt) {
            const std::string c = code, s = secret;
            const std::uint64_t gen = hostGen;
            lk.unlock();
            const WsOutcome outcome = host_socket(c, s, gen);
            lk.lock();
            if (hostGen != gen || stop) {
                continue;
            }
            if (outcome == WsOutcome::Retry) {
                wsBackoffS =
                    wsBackoffS == 0 ? kWsBackoffFirstS : std::min(wsBackoffS * 2, kWsBackoffCapS);
                wsRetryAt = Clock::now() + std::chrono::seconds(wsBackoffS);
                Log.info("[mp-rdv] host socket for room {} reconnecting in {} s", c, wsBackoffS);
            } else if (outcome == WsOutcome::RoomGone || outcome == WsOutcome::Replaced) {
                hosting = false;
                secret.clear();
                createStatus = Status::ServerError;
            }
            continue;
        }
        if (hosting) {
            cv.wait_until(lk, wsRetryAt);
        } else {
            cv.wait(lk);
        }
    }

    // Shutdown: delete the hosted room and any close still queued, within one 2 s budget.
    std::vector<std::pair<std::string, std::string>> rooms;
    {
        std::lock_guard lk(mu);
        if (hosting && !code.empty() && !secret.empty()) {
            rooms.emplace_back(code, secret);
        }
        for (const Job& j : jobs) {
            if (j.kind == Job::Kind::Close) {
                rooms.emplace_back(j.code, j.secret);
            }
        }
        jobs.clear();
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(kShutdownDeleteMs);
    for (const auto& [c, s] : rooms) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            break;
        }
        delete_room(c, s, static_cast<int>(left), nullptr);
    }
    {
        std::lock_guard lk(mu);
        done = true;
    }
    doneCv.notify_all();
}

void Client::Shared::run_job(const Job& job) {
    switch (job.kind) {
    case Job::Kind::Create: {
        const json body = {
            {"v", 1}, {"candidates", encode_candidates(job.cands)}, {"maxPlayers", job.maxPlayers}};
        Log.info("[mp-rdv] creating room ({} candidates, max {} players)", job.cands.size(),
            job.maxPlayers);
        log_candidates("our", job.cands);
        const HttpResponse r =
            https_request("POST", baseUrl + "/v1/rooms", dump(body), kRequestTimeoutMs, &stop);
        Status st = map_http(r);
        std::string newCode, newSecret;
        if (st == Status::Ok) {
            const json j = json::parse(r.body, nullptr, false);
            newCode = normalize_code(jstr(j, "code"));
            newSecret = jstr(j, "hostSecret");
            if (newCode.empty() || !is_hex(newSecret, 32)) {
                Log.warn("[mp-rdv] create: malformed reply from server");
                st = Status::ServerError;
            }
        } else {
            Log.warn("[mp-rdv] create failed: {} -> {}", describe(r), status_name(st));
        }
        bool stale = false;
        {
            std::lock_guard lk(mu);
            stale = job.gen != hostGen;
            if (!stale) {
                createStatus = st;
                if (st == Status::Ok) {
                    code = newCode;
                    secret = newSecret;
                    hosting = true;
                    wsBackoffS = 0;
                    wsRetryAt = Clock::now();
                }
            }
        }
        if (st == Status::Ok) {
            if (stale) {
                // Closed or replaced while the create was in flight: don't leave it alive.
                Log.info("[mp-rdv] room {} was cancelled before it was created; deleting", newCode);
                delete_room(newCode, newSecret, kCloseTimeoutMs, &stop);
            } else {
                Log.info("[mp-rdv] room {} created (secret {})", newCode, redact(newSecret));
            }
        }
        break;
    }
    case Job::Kind::Join: {
        const json body = {
            {"v", 1}, {"name", job.name}, {"candidates", encode_candidates(job.cands)}};
        Log.info("[mp-rdv] joining room {} ({} candidates)", job.code, job.cands.size());
        log_candidates("our", job.cands);
        const HttpResponse r = https_request(
            "POST", room_url(job.code) + "/join", dump(body), kRequestTimeoutMs, &stop);
        Status st = map_http(r);
        std::vector<Candidate> hc;
        std::string tok;
        if (st == Status::Ok) {
            const json j = json::parse(r.body, nullptr, false);
            tok = jstr(j, "token");
            if (j.is_object() && j.contains("host") && j["host"].is_object() &&
                j["host"].contains("candidates"))
            {
                hc = decode_candidates(j["host"]["candidates"]);
            }
            if (!is_hex(tok, 32)) {
                Log.warn("[mp-rdv] join: malformed reply from server");
                st = Status::ServerError;
            } else {
                Log.info("[mp-rdv] joined room {}: {} host candidates, token {}", job.code,
                    hc.size(), redact(tok));
                log_candidates("host", hc);
            }
        } else {
            Log.warn("[mp-rdv] join {} failed: {} -> {}", job.code, describe(r), status_name(st));
        }
        std::lock_guard lk(mu);
        if (job.gen == joinGen) {
            joinStatus = st;
            if (st == Status::Ok) {
                hostCands = std::move(hc);
                token = std::move(tok);
            }
        }
        break;
    }
    case Job::Kind::Close:
        delete_room(job.code, job.secret, kCloseTimeoutMs, &stop);
        break;
    }
}

void Client::Shared::accept_guests(const json& msg, std::uint64_t gen) {
    const auto it = msg.find("guests");
    if (it == msg.end() || !it->is_array()) {
        return;
    }
    std::size_t n = 0;
    for (const json& g : *it) {
        if (++n > kMaxGuestsPerMessage) {
            break;
        }
        JoinRequest jr;
        jr.joinId = jstr(g, "joinId");
        jr.token = jstr(g, "token");
        jr.name = sanitize_name(jstr(g, "name"));
        const bool idOk = !jr.joinId.empty() && jr.joinId.size() <= 64 &&
                          std::all_of(jr.joinId.begin(), jr.joinId.end(), [](unsigned char c) {
                              return std::isalnum(c) != 0 || c == '-' || c == '_';
                          });
        if (!idOk || !is_hex(jr.token, 32)) {
            Log.warn("[mp-rdv] ignoring a malformed guest entry");
            continue;
        }
        if (g.is_object() && g.contains("candidates")) {
            jr.candidates = decode_candidates(g["candidates"]);
        }
        std::lock_guard lk(mu);
        if (hostGen != gen || !seenJoinIds.insert(jr.joinId).second) {
            continue;
        }
        Log.info("[mp-rdv] guest '{}' wants to join (joinId {}, {} candidates, token {})", jr.name,
            jr.joinId, jr.candidates.size(), redact(jr.token));
        log_candidates("guest", jr.candidates);
        joins.push_back(std::move(jr));
    }
}

namespace {

bool ws_send(TlsStream& ts, int opcode, std::string_view payload, std::string* error) {
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    const std::size_t len = payload.size();
    if (len < 126) {
        frame += static_cast<char>(0x80 | len);
    } else if (len <= 0xFFFF) {
        frame += static_cast<char>(0x80 | 126);
        frame += static_cast<char>(len >> 8);
        frame += static_cast<char>(len & 0xFF);
    } else {
        *error = "frame too large";
        return false;
    }
    unsigned char mask[4];
    if (!random_bytes(mask, sizeof(mask))) {
        *error = "no randomness for the frame mask";
        return false;
    }
    frame.append(reinterpret_cast<const char*>(mask), 4);
    for (std::size_t i = 0; i < len; ++i) {
        frame += static_cast<char>(payload[i] ^ mask[i & 3]);
    }
    return ts.write_all(frame.data(), frame.size(), 5000, nullptr, error);
}

struct WsFrame {
    bool fin = false;
    int opcode = 0;
    std::string payload;
};

/// 1 = one frame parsed and removed from @p buf, 0 = need more bytes, -1 = protocol violation.
int ws_parse(std::string& buf, WsFrame* f) {
    if (buf.size() < 2) {
        return 0;
    }
    const auto b = [&](std::size_t i) { return static_cast<unsigned char>(buf[i]); };
    f->fin = (b(0) & 0x80) != 0;
    f->opcode = b(0) & 0x0F;
    const bool masked = (b(1) & 0x80) != 0;
    std::uint64_t len = b(1) & 0x7F;
    std::size_t pos = 2;
    if (len == 126) {
        if (buf.size() < 4) {
            return 0;
        }
        len = (std::uint64_t{b(2)} << 8) | b(3);
        pos = 4;
    } else if (len == 127) {
        if (buf.size() < 10) {
            return 0;
        }
        len = 0;
        for (std::size_t i = 2; i < 10; ++i) {
            len = (len << 8) | b(i);
        }
        pos = 10;
    }
    if (len > kWsMaxMessage || (f->opcode >= 8 && (len > 125 || !f->fin))) {
        return -1;
    }
    unsigned char mask[4] = {};
    if (masked) {
        if (buf.size() < pos + 4) {
            return 0;
        }
        for (int i = 0; i < 4; ++i) {
            mask[i] = b(pos + i);
        }
        pos += 4;
    }
    if (buf.size() < pos + len) {
        return 0;
    }
    f->payload.assign(buf, pos, static_cast<std::size_t>(len));
    if (masked) {
        for (std::size_t i = 0; i < f->payload.size(); ++i) {
            f->payload[i] = static_cast<char>(f->payload[i] ^ mask[i & 3]);
        }
    }
    buf.erase(0, pos + static_cast<std::size_t>(len));
    return 1;
}

std::string find_header(std::string_view head, std::string_view name) {
    std::size_t pos = head.find("\r\n");
    while (pos != std::string_view::npos && pos + 2 < head.size()) {
        pos += 2;
        std::size_t end = head.find("\r\n", pos);
        const std::string_view line =
            head.substr(pos, end == std::string_view::npos ? end : end - pos);
        const std::size_t colon = line.find(':');
        if (colon == name.size() && std::equal(name.begin(), name.end(), line.begin(),
                                        [](char a, char b) {
                                            return std::tolower(static_cast<unsigned char>(a)) ==
                                                   std::tolower(static_cast<unsigned char>(b));
                                        }))
        {
            std::string_view v = line.substr(colon + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
                v.remove_prefix(1);
            }
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) {
                v.remove_suffix(1);
            }
            return std::string(v);
        }
        pos = end;
    }
    return {};
}

std::string base64(const unsigned char* data, std::size_t n) {
    unsigned char out[64] = {};
    std::size_t olen = 0;
    if (mbedtls_base64_encode(out, sizeof(out), &olen, data, n) != 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(out), olen);
}

}  // namespace

WsOutcome Client::Shared::host_socket(
    const std::string& c, const std::string& s, std::uint64_t gen) {
    std::string host, port, target, err;
    if (!parse_https_url(room_url(c) + "/ws?secret=" + s, &host, &port, &target, &err)) {
        Log.error("[mp-rdv] host socket: {}", err);
        return WsOutcome::Retry;
    }
    TlsStream ts;
    if (!ts.connect(host, port, kRequestTimeoutMs, &stop, &err)) {
        Log.warn("[mp-rdv] host socket for room {}: {}", c, err);
        return WsOutcome::Retry;
    }

    unsigned char nonce[16];
    if (!random_bytes(nonce, sizeof(nonce))) {
        Log.error("[mp-rdv] host socket: no randomness");
        return WsOutcome::Retry;
    }
    const std::string key = base64(nonce, sizeof(nonce));
    std::string req = "GET " + target + " HTTP/1.1\r\n";
    req += "Host: " + host + (port == "443" ? "" : ":" + port) + "\r\n";
    req += "User-Agent: Dusklight-rdv/1\r\n";
    req += "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n\r\n";
    if (!ts.write_all(req.data(), req.size(), kRequestTimeoutMs, &stop, &err)) {
        Log.warn("[mp-rdv] host socket for room {}: {}", c, err);
        return WsOutcome::Retry;
    }

    // Upgrade response.
    std::string buf;
    char chunk[4096];
    const auto headDeadline = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
    std::size_t headEnd;
    while ((headEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        const int n = ts.read_some(chunk, sizeof(chunk), &err);
        if (n > 0) {
            buf.append(chunk, static_cast<std::size_t>(n));
            if (buf.size() > 16 * 1024) {
                Log.warn("[mp-rdv] host socket: oversized upgrade response");
                return WsOutcome::Retry;
            }
            continue;
        }
        if (n == TlsStream::kReadEof || n == TlsStream::kReadError) {
            Log.warn("[mp-rdv] host socket for room {}: {}", c,
                n == TlsStream::kReadEof ? "closed" : err);
            return WsOutcome::Retry;
        }
        if (stop) {
            return WsOutcome::Interrupted;
        }
        if (Clock::now() >= headDeadline) {
            Log.warn("[mp-rdv] host socket for room {}: no upgrade response", c);
            return WsOutcome::Retry;
        }
    }
    const std::string_view head = std::string_view(buf).substr(0, headEnd);
    const int status = head.size() >= 12 && head.substr(0, 7) == "HTTP/1." ?
                           std::atoi(std::string(head.substr(9, 3)).c_str()) :
                           0;
    if (status == 404) {
        Log.warn("[mp-rdv] room {} is gone (404 on the host socket); hosting ended", c);
        return WsOutcome::RoomGone;
    }
    if (status != 101) {
        Log.warn("[mp-rdv] host socket for room {}: upgrade refused (HTTP {})", c, status);
        return WsOutcome::Retry;
    }
    {
        const std::string expectIn = key + "258EAFA5-E914-47DA-95CA-C5AB0DC11B85";
        unsigned char digest[20];
        if (mbedtls_sha1(reinterpret_cast<const unsigned char*>(expectIn.data()), expectIn.size(),
                digest) != 0 ||
            find_header(head, "Sec-WebSocket-Accept") != base64(digest, sizeof(digest)))
        {
            Log.warn("[mp-rdv] host socket for room {}: bad Sec-WebSocket-Accept", c);
            return WsOutcome::Retry;
        }
    }
    buf.erase(0, headEnd + 4);
    {
        std::lock_guard lk(mu);
        wsBackoffS = 0;
    }
    Log.info("[mp-rdv] host socket open for room {}", c);

    auto lastRx = Clock::now();
    auto lastPing = lastRx;
    int msgOpcode = -1;
    std::string msg;
    for (;;) {
        // Jobs (a guest-side join, a close) run here without dropping the socket.
        Job job;
        while (pop_job(&job)) {
            run_job(job);
        }
        if (!still_hosting(gen)) {
            ws_send(ts, 0x8, std::string_view("\x03\xE8", 2), &err);  // 1000 normal closure
            return WsOutcome::Interrupted;
        }

        const int n = ts.read_some(chunk, sizeof(chunk), &err);
        const auto now = Clock::now();
        if (n == TlsStream::kReadEof || n == TlsStream::kReadError) {
            Log.warn("[mp-rdv] host socket for room {} dropped: {}", c,
                n == TlsStream::kReadEof ? "closed" : err);
            return WsOutcome::Retry;
        }
        if (n > 0) {
            lastRx = now;
            buf.append(chunk, static_cast<std::size_t>(n));
            WsFrame f;
            int pr;
            while ((pr = ws_parse(buf, &f)) == 1) {
                if (f.opcode == 0x9) {
                    ws_send(ts, 0xA, f.payload, &err);
                    continue;
                }
                if (f.opcode == 0xA) {
                    continue;
                }
                if (f.opcode == 0x8) {
                    const int code = f.payload.size() >= 2 ?
                                         (static_cast<unsigned char>(f.payload[0]) << 8) |
                                             static_cast<unsigned char>(f.payload[1]) :
                                         1005;
                    ws_send(ts, 0x8, std::string_view(f.payload).substr(0, 2), &err);
                    if (code == 4000) {
                        Log.warn("[mp-rdv] room {}: another host socket replaced this one; hosting "
                                 "ended here",
                            c);
                        return WsOutcome::Replaced;
                    }
                    if (code == 1000) {
                        Log.warn("[mp-rdv] room {} closed by the server; hosting ended", c);
                        return WsOutcome::RoomGone;
                    }
                    Log.warn(
                        "[mp-rdv] host socket for room {} closed by server (code {})", c, code);
                    return WsOutcome::Retry;
                }
                if (f.opcode == 0x1 || f.opcode == 0x2) {
                    msgOpcode = f.opcode;
                    msg = std::move(f.payload);
                } else if (f.opcode == 0x0 && msgOpcode >= 0) {
                    if (msg.size() + f.payload.size() > kWsMaxMessage) {
                        Log.warn("[mp-rdv] host socket: message over 64 KiB");
                        return WsOutcome::Retry;
                    }
                    msg += f.payload;
                } else {
                    Log.warn("[mp-rdv] host socket: unexpected opcode {}", f.opcode);
                    return WsOutcome::Retry;
                }
                if (!f.fin) {
                    continue;
                }
                const int op = msgOpcode;
                msgOpcode = -1;
                if (op != 0x1 || msg == "pong") {
                    continue;  // binary is ignored; pong is the keepalive answer
                }
                const json j = json::parse(msg, nullptr, false);
                if (jstr(j, "type") == "guests") {
                    accept_guests(j, gen);
                } else {
                    Log.debug("[mp-rdv] host socket: ignoring message type '{}'", jstr(j, "type"));
                }
            }
            if (pr < 0) {
                Log.warn("[mp-rdv] host socket for room {}: protocol error", c);
                return WsOutcome::Retry;
            }
        }
        if (now - lastPing >= kWsPingInterval) {
            lastPing = now;
            if (!ws_send(ts, 0x1, "ping", &err)) {
                Log.warn("[mp-rdv] host socket for room {}: ping failed: {}", c, err);
                return WsOutcome::Retry;
            }
        }
        if (now - lastRx > kWsSilenceLimit) {
            Log.warn("[mp-rdv] host socket for room {} silent for 35 s; reconnecting", c);
            return WsOutcome::Retry;
        }
    }
}

}  // namespace dusk::mp::rdv
