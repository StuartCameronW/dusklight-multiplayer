#pragma once

/**
 * Rendezvous client: talks to the Dusklight rendezvous service (tools/mp-rendezvous, a Cloudflare
 * Worker) so two players can find each other by a six-character join code instead of an IP.
 *
 * What it does
 *  - Host: create_room() POSTs our candidates and gets back a code + hostSecret. It then holds a
 *    WebSocket to /v1/rooms/{code}/ws (Hibernation API, so an idle host costs the free plan
 *    nothing). The server pushes {"type":"guests",...} on connect and whenever a guest joins;
 *    those arrive through take_joins(), de-duplicated by joinId. The client sends a text "ping"
 *    every 30 s, treats 35 s of silence as a dead socket and reconnects with 2/4/8/16/30 s backoff.
 *    Close code 4000 ("replaced"), 1000 ("room closed") or a 404 on connect end hosting:
 *    create_status() then reports ServerError.
 *  - Guest: join() POSTs our candidates under a code and gets back the host's candidates and the
 *    session token to put in Hello.
 *  - close_room() / the destructor close the hosted room with POST /v1/rooms/{code}/close
 *    (borealis has no DELETE; same semantics). Destructor: best effort, 2 s.
 *
 * Transport: borealis::http (HTTPS) and borealis::ws (WebSocket), i.e. the platform's native
 * stack (WinHTTP, URLSession, libcurl, Android) verifying certificates against the OS store. No
 * TLS code or CA roots of our own. If either backend is missing, calls report NetworkError and say
 * so in the log.
 *
 * Threading
 *  - Every public call returns at once. One worker thread per Client takes jobs from a queue
 *    (mutex + condition variable), starts borealis requests, and every ~50 ms polls their Tasks
 *    and the host socket and runs the ping / silence / backoff timers. All borealis handles live
 *    on that thread only. The getters read state under the mutex, so any thread may call them.
 *    Intended use: poll the getters from the game thread each frame.
 *  - The destructor stops the worker, which closes a hosted room (2 s budget), and joins it. Every
 *    borealis call is non-blocking, so the ~3 s detach fallback should never trigger.
 *
 * Configuration: @c baseUrl (e.g. "https://dusk-rdv.example.workers.dev") comes from a user
 * setting owned by the network manager, not from this file. The host socket URL is the same with
 * wss://. An http:// loopback URL (127.0.0.1 / localhost / [::1]) is accepted for tests and uses
 * plaintext ws://, but borealis::http itself only speaks https://, so create/join fail there.
 *
 * Privacy (internet-play plan §4): the token and hostSecret are never logged beyond their first 4
 * characters, and candidate IPs only at debug level.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dusk::mp::rdv {

struct Candidate {
    std::string ip;
    std::uint16_t port = 0;
    bool lan = false;
};

struct JoinRequest {
    std::string joinId, name, token;
    std::vector<Candidate> candidates;
};

enum class Status { Pending, Ok, NoRoom, Full, RateLimited, NetworkError, ServerError };

const char* status_name(Status s);

class Client {  // every call returns at once; a worker thread drives borealis
public:
    explicit Client(std::string baseUrl);
    ~Client();  // stops the thread; a hosted room is closed best-effort
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Host. Replaces (and closes) any room this client already hosts.
    void create_room(std::vector<Candidate> c, int maxPlayers);
    /// Pending until the server answers; Ok fills @p code. After close_room() it is NoRoom; if the
    /// server ends the room (404 / close 1000 / replaced 4000) it becomes ServerError.
    Status create_status(std::string* code) const;
    /// Host: drains joins pushed over the WebSocket (starts after create Ok). Each joinId once.
    std::vector<JoinRequest> take_joins();

    /// Guest. @p code is normalised with normalize_code(); an invalid code yields NoRoom without
    /// contacting the server.
    void join(std::string code, std::string name, std::vector<Candidate> c);
    Status join_status(std::vector<Candidate>* host, std::string* token) const;

    void close_room();

    struct Shared;

private:
    std::shared_ptr<Shared> m;
};

/// Upper-cases, strips '-' and spaces, maps O->0 and I/L->1; returns "" unless the result is
/// exactly 6 characters of 0123456789ABCDEFGHJKMNPQRSTVWXYZ.
std::string normalize_code(std::string typed);

}  // namespace dusk::mp::rdv
