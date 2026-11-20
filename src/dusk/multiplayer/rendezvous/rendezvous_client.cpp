#include "rendezvous_client.hpp"

#include <borealis/http.hpp>
#include <borealis/ws.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <list>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>

#include "dusk/logging.h"

namespace dusk::mp::rdv {
namespace {

constexpr borealis::Log Log{"dusk::mp::rdv"};

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;
using HttpResult = borealis::http::Result;

constexpr auto kConnectTimeout = std::chrono::milliseconds(5000);
constexpr auto kRequestTimeout = std::chrono::milliseconds(10000);
constexpr std::size_t kMaxHttpBody = 64 * 1024;
/// Destructor: budget for the best-effort room close, and how long it waits for the worker.
constexpr auto kShutdownBudget = std::chrono::milliseconds(2000);
constexpr auto kShutdownJoinWait = std::chrono::milliseconds(3000);
/// Worker poll cadence while a request or the host socket is live.
constexpr auto kTick = std::chrono::milliseconds(50);
/// Host socket: client ping cadence, and the silence after which the socket is presumed dead. The
/// server answers every text "ping" with a text "pong", so 35 s without a message means the path
/// is gone.
constexpr auto kWsConnectTimeout = std::chrono::milliseconds(10000);
constexpr auto kWsPingInterval = std::chrono::seconds(30);
constexpr auto kWsSilenceLimit = std::chrono::seconds(35);
constexpr int kWsBackoffFirstS = 2;
constexpr int kWsBackoffCapS = 30;
constexpr std::size_t kWsMaxMessage = 64 * 1024;
constexpr std::size_t kMaxCandidates = 16;
constexpr std::size_t kMaxGuestsPerMessage = 16;
constexpr std::size_t kMaxNameBytes = 32;
/// Host: joinIds remembered for de-duplication; the oldest is forgotten past this.
constexpr std::size_t kMaxSeenJoinIds = 256;
constexpr std::string_view kCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// First 4 characters of a secret, for logs.
std::string redact(const std::string& s) {
    return s.substr(0, 4) + "...";
}

/// Backend error text may quote the request URL, which carries the host secret.
std::string scrub(std::string text, const std::string& secret) {
    if (secret.empty()) {
        return text;
    }
    const std::string r = redact(secret);
    for (std::size_t pos = text.find(secret); pos != std::string::npos;
        pos = text.find(secret, pos + r.size()))
    {
        text.replace(pos, secret.size(), r);
    }
    return text;
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
        // The server refuses the whole request over one non-global public candidate (a UPnP
        // "external" address behind CGNAT is 100.64/10, for one), so leave those out here.
        if (!c.lan && !is_global_unicast_v4(c.ip)) {
            Log.debug("[mp-rdv] not sending {} as a public candidate (not global unicast)", c.ip);
            continue;
        }
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
        // The host punches public candidates before anyone is let in: never toward loopback, a
        // private or link-local network, multicast or broadcast, whatever the server let through.
        if (!c.lan && !is_global_unicast_v4(c.ip)) {
            Log.debug("[mp-rdv] dropping public candidate {} (not global unicast)", c.ip);
            continue;
        }
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

const char* http_error_name(borealis::http::Error e) {
    using E = borealis::http::Error;
    switch (e) {
    case E::None:
        return "none";
    case E::NoBackend:
        return "no HTTPS backend";
    case E::InvalidUrl:
        return "invalid URL";
    case E::UnsupportedScheme:
        return "unsupported URL scheme (https:// only)";
    case E::Timeout:
        return "timed out";
    case E::TooLarge:
        return "reply too large";
    case E::Canceled:
        return "cancelled";
    case E::Io:
        return "I/O error";
    case E::Network:
        return "network error";
    }
    return "?";
}

const char* ws_error_name(borealis::ws::Error e) {
    using E = borealis::ws::Error;
    switch (e) {
    case E::None:
        return "none";
    case E::NoBackend:
        return "no WebSocket backend";
    case E::InvalidUrl:
        return "invalid URL";
    case E::UnsupportedScheme:
        return "unsupported URL scheme";
    case E::Timeout:
        return "timed out";
    case E::TooLarge:
        return "message too large";
    case E::Canceled:
        return "cancelled";
    case E::Network:
        return "network error";
    case E::Protocol:
        return "protocol error";
    case E::Handshake:
        return "handshake failed";
    }
    return "?";
}

Status map_http(const HttpResult& r) {
    if (r.error != borealis::http::Error::None) {
        return Status::NetworkError;
    }
    const int s = r.response.statusCode;
    if (s >= 200 && s < 300) {
        return Status::Ok;
    }
    switch (s) {
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

std::string describe(const HttpResult& r, const std::string& secret = {}) {
    if (r.error != borealis::http::Error::None) {
        std::string s = http_error_name(r.error);
        if (!r.message.empty()) {
            s += ": " + scrub(r.message, secret);
        }
        return s;
    }
    return "HTTP " + std::to_string(r.response.statusCode);
}

HttpResult take_result(borealis::Task<HttpResult>& task) {
    HttpResult failed;
    failed.error = borealis::http::Error::Network;
    try {
        if (auto r = task.try_take()) {
            return std::move(*r);
        }
        failed.message = "request produced no result";
    } catch (const std::exception& e) {
        failed.message = e.what();
    } catch (...) {
        failed.message = "request threw";
    }
    return failed;
}

/// Parses a create reply. Returns the final status; on Ok fills @p code and @p secret.
Status parse_create(const HttpResult& r, std::string* code, std::string* secret) {
    Status st = map_http(r);
    if (st != Status::Ok) {
        return st;
    }
    const json j = json::parse(r.response.body, nullptr, false);
    *code = normalize_code(jstr(j, "code"));
    *secret = jstr(j, "hostSecret");
    if (code->empty() || !is_hex(*secret, 32)) {
        Log.warn("[mp-rdv] create: malformed reply from server");
        return Status::ServerError;
    }
    return Status::Ok;
}

/// "host[:port]..." after the scheme: true for loopback hosts only.
bool is_loopback_authority(std::string_view rest) {
    rest = rest.substr(0, rest.find('/'));
    std::string_view host;
    if (!rest.empty() && rest.front() == '[') {
        host = rest.substr(0, rest.find(']') + 1);
    } else {
        host = rest.substr(0, rest.find(':'));
    }
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

struct Job {
    enum class Kind { Create, Join, Close, Release } kind;
    std::uint64_t gen = 0;
    std::vector<Candidate> cands;
    int maxPlayers = 0;
    std::string code, name, secret, joinId;
};

enum class WsOutcome { Retry, RoomGone, Replaced };

}  // namespace

struct Client::Shared {
    std::string baseUrl;    // "https://host[:port]", no trailing '/'
    std::string wsBaseUrl;  // the same with wss:// (ws:// for a loopback http:// test URL)
    bool plaintextLoopback = false;
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
    std::deque<std::string> seenJoinOrder;  // oldest first, to keep seenJoinIds bounded
    Clock::time_point wsRetryAt{};
    int wsBackoffS = 0;

    // Guest.
    std::uint64_t joinGen = 0;
    Status joinStatus = Status::NoRoom;
    std::vector<Candidate> hostCands;
    std::string token;

    // Worker thread only: no other thread touches a borealis handle.
    struct InFlight {
        Job job;
        borealis::Task<HttpResult> task;
    };
    std::list<InFlight> inflight;
    struct HostSocket {
        borealis::ws::Connection conn;
        std::uint64_t gen = 0;
        std::string code, secret;
        bool open = false;
        Clock::time_point lastRx{}, lastPing{};
    };
    HostSocket ws;

    void worker();
    void shutdown();
    void start_job(Job job);
    void finish_job(const Job& job, const HttpResult& r);
    void pump_requests();
    void drive_host_socket();
    WsOutcome classify_close(const borealis::ws::Event& ev) const;
    void end_socket(WsOutcome outcome, std::uint64_t gen, const std::string& c);
    void accept_guests(const json& msg, std::uint64_t gen);
    std::string room_url(const std::string& c) const { return baseUrl + "/v1/rooms/" + c; }
};

bool is_global_unicast_v4(const std::string& ip) {
    std::uint32_t addr = 0;
    std::size_t i = 0;
    for (int octet = 0; octet < 4; ++octet) {
        if (octet > 0) {
            if (i >= ip.size() || ip[i] != '.') {
                return false;
            }
            ++i;
        }
        const std::size_t start = i;
        std::uint32_t v = 0;
        while (i < ip.size() && i - start < 3 && ip[i] >= '0' && ip[i] <= '9') {
            v = v * 10 + static_cast<std::uint32_t>(ip[i] - '0');
            ++i;
        }
        if (i == start || v > 255 || (i - start > 1 && ip[start] == '0')) {
            return false;
        }
        addr = (addr << 8) | v;
    }
    if (i != ip.size()) {
        return false;
    }
    // Same list as the Worker's NON_GLOBAL_V4 (tools/mp-rendezvous/src/index.js).
    static constexpr struct {
        std::uint32_t base;
        int bits;
    } kNonGlobal[] = {
        {0x00000000u, 8},   // 0/8
        {0x0A000000u, 8},   // 10/8
        {0x64400000u, 10},  // 100.64/10
        {0x7F000000u, 8},   // 127/8
        {0xA9FE0000u, 16},  // 169.254/16
        {0xAC100000u, 12},  // 172.16/12
        {0xC0000000u, 24},  // 192.0.0/24
        {0xC0A80000u, 16},  // 192.168/16
        {0xC6120000u, 15},  // 198.18/15
        {0xE0000000u, 4},   // 224/4
        {0xF0000000u, 4},   // 240/4 (and broadcast)
    };
    for (const auto& r : kNonGlobal) {
        const std::uint32_t mask = ~0u << (32 - r.bits);
        if ((addr & mask) == r.base) {
            return false;
        }
    }
    return true;
}

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
    constexpr std::string_view https = "https://", http = "http://";
    if (baseUrl.starts_with(https)) {
        m->wsBaseUrl = "wss://" + baseUrl.substr(https.size());
    } else if (baseUrl.starts_with(http) &&
               is_loopback_authority(std::string_view(baseUrl).substr(http.size())))
    {
        m->wsBaseUrl = "ws://" + baseUrl.substr(http.size());
        m->plaintextLoopback = true;
        Log.warn("[mp-rdv] plaintext rendezvous URL '{}' (loopback test only); note that "
                 "borealis::http accepts https:// only, so create/join requests will fail",
            baseUrl);
    } else {
        Log.error("[mp-rdv] rendezvous URL '{}' is unusable: it must start with https://", baseUrl);
    }
    if (!borealis::http::available()) {
        Log.error("[mp-rdv] no HTTPS backend on this platform; internet play is unavailable");
    }
    if (!borealis::ws::available()) {
        Log.error("[mp-rdv] no WebSocket backend on this platform; hosting by code is unavailable");
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
        // Should not happen (every borealis call is non-blocking and the shutdown budget is 2 s),
        // but the game thread must never hang here. The thread holds its own reference to the
        // shared state, so letting it finish on its own is safe.
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
        m->seenJoinOrder.clear();
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

void Client::release_join(std::string joinId) {
    {
        std::lock_guard lk(m->mu);
        if (!m->hosting || m->code.empty() || m->secret.empty() || joinId.empty()) {
            return;
        }
        Job job{Job::Kind::Release};
        job.code = m->code;
        job.secret = m->secret;
        job.joinId = std::move(joinId);
        m->jobs.push_back(std::move(job));
    }
    m->cv.notify_all();
}

// ---------------------------------------------------------------------------------------------
// Worker thread. borealis does the I/O on its own backend threads; this thread only starts
// requests, polls their Tasks and the host socket, and runs the host-socket timers.

void Client::Shared::worker() {
    for (;;) {
        std::deque<Job> fresh;
        {
            std::unique_lock lk(mu);
            const auto wake = [this] { return stop.load() || !jobs.empty(); };
            if (inflight.empty() && !hosting && !ws.conn) {
                cv.wait(lk, wake);
            } else {
                cv.wait_for(lk, kTick, wake);
            }
            if (stop) {
                break;
            }
            fresh.swap(jobs);
        }
        for (Job& job : fresh) {
            start_job(std::move(job));
        }
        pump_requests();
        drive_host_socket();
    }
    shutdown();
}

void Client::Shared::shutdown() {
    if (ws.conn) {
        ws.conn.close(1000);
    }
    ws = HostSocket{};

    // Close the hosted room and any close still queued; closes already in flight keep running.
    std::vector<Job> closes;
    {
        std::lock_guard lk(mu);
        if (hosting && !code.empty() && !secret.empty()) {
            Job j{Job::Kind::Close};
            j.code = code;
            j.secret = secret;
            closes.push_back(std::move(j));
        }
        for (Job& j : jobs) {
            if (j.kind == Job::Kind::Close) {
                closes.push_back(std::move(j));
            }
        }
        jobs.clear();
    }
    inflight.remove_if([](const InFlight& f) { return f.job.kind == Job::Kind::Join; });
    for (Job& j : closes) {
        start_job(std::move(j));
    }

    const auto deadline = Clock::now() + kShutdownBudget;
    while (!inflight.empty() && Clock::now() < deadline) {
        for (auto it = inflight.begin(); it != inflight.end();) {
            if (!it->task.ready()) {
                ++it;
                continue;
            }
            const HttpResult r = take_result(it->task);
            Job job = std::move(it->job);
            it = inflight.erase(it);
            if (job.kind == Job::Kind::Create) {
                // A create that lands during shutdown must not leave a room alive.
                Job close{Job::Kind::Close};
                if (parse_create(r, &close.code, &close.secret) == Status::Ok) {
                    Log.info(
                        "[mp-rdv] room {} was created during shutdown; closing it", close.code);
                    start_job(std::move(close));
                }
            } else {
                finish_job(job, r);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!inflight.empty()) {
        Log.warn("[mp-rdv] {} rendezvous request(s) still running at shutdown; abandoned",
            inflight.size());
    }
    inflight.clear();  // dropping a Task cancels it
    {
        std::lock_guard lk(mu);
        done = true;
    }
    doneCv.notify_all();
}

void Client::Shared::start_job(Job job) {
    if (!borealis::http::available()) {
        Log.error("[mp-rdv] no HTTPS backend on this platform; request not sent");
    }
    borealis::http::Request req;
    req.method = borealis::http::Method::Post;
    req.connectTimeout = kConnectTimeout;
    req.idleTimeout = kRequestTimeout;
    req.totalTimeout = kRequestTimeout;
    req.maxBodyBytes = kMaxHttpBody;
    switch (job.kind) {
    case Job::Kind::Create: {
        if (!borealis::ws::available()) {
            // A room we could never hear guests on is worse than no room.
            Log.error("[mp-rdv] cannot host: no WebSocket backend on this platform");
            std::lock_guard lk(mu);
            if (job.gen == hostGen) {
                createStatus = Status::NetworkError;
            }
            return;
        }
        const json body = {
            {"v", 1}, {"candidates", encode_candidates(job.cands)}, {"maxPlayers", job.maxPlayers}};
        Log.info("[mp-rdv] creating room ({} candidates, max {} players)", job.cands.size(),
            job.maxPlayers);
        log_candidates("our", job.cands);
        req.url = baseUrl + "/v1/rooms";
        req.headers.push_back({"content-type", "application/json"});
        req.body = dump(body);
        break;
    }
    case Job::Kind::Join: {
        const json body = {
            {"v", 1}, {"name", job.name}, {"candidates", encode_candidates(job.cands)}};
        Log.info("[mp-rdv] joining room {} ({} candidates)", job.code, job.cands.size());
        log_candidates("our", job.cands);
        req.url = room_url(job.code) + "/join";
        req.headers.push_back({"content-type", "application/json"});
        req.body = dump(body);
        break;
    }
    case Job::Kind::Close:
        // borealis has no DELETE; the Worker's POST .../close has the same semantics (204). The
        // secret goes in the body so it never sits in a URL.
        req.url = room_url(job.code) + "/close";
        req.headers.push_back({"content-type", "application/json"});
        req.body = dump(json{{"secret", job.secret}});
        break;
    case Job::Kind::Release:
        req.url = room_url(job.code) + "/release";
        req.headers.push_back({"content-type", "application/json"});
        req.body = dump(json{{"secret", job.secret}, {"joinId", job.joinId}});
        break;
    }
    InFlight f{std::move(job), borealis::http::start(std::move(req))};
    inflight.push_back(std::move(f));
}

void Client::Shared::pump_requests() {
    for (auto it = inflight.begin(); it != inflight.end();) {
        if (!it->task.ready()) {
            ++it;
            continue;
        }
        const HttpResult r = take_result(it->task);
        Job job = std::move(it->job);
        it = inflight.erase(it);
        finish_job(job, r);  // may append a close; std::list keeps @c it valid
    }
}

void Client::Shared::finish_job(const Job& job, const HttpResult& r) {
    switch (job.kind) {
    case Job::Kind::Create: {
        std::string newCode, newSecret;
        const Status st = parse_create(r, &newCode, &newSecret);
        if (map_http(r) != Status::Ok) {  // a malformed 2xx reply was logged by parse_create
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
                Log.info("[mp-rdv] room {} was cancelled before it was created; closing", newCode);
                Job close{Job::Kind::Close};
                close.code = newCode;
                close.secret = newSecret;
                start_job(std::move(close));
            } else {
                Log.info("[mp-rdv] room {} created (secret {})", newCode, redact(newSecret));
            }
        }
        break;
    }
    case Job::Kind::Join: {
        Status st = map_http(r);
        std::vector<Candidate> hc;
        std::string tok;
        if (st == Status::Ok) {
            const json j = json::parse(r.response.body, nullptr, false);
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
    case Job::Kind::Close: {
        const int s = r.response.statusCode;
        if (r.error == borealis::http::Error::None && (s == 204 || s == 200 || s == 404)) {
            Log.info("[mp-rdv] room {} closed", job.code);
        } else {
            Log.warn("[mp-rdv] closing room {} failed: {}", job.code, describe(r, job.secret));
        }
        break;
    }
    case Job::Kind::Release: {
        const int s = r.response.statusCode;
        if (r.error == borealis::http::Error::None && (s == 204 || s == 200 || s == 404)) {
            Log.info("[mp-rdv] room {}: released the slot of join {}", job.code, job.joinId);
        } else {
            Log.warn("[mp-rdv] room {}: releasing join {} failed: {}", job.code, job.joinId,
                describe(r, job.secret));
        }
        break;
    }
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
        seenJoinOrder.push_back(jr.joinId);
        while (seenJoinOrder.size() > kMaxSeenJoinIds) {
            seenJoinIds.erase(seenJoinOrder.front());
            seenJoinOrder.pop_front();
        }
        Log.info("[mp-rdv] guest '{}' wants to join (joinId {}, {} candidates, token {})", jr.name,
            jr.joinId, jr.candidates.size(), redact(jr.token));
        log_candidates("guest", jr.candidates);
        joins.push_back(std::move(jr));
    }
}

WsOutcome Client::Shared::classify_close(const borealis::ws::Event& ev) const {
    const std::string& c = ws.code;
    if (ev.status == 404) {
        Log.warn("[mp-rdv] room {} is gone (404 on the host socket); hosting ended", c);
        return WsOutcome::RoomGone;
    }
    if (ev.code == 4000) {
        Log.warn("[mp-rdv] room {}: another host socket replaced this one; hosting ended here", c);
        return WsOutcome::Replaced;
    }
    if (ev.code == 1000) {
        Log.warn("[mp-rdv] room {} closed by the server; hosting ended", c);
        return WsOutcome::RoomGone;
    }
    if (ev.status != 0 && ev.status != 101) {
        Log.warn("[mp-rdv] host socket for room {}: upgrade refused (HTTP {})", c, ev.status);
    } else {
        Log.warn("[mp-rdv] host socket for room {} closed: {} (code {}){}{}", c,
            ws_error_name(ev.error), ev.code, ev.message.empty() ? "" : ": ",
            scrub(ev.message, ws.secret));
    }
    return WsOutcome::Retry;
}

void Client::Shared::end_socket(WsOutcome outcome, std::uint64_t gen, const std::string& c) {
    std::lock_guard lk(mu);
    if (hostGen != gen || stop) {
        return;
    }
    if (outcome == WsOutcome::Retry) {
        wsBackoffS = wsBackoffS == 0 ? kWsBackoffFirstS : std::min(wsBackoffS * 2, kWsBackoffCapS);
        wsRetryAt = Clock::now() + std::chrono::seconds(wsBackoffS);
        Log.info("[mp-rdv] host socket for room {} reconnecting in {} s", c, wsBackoffS);
    } else {
        hosting = false;
        secret.clear();
        createStatus = Status::ServerError;
    }
}

void Client::Shared::drive_host_socket() {
    std::string c, s;
    std::uint64_t gen = 0;
    bool want = false;
    Clock::time_point retryAt;
    {
        std::lock_guard lk(mu);
        want = hosting && !stop;
        c = code;
        s = secret;
        gen = hostGen;
        retryAt = wsRetryAt;
    }
    if (ws.conn && (!want || ws.gen != gen)) {
        // Room closed or replaced: the close request tells the server; just drop the socket.
        ws.conn.close(1000);
        ws = HostSocket{};
    }
    if (!want) {
        return;
    }

    if (!ws.conn) {
        if (Clock::now() < retryAt) {
            return;
        }
        borealis::ws::Options o;
        o.url = wsBaseUrl + "/v1/rooms/" + c + "/ws?secret=" + s;
        o.connectTimeout = kWsConnectTimeout;
        o.keepaliveInterval = std::chrono::milliseconds(0);
        o.maxMessageBytes = kWsMaxMessage;
        o.allowPlaintext = plaintextLoopback;
        ws.gen = gen;
        ws.code = c;
        ws.secret = s;
        ws.open = false;
        ws.conn = borealis::ws::connect(std::move(o));
        if (!ws.conn) {
            Log.warn("[mp-rdv] host socket for room {}: connect returned no connection", c);
            ws = HostSocket{};
            end_socket(WsOutcome::Retry, gen, c);
            return;
        }
        Log.debug("[mp-rdv] host socket for room {} connecting", c);
    }

    borealis::ws::Event ev;
    while (ws.conn.poll(ev)) {
        switch (ev.kind) {
        case borealis::ws::Event::Kind::Open: {
            ws.open = true;
            ws.lastRx = ws.lastPing = Clock::now();
            {
                std::lock_guard lk(mu);
                wsBackoffS = 0;
            }
            Log.info("[mp-rdv] host socket open for room {}", c);
            break;
        }
        case borealis::ws::Event::Kind::Message: {
            ws.lastRx = Clock::now();
            if (ev.messageKind != borealis::ws::MessageKind::Text || ev.data == "pong") {
                break;  // binary is ignored; "pong" is the keepalive answer
            }
            const json j = json::parse(ev.data, nullptr, false);
            if (jstr(j, "type") == "guests") {
                accept_guests(j, gen);
            } else {
                Log.debug("[mp-rdv] host socket: ignoring message type '{}'", jstr(j, "type"));
            }
            break;
        }
        case borealis::ws::Event::Kind::Closed: {
            const WsOutcome outcome = classify_close(ev);
            ws = HostSocket{};
            end_socket(outcome, gen, c);
            return;
        }
        }
    }

    if (!ws.open) {
        return;  // still connecting; Options::connectTimeout bounds this
    }
    const auto now = Clock::now();
    if (now - ws.lastRx > kWsSilenceLimit) {
        Log.warn("[mp-rdv] host socket for room {} silent for 35 s; reconnecting", c);
        ws.conn.close(1000);
        ws = HostSocket{};
        end_socket(WsOutcome::Retry, gen, c);
        return;
    }
    if (now - ws.lastPing >= kWsPingInterval) {
        ws.lastPing = now;
        const borealis::ws::SendResult sr = ws.conn.send(borealis::ws::MessageKind::Text, "ping");
        if (sr != borealis::ws::SendResult::Ok) {
            Log.warn("[mp-rdv] host socket for room {}: ping not sent (result {})", c,
                static_cast<int>(sr));
            ws = HostSocket{};
            end_socket(WsOutcome::Retry, gen, c);
        }
    }
}

}  // namespace dusk::mp::rdv
