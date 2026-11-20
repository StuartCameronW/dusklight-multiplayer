#include "network_manager.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <aurora/card.h>
#include <borealis/io.hpp>

#include "../net/enet_transport.hpp"
#include "../net/packet.hpp"
#include "../net/serializer.hpp"
#include "../net/upnp.hpp"
#include "../policy/mp_policy.hpp"
#include "../rendezvous/rendezvous_client.hpp"
#include "../replication/enemy_sync.hpp"  // batch 8: enemies
#include "../replication/item_claims.hpp"
#include "../replication/object_channel.hpp"
#include "../replication/object_drops.hpp"  // batch 7: drops
#include "../replication/player_bridge.hpp"
#include "../replication/player_state.hpp"
#include "../replication/replication_manager.hpp"
#include "../replication/world_clock.hpp"
#include "../replication/world_state.hpp"
#include "dusk/logging.h"
#include "dusk/save_manager.hpp"
#include "dusk/settings.h"
#include "dusk/ui/ui.hpp"
#include "dusk/version.hpp"
#include "fmt/format.h"
#include "m_Do/m_Do_MemCard.h"
#include "mod_check.hpp"
#include "pause.hpp"  // batch 8: pause
#include "policy_sync.hpp"
#include "presence.hpp"
#include "save_guard.hpp"
#include "trace.hpp"
#include "world_sync.hpp"

namespace dusk::mp::world {
/// Agent S's engine-free self-test of world_state (world_state.cpp). Declared here as well as
/// wherever S declares it, so this file does not depend on which header that is.
bool selftest(std::string& failure);
}  // namespace dusk::mp::world

namespace dusk::mp {
namespace {

constexpr borealis::Log Log{"dusk::mp"};

/// Heartbeat cadence, in sim ticks. The sim runs at 30 Hz, so this is roughly once a second.
constexpr std::uint64_t kHeartbeatIntervalTicks = 30;

/// How often to rebuild our mod fingerprint and look for a change (check_mod_changes): ~1 s. A
/// guest who loads a save on a different randomizer seed is removed within about a second of the
/// load. Rebuilding is cheap: the per-bundle facts are cached (mod_check.cpp), the rest is a walk
/// over the active mods and an in-memory save-blob lookup.
constexpr std::uint64_t kModRecheckTicks = 30;

/// How often to log the measured tick rate while a session is live.
constexpr std::uint64_t kRateReportIntervalTicks = 150;

/// How often to log interpolation health (buffer depth, starvations). ~10 s at 30 Hz.
constexpr std::uint64_t kInterpReportIntervalTicks = 300;

/**
 * Client: how long to wait for the host's HelloAck before giving up on the join.
 *
 * Our own deadline rather than ENet's, because ENet's connect timeout falls out of its retransmit
 * backoff from a 500 ms initial guess at the RTT and lands somewhere near 15 s — long enough that a
 * player who typed the wrong address assumes the game froze. And it only covers "nobody answered":
 * something that accepts the connection and never replies would leave us waiting forever.
 */
constexpr std::chrono::seconds kJoinTimeout{10};

/// Host: how long a connected peer gets to send its Hello. A real client sends it the instant it
/// connects, on the reliable channel, so this only ever fires for something that is not one of us
/// and would otherwise sit in a player slot indefinitely.
constexpr std::chrono::seconds kHelloTimeout{5};

/// Extra ENet peer slots beyond the player cap. Without a spare, ENet ignores the connect of the
/// player who makes it one too many, and they wait out a timeout and are told "unreachable". With
/// one, we can accept the connection just long enough to tell them the session is full.
constexpr std::size_t kSpareConnectionSlots = 2;

// --- Internet play (mp-internet-play.md §3-§6) ---

/// How long STUN may retry before we go with whatever addresses we have.
constexpr int kStunTimeoutMs = 3000;
/// Host: the longest the candidate gathering waits on STUN and UPnP before registering the room.
constexpr std::chrono::seconds kGatherBudget{4};
/// Guest: the longest the rendezvous may take to answer a join before we call it unreachable.
constexpr std::chrono::seconds kRendezvousTimeout{20};
/// Host: a join request the rendezvous announced is forgotten if its Hello has not come by then.
constexpr std::chrono::seconds kPendingJoinLifetime{60};
/// Host: how long a join-code guest waits on Allow / Deny before it is declined on its own.
constexpr std::chrono::seconds kApprovalTimeout{30};
/// Host: how long to send DUSKPUNCH toward each of a new guest's public candidates.
constexpr int kPunchDurationMs = 6000;
/// Guest: per-candidate connect budget, LAN then public.
constexpr std::chrono::milliseconds kLanAttemptBudget{2000};
constexpr std::chrono::milliseconds kPublicAttemptBudget{6000};

const char* const kStrictRouterMessage =
    "Couldn't connect — one of you is behind a strict router. Try Tailscale (see "
    "docs/multiplayer-internet.md), or join by address under Advanced.";

}  // namespace

/// The UPnP request, run on its own thread because miniupnpc blocks for seconds. The game thread
/// only ever reads it under the lock.
struct UpnpJob {
    std::mutex mutex;
    bool done = false;
    bool ok = false;
    std::string externalIp;
    /// The session ended before the request did: the thread removes the mapping it just made.
    bool cancelled = false;
};

namespace {

/// "ABC123" -> "ABC-123", the way codes are shown and read out.
std::string display_code(const std::string& code) {
    return code.size() == 6 ? code.substr(0, 3) + "-" + code.substr(3) : code;
}

/// First four characters and an ellipsis: enough to match two log lines, never the whole secret.
std::string token_prefix(const std::string& token) {
    return token.size() <= 4 ? std::string("…") : token.substr(0, 4) + "…";
}

/// "host:port", port defaulting to the standard STUN 3478. False if there is no host.
bool parse_stun_server(const std::string& text, std::string& host, std::uint16_t& port) {
    host.clear();
    port = 3478;
    const std::size_t colon = text.rfind(':');
    host = colon == std::string::npos ? text : text.substr(0, colon);
    if (colon != std::string::npos) {
        const long parsed = std::strtol(text.c_str() + colon + 1, nullptr, 10);
        if (parsed > 0 && parsed <= 0xFFFF) {
            port = static_cast<std::uint16_t>(parsed);
        }
    }
    return !host.empty();
}

void begin_configured_stun(ITransport& transport) {
    std::string host;
    std::uint16_t port = 0;
    const std::string configured = getSettings().multiplayer.stunServer.getValue();
    if (!parse_stun_server(configured, host, port)) {
        Log.warn("[mp-net] multiplayer.stunServer '{}' has no host; skipping STUN", configured);
        return;
    }
    transport.begin_stun(host, port, kStunTimeoutMs);
}

std::string rendezvous_url() {
    const std::string url = getSettings().multiplayer.rendezvousUrl.getValue();
    return url.empty() ? std::string(kDefaultRendezvousUrl) : url;
}

/// Adds @p c unless an identical ip:port is already there (a UPnP address is often the STUN one).
void add_candidate(std::vector<rdv::Candidate>& out, rdv::Candidate c) {
    if (c.ip.empty() || c.port == 0) {
        return;
    }
    for (const auto& existing : out) {
        if (existing.ip == c.ip && existing.port == c.port) {
            return;
        }
    }
    out.push_back(std::move(c));
}

/// A Client's destructor closes a hosted room and joins its thread (up to ~2 s), which the game
/// thread must not wait for. Let a short-lived thread do it.
void destroy_rdv_async(std::unique_ptr<rdv::Client> client) {
    if (!client) {
        return;
    }
    std::thread([c = std::move(client)]() mutable { c.reset(); }).detach();
}

/// UPnP only where it is built (not phones/TVs, which only join) and the player has it on.
bool upnp_wanted() {
#ifdef DUSK_MP_HAS_UPNP
    // Loopback-only test runs have nothing for a router to forward.
    if (const char* loop = std::getenv("DUSK_MP_BIND_LOOPBACK"); loop != nullptr && loop[0] == '1')
    {
        return false;
    }
    return getSettings().multiplayer.upnp.getValue();
#else
    return false;
#endif
}

/// Heartbeats older than this are forgotten unanswered. They are broadcast, so one sequence number
/// is answered once per peer; keeping it around until it ages out is what lets every peer's reply
/// produce an RTT, not just the first one.
constexpr std::chrono::seconds kHeartbeatExpiry{10};

/**
 * The most one frame may add to the silence watchdog's clock (mAttentiveTime). Above any frame a
 * live session produces — 200 ms at the 5 Hz the sim falls to under load — so a slow sim still
 * times out on schedule, but far below a real stall, so our own 3 s hitch charges peers 1 s, not 3.
 */
constexpr std::chrono::seconds kMaxAttentiveStep{1};

/// The watchdog's deadline. The same number the transport is configured with, so the two agree on
/// what "gone" means; only the watchdog actually meets it.
constexpr std::chrono::milliseconds kPeerSilenceTimeout{kPeerSilenceTimeoutMs};

/**
 * ★ Test knobs for the two refusals, which a scripted run otherwise cannot reach: "full" needs
 * eight running copies, and "wrong version" needs two different builds. Same trade as
 * DUSK_MP_TEST_SAVE_REFUSAL — without them these paths could only be reasoned about. Read once;
 * never set in normal play.
 *
 * DUSK_MP_TEST_MAX_PLAYERS (host): lower the player cap, host included. Clamped to [2,
 * kMaxPlayers]. DUSK_MP_TEST_PROTOCOL_VERSION (client): the version this client CLAIMS in its
 * Hello.
 */
std::size_t max_players() {
    static const std::size_t value = [] {
        const char* env = std::getenv("DUSK_MP_TEST_MAX_PLAYERS");
        const long parsed = env != nullptr ? std::strtol(env, nullptr, 10) : 0;
        return parsed >= 2 && parsed <= static_cast<long>(kMaxPlayers) ?
                   static_cast<std::size_t>(parsed) :
                   kMaxPlayers;
    }();
    return value;
}

std::uint32_t advertised_protocol_version() {
    static const std::uint32_t value = [] {
        const char* env = std::getenv("DUSK_MP_TEST_PROTOCOL_VERSION");
        return env != nullptr && env[0] != '\0' ?
                   static_cast<std::uint32_t>(std::strtoul(env, nullptr, 10)) :
                   kProtocolVersion;
    }();
    return value;
}

/// DUSK_MP_TEST_LEAVE_AT_TICK: call leave_session() on this session tick, exactly as the overlay's
/// Leave button does. The button is the only other way in, and a scripted run cannot press it.
std::uint64_t test_leave_tick() {
    static const std::uint64_t value = [] {
        const char* env = std::getenv("DUSK_MP_TEST_LEAVE_AT_TICK");
        return env != nullptr ? std::strtoull(env, nullptr, 10) : 0ull;
    }();
    return value;
}

/**
 * ★ Test knob (mp-m4-m5-world.md I.6): DUSK_MP_TEST_STALL_MS=<ms>@<tick> sleeps the game thread
 * once, for <ms>, on session tick <tick>, to measure what a real stall (shader compile, the Windows
 * modal drag loop, a DVD error box) does to the far end: does it drop us, and after how long. Read
 * once; never set in normal play.
 */
struct StallKnob {
    std::uint64_t ms = 0;
    std::uint64_t tick = 0;
};

const StallKnob& test_stall() {
    static const StallKnob value = [] {
        StallKnob knob;
        const char* env = std::getenv("DUSK_MP_TEST_STALL_MS");
        if (env == nullptr || env[0] == '\0') {
            return knob;
        }
        char* end = nullptr;
        const unsigned long long ms = std::strtoull(env, &end, 10);
        if (end == nullptr || *end != '@') {
            Log.warn("DUSK_MP_TEST_STALL_MS='{}' is not <ms>@<tick>; ignored", env);
            return knob;
        }
        knob.ms = ms;
        knob.tick = std::strtoull(end + 1, nullptr, 10);
        return knob;
    }();
    return value;
}

/// Rate limit for per-packet warnings that a misbehaving peer could produce at the sim rate.
bool warn_budget() {
    static int remaining = 50;
    return remaining > 0 && remaining-- > 0;
}

/**
 * ★ Pre-session backup of the host's card (mp-m4-m5-world.md II.8 c). From M5 on, a GUEST's changes
 * are written into the HOST's real save, and the host autosaves at every stage entry and shutter
 * door. A guest bug that slips past the proposal limits would be saved one door later. With this,
 * a bad session costs that session, not the file.
 *
 * Not save_manager::create_backup, although it exists: that one prunes the Save Manager's backups
 * down to ten, and this copy must never delete or overwrite any backup. Not data.cpp's startup
 * backup either (internal to data.cpp, and it prunes too). So: one plain copy per session into
 * <card folder>/mp-backups/, named mp-backup-<yyyymmdd-hhmmss>.<gci|raw>, plus the save's mod
 * sidecar when there is one.
 *
 * Into a subfolder rather than next to the card file on purpose: a second .gci in the GCI folder
 * would be mounted as another save file by the card layer. The Save Manager's own backups/ already
 * lives in the same place.
 *
 * Best-effort, like every backup here: a failure warns and hosting goes on.
 */
void backup_host_card_for_session() {
    if (saving_disabled()) {
        // This instance writes no save at all (P0: another Dusklight holds the data directory),
        // so there is nothing a session could damage, and the card belongs to the other instance.
        Log.info("[mp-world] pre-session backup skipped: saving is disabled on this instance");
        return;
    }

    const auto& diskId = dusk::version::getDiskID();
    const std::string game{diskId.gameName, sizeof(diskId.gameName)};
    const std::string maker{diskId.company, sizeof(diskId.company)};
    const char* fileName = mDoMemCd_GetFileName();
    const std::string saveName = fileName != nullptr ? fileName : "";
    const auto cardType = aurora_card_get_type(0);
    const auto storage = save_manager::resolve_storage(game,
        cardType == AURORA_CARD_RAW_IMAGE ? save_manager::StorageKind::RawImage :
                                            save_manager::StorageKind::GciDirectory,
        0);
    if (!storage) {
        Log.warn(
            "[mp-world] pre-session backup skipped: no card storage ({})", storage.result.message);
        return;
    }

    const bool gciFolder = storage.value.kind == save_manager::StorageKind::GciDirectory;
    const std::filesystem::path source =
        gciFolder ?
            storage.value.path / borealis::io::fs_path_from_utf8(
                                     save_manager::card_file_stem(maker, game, saveName) + ".gci") :
            storage.value.path;
    const std::string extension = gciFolder ? ".gci" : ".raw";

    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        Log.info("[mp-world] pre-session backup: no card file at '{}' yet; nothing to back up",
            borealis::io::fs_path_to_string(source));
        return;
    }

    const std::filesystem::path dir =
        (gciFolder ? storage.value.path : storage.value.path.parent_path()) / "mp-backups";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        Log.warn("[mp-world] pre-session backup failed: cannot create '{}': {}",
            borealis::io::fs_path_to_string(dir), ec.message());
        return;
    }

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::array<char, 32> stamp{};
    if (std::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H%M%S", &local) == 0) {
        Log.warn("[mp-world] pre-session backup failed: could not format a timestamp");
        return;
    }

    // Never overwrite: two sessions in one second get -1, -2, ...
    const std::string stem = std::string("mp-backup-") + stamp.data();
    std::filesystem::path target = dir / (stem + extension);
    for (int n = 1; std::filesystem::exists(target, ec) && n < 1000; ++n) {
        target = dir / fmt::format("{}-{}{}", stem, n, extension);
    }
    // copy_options::none fails rather than overwrite if the name was taken in between.
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, ec);
    if (ec) {
        Log.warn("[mp-world] pre-session backup of '{}' to '{}' failed: {}",
            borealis::io::fs_path_to_string(source), borealis::io::fs_path_to_string(target),
            ec.message());
        return;
    }
    Log.info("[mp-world] pre-session backup: '{}' -> '{}'", borealis::io::fs_path_to_string(source),
        borealis::io::fs_path_to_string(target));

    // A randomizer's seed lives in the mod sidecar; a backup without it is not the whole save.
    const auto sidecar = save_manager::save_sidecar_directory(
        storage.value.path, storage.value.kind, maker, game, saveName);
    if (!std::filesystem::is_directory(sidecar, ec)) {
        return;
    }
    std::filesystem::path sidecarTarget = target;
    sidecarTarget += ".mods";
    if (std::filesystem::exists(sidecarTarget, ec)) {
        return;
    }
    std::filesystem::copy(sidecar, sidecarTarget, std::filesystem::copy_options::recursive, ec);
    if (ec) {
        Log.warn("[mp-world] pre-session backup of the mod data '{}' failed: {}",
            borealis::io::fs_path_to_string(sidecar), ec.message());
        return;
    }
    Log.info("[mp-world] pre-session backup: mod data '{}' -> '{}'",
        borealis::io::fs_path_to_string(sidecar), borealis::io::fs_path_to_string(sidecarTarget));
}

/// Both the toast and the log, always together, so a run's log shows exactly what the player saw.
void notify(const std::string& title, const std::string& content, int seconds = 5) {
    Log.info("Notify: {} — {}", title, content);
    dusk::ui::push_toast({
        .title = title,
        .content = content,
        .duration = std::chrono::seconds(seconds),
    });
}

const char* reason_text(std::uint32_t reason) {
    switch (static_cast<DisconnectReason>(reason)) {
    case DisconnectReason::ConnectionLost:
        return "connection lost";
    case DisconnectReason::Left:
        return "left";
    case DisconnectReason::VersionMismatch:
        return "protocol version mismatch";
    case DisconnectReason::SessionFull:
        return "session full";
    case DisconnectReason::HandshakeTimeout:
        return "handshake timed out";
    case DisconnectReason::ModMismatch:
        return "gameplay mods differ";
    case DisconnectReason::SaveReset:
        return "save reset";
    case DisconnectReason::BadToken:
        return "unknown join token";
    case DisconnectReason::Declined:
        return "join declined";
    }
    return "unknown reason";
}

std::uint32_t ms_since(std::chrono::steady_clock::time_point then) {
    if (then == std::chrono::steady_clock::time_point{}) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - then)
            .count());
}

const char* env_or_null(const char* name) {
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

std::uint16_t env_port(const char* name, std::uint16_t fallback) {
    const char* value = env_or_null(name);
    if (value == nullptr) {
        return fallback;
    }
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed <= 0 || parsed > 0xFFFF) {
        Log.warn("{}='{}' is not a valid port; using {}", name, value, fallback);
        return fallback;
    }
    return static_cast<std::uint16_t>(parsed);
}

/// Distinct, readable tints so players can tell each other apart at a glance. tpmp made every
/// player pick a name and a colour and it is genuinely necessary once more than two people play.
constexpr std::uint32_t kPlayerColors[] = {
    0x4FC3F7,  // blue
    0xEF5350,  // red
    0x66BB6A,  // green
    0xFFCA28,  // amber
    0xAB47BC,  // purple
    0xFF7043,  // orange
    0x26A69A,  // teal
    0xEC407A,  // pink
};

/// Splits "host" or "host:port". IPv6 literals are not handled yet — ENet 1.3 is IPv4 only.
void split_address(const std::string& input, std::string& host, std::uint16_t& port) {
    const std::size_t colon = input.rfind(':');
    if (colon == std::string::npos) {
        host = input;
        return;
    }
    host = input.substr(0, colon);
    const long parsed = std::strtol(input.c_str() + colon + 1, nullptr, 10);
    if (parsed > 0 && parsed <= 0xFFFF) {
        port = static_cast<std::uint16_t>(parsed);
    }
}

}  // namespace

NetworkManager& network_manager() {
    static NetworkManager sInstance;
    return sInstance;
}

// --- batch 7: death (decomp hooks, declared in dusk/multiplayer.hpp) ----------------------------

bool death_menu_hides_title() {
    // The first reader of MultiplayerPolicy::onDeath. RespawnSelf means death respawns only the
    // player who died, through vanilla Retry, which never touches the card. "Return to the title"
    // would wipe the save info under a live session (mp-death-rule.md, H1).
    const NetworkManager& net = network_manager();
    return net.is_active() && net.session_live() && g_mpPolicy.onDeath == DeathRule::RespawnSelf;
}

void death_menu_title_refused() {
    notify("Retry is the only choice in co-op",
        "Leave the session from the multiplayer menu to return to the title.", 5);
}

void NetworkManager::set_startup_options(const StartupOptions& options) {
    if (mInitialized) {
        // The session is opened on the first tick, so anything arriving later is a wiring mistake
        // rather than a user error. Say so instead of silently doing nothing.
        Log.warn("Startup options arrived after the session was already initialized; ignoring");
        return;
    }
    mStartup.host = options.host;
    mStartup.connect = options.connect != nullptr ? options.connect : "";
    mStartup.port = options.port;
    mStartup.nickname = options.nickname != nullptr ? options.nickname : "";
    mStartup.color = options.color != nullptr ? options.color : "";
    mStartup.trace = options.trace != nullptr ? options.trace : "";
    mStartup.hostWithCode = options.hostWithCode;
    mStartup.joinCode = options.joinCode != nullptr ? options.joinCode : "";
}

void NetworkManager::ensure_initialized() {
    if (mInitialized) {
        return;
    }
    mInitialized = true;

    // Test hook: DUSK_MP_SELFTEST=world runs world_state's engine-free self-test once, session or
    // not, and says so in a line the harness greps for.
    if (const char* selftest = env_or_null("DUSK_MP_SELFTEST");
        selftest != nullptr && std::string_view(selftest) == "world")
    {
        std::string failure;
        if (world::selftest(failure)) {
            Log.info("[mp-world] selftest PASS");
        } else {
            Log.error("[mp-world] selftest FAIL: {}", failure);
        }
    }

    // Command line first, environment second. The real Host/Join lobby replaces both at M2
    // (see 05-ui-integration.md); until then these are how a session gets opened.
    const std::uint16_t port =
        mStartup.port != 0 ? mStartup.port : env_port("DUSK_MP_PORT", kDefaultPort);

    if (!mStartup.nickname.empty()) {
        mLocalNickname = mStartup.nickname;
    } else if (const char* name = env_or_null("DUSK_MP_NAME")) {
        mLocalNickname = name;
    }
    if (mLocalNickname.empty()) {
        mLocalNickname = "Player";
    }

    const char* envColor = env_or_null("DUSK_MP_COLOR");
    const char* color = !mStartup.color.empty() ? mStartup.color.c_str() : envColor;
    if (color != nullptr) {
        mLocalColor = static_cast<std::uint32_t>(std::strtoul(color, nullptr, 16)) & 0xFFFFFFu;
        mColorExplicit = true;
    } else {
        // The host takes the first palette slot; a client adopts the slot matching the player id
        // the host assigns it, so two players never end up the same colour by accident.
        mLocalColor = kPlayerColors[0];
    }

    const char* envTarget = env_or_null("DUSK_MP_CONNECT");
    const char* target = !mStartup.connect.empty() ? mStartup.connect.c_str() : envTarget;
    const bool wantsHostCode = mStartup.hostWithCode;
    const bool wantsHost = wantsHostCode || mStartup.host || env_or_null("DUSK_MP_HOST") != nullptr;
    const bool wantsJoinCode = !mStartup.joinCode.empty();

    if (wantsJoinCode && (wantsHost || target != nullptr)) {
        Log.warn("A join code and another session were both requested; not starting a session");
        return;
    }
    if (target != nullptr && wantsHost) {
        // Both were asked for. Hosting and joining are mutually exclusive, and guessing which one
        // was meant is worse than refusing: pick neither and say why.
        Log.warn("Both a host and a join were requested ('{}'); not starting a session", target);
        return;
    }

    // Opened before the session so the trace covers the handshake as well as the play.
    if (!mStartup.trace.empty()) {
        trace::open(mStartup.trace, wantsHost ? "host" : "client");
    }

    if (wantsJoinCode) {
        join_by_code(mStartup.joinCode);
        return;
    }

    if (target != nullptr) {
        std::string address;
        std::uint16_t targetPort = port;
        split_address(target, address, targetPort);
        join(address, targetPort);
        return;
    }

    if (wantsHostCode) {
        host_with_code(port);
    } else if (wantsHost) {
        host(port);
    }
}

bool NetworkManager::host(std::uint16_t port) {
    if (mRole != Role::Inactive) {
        Log.warn("Already in a session");
        return false;
    }

    auto transport = std::make_unique<EnetTransport>();
    // Peers, not players: the host occupies one of the kMaxPlayers slots itself, and the snapshot
    // packet is sized so that all of them fit in one unfragmented datagram. The cap itself is
    // enforced at Hello; the spare connections exist only so a refusal can be delivered.
    if (!transport->listen(port, kMaxPlayers - 1 + kSpareConnectionSlots)) {
        notify("Couldn't host a session",
            fmt::format(
                "UDP port {} is unavailable. Is another copy of Dusklight already hosting?", port),
            8);
        return false;
    }

    mTransport = std::move(transport);
    mRole = Role::Host;
    mEndpoint = fmt::format("port {}", port);
    mLastEndMessage.clear();
    mRateWindowStart = std::chrono::steady_clock::now();
    mRateWindowStartTick = mSimTick;
    // The baseline check_mod_changes compares against. Each Hello still builds a fresh list, so a
    // guest is always judged against what the host is running at that moment.
    mSentMods = local_mod_fingerprint();
    Log.info("Session started as HOST (protocol v{})", kProtocolVersion);
    Log.info("Mods: hosting with {}", describe_mod_fingerprint(mSentMods));
    // v24: the session's policy is this machine's, with the persisted MP settings on top.
    policy_sync::on_host_started();
    // Once per session, before any guest can have changed anything (II.8 c).
    backup_host_card_for_session();
    notify("Hosting a co-op session", fmt::format("Waiting for players on UDP port {}.", port));
    return true;
}

bool NetworkManager::join(const std::string& address, std::uint16_t port) {
    if (mRole != Role::Inactive) {
        Log.warn("Already in a session");
        return false;
    }

    auto transport = std::make_unique<EnetTransport>();
    if (!transport->connect(address, port)) {
        notify("Couldn't join", fmt::format("'{}' is not an address Dusklight can reach.", address),
            8);
        return false;
    }

    begin_client_session(std::move(transport), fmt::format("{}:{}", address, port));
    Log.info("Session started as CLIENT (protocol v{})", kProtocolVersion);
    return true;
}

void NetworkManager::begin_client_session(
    std::unique_ptr<ITransport> transport, std::string endpoint) {
    mTransport = std::move(transport);
    mRole = Role::Client;
    mJoined = false;
    mJoinStarted = std::chrono::steady_clock::now();
    mEndpoint = std::move(endpoint);
    mHostNickname.clear();
    mLastEndMessage.clear();
    mHaveHostMods = false;
    mRateWindowStart = std::chrono::steady_clock::now();
    mRateWindowStartTick = mSimTick;
    // The host owns the save. Latch it here, at the moment we become a guest, rather than at the
    // first applied packet: v1 policy makes the host authoritative over inventory, rupees and quest
    // flags, so by the time any of that has arrived it is already too late to decide. It becomes
    // sticky for the life of the process when the host accepts us (HelloAck); a join that never
    // gets that far gives saving back in shutdown() — see save_guard.hpp.
    begin_guest_join();
}

// --- Internet play: host side ---

bool NetworkManager::host_with_code(std::uint16_t port) {
    if (!host(port)) {
        return false;
    }

    mHostCode = {};
    mHostCode.phase = HostCodeState::Phase::Gathering;
    mHostCode.startedAt = std::chrono::steady_clock::now();
    mHostCode.port = port;
    mHostCode.status = "Getting your public address…";

    // Both run while the game carries on: STUN inside the transport's poll(), UPnP on a thread
    // of its own because miniupnpc blocks for seconds.
    begin_configured_stun(*mTransport);
    if (upnp_wanted()) {
        auto job = std::make_shared<UpnpJob>();
        mHostCode.upnp = job;
        std::thread([job, port] {
            std::string externalIp;
            const bool ok = upnp::map_udp(port, &externalIp);
            bool unmapNow = false;
            {
                std::lock_guard lock(job->mutex);
                job->done = true;
                job->ok = ok;
                job->externalIp = externalIp;
                unmapNow = ok && job->cancelled;
            }
            if (unmapNow) {
                // The session ended while we were asking; nobody else will remove it.
                upnp::unmap_udp(port);
            }
        }).detach();
    } else {
        Log.info("[mp-net] UPnP off for this session");
    }
    Log.info(
        "[mp-rdv] hosting with a join code: gathering addresses (rendezvous {})", rendezvous_url());
    return true;
}

void NetworkManager::tick_host_code() {
    auto& hc = mHostCode;
    const auto now = std::chrono::steady_clock::now();

    switch (hc.phase) {
    case HostCodeState::Phase::Off:
    case HostCodeState::Phase::Failed:
        return;

    case HostCodeState::Phase::Gathering: {
        Endpoint stunPublic;
        const StunState stun = mTransport->stun_state(&stunPublic);
        const bool stunSettled = stun != StunState::Pending;
        bool upnpSettled = true;
        bool upnpOk = false;
        std::string upnpIp;
        if (hc.upnp) {
            std::lock_guard lock(hc.upnp->mutex);
            upnpSettled = hc.upnp->done;
            upnpOk = hc.upnp->ok;
            upnpIp = hc.upnp->externalIp;
        }
        if (!(stunSettled && upnpSettled) && now - hc.startedAt < kGatherBudget) {
            return;
        }

        std::vector<rdv::Candidate> candidates;
        for (const Endpoint& e : mTransport->lan_endpoints()) {
            add_candidate(candidates, {e.ip, e.port, true});
        }
        if (stun == StunState::Done) {
            add_candidate(candidates, {stunPublic.ip, stunPublic.port, false});
        }
        if (upnpOk && !upnpIp.empty()) {
            add_candidate(candidates, {upnpIp, hc.port, false});
        }
        const auto gatherMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - hc.startedAt);
        Log.info("[mp-net] host candidates after {} ms: {} (STUN {}, UPnP {})", gatherMs.count(),
            candidates.size(),
            stun == StunState::Done    ? "ok" :
            stun == StunState::Pending ? "still waiting" :
                                         "failed",
            !hc.upnp     ? "off" :
            !upnpSettled ? "still waiting" :
            upnpOk       ? "mapped" :
                           "failed");
        for (const auto& c : candidates) {
            Log.debug("[mp-net]   host candidate {}:{}{}", c.ip, c.port, c.lan ? " (lan)" : "");
        }

        hc.rdv = std::make_unique<rdv::Client>(rendezvous_url());
        hc.rdv->create_room(std::move(candidates), static_cast<int>(max_players()));
        hc.phase = HostCodeState::Phase::Creating;
        hc.status = "Getting a join code…";
        return;
    }

    case HostCodeState::Phase::Creating: {
        std::string code;
        const rdv::Status s = hc.rdv->create_status(&code);
        if (s == rdv::Status::Pending) {
            return;
        }
        if (s != rdv::Status::Ok) {
            hc.phase = HostCodeState::Phase::Failed;
            hc.status = fmt::format("Couldn't reach the join-code service: {}. Players can still "
                                    "join by address.",
                rdv::status_name(s));
            Log.warn("[mp-rdv] could not create a room: {}", rdv::status_name(s));
            notify("No join code", hc.status, 8);
            return;
        }
        hc.code = code;
        hc.phase = HostCodeState::Phase::Ready;
        hc.status = "Join code ready";
        // ★ Exact wording: the test harness greps for it.
        Log.info("[mp-rdv] room code {} ready", code);
        notify("Join code ready",
            fmt::format("Give {} to the other player. It is also on the menu's Session page.",
                display_code(code)),
            10);
        return;
    }

    case HostCodeState::Phase::Ready: {
        if (hc.rdv->create_status(nullptr) != rdv::Status::Ok) {
            hc.phase = HostCodeState::Phase::Failed;
            hc.status = "The join code stopped working. Players already in stay; new players can "
                        "join by address.";
            Log.warn("[mp-rdv] room {} ended by the service", hc.code);
            notify("Join code expired", hc.status, 8);
            return;
        }
        for (rdv::JoinRequest& join : hc.rdv->take_joins()) {
            std::size_t punched = 0;
            for (const auto& c : join.candidates) {
                Log.debug(
                    "[mp-net]   guest candidate {}:{}{}", c.ip, c.port, c.lan ? " (lan)" : "");
                if (!c.lan && c.port != 0) {
                    mTransport->punch({c.ip, c.port}, kPunchDurationMs);
                    ++punched;
                }
            }
            Log.info("[mp-rdv] join request from '{}' (token {}), {} candidate(s), punching {}",
                join.name, token_prefix(join.token), join.candidates.size(), punched);
            if (join.token.empty()) {
                continue;
            }
            hc.pendingJoins[join.token] = {
                std::move(join.name), std::move(join.joinId), now + kPendingJoinLifetime};
        }
        for (auto it = hc.pendingJoins.begin(); it != hc.pendingJoins.end();) {
            if (now > it->second.expires) {
                Log.info("[mp-rdv] join request from '{}' expired unused", it->second.name);
                it = hc.pendingJoins.erase(it);
            } else {
                ++it;
            }
        }
        return;
    }
    }
}

void NetworkManager::answer_join_request(PeerId peer, bool allow) {
    if (mRole != Role::Host) {
        return;
    }
    if (auto it = mPeers.find(peer); it != mPeers.end() && it->second.awaitingApproval) {
        it->second.approvalDecision = allow ? 1 : 2;
    }
}

void NetworkManager::tick_join_approvals() {
    const auto now = std::chrono::steady_clock::now();
    for (auto& [id, session] : mPeers) {
        if (!session.awaitingApproval || session.refused) {
            continue;
        }
        const bool timedOut = now - session.approvalAskedAt > kApprovalTimeout;
        if (session.approvalDecision == 1 && !timedOut) {
            session.awaitingApproval = false;
            // Re-checked: the session may have filled, or the mods changed, while they waited.
            std::size_t admitted = 0;
            for (const auto& entry : mPeers) {
                if (entry.second.handshakeComplete) {
                    ++admitted;
                }
            }
            const ModFingerprint ourMods = local_mod_fingerprint();
            const std::uint32_t color =
                session.helloColorExplicit != 0 ? session.helloPreferredColor : kPlayerColors[0];
            if (admitted + 1 >= max_players()) {
                Log.warn(
                    "[mp-rdv] '{}' allowed, but the session filled meanwhile", session.nickname);
                send_hello_ack(id, false, 0, color, ourMods);
                refuse_peer(session, DisconnectReason::SessionFull);
                continue;
            }
            if (diff_mod_fingerprints(ourMods, session.mods, "we", session.nickname)
                    .gameplayMismatch)
            {
                Log.warn("[mp-rdv] '{}' allowed, but the gameplay mods changed meanwhile",
                    session.nickname);
                send_hello_ack(id, false, 0, color, ourMods);
                refuse_peer(session, DisconnectReason::ModMismatch);
                continue;
            }
            Log.info("[mp-rdv] host allowed '{}' after {} ms", session.nickname,
                ms_since(session.approvalAskedAt));
            admit_peer(id, ourMods);
        } else if (session.approvalDecision == 2 || timedOut) {
            session.awaitingApproval = false;
            Log.info("[mp-rdv] join by '{}' {}", session.nickname,
                timedOut ? "not answered in time; declining" : "denied by the host");
            if (timedOut) {
                notify(fmt::format("{} wasn't let in", session.nickname),
                    "Nobody answered their join request in time.", 5);
            }
            refuse_peer(session, DisconnectReason::Declined);
        }
    }
}

// --- Internet play: guest side ---

bool NetworkManager::join_by_code(const std::string& typed) {
    if (mRole != Role::Inactive) {
        Log.warn("Already in a session");
        return false;
    }

    const std::string code = rdv::normalize_code(typed);
    if (code.empty()) {
        notify("Couldn't join",
            fmt::format(
                "'{}' isn't a join code. Codes are six letters and digits, like ABC-123.", typed),
            8);
        return false;
    }

    // One socket for STUN and the connect, so the public address STUN reports (and the host
    // punches toward) is the one the connection really comes from.
    auto transport = std::make_unique<EnetTransport>();
    if (!transport->open_client()) {
        notify("Couldn't join", "Dusklight couldn't open a network socket.", 8);
        return false;
    }
    begin_configured_stun(*transport);

    begin_client_session(std::move(transport), fmt::format("join code {}", display_code(code)));
    mCodeJoin = {};
    mCodeJoin.phase = CodeJoinState::Phase::Stun;
    mCodeJoin.code = code;
    mCodeJoin.startedAt = std::chrono::steady_clock::now();
    mCodeJoin.phaseStartedAt = mCodeJoin.startedAt;
    mCodeJoin.status = "Getting your public address…";
    Log.info(
        "Session started as CLIENT (protocol v{}), joining by code {}", kProtocolVersion, code);
    return true;
}

void NetworkManager::fail_code_join(const std::string& message) {
    Log.warn("[mp-rdv] join by code failed: {}", message);
    end_client_session("Couldn't join", message);
}

void NetworkManager::start_code_join_attempt() {
    auto& cj = mCodeJoin;
    const rdv::Candidate& c = cj.hostCandidates[cj.attempt];

    // ENet's client socket takes exactly one connect, so each attempt needs its own socket. The
    // first public attempt gets the one STUN measured: that is the address the host was told and
    // is punching toward. LAN addresses need no NAT mapping, so they use fresh sockets.
    std::unique_ptr<ITransport> next;
    if (!c.lan && !cj.stunSocketUsed) {
        next = cj.stunTransport ? std::move(cj.stunTransport) : std::move(mTransport);
        cj.stunSocketUsed = true;
    } else {
        if (!cj.stunSocketUsed && !cj.stunTransport) {
            cj.stunTransport = std::move(mTransport);
        }
        next = std::make_unique<EnetTransport>();
    }
    // Replaces (and so closes) the previous attempt's socket, which never connected.
    mTransport = std::move(next);
    cj.attemptFailed = false;
    cj.phaseStartedAt = std::chrono::steady_clock::now();
    cj.status = fmt::format("Connecting ({} of {})…", cj.attempt + 1, cj.hostCandidates.size());
    Log.debug("[mp-net] trying host candidate {}:{}{}", c.ip, c.port, c.lan ? " (lan)" : "");
    if (!mTransport->connect(c.ip, c.port)) {
        cj.attemptFailed = true;
    }
}

void NetworkManager::tick_code_join() {
    auto& cj = mCodeJoin;
    const auto now = std::chrono::steady_clock::now();

    switch (cj.phase) {
    case CodeJoinState::Phase::None:
    case CodeJoinState::Phase::Connected:
        return;

    case CodeJoinState::Phase::Stun: {
        Endpoint stunPublic;
        const StunState stun = mTransport->stun_state(&stunPublic);
        if (stun == StunState::Pending && now - cj.phaseStartedAt < kGatherBudget) {
            return;
        }
        std::vector<rdv::Candidate> candidates;
        for (const Endpoint& e : mTransport->lan_endpoints()) {
            add_candidate(candidates, {e.ip, e.port, true});
        }
        if (stun == StunState::Done) {
            add_candidate(candidates, {stunPublic.ip, stunPublic.port, false});
        } else {
            Log.warn("[mp-net] STUN gave no public address; the host can only reach us on the LAN");
        }
        Log.info("[mp-net] guest candidates after {} ms: {}", ms_since(cj.phaseStartedAt),
            candidates.size());
        for (const auto& c : candidates) {
            Log.debug("[mp-net]   guest candidate {}:{}{}", c.ip, c.port, c.lan ? " (lan)" : "");
        }
        cj.rdv = std::make_unique<rdv::Client>(rendezvous_url());
        cj.rdv->join(cj.code, mLocalNickname, std::move(candidates));
        cj.phase = CodeJoinState::Phase::Rendezvous;
        cj.phaseStartedAt = now;
        cj.status = "Looking up the join code…";
        return;
    }

    case CodeJoinState::Phase::Rendezvous: {
        std::vector<rdv::Candidate> host;
        std::string token;
        const rdv::Status s = cj.rdv->join_status(&host, &token);
        switch (s) {
        case rdv::Status::Pending:
            if (now - cj.phaseStartedAt > kRendezvousTimeout) {
                fail_code_join("Couldn't reach the join-code service: it did not answer. Check "
                               "your internet connection, or join by address under Advanced.");
            }
            return;
        case rdv::Status::NoRoom:
            fail_code_join("No game with that code. Check it with the host: a code stops working "
                           "when they stop hosting.");
            return;
        case rdv::Status::Full:
            fail_code_join("That game is full.");
            return;
        case rdv::Status::RateLimited:
            fail_code_join("Too many join attempts from this network. Wait a minute and try "
                           "again.");
            return;
        case rdv::Status::NetworkError:
        case rdv::Status::ServerError:
            fail_code_join(fmt::format("Couldn't reach the join-code service ({}). Check your "
                                       "internet connection, or join by address under Advanced.",
                rdv::status_name(s)));
            return;
        case rdv::Status::Ok:
            break;
        }

        cj.token = token;
        // LAN first, so players in one house never need the internet; the order within each group
        // is the host's.
        cj.hostCandidates.clear();
        for (const bool lan : {true, false}) {
            for (const auto& c : host) {
                if (c.lan == lan) {
                    add_candidate(cj.hostCandidates, c);
                }
            }
        }
        std::size_t lanCount = 0;
        for (const auto& c : cj.hostCandidates) {
            lanCount += c.lan ? 1 : 0;
            Log.debug("[mp-net]   host candidate {}:{}{}", c.ip, c.port, c.lan ? " (lan)" : "");
        }
        Log.info("[mp-rdv] code {} found after {} ms: host has {} candidate(s) ({} LAN), token {}",
            cj.code, ms_since(cj.phaseStartedAt), cj.hostCandidates.size(), lanCount,
            token_prefix(token));
        destroy_rdv_async(std::move(cj.rdv));
        if (cj.hostCandidates.empty()) {
            fail_code_join(kStrictRouterMessage);
            return;
        }
        cj.phase = CodeJoinState::Phase::Connecting;
        cj.attempt = 0;
        start_code_join_attempt();
        return;
    }

    case CodeJoinState::Phase::Connecting: {
        const rdv::Candidate& c = cj.hostCandidates[cj.attempt];
        const auto budget = c.lan ? kLanAttemptBudget : kPublicAttemptBudget;
        if (!cj.attemptFailed && now - cj.phaseStartedAt < budget) {
            return;
        }
        Log.info("[mp-net] host candidate {} of {} ({}) {} after {} ms", cj.attempt + 1,
            cj.hostCandidates.size(), c.lan ? "LAN" : "public",
            cj.attemptFailed ? "failed" : "timed out", ms_since(cj.phaseStartedAt));
        if (++cj.attempt >= cj.hostCandidates.size()) {
            fail_code_join(kStrictRouterMessage);
            return;
        }
        start_code_join_attempt();
        return;
    }
    }
}

void NetworkManager::teardown_internet() {
    // Host: the room goes (the Client's destructor closes it), off the game thread.
    destroy_rdv_async(std::move(mHostCode.rdv));
    if (mHostCode.upnp) {
        bool unmapNow = false;
        {
            std::lock_guard lock(mHostCode.upnp->mutex);
            mHostCode.upnp->cancelled = true;
            unmapNow = mHostCode.upnp->done && mHostCode.upnp->ok;
        }
        if (unmapNow) {
            const std::uint16_t port = mHostCode.port;
            std::thread([port] { upnp::unmap_udp(port); }).detach();
        }
        // Not yet done: its thread sees `cancelled` and removes the mapping itself.
    }
    mHostCode = {};

    destroy_rdv_async(std::move(mCodeJoin.rdv));
    mCodeJoin = {};
}

NetworkManager::~NetworkManager() {
    // Process exit. Hang up so everyone else sees "left" at once instead of a frozen puppet until
    // the timeout; nothing else, because the game this would otherwise tidy up is already gone.
    if (mTransport) {
        mTransport->disconnect_all(static_cast<std::uint32_t>(DisconnectReason::Left));
        mTransport.reset();
    }
}

void NetworkManager::shutdown(DisconnectReason reason) {
    if (mRole == Role::Client && !mJoined) {
        // Never accepted, so nothing of the host's was applied (every host packet waits for the
        // handshake). Saving comes back; a session that did get going stays latched.
        abandon_guest_join();
    }
    if (mTransport) {
        mTransport->disconnect_all(static_cast<std::uint32_t>(reason));
        mTransport.reset();
    }
    // Internet play: the join code stops working and the router forward goes, both off-thread.
    teardown_internet();
    // Every puppet goes, and the clock stops being anyone else's: with the role back to Inactive,
    // WorldClock::should_advance and reconcile hand control straight back to vanilla. The save
    // flag is deliberately NOT touched — see save_guard.hpp for why leaving does not undo joining.
    replication_manager().clear();
    world_clock().clear();
    // v22: the shared world, presence and replicated objects all belong to the session. The world
    // in memory stays as it is (a guest's save stays latched; see save_guard.hpp).
    world_sync::reset();
    presence::reset();
    objects::reset();
    // v23: every chest / floor-item lease and pending claim belonged to this session.
    item_claims::reset();
    // v24: a guest ran the host's policy; its own comes back.
    policy_sync::reset();
    /* --- batch 8: enemies --- */
    // Every synced enemy back to local, every stand-in gone, slot 0 back on Link.
    enemies::reset();
    mLastPresence.clear();
    mPeers.clear();
    mPendingHeartbeats.clear();
    mAttentiveTime = {};
    mLastPollAt = {};
    mRole = Role::Inactive;
    mJoined = false;
    mHostNickname.clear();
    mHaveHostMods = false;
    mLocalPlayerId = 0;
    mNextPlayerId = 1;
    mLeaveRequested = false;
    mLeaveForSaveReset = false;
    trace::close();
    Log.info("Session ended");
}

void NetworkManager::pre_actor_tick() {
    // Test-only knob. Before ensure_initialized(), because a join starts there and engages the
    // save guard, and this half has to open its handle while the guard is still off.
    prepare_save_guard_probe_if_requested();
    ensure_initialized();
    // Test-only knob. Before the early-out, so it also runs after the guest has left.
    run_save_guard_probe_if_requested();
    if (!mTransport) {
        return;
    }

    if (test_leave_tick() != 0 && mSimTick == test_leave_tick()) {
        Log.info("DUSK_MP_TEST_LEAVE_AT_TICK: leaving at tick {}", mSimTick);
        leave_session();
    }

    if (mLeaveRequested) {
        const bool wasClient = mRole == Role::Client;
        const bool wasJoined = mJoined;
        const bool forSaveReset = mLeaveForSaveReset;
        shutdown(forSaveReset ? DisconnectReason::SaveReset : DisconnectReason::Left);
        if (forSaveReset) {
            // Batch 7 (death rule H2): world_sync saw this machine's save memory reset under a
            // live session. Said in terms of what the player just did, not "you left".
            if (wasClient) {
                mLastEndMessage = "Going back to the title screen took you out of the session.";
                notify("Left the co-op session", "Going back to the title screen (or reloading a "
                                                 "save) ends your part in the session.");
                if (wasJoined) {
                    notify_save_disabled_after_session_end(SessionEnd::LeftByChoice);
                }
            } else {
                mLastEndMessage = "Going back to the title screen ended the session.";
                notify("Co-op session ended", "Going back to the title screen (or reloading your "
                                              "save) ends the session. Everyone who was connected "
                                              "has been told.");
            }
            return;
        }
        if (wasClient) {
            mLastEndMessage = "You left the session.";
            notify("Left the co-op session", "You are playing on your own now.");
            if (wasJoined) {
                notify_save_disabled_after_session_end(SessionEnd::LeftByChoice);
            }
        } else {
            mLastEndMessage = "You ended the session.";
            notify("Co-op session ended", "Everyone who was connected has been told.");
        }
        return;
    }

    ++mSimTick;

    // Test knob, once per process. Before the drain, so everything the far end sent during the
    // stall arrives together afterwards, as it would after a real one.
    static bool s_stalled = false;
    if (!s_stalled && test_stall().ms != 0 && mSimTick == test_stall().tick) {
        s_stalled = true;
        Log.warn("DUSK_MP_TEST_STALL_MS: stalling the game thread for {} ms at tick {}",
            test_stall().ms, mSimTick);
        const auto stallStart = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(test_stall().ms));
        Log.warn("DUSK_MP_TEST_STALL_MS: resumed after {} ms (tick {}, {} peer(s) still connected "
                 "before the drain)",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stallStart)
                .count(),
            mSimTick, mPeers.size());
    }

    // Advance the watchdog's clock before draining, so packets that queued up while we were away
    // are stamped with the time we came back.
    const auto pollAt = std::chrono::steady_clock::now();
    if (mLastPollAt != std::chrono::steady_clock::time_point{}) {
        mAttentiveTime +=
            std::min<std::chrono::steady_clock::duration>(pollAt - mLastPollAt, kMaxAttentiveStep);
    }
    mLastPollAt = pollAt;

    mEventScratch.clear();
    mTransport->poll(mEventScratch);
    for (const TransportEvent& event : mEventScratch) {
        handle_event(event);
        if (!mTransport) {
            // That event ended the session (the host went away). The rest of the batch belongs to
            // a connection that no longer exists.
            return;
        }
    }

    // Internet play: candidate gathering and join requests (host), the rendezvous and the
    // connect attempts (guest). After the drain, so a connect that just landed is not retried.
    if (mRole == Role::Host) {
        tick_host_code();
        tick_join_approvals();
    } else if (mRole == Role::Client) {
        tick_code_join();
    }
    if (!mTransport) {
        return;
    }

    check_handshake_deadlines();
    if (!mTransport) {
        return;
    }

    // After the drain, so a peer is judged on everything that reached us this frame.
    check_peer_silence();
    if (!mTransport) {
        return;
    }

    check_mod_changes();
    if (!mTransport) {
        return;
    }

    // Apply inbound state BEFORE actors run, so this tick's actors see fresh remote positions.
    // Advancing the interpolation cursor here (once per sim tick, never per rendered frame) is
    // what keeps playback locked to the same clock the poses were captured on.
    replication_manager().drive_puppets();

    /* --- batch 8: enemies --- */
    // After drive_puppets: ghosts play on their authority's puppet timeline, and the target
    // stand-ins are refilled from the poses just pushed.
    if (session_live()) {
        enemies::pre_tick();
    }
}

void NetworkManager::post_actor_tick() {
    if (!mTransport) {
        return;
    }

    // Captured after the actor pass, so the pose sent is the one this tick actually ended at.
    if (mRole == Role::Client) {
        send_local_state();
        send_time_override();
    } else if (mRole == Role::Host) {
        broadcast_snapshot();
    }

    // v22, after the actor pass for the same reason as the pose: the world diff, object states and
    // presence describe where this tick ended.
    if (session_live()) {
        world_sync::tick();
        objects::tick();
        // v23: guest claim timeouts and host lease expiry (replication/item_claims.hpp).
        item_claims::tick();
        send_presence();
        /* --- batch 8: enemies --- */
        enemies::post_tick();
    }

    send_heartbeats();
    report_tick_rate();
    report_interpolation();
    // Sampled here rather than in pre_actor_tick: the puppets have executed by now, so what gets
    // recorded is where they ended the tick, not where they were asked to go.
    trace::write_tick(mSimTick, mLocalPlayerId);
    mTransport->flush();
}

void NetworkManager::handle_event(const TransportEvent& event) {
    switch (event.type) {
    case TransportEventType::Connected: {
        PeerSession session;
        session.id = event.peer;
        session.connectedAt = std::chrono::steady_clock::now();
        session.lastHeard = session.connectedAt;
        session.heardAtAttentive = mAttentiveTime;
        mPeers[event.peer] = session;
        Log.info("Peer {} connected ({} total)", event.peer, mPeers.size());
        if (mRole == Role::Client && mCodeJoin.phase == CodeJoinState::Phase::Connecting) {
            const rdv::Candidate& c = mCodeJoin.hostCandidates[mCodeJoin.attempt];
            Log.info("[mp-net] connected to the host via {}:{} ({} candidate {} of {}) after {} "
                     "ms; {} ms since the code was entered",
                c.ip, c.port, c.lan ? "LAN" : "public", mCodeJoin.attempt + 1,
                mCodeJoin.hostCandidates.size(), ms_since(mCodeJoin.phaseStartedAt),
                ms_since(mCodeJoin.startedAt));
            mCodeJoin.phase = CodeJoinState::Phase::Connected;
            // The handshake deadline counts from here, not from when the code was typed.
            mJoinStarted = std::chrono::steady_clock::now();
            mCodeJoin.stunTransport.reset();
            mCodeJoin.status = "Waiting for the host to let you in…";
        }
        // The client speaks first so the host can vet its protocol version.
        if (mRole == Role::Client) {
            send_hello(event.peer);
        }
        break;
    }
    case TransportEventType::Disconnected:
        handle_disconnect(event.peer, event.reason);
        break;
    case TransportEventType::Data:
        if (auto it = mPeers.find(event.peer); it != mPeers.end()) {
            it->second.lastHeard = std::chrono::steady_clock::now();
            it->second.heardAtAttentive = mAttentiveTime;
        }
        handle_packet(event.peer, event.data.data(), event.data.size());
        break;
    }
}

void NetworkManager::handle_disconnect(PeerId peer, std::uint32_t reason) {
    // "Silent for" is the time-to-detection: for a clean leave it is about one tick, for a peer
    // that vanished it is how long the transport took to give up on it.
    const auto known = mPeers.find(peer);
    Log.info("Peer {} disconnected ({}), silent for {} ms", peer, reason_text(reason),
        known != mPeers.end() ? ms_since(known->second.lastHeard) : 0);

    if (mRole == Role::Client && mCodeJoin.phase == CodeJoinState::Phase::Connecting) {
        // One of the host's candidates did not answer. tick_code_join() moves on to the next one.
        mCodeJoin.attemptFailed = true;
        return;
    }

    if (mRole == Role::Client) {
        // A client has exactly one connection, so whatever this was, the session is over. Say why
        // in terms of what the player was doing: a join that never landed reads differently from
        // a game that was going and stopped.
        const std::string host = mHostNickname.empty() ? mEndpoint : mHostNickname;
        switch (static_cast<DisconnectReason>(reason)) {
        case DisconnectReason::Left:
            end_client_session("Co-op session ended", fmt::format("{} ended the session.", host));
            break;
        case DisconnectReason::SaveReset:
            // Batch 7 (death rule H2): the host's own world was replaced, so it ended the session
            // instead of overwriting ours with a different one.
            end_client_session("Co-op session ended",
                fmt::format("{} went back to the title screen or reloaded their save, which ends "
                            "the session.",
                    host));
            break;
        case DisconnectReason::VersionMismatch:
            end_client_session("Couldn't join",
                "The host is running a different version of Dusklight. Both players need the "
                "same build.");
            break;
        case DisconnectReason::SessionFull:
            end_client_session("Couldn't join", "The session is full.");
            break;
        case DisconnectReason::BadToken:
            end_client_session("Couldn't join", "The host didn't recognise this join request. Ask "
                                                "for the code again and retry.");
            break;
        case DisconnectReason::Declined:
            end_client_session("Couldn't join",
                "The host didn't let you in (they pressed Deny, or didn't answer in time).");
            break;
        case DisconnectReason::HandshakeTimeout:
            end_client_session("Couldn't join", "The host stopped waiting for us to introduce "
                                                "ourselves. Try joining again.");
            break;
        case DisconnectReason::ModMismatch: {
            // At join, the HelloAck before this hang-up has normally ended the session already
            // with the mods named, and this is the fallback for an ack that could not be read.
            // Mid-session (v18), the host's ModsUpdate arrived just before the hang-up, so name
            // the difference against what we run now — typically a save on another seed.
            std::string detail;
            if (mHaveHostMods) {
                detail = summarize_mod_mismatch(
                    diff_mod_fingerprints(local_mod_fingerprint(), mHostMods, "you", "the host"));
            }
            if (mJoined) {
                end_client_session("Removed from the session: mods no longer match",
                    detail.empty() ? std::string("Your gameplay mods or randomizer seed stopped "
                                                 "matching the host's.") :
                                     detail);
            } else {
                end_client_session("Couldn't join",
                    detail.empty() ?
                        std::string("Your gameplay mods don't match the host's. Both players need "
                                    "the same gameplay mods, at the same versions, enabled.") :
                        detail);
            }
            break;
        }
        case DisconnectReason::ConnectionLost:
        default:
            if (mJoined) {
                end_client_session(
                    "Lost connection to the host", fmt::format("{} stopped responding.", host));
            } else {
                end_client_session("Couldn't join",
                    fmt::format("No answer from {}. Check the address and that the host is "
                                "running, and that UDP is not blocked by a firewall.",
                        mEndpoint));
            }
            break;
        }
        return;
    }

    const auto it = mPeers.find(peer);
    if (it == mPeers.end()) {
        return;
    }
    const PeerSession session = it->second;
    mPeers.erase(it);

    if (mRole != Role::Host || !session.handshakeComplete) {
        // Never admitted, so no puppet, no roster entry and nothing anyone else was told about.
        return;
    }

    // Despawn locally and tell the remaining clients, otherwise a puppet stands frozen in the world
    // forever — one of the ways tpmp sessions visibly rot.
    replication_manager().remove_player(session.playerId);
    // ★ And drop their vote on the world clock. Under the AllPlayers rule, someone who quits while
    // standing in a dungeon would otherwise stop the sun for everyone left, permanently, with no
    // way to see why.
    world_clock().forget_peer(session.playerId);
    // v22: their proposal sequence and owed sync, their presence, and the objects they owned.
    world_sync::on_peer_left(session.playerId);
    presence::on_peer_left(session.playerId);
    mLastPresence.erase(session.playerId);
    objects::on_peer_left(session.playerId);
    // v23: a chest or item they held a claim on is free again at once, not after the lease lapses.
    item_claims::on_peer_left(session.playerId);
    /* --- batch 8: enemies --- */
    enemies::on_peer_left(session.playerId);
    broadcast_peer_left(session.playerId);

    if (session.refused) {
        // We removed them (recheck_peer_mods), and already said why.
    } else if (reason == static_cast<std::uint32_t>(DisconnectReason::Left)) {
        notify(fmt::format("{} left", session.nickname), "They left the session.", 4);
    } else if (reason == static_cast<std::uint32_t>(DisconnectReason::SaveReset)) {
        notify(fmt::format("{} left", session.nickname),
            "They went back to the title screen or reloaded their save.", 5);
    } else {
        notify(fmt::format("{} lost connection", session.nickname),
            "They stopped responding and were removed from the session.", 5);
    }
}

void NetworkManager::end_client_session(const std::string& title, const std::string& message) {
    const bool wasJoined = mJoined;
    // Everything a host drove goes with it — puppets, and the world clock, which reverts to this
    // machine's own the moment the role is Inactive. See shutdown().
    shutdown();
    mLastEndMessage = message;
    notify(title, message, 8);
    // ★ And say out loud that saving is still off. This is the one moment the rule becomes
    // surprising: up to here "I can't save" reads as part of being in someone else's game, and the
    // instant that game ends a player reasonably assumes their own came back. It did not — the
    // world in memory is still the host's (save_guard.hpp), and the flag is sticky for exactly that
    // reason. Only after a session that actually got going: a join that never connected has nothing
    // of the host's in memory to warn about, though the flag stays latched regardless.
    if (wasJoined) {
        notify_save_disabled_after_session_end(SessionEnd::HostGone);
    }
}

void NetworkManager::refuse_peer(PeerSession& session, DisconnectReason reason) {
    session.refused = true;
    Log.warn("Refusing peer {}: {}", session.id, reason_text(static_cast<std::uint32_t>(reason)));
    // After queued packets, so the HelloAck that says no is delivered before the line drops. The
    // reason travels in the hang-up itself, which even a client whose protocol we cannot parse can
    // read.
    mTransport->disconnect(session.id, static_cast<std::uint32_t>(reason), true);
}

void NetworkManager::check_handshake_deadlines() {
    const auto now = std::chrono::steady_clock::now();

    if (mRole == Role::Client) {
        // A join by code has its own deadlines until it is connected (tick_code_join), and then
        // may wait on the host's Allow / Deny, which the host itself times out.
        const bool codeJoin = mCodeJoin.phase != CodeJoinState::Phase::None;
        if (codeJoin && mCodeJoin.phase != CodeJoinState::Phase::Connected) {
            return;
        }
        const auto deadline = codeJoin && !mCodeJoin.token.empty() ?
                                  std::chrono::seconds(kJoinTimeout + kApprovalTimeout) :
                                  kJoinTimeout;
        if (!mJoined && now - mJoinStarted > deadline) {
            Log.warn("No HelloAck from {} after {} s; giving up", mEndpoint, deadline.count());
            end_client_session("Couldn't join",
                fmt::format("No answer from {}. Check the address and that the host is running, "
                            "and that UDP is not blocked by a firewall.",
                    mEndpoint));
        }
        return;
    }

    for (auto& [id, session] : mPeers) {
        // A guest waiting on Allow / Deny did say Hello; tick_join_approvals() times it out.
        if (!session.handshakeComplete && !session.refused && !session.awaitingApproval &&
            now - session.connectedAt > kHelloTimeout)
        {
            refuse_peer(session, DisconnectReason::HandshakeTimeout);
        }
    }
}

void NetworkManager::check_peer_silence() {
    // Collected first: handle_disconnect erases from mPeers, and on a client it tears the whole
    // session down.
    std::vector<PeerId> silent;
    for (const auto& [id, session] : mPeers) {
        if (mAttentiveTime - session.heardAtAttentive > kPeerSilenceTimeout) {
            silent.push_back(id);
        }
    }

    for (const PeerId id : silent) {
        const auto it = mPeers.find(id);
        if (it == mPeers.end()) {
            continue;
        }
        Log.warn("Peer {} silent for {} ms; dropping (watchdog)", id,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                mAttentiveTime - it->second.heardAtAttentive)
                .count());
        // Transport first, and without waiting: a dropped peer produces no Disconnected event, so
        // the handle_disconnect below is the only one this peer ever gets — one PeerLeft, one
        // notice. On a client, shutdown()'s hang-up then has nobody left to wait on.
        const auto reason = static_cast<std::uint32_t>(DisconnectReason::ConnectionLost);
        mTransport->drop(id, reason);
        // The same path a transport timeout takes: host despawns the puppet and broadcasts
        // PeerLeft; client ends the session as "stopped responding".
        handle_disconnect(id, reason);
        if (!mTransport) {
            return;
        }
    }
}

std::string NetworkManager::player_name(std::uint32_t playerId) const {
    const auto& players = replication_manager().players();
    if (const auto it = players.find(playerId); it != players.end() && !it->second.nickname.empty())
    {
        return it->second.nickname;
    }
    return fmt::format("Player {}", playerId);
}

SessionStatus NetworkManager::status() const {
    SessionStatus out;
    out.role = mRole;
    out.connected = mRole == Role::Host || (mRole == Role::Client && mJoined);
    out.endpoint = mEndpoint;
    out.localNickname = mLocalNickname;
    out.localColor = mLocalColor;
    out.localPlayerId = mLocalPlayerId;
    out.lastEndMessage = mLastEndMessage;
    if (mRole == Role::Host && mHostCode.phase != HostCodeState::Phase::Off) {
        out.internetStatus = mHostCode.status;
        if (mHostCode.phase == HostCodeState::Phase::Ready) {
            out.roomCode = display_code(mHostCode.code);
        }
    } else if (mRole == Role::Client && mCodeJoin.phase != CodeJoinState::Phase::None && !mJoined) {
        out.internetStatus = mCodeJoin.status;
    }
    if (mRole == Role::Host) {
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [id, session] : mPeers) {
            if (!session.awaitingApproval || session.refused) {
                continue;
            }
            const auto waited =
                std::chrono::duration_cast<std::chrono::seconds>(now - session.approvalAskedAt);
            JoinApproval a;
            a.peer = id;
            a.nickname = session.nickname;
            a.secondsLeft = waited < kApprovalTimeout ?
                                static_cast<std::uint32_t>((kApprovalTimeout - waited).count()) :
                                0;
            out.approvals.push_back(std::move(a));
        }
    }

    // Direct connections first: on a client that is just the host.
    for (const auto& [id, session] : mPeers) {
        PlayerStatus p;
        p.playerId = session.playerId;
        p.nickname = session.nickname.empty() ? "(connecting)" : session.nickname;
        p.color = session.color;
        p.direct = true;
        p.handshakeComplete = session.handshakeComplete;
        p.rttMs = mTransport ? mTransport->rtt_ms(id) : 0;
        p.silentMs = ms_since(session.lastHeard);
        out.players.push_back(std::move(p));
    }

    // A client reaches everyone else through the host. The roster it was given is the replication
    // view, so read it from there rather than keeping a second copy that could disagree.
    if (mRole == Role::Client) {
        for (const auto& [playerId, player] : replication_manager().players()) {
            if (playerId == kHostPlayerId || playerId == mLocalPlayerId) {
                continue;
            }
            PlayerStatus p;
            p.playerId = playerId;
            p.nickname = player.nickname;
            p.color = player.color;
            p.handshakeComplete = true;
            out.players.push_back(std::move(p));
        }
    }
    return out;
}

void NetworkManager::send_hello(PeerId peer) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::Hello));
    w.write_u32(advertised_protocol_version());
    w.write_string(mLocalNickname);
    w.write_u8(mColorExplicit ? 1 : 0);
    w.write_u32(mLocalColor);
    mSentMods = local_mod_fingerprint();
    write_mod_fingerprint(w, mSentMods);
    // v28: the join-code session token, empty for a direct-IP join.
    const std::string& token = mCodeJoin.token;
    const std::size_t tokenLength = std::min(token.size(), kMaxJoinTokenLength);
    w.write_u8(static_cast<std::uint8_t>(tokenLength));
    for (std::size_t i = 0; i < tokenLength; ++i) {
        w.write_u8(static_cast<std::uint8_t>(token[i]));
    }
    Log.info("Mods sent in Hello: {}", describe_mod_fingerprint(mSentMods));
    if (tokenLength != 0) {
        Log.info("[mp-rdv] Hello carries join token {}", token_prefix(token));
    }
    mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);
}

void NetworkManager::send_hello_ack(PeerId peer, bool accepted, std::uint32_t playerId,
    std::uint32_t assignedColor, const ModFingerprint& ourMods) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::HelloAck));
    w.write_u8(accepted ? 1 : 0);
    w.write_u32(kProtocolVersion);
    w.write_u32(playerId);
    w.write_u32(assignedColor);
    w.write_string(mLocalNickname);
    w.write_u32(mLocalColor);
    write_mod_fingerprint(w, ourMods);
    mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);
}

void NetworkManager::admit_peer(PeerId peer, const ModFingerprint& ourMods) {
    const auto peerIt = mPeers.find(peer);
    if (peerIt == mPeers.end()) {
        return;
    }
    PeerSession& session = peerIt->second;
    const std::string nickname = session.nickname;

    // World identity comes from our own counter, never from the transport's peer id — see
    // PeerSession::playerId for why reusing ENet's id space would collide with the host.
    const std::uint32_t playerId = mNextPlayerId++;

    // The host owns colour assignment — otherwise its record of a peer goes stale the moment
    // the peer picks a different one, and the puppet would be tinted wrong at M1.
    const std::uint32_t assignedColor = session.helloColorExplicit != 0 ?
                                            session.helloPreferredColor :
                                            kPlayerColors[playerId % std::size(kPlayerColors)];

    send_hello_ack(peer, true, playerId, assignedColor, ourMods);

    session.playerId = playerId;
    session.color = assignedColor;
    session.handshakeComplete = true;

    // Order matters: the newcomer must learn the existing roster before anyone is told about
    // the newcomer, so no client can receive a PeerJoined for a session it has an incomplete
    // view of.
    send_peer_list(peer);
    broadcast_peer_joined(session);
    // Presence is sent only on change, so a newcomer would otherwise never learn where anyone
    // already is until they moved (mp-m4-m5-world.md I.3: "sent on change and once on join").
    send_cached_presence(peer, playerId);
    // v24: the session's policy, before the guest can ask for (and so receive) a WorldSync:
    // same ordered channel, so the world it installs is read under the host's policy.
    policy_sync::send_to(playerId);
    /* --- batch 8: enemies --- */
    enemies::on_peer_joined(playerId);

    replication_manager().add_player(playerId, nickname, assignedColor);
    Log.info(
        "Peer {} admitted as player {} ('{}', #{:06X})", peer, playerId, nickname, assignedColor);
    notify(fmt::format("{} joined", nickname), "They are now in your world.", 4);
}

void NetworkManager::send_mods_update(PeerId peer, const ModFingerprint& fp) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::ModsUpdate));
    write_mod_fingerprint(w, fp);
    mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);
}

bool NetworkManager::recheck_peer_mods(PeerSession& session, const ModFingerprint& ours) {
    const ModDiff diff = diff_mod_fingerprints(ours, session.mods, "we", session.nickname);
    if (!diff.gameplayMismatch) {
        return true;
    }
    for (const auto& line : diff.gameplayLines) {
        Log.warn("Mods: gameplay difference with player {} ('{}'): {}", session.playerId,
            session.nickname, line);
    }
    Log.warn("Removing player {} ('{}'): gameplay mods no longer match", session.playerId,
        session.nickname);
    notify(fmt::format("{} was removed", session.nickname),
        fmt::format("Your gameplay mods no longer match. {}", summarize_mod_mismatch(diff)), 8);
    // Ours first, on the same reliable channel, so it lands before the hang-up and the guest can
    // name the difference in its own words.
    send_mods_update(session.id, ours);
    refuse_peer(session, DisconnectReason::ModMismatch);
    return false;
}

void NetworkManager::check_mod_changes() {
    if (mSimTick % kModRecheckTicks != 0) {
        return;
    }
    // A client has nothing to report until it is admitted: the host has its Hello list, and a
    // change before the HelloAck is picked up on the first check after it.
    if (mRole == Role::Client && !mJoined) {
        return;
    }
    ModFingerprint now = local_mod_fingerprint();
    if (same_mod_fingerprint(now, mSentMods)) {
        return;
    }
    Log.info("Mods: ours changed to {}", describe_mod_fingerprint(now));
    mSentMods = std::move(now);

    if (mRole == Role::Client) {
        for (const auto& [id, session] : mPeers) {
            send_mods_update(id, mSentMods);
        }
        if (mHaveHostMods) {
            // Logged here too, so a guest-side change shows the comparison the host is about to
            // make even if the hang-up beats the host's own ModsUpdate.
            const ModDiff diff = diff_mod_fingerprints(mSentMods, mHostMods, "you", "the host");
            for (const auto& line : diff.gameplayLines) {
                Log.warn("Mods: gameplay difference: {}", line);
            }
        }
        return;
    }

    for (auto& [id, session] : mPeers) {
        if (!session.handshakeComplete || session.refused) {
            continue;
        }
        // Everyone hears the new list; anyone it no longer matches is removed.
        if (recheck_peer_mods(session, mSentMods)) {
            send_mods_update(id, mSentMods);
        }
    }
}

void NetworkManager::handle_packet(PeerId peer, const std::uint8_t* data, std::size_t size) {
    Reader r(data, size);
    std::uint8_t rawId = 0;
    if (!r.read_u8(rawId)) {
        return;
    }

    const PacketId id = static_cast<PacketId>(rawId);
    const auto sender = mPeers.find(peer);
    if (sender == mPeers.end()) {
        return;
    }

    // ★ Who may say what. Every packet below changes someone's world, so it has to come from the
    // side that is allowed to change it: roster and snapshot updates only ever flow down from the
    // host, poses and Hellos only ever up to it. Without this a client could send the host a
    // PeerLeft and make it despawn a third player. And a host acts on nothing from a peer it has
    // not admitted (or has refused) beyond the Hello itself — a heartbeat is harmless either way.
    //
    // v22: a guest only PROPOSES world changes and asks for syncs; only the host syncs, commits
    // and digests. A host taking a WorldSync would put someone else's world into the one save the
    // session writes. Presence and the owner-originated object packets go either way (the host
    // relays them), and are policed per packet below.
    // v23: a chest / floor-item claim is asked of the host and answered by it, never relayed.
    const bool hostBound = id == PacketId::Hello || id == PacketId::PlayerStateUpdate ||
                           id == PacketId::TimeOverride || id == PacketId::WorldPropose ||
                           id == PacketId::WorldSyncRequest || id == PacketId::ObjClaim ||
                           id == PacketId::ClaimRequest;
    const bool clientBound = id == PacketId::HelloAck || id == PacketId::WorldSnapshot ||
                             id == PacketId::PeerList || id == PacketId::PeerJoined ||
                             id == PacketId::PeerLeft || id == PacketId::WorldSync ||
                             id == PacketId::WorldCommit || id == PacketId::WorldDigest ||
                             id == PacketId::ObjClaimResult || id == PacketId::ClaimReply;
    if ((hostBound && mRole != Role::Host) || (clientBound && mRole != Role::Client)) {
        if (warn_budget()) {
            Log.warn("Ignoring packet {} from peer {}: wrong direction for this role", rawId, peer);
        }
        return;
    }
    /* --- batch 7: drops --- */
    // DropTaken is the host's word on who got a drop: client-bound, never relayed.
    if (id == PacketId::DropTaken && mRole != Role::Client) {
        if (warn_budget()) {
            Log.warn("Ignoring packet {} from peer {}: wrong direction for this role", rawId, peer);
        }
        return;
    }
    /* --- batch 8: enemies --- */
    // EnemyAuthority is the host's table: client-bound, never relayed. EnemyUpdate and RoomsLoaded
    // go either way and are policed in enemies::on_packet (a guest speaks only for itself, and only
    // for rooms the host's table gives it).
    if (id == PacketId::EnemyAuthority && mRole != Role::Client) {
        if (warn_budget()) {
            Log.warn("Ignoring packet {} from peer {}: wrong direction for this role", rawId, peer);
        }
        return;
    }
    if (mRole == Role::Host && id != PacketId::Hello && id != PacketId::Heartbeat &&
        id != PacketId::HeartbeatAck &&
        (!sender->second.handshakeComplete || sender->second.refused))
    {
        return;
    }
    // v22: nothing of the host's world, presence or objects before it has accepted us. A join that
    // fails may only give saving back if nothing of the host's was applied (save_guard.hpp).
    if (mRole == Role::Client && !mJoined && rawId >= static_cast<std::uint8_t>(PacketId::Presence))
    {
        return;
    }

    switch (id) {
    case PacketId::Hello: {
        if (sender->second.handshakeComplete || sender->second.refused ||
            sender->second.awaitingApproval)
        {
            // Already decided. A second Hello must not mint a second player id and puppet.
            Log.warn("Ignoring repeated Hello from peer {}", peer);
            return;
        }
        std::uint32_t version = 0;
        std::string nickname;
        std::uint8_t colorExplicit = 0;
        std::uint32_t preferredColor = 0;
        if (!r.read_u32(version) || !r.read_string(nickname) || !r.read_u8(colorExplicit) ||
            !r.read_u32(preferredColor) || !r.ok())
        {
            Log.warn("Malformed Hello from peer {}", peer);
            return;
        }

        std::size_t admitted = 0;
        for (const auto& entry : mPeers) {
            if (entry.second.handshakeComplete) {
                ++admitted;
            }
        }
        const bool versionOk = version == kProtocolVersion;

        // ★ Mods, compared only once the versions agree: a peer on another protocol may lay the
        // rest of its Hello out differently, and is being refused for that anyway. Ours is
        // rebuilt per Hello rather than cached, because mods can be toggled while hosting.
        const ModFingerprint ourMods = local_mod_fingerprint();
        bool modsOk = true;
        ModFingerprint theirMods;
        std::string token;
        if (versionOk) {
            if (!read_mod_fingerprint(r, theirMods)) {
                Log.warn("Malformed Hello from peer {} (mod list)", peer);
                return;
            }
            // v28: the join-code session token, after the mods.
            std::uint8_t tokenLength = 0;
            if (!r.read_u8(tokenLength) || tokenLength > kMaxJoinTokenLength) {
                Log.warn("Malformed Hello from peer {} (token)", peer);
                return;
            }
            for (std::uint8_t i = 0; i < tokenLength; ++i) {
                std::uint8_t ch = 0;
                if (!r.read_u8(ch)) {
                    Log.warn("Malformed Hello from peer {} (token)", peer);
                    return;
                }
                token.push_back(static_cast<char>(ch));
            }
            const ModDiff diff = diff_mod_fingerprints(ourMods, theirMods, "we", nickname);
            Log.info("Mods: ours {}", describe_mod_fingerprint(ourMods));
            Log.info(
                "Mods: peer {} ('{}') {}", peer, nickname, describe_mod_fingerprint(theirMods));
            for (const auto& line : diff.cosmeticLines) {
                Log.info("Mods: cosmetic difference (allowed): {}", line);
            }
            if (diff.gameplayMismatch) {
                modsOk = false;
                for (const auto& line : diff.gameplayLines) {
                    Log.warn("Mods: gameplay difference: {}", line);
                }
            } else {
                Log.info("Mods: gameplay mods match ({} mod(s), hash 0x{:08X})",
                    ourMods.gameplayTotal, ourMods.gameplayHash);
            }
        }

        // The host is one of the kMaxPlayers.
        const bool roomOk = admitted + 1 < max_players();
        const bool accepted = versionOk && modsOk && roomOk;
        if (!versionOk) {
            Log.warn("Rejecting peer {}: protocol v{} != our v{}", peer, version, kProtocolVersion);
        } else if (!modsOk) {
            Log.warn("Rejecting peer {} ('{}'): gameplay mods differ", peer, nickname);
            notify(fmt::format("{} couldn't join", nickname),
                "Their gameplay mods don't match yours. The log lists the differences.", 6);
        } else if (!roomOk) {
            Log.warn("Rejecting peer {}: session full ({} players)", peer, max_players());
        }

        PeerSession& session = sender->second;

        if (!accepted) {
            // The host owns colour assignment; a refusal carries what playerId 0 would get.
            const std::uint32_t assignedColor =
                colorExplicit != 0 ? preferredColor : kPlayerColors[0];
            send_hello_ack(peer, false, 0, assignedColor, ourMods);
            // Hang up, not just say no. A refused peer left connected holds a player slot and keeps
            // a puppet-less ghost in the transport until it gives up on its own.
            refuse_peer(session, !versionOk ? DisconnectReason::VersionMismatch :
                                 !modsOk    ? DisconnectReason::ModMismatch :
                                              DisconnectReason::SessionFull);
            break;
        }

        session.nickname = nickname;
        session.mods = std::move(theirMods);
        session.helloColorExplicit = colorExplicit;
        session.helloPreferredColor = preferredColor;

        // v28: a guest that came through a join code. Empty = direct IP / LAN, admitted as ever.
        if (!token.empty()) {
            const auto pending = mHostCode.pendingJoins.find(token);
            if (pending == mHostCode.pendingJoins.end()) {
                // Not one the rendezvous gave us (or expired, or already used): someone who
                // learned our address but not through a code. No HelloAck: the reason in the
                // hang-up says it all.
                Log.warn("[mp-rdv] Rejecting peer {} ('{}'): unknown join token {}", peer, nickname,
                    token_prefix(token));
                refuse_peer(session, DisconnectReason::BadToken);
                break;
            }
            Log.info("[mp-rdv] peer {} ('{}') presented the token of join request '{}'", peer,
                nickname, pending->second.name);
            // Single use.
            mHostCode.pendingJoins.erase(pending);
            if (!getSettings().multiplayer.autoApproveJoins.getValue()) {
                // §4.2: a code is not an invitation. Nothing of the world goes out until the host
                // says yes (tick_join_approvals).
                session.awaitingApproval = true;
                session.approvalAskedAt = std::chrono::steady_clock::now();
                session.approvalDecision = 0;
                Log.info("[mp-rdv] '{}' wants to join; waiting for the host to allow it", nickname);
                notify(fmt::format("{} wants to join", nickname),
                    "Open the menu's Session page to Allow or Deny.",
                    static_cast<int>(kApprovalTimeout.count()));
                break;
            }
            Log.info("[mp-rdv] auto-approving '{}' (multiplayer.autoApproveJoins)", nickname);
        }

        admit_peer(peer, ourMods);
        break;
    }
    case PacketId::HelloAck: {
        std::uint8_t accepted = 0;
        std::uint32_t version = 0;
        std::uint32_t assignedId = 0;
        std::uint32_t assignedColor = 0;
        std::string hostNickname;
        std::uint32_t hostColor = 0;
        if (!r.read_u8(accepted) || !r.read_u32(version) || !r.read_u32(assignedId) ||
            !r.read_u32(assignedColor) || !r.read_string(hostNickname) || !r.read_u32(hostColor) ||
            !r.ok())
        {
            Log.warn("Malformed HelloAck from peer {}", peer);
            return;
        }

        // The host's mod list. Only from a host on our own protocol: an older one never sends it,
        // and a mismatched one is being reported as exactly that.
        ModFingerprint hostMods;
        bool haveHostMods = false;
        ModDiff modDiff;
        if (version == advertised_protocol_version()) {
            haveHostMods = read_mod_fingerprint(r, hostMods);
            if (!haveHostMods) {
                Log.warn("HelloAck from peer {} has no readable mod list", peer);
            } else {
                modDiff = diff_mod_fingerprints(mSentMods, hostMods, "you", "the host");
                Log.info("Mods: host {}", describe_mod_fingerprint(hostMods));
                for (const auto& line : modDiff.cosmeticLines) {
                    Log.info("Mods: cosmetic difference (allowed): {}", line);
                }
                for (const auto& line : modDiff.gameplayLines) {
                    Log.warn("Mods: gameplay difference: {}", line);
                }
            }
        }

        if (accepted == 0) {
            Log.error("Host rejected us (host protocol v{}, ours v{})", version,
                advertised_protocol_version());
            // Leave now rather than wait for the host's hang-up: a pre-v11 host never sends one,
            // and would leave us connected to nothing until the transport gave up. The host
            // states its own version and its mods, so with both matching the refusal was for
            // room.
            if (version != advertised_protocol_version()) {
                end_client_session("Couldn't join",
                    fmt::format("The host is running a different version of Dusklight (protocol "
                                "v{}, you have v{}). Both players need the same build.",
                        version, advertised_protocol_version()));
            } else if (haveHostMods && modDiff.gameplayMismatch) {
                end_client_session(
                    "Couldn't join: mods don't match", summarize_mod_mismatch(modDiff));
            } else {
                end_client_session("Couldn't join", "The session is full.");
            }
            return;
        }
        if (haveHostMods && modDiff.gameplayMismatch) {
            // The host compared the same two lists and admitted us anyway, so the two ends
            // disagree about the rule itself. Not refused here (the host owns admission), but
            // loud, because it means the session may desync.
            Log.error("Host admitted us although our gameplay mods differ; the two builds "
                      "classify mods differently");
        }
        if (mJoined) {
            Log.warn("Ignoring repeated HelloAck from peer {}", peer);
            return;
        }
        if (haveHostMods) {
            mHostMods = std::move(hostMods);
            mHaveHostMods = true;
        }

        // The host is authoritative on colour and identity, so take what it assigns rather than
        // deriving either locally — that keeps both ends agreeing on what this player is.
        mLocalColor = assignedColor;
        mLocalPlayerId = assignedId;

        if (auto it = mPeers.find(peer); it != mPeers.end()) {
            it->second.playerId = kHostPlayerId;
            it->second.nickname = hostNickname;
            it->second.color = hostColor;
            it->second.handshakeComplete = true;
        }

        // The host is a player too, and it is the one player never announced by PeerJoined.
        replication_manager().add_player(kHostPlayerId, hostNickname, hostColor);

        confirm_guest_join();
        mJoined = true;
        mHostNickname = hostNickname;
        Log.info("Handshake complete — we are player {} ('{}', #{:06X}), host is '{}' (#{:06X})",
            assignedId, mLocalNickname, mLocalColor, hostNickname, hostColor);
        notify(fmt::format("Joined {}'s session", hostNickname),
            "You are playing in their world. Their save is the one that counts.", 6);
        break;
    }
    case PacketId::Heartbeat: {
        std::uint32_t seq = 0;
        std::uint64_t senderTick = 0;
        if (!r.read_u32(seq) || !r.read_u64(senderTick) || !r.ok()) {
            return;
        }

        Writer w;
        w.write_u8(static_cast<std::uint8_t>(PacketId::HeartbeatAck));
        w.write_u32(seq);
        w.write_u64(senderTick);
        mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);
        break;
    }
    case PacketId::HeartbeatAck: {
        std::uint32_t seq = 0;
        std::uint64_t echoedTick = 0;
        if (!r.read_u32(seq) || !r.read_u64(echoedTick) || !r.ok()) {
            return;
        }

        const auto it = mPendingHeartbeats.find(seq);
        if (it == mPendingHeartbeats.end()) {
            return;
        }

        // Left in the map on purpose: the heartbeat was broadcast, so every peer answers this same
        // sequence, and erasing on the first reply silently denied everyone else an RTT. Expired
        // in send_heartbeats() instead.
        const auto elapsed = std::chrono::steady_clock::now() - it->second;
        const auto rttMs =
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count();

        if (auto peerIt = mPeers.find(peer); peerIt != mPeers.end()) {
            peerIt->second.lastRttMs = static_cast<std::uint32_t>(rttMs);
        }
        Log.info("Heartbeat #{} from peer {} — RTT {:.1f} ms (enet estimate {} ms)", seq, peer,
            rttMs, mTransport->rtt_ms(peer));
        break;
    }
    case PacketId::PlayerStateUpdate: {
        std::uint64_t senderTick = 0;
        PlayerState state;
        if (!r.read_u64(senderTick) || !state.read(r) || !r.ok()) {
            return;
        }
        const auto it = mPeers.find(peer);
        if (it == mPeers.end() || !it->second.handshakeComplete) {
            // Unauthenticated pose. Dropping it is the whole point of gating on the handshake.
            return;
        }
        // Stamped with the SENDER's tick, which is the only clock that describes when the pose was
        // actually true. Stamping with our own arrival tick instead (as this did until measured)
        // bakes network jitter into the spatial timeline: two poses one tick of motion apart that
        // arrive two ticks apart get stretched into a half-speed glide, and the next on-time one
        // lurches to catch up. Measured at 34.6% mean tick-to-tick change in the puppet's step
        // against the sender's own 1.3%.
        //
        // Mixing tick origins is not a risk here even though the two ends share no epoch: buffers
        // are per-player, so one only ever sees one sender's ticks, and playback is seeded relative
        // to the newest sample rather than to any absolute time.
        replication_manager().record_remote(it->second.playerId, senderTick, state);
        // Under the AllPlayers rule this is the vote that decides whether the sun moves. Taken from
        // the pose packet rather than asked for separately: it is one bit that was already free in
        // the flags byte, and it arrives at the sim rate for the same cost as nothing.
        world_clock().set_peer_time_can_pass(
            it->second.playerId, state.time_can_pass(), state.in_world());
        break;
    }
    case PacketId::ModsUpdate: {
        ModFingerprint theirs;
        if (!read_mod_fingerprint(r, theirs)) {
            Log.warn("Malformed ModsUpdate from peer {}", peer);
            return;
        }
        if (mRole == Role::Client) {
            if (!mJoined) {
                return;
            }
            // Recorded, not judged: the host owns admission and will hang up if we differ. Logged
            // so both ends' logs show the same comparison.
            const ModDiff diff =
                diff_mod_fingerprints(local_mod_fingerprint(), theirs, "you", "the host");
            Log.info("Mods: host changed to {}", describe_mod_fingerprint(theirs));
            for (const auto& line : diff.gameplayLines) {
                Log.warn("Mods: gameplay difference: {}", line);
            }
            mHostMods = std::move(theirs);
            mHaveHostMods = true;
            break;
        }
        // Host. The gate above guarantees an admitted, unrefused peer.
        auto& session = sender->second;
        Log.info("Mods: peer {} ('{}') changed to {}", peer, session.nickname,
            describe_mod_fingerprint(theirs));
        session.mods = std::move(theirs);
        recheck_peer_mods(session, local_mod_fingerprint());
        break;
    }
    case PacketId::TimeOverride: {
        float daytime = 0.0f;
        std::uint16_t date = 0;
        if (!r.read_f32(daytime) || !r.read_u16(date) || !r.ok()) {
            return;
        }
        const auto it = mPeers.find(peer);
        if (it == mPeers.end() || !it->second.handshakeComplete) {
            return;
        }
        if (mRole != Role::Host) {
            // Only the host arbitrates. A client receiving one of these means someone is confused
            // about who is authoritative, and adopting it would make the confusion contagious.
            Log.warn("Ignoring a TimeOverride from peer {} — this process is not the host", peer);
            return;
        }
        world_clock().receive_override(it->second.playerId, daytime, date);
        break;
    }
    case PacketId::WorldSnapshot: {
        std::uint64_t hostTick = 0;
        std::uint8_t count = 0;
        if (!r.read_u64(hostTick) || !r.read_u8(count)) {
            return;
        }
        if (count > kMaxPlayers) {
            Log.warn("Snapshot claims {} players (max {}) — dropping", count, kMaxPlayers);
            return;
        }
        if (const auto host = mPeers.find(peer);
            host == mPeers.end() || !host->second.handshakeComplete)
        {
            // Before HelloAck: drop it like every other unauthenticated packet. It carries the
            // host's clock, and a failed join may only give saving back if nothing of the host's
            // was applied (save_guard.hpp, abandon_guest_join).
            return;
        }
        for (std::uint8_t i = 0; i < count; ++i) {
            std::uint32_t playerId = 0;
            std::uint64_t originTick = 0;
            PlayerState state;
            if (!r.read_u32(playerId) || !r.read_u64(originTick) || !state.read(r)) {
                return;
            }
            if (playerId == mLocalPlayerId) {
                // Our own pose echoed back. We are authoritative over ourselves at M1, so
                // applying it would fight local input.
                continue;
            }
            // Each entry carries the clock of whoever produced it, so every player is interpolated
            // on the timeline they actually moved on. hostTick is kept only as the snapshot's own
            // send time; using it for the poses inside would re-quantise them onto our arrival
            // pattern and distort their speed.
            replication_manager().record_remote(playerId, originTick, state);
        }
        {
            // Read the clock BEFORE the ok() check so a truncated packet cannot half-apply it.
            float daytime = 0.0f;
            std::uint16_t date = 0;
            if (r.read_f32(daytime) && r.read_u16(date) && r.ok()) {
                world_clock().receive(daytime, date);
            }
        }
        if (!r.ok()) {
            Log.warn("Truncated snapshot from peer {}", peer);
        }
        break;
    }
    case PacketId::PeerList: {
        std::uint8_t count = 0;
        if (!r.read_u8(count) || count > kMaxPlayers) {
            return;
        }
        for (std::uint8_t i = 0; i < count; ++i) {
            std::uint32_t playerId = 0;
            std::string nickname;
            std::uint32_t color = 0;
            if (!r.read_u32(playerId) || !r.read_string(nickname) || !r.read_u32(color)) {
                return;
            }
            if (playerId == mLocalPlayerId) {
                continue;
            }
            replication_manager().add_player(playerId, nickname, color);
            Log.info("Roster: player {} ('{}', #{:06X})", playerId, nickname, color);
        }
        break;
    }
    case PacketId::PeerJoined: {
        std::uint32_t playerId = 0;
        std::string nickname;
        std::uint32_t color = 0;
        if (!r.read_u32(playerId) || !r.read_string(nickname) || !r.read_u32(color) || !r.ok()) {
            return;
        }
        if (playerId == mLocalPlayerId) {
            return;
        }
        replication_manager().add_player(playerId, nickname, color);
        Log.info("Player {} ('{}', #{:06X}) joined", playerId, nickname, color);
        notify(fmt::format("{} joined", nickname), "Another player is in the session.", 4);
        break;
    }
    case PacketId::PeerLeft: {
        std::uint32_t playerId = 0;
        if (!r.read_u32(playerId) || !r.ok()) {
            return;
        }
        if (playerId == mLocalPlayerId || playerId == kHostPlayerId) {
            // Neither is a third player. Losing the host is a transport event, not a roster one.
            return;
        }
        // Named before removal; the roster is the only place the name lives on a client.
        const std::string name = player_name(playerId);
        const bool known = replication_manager().players().count(playerId) != 0;
        replication_manager().remove_player(playerId);
        presence::on_peer_left(playerId);
        objects::on_peer_left(playerId);
        /* --- batch 8: enemies --- */
        enemies::on_peer_left(playerId);
        Log.info("Player {} left", playerId);
        if (known) {
            notify(fmt::format("{} left", name), "They are no longer in the session.", 4);
        }
        break;
    }
    case PacketId::Presence: {
        const std::uint32_t from = sender->second.playerId;
        if (mRole == Role::Host) {
            // The payload names its player ([u32 playerId] first, mp-m4-m5-world.md I.3), and is
            // relayed verbatim, so the far ends believe that name. A guest speaks only for itself.
            Reader peek = r;
            std::uint32_t claimed = 0;
            if (!peek.read_u32(claimed) || claimed != from) {
                if (warn_budget()) {
                    Log.warn("Dropping Presence from player {}: it claims to be player {}", from,
                        claimed);
                }
                return;
            }
            relay_to_others(peer, data, size, kChannelControl, true);
            mLastPresence[from].assign(data, data + size);
        }
        presence::on_packet(r, from);
        break;
    }
    case PacketId::WorldSync:
    case PacketId::WorldPropose:
    case PacketId::WorldCommit:
    case PacketId::WorldSyncRequest:
    case PacketId::WorldDigest:
        world_sync::on_packet(id, r, sender->second.playerId);
        break;
    case PacketId::ClaimRequest:  // v23, host-bound: judged by the host, answered to the asker
    case PacketId::ClaimReply:    // v23, client-bound: the host's verdict
        item_claims::on_packet(id, r, sender->second.playerId);
        break;
    case PacketId::ObjSpawn:
    case PacketId::ObjState:
    case PacketId::ObjEvent:
    case PacketId::ObjDespawn: {
        const std::uint32_t from = sender->second.playerId;
        if (mRole == Role::Host) {
            // Owner-originated: the host checks the sender may speak for that object before anyone
            // else hears it (mp-object-channel.md 1.2 "Direction policing").
            if (!objects::relay_allowed(id, r, from)) {
                if (warn_budget()) {
                    Log.warn("Dropping object packet {} from player {}: not theirs to send", rawId,
                        from);
                }
                return;
            }
            // Relayed on the channel it travels on: the state stream unreliable on channel 1, the
            // rest reliable and ordered on channel 2 so a despawn never overtakes its spawn. The
            // transport does not report the arrival channel; this is the same mapping by packet id.
            const bool stateStream = id == PacketId::ObjState;
            relay_to_others(
                peer, data, size, stateStream ? kChannelState : kChannelObjects, !stateStream);
        }
        objects::on_packet(id, r, from);
        break;
    }
    case PacketId::ObjClaim:        // host-bound: judged by the host, never relayed
    case PacketId::ObjClaimResult:  // client-bound: the host's verdict
        objects::on_packet(id, r, sender->second.playerId);
        break;
    case PacketId::PolicySync:  // v24, client-bound: the host's session-wide policy
        // Not in the clientBound list above (kept out of that shared expression so parallel
        // packet additions merge cleanly); on_packet refuses it on anything but a Client, which
        // is the same lock. A guest only reaches here once admitted (the mJoined gate above).
        policy_sync::on_packet(r);
        break;
    /* --- batch 7: drops --- */
    case PacketId::DropTaken:  // client-bound: the host granted a drop to someone
        objects::drops::on_packet(id, r, sender->second.playerId);
        break;
    /* --- batch 8: enemies --- */
    case PacketId::EnemyAuthority:  // client-bound (checked above): the host's authority table
        enemies::on_packet(id, r, sender->second.playerId);
        break;
    case PacketId::EnemyUpdate: {  // a room's authority -> host -> others, unreliable
        // Applied first, relayed only if on_packet vouched for it (the sender owns that room).
        if (enemies::on_packet(id, r, sender->second.playerId) && mRole == Role::Host) {
            relay_to_others(peer, data, size, kChannelState, false);
        }
        break;
    }
    case PacketId::RoomsLoaded: {  // peer -> host -> others, reliable
        if (enemies::on_packet(id, r, sender->second.playerId) && mRole == Role::Host) {
            relay_to_others(peer, data, size, kChannelObjects, true);
        }
        break;
    }
    /* --- batch 9: enemy hits --- */
    case PacketId::EnemyHit:        // attacker -> host -> the room's authority, reliable
    case PacketId::EnemyFlagHit: {  // same route
        // True only on the host, for a guest's event addressed to another guest's room: relayed
        // to the others, and only the addressed authority acts on it.
        if (enemies::on_packet(id, r, sender->second.playerId) && mRole == Role::Host) {
            relay_to_others(peer, data, size, kChannelObjects, true);
        }
        break;
    }
    /* --- batch 10B: enemy death and drops --- */
    case PacketId::EnemyDespawn: {  // a room's authority -> host -> others, reliable
        // Applied first, relayed only if on_packet vouched for it (the sender owns that room).
        if (enemies::on_packet(id, r, sender->second.playerId) && mRole == Role::Host) {
            relay_to_others(peer, data, size, kChannelObjects, true);
        }
        break;
    }
    default:
        Log.warn("Unknown packet id {} from peer {}", rawId, peer);
        break;
    }
}

void NetworkManager::send_peer_list(PeerId peer) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::PeerList));

    // The host is always in the roster, and is always first.
    std::uint8_t count = 1;
    for (const auto& entry : mPeers) {
        if (entry.second.handshakeComplete && entry.first != peer) {
            ++count;
        }
    }
    w.write_u8(count);
    w.write_u32(kHostPlayerId);
    w.write_string(mLocalNickname);
    w.write_u32(mLocalColor);

    for (const auto& entry : mPeers) {
        if (!entry.second.handshakeComplete || entry.first == peer) {
            continue;
        }
        w.write_u32(entry.second.playerId);
        w.write_string(entry.second.nickname);
        w.write_u32(entry.second.color);
    }

    mTransport->send(peer, w.data().data(), w.size(), kChannelControl, true);
}

void NetworkManager::broadcast_peer_joined(const PeerSession& joined) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::PeerJoined));
    w.write_u32(joined.playerId);
    w.write_string(joined.nickname);
    w.write_u32(joined.color);

    for (const auto& entry : mPeers) {
        if (!entry.second.handshakeComplete || entry.first == joined.id) {
            continue;
        }
        mTransport->send(entry.first, w.data().data(), w.size(), kChannelControl, true);
    }
}

void NetworkManager::broadcast_peer_left(std::uint32_t playerId) {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::PeerLeft));
    w.write_u32(playerId);
    mTransport->broadcast(w.data().data(), w.size(), kChannelControl, true);
}

namespace {

/**
 * Rewrite a held pose as Link standing in his ordinary idle at the same spot and facing
 * (puppet-events.md §3(a)). The rewrite sends the sender's answer, "standing", rather than a
 * made-up proc. Every field goes to the value the capture itself sends for a standing Link with
 * nothing in his hands:
 *
 *   - no action override, slot-1 anime, UPPER_2 overlay or root offsets (clear_action_anim's set);
 *   - no held item, bow BCK, iron ball or clawshot. A TOOL demo really does delete his items
 *     (d_a_alink_demo.inc:651-653), and the sword and shield go back on his back with them;
 *   - zero gait rate, standing (ZeroSpeed + ModeIdle), no skid, guard or foot-IK opt-out;
 *   - the plain WAIT idle, no attention set, head and torso straight, no lean;
 *   - no lantern flame: it is a held item.
 *
 * Position, facing, outfit, gear, location and the time vote are kept.
 */
void make_stand_in_idle(PlayerState& pose) {
    pose.moveRate = 0.0f;
    pose.flags |= kPlayerStateZeroSpeed | kPlayerStateModeIdle;
    pose.flags &= static_cast<std::uint8_t>(
        ~(kPlayerStateSharpTurn | kPlayerStateUpperGuard | kPlayerStateNoFootIk));
    pose.equip &= static_cast<std::uint8_t>(~(kPlayerEquipSwordInHand | kPlayerEquipShieldInHand));
    pose.idleKind = kPlayerIdleWait;
    pose.atnKind = kPlayerAtnNone;

    pose.neckX = 0;
    pose.neckY = 0;
    pose.bodyX = 0;
    pose.bodyTwist = 0;
    pose.bodyZ = 0;
    pose.bankZ = 0;

    // block (2)
    pose.actUnder = kPlayerActNone;
    pose.actUpper = kPlayerActNone;
    pose.actFrame = 0;
    pose.actUpperFrame = 0;
    pose.actRate = 0;
    pose.actMorf = 0;
    pose.actHandL = kPlayerActBodyHand;
    pose.actHandR = kPlayerActBodyHand;
    pose.actRootX = 0;
    pose.actRootY = 0;
    pose.actRootZ = 0;
    pose.locPhase = 0;
    // block (4)
    pose.act1Under = kPlayerActNone;
    pose.act1Upper = kPlayerActNone;
    pose.act1Phase = 0;
    pose.act1Ratio = 0;
    pose.actRootRotX = 0;
    pose.actRootRotZ = 0;
    pose.actYawOff = 0;
    pose.actPitch = 0;
    // block (3) and the v19/v20 item blocks
    pose.heldItem = kPlayerHeldNone;
    pose.heldFlags = 0;
    pose.heldMat = 0;
    pose.upper2Idx = kPlayerActNone;
    pose.upper2Frame = 0;
    pose.upper2Ratio = 0;
    pose.upper2Morf = 0;
    pose.heldBck = 0;
    pose.heldBckFrame = 0;
    pose.ironBallX = 0;
    pose.ironBallY = 0;
    pose.ironBallZ = 0;
    pose.ironBallAngX = 0;
    pose.ironBallAngY = 0;
    pose.ironBallLinks = 0;
    pose.ironBallState = 0;
    pose.clawFlags = 0;
    std::fill(std::begin(pose.clawTipFrame), std::end(pose.clawTipFrame), std::uint8_t{0});
    std::fill(std::begin(pose.clawTipPos), std::end(pose.clawTipPos), std::int16_t{0});
    std::fill(std::begin(pose.clawTipAng), std::end(pose.clawTipAng), std::int16_t{0});
    // v19 attack and v20 effects: no hitbox, no flame, no flash.
    pose.atMask = 0;
    pose.fxFlags = 0;
    pose.dmgFlash = 0;
}

}  // namespace

/**
 * ★ Always produce something to send. Going quiet is what used to cost the session its latency.
 *
 * capture_local() fails whenever world_is_playable() does: a load, the title screen, a cutscene, a
 * warp transition. That is a large fraction of a real session. The old code returned early and
 * sent nothing at all, which looks harmless — until you read it from the far end, where the
 * interpolation buffer has no way to tell "he has nothing to report" from "the link is dropping
 * packets". It assumes the latter, widens by up to five ticks per event, and then gives the
 * latency back at 0.5 ticks per five seconds. Measured on a staggered two-instance boot: the
 * buffer pinned at its 12-tick ceiling (400 ms) and was still at 5.5 ticks two minutes later.
 *
 * So we send every tick regardless. Holding the LAST GOOD pose, rather than sending a cleared
 * one, is deliberate: it reproduces exactly what the far end already displayed during these gaps
 * (its buffer held the last sample), so this change moves no puppet by a single unit. It only
 * stops the timeline stalling.
 *
 * Two exceptions to "hold it as it was" (puppet-events.md). During an STB cutscene the held pose is
 * rewritten as a standing idle at the same spot, which is Stuart's rule that the partner "sees the
 * other player standing still" (make_stand_in_idle). And a held pose from an earlier Link actor is
 * sent as not in the world: see below. The smaller events (talk, chest, item get, door, the
 * stage-entry walk-in) do not come here any more. capture_local() reads them live.
 *
 * Before the first readable pose there is genuinely nothing to show, so the flags stay clear and
 * drive_puppets() declines to spawn a puppet at the origin.
 */
PlayerState NetworkManager::wire_local_state() {
    // Edge state for the P2 stand-in log, shared by both paths so its "off" line is written when
    // live capture resumes, not only when the held pose stops being a cutscene.
    static bool s_standInOn = false;
    static int s_standInLogs = 0;
    static bool s_staleOn = false;
    static int s_staleLogs = 0;

    PlayerState state;
    if (replication_manager().capture_local(state)) {
        mLastLocalState = state;
        mHaveLastLocal = true;
        if (s_standInOn && s_standInLogs < 100) {
            s_standInLogs++;
            Log.info("Local stand-in idle off: live capture resumed at ({:.0f}, {:.0f}, {:.0f})",
                state.posX, state.posY, state.posZ);
        }
        s_standInOn = false;
        s_staleOn = false;
        pause::fill_wire_bit(state);  // batch 8: pause (gear bit 7, the menu as it is now)
        return state;
    }
    if (!mHaveLastLocal) {
        return PlayerState{};
    }
    PlayerState held = mLastLocalState;
    /* The held pose is a still frame, so nothing that is an event of one tick may repeat in it: a
     * swing caught as the cutscene began would otherwise keep its hitbox live on the far end for
     * the whole scene, and the spin and charge particles and the hit flash would play on a frozen
     * puppet. The lantern's flame is a state, not an event, and stays. */
    held.atMask = 0;
    held.fxFlags &= static_cast<std::uint8_t>(
        ~(kPlayerFxSwordTrail | kPlayerFxSpin | kPlayerFxSwordCharge | kPlayerFxVariantMask |
            kPlayerFxDmgTintMask));
    held.dmgFlash = 0;

    /* The held pose belongs to one Link actor. Once a different one exists (the new scene's Link
     * after a load, still being created or in an entry cutscene), it is the old stage's pose: it
     * has the old position, room and location epoch. Saying "in world" with it put the puppet back
     * at the exit it had just left, because the scene-change latch below clears as soon as the new
     * Link appears (puppet-events.md §2, REASONED from player_bridge.cpp, not measured). No Link at
     * all, mid-load, is the same answer. */
    const bool sameLink = local_player_is_last_captured();

    /* v21 (wolf piece A): the form block is the one part of the held pose kept CURRENT. The model
     * swap of a transformation is a held stretch (local_pose_is_live is off while his
     * mClothesChangeWaitTimer runs), and it is exactly when the far end needs kPlayerFormChanging
     * to draw the blob. refresh_held_form reads only scalars, never his models, which are freed for
     * part of that window. It does nothing unless this is the Link the pose came from. */
    if (sameLink) {
        refresh_held_form(held);
    }

    /* P2: an STB cutscene. Link is on the cast's marks playing demo-archive BCKs the far end does
     * not have, so his real pose cannot go out. Instead of a still frame of whatever he was doing
     * on the last free tick (often mid-stride or mid-swing), send what Stuart's rule asks for: him
     * standing at his last live position. The receiver plays its own WAIT through the ordinary path
     * and needs no change, and older receivers see the same thing. */
    const LocalEventKind kind = local_event_kind();
    const bool standIn = sameLink && kind == LocalEventKind::Cutscene;
    if (standIn) {
        make_stand_in_idle(held);
    }
    /* v21 event byte (agent EV): the event as it is NOW, not as it was on the last live tick, and
     * the stand-in bit where the stand-in was applied. This is the path a hidden Link (warp,
     * cannon) and an STB cutscene take, so without the refresh the far end would never see either.
     */
    fill_event(held);
    if (standIn) {
        held.evt |= kPlayerEvtStandIn;
    }
    if (standIn != s_standInOn && s_standInLogs < 100) {
        s_standInLogs++;
        Log.info("Local stand-in idle {} at ({:.0f}, {:.0f}, {:.0f})", standIn ? "on" : "off",
            held.posX, held.posY, held.posZ);
    }
    s_standInOn = standIn;

    /* Leaving the scene is the one gap where holding the pose is wrong: the load that follows can
     * stall this process for seconds, and the far end would show us frozen at the exit the whole
     * time (~2 s mid-roll, measured). Cleared in-world flag = the far end despawns the puppet,
     * the same path it takes for a load it can see. Cutscenes still hold, as the stand-in above. */
    const bool leaving = local_scene_change_queued();
    const bool stale = !sameLink && !leaving;
    if (stale && !s_staleOn && s_staleLogs < 100) {
        s_staleLogs++;
        Log.info("Local held pose is from an earlier Link (or none); sent as not in world");
    }
    s_staleOn = stale;
    if (leaving || !sameLink) {
        held.flags &= ~kPlayerStateInWorld;
    }
    /* batch 8: pause. The held copy's gear byte is from the last live tick; the menu bit must be
     * the current one (a menu can be open during an event, which is this path). */
    pause::fill_wire_bit(held);
    return held;
}

void NetworkManager::send_local_state() {
    const PlayerState state = wire_local_state();

    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::PlayerStateUpdate));
    w.write_u64(mSimTick);
    state.write(w);
    // Unreliable and unsequenced: a lost pose is always superseded by the next tick's, so
    // retransmitting it would only add latency to data that is already obsolete.
    mTransport->broadcast(w.data().data(), w.size(), kChannelState, false);
}

void NetworkManager::broadcast_snapshot() {
    if (mPeers.empty()) {
        return;
    }

    // Never conditional, and never skipped: see wire_local_state(). The host loading a room used
    // to drop itself out of its own snapshot, starving every client's buffer for the whole load.
    const PlayerState localState = wire_local_state();

    // Serialize once and send the same bytes to everyone — clients skip their own entry. Building
    // a per-recipient packet would cost N serializations for no benefit at this size.
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::WorldSnapshot));
    w.write_u64(mSimTick);

    std::uint8_t count = 1;
    for (const auto& entry : replication_manager().players()) {
        if (entry.second.hasLatest) {
            ++count;
        }
    }
    if (count > kMaxPlayers) {
        count = static_cast<std::uint8_t>(kMaxPlayers);
    }
    w.write_u8(count);

    w.write_u32(kHostPlayerId);
    // Our own pose was captured this tick, so our clock is its origin.
    w.write_u64(mSimTick);
    localState.write(w);
    std::uint8_t written = 1;
    for (const auto& entry : replication_manager().players()) {
        if (written >= count) {
            break;
        }
        if (!entry.second.hasLatest) {
            continue;
        }
        w.write_u32(entry.second.playerId);
        // Relayed verbatim on the ORIGINATING player's clock, not ours. Re-stamping here would
        // charge every third player the jitter of the first hop on top of their own, and would
        // turn a pose we simply haven't had an update for into a fresh sample claiming that player
        // stood still — which reads as a stutter rather than as the missing data it is.
        w.write_u64(entry.second.latestTick);
        entry.second.latest.write(w);
        ++written;
    }

    // The shared world clock rides along at the sim rate. Small, and a client that misses one is
    // corrected on the very next tick rather than drifting until some slower timer comes round.
    w.write_f32(world_clock().daytime());
    w.write_u16(world_clock().date());

    // v28 (§4.2): a join-code guest waiting on Allow / Deny is connected but must not see where
    // anyone is. Only then is it worth sending peer by peer.
    const bool anyAwaiting = std::any_of(mPeers.begin(), mPeers.end(),
        [](const auto& entry) { return entry.second.awaitingApproval; });
    if (anyAwaiting) {
        for (const auto& entry : mPeers) {
            if (entry.second.handshakeComplete && !entry.second.refused) {
                mTransport->send(entry.first, w.data().data(), w.size(), kChannelState, false);
            }
        }
        return;
    }
    mTransport->broadcast(w.data().data(), w.size(), kChannelState, false);
}

void NetworkManager::send_time_override() {
    float daytime = 0.0f;
    std::uint16_t date = 0;
    if (!world_clock().consume_pending_override(daytime, date)) {
        return;
    }

    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::TimeOverride));
    w.write_f32(daytime);
    w.write_u16(date);
    // Reliable: these are rare, discrete, and a dropped one loses the whole point of the event that
    // produced it — the player would watch the sky snap back to whatever the host still thinks.
    mTransport->broadcast(w.data().data(), w.size(), kChannelControl, true);
}

void NetworkManager::send_to_player(
    std::uint32_t playerId, const Writer& w, std::uint8_t channel, bool reliable) {
    if (!mTransport || w.size() == 0) {
        return;
    }
    for (const auto& [id, session] : mPeers) {
        if (session.handshakeComplete && !session.refused && session.playerId == playerId) {
            mTransport->send(id, w.data().data(), w.size(), channel, reliable);
            return;
        }
    }
}

void NetworkManager::send_to_session(
    const Writer& w, std::uint8_t channel, bool reliable, std::uint32_t exceptPlayerId) {
    if (!mTransport || w.size() == 0) {
        return;
    }
    // Admitted peers only, not the transport's broadcast: a peer still in its handshake, or one
    // being refused, has no business receiving the host's world.
    for (const auto& [id, session] : mPeers) {
        if (!session.handshakeComplete || session.refused || session.playerId == exceptPlayerId) {
            continue;
        }
        mTransport->send(id, w.data().data(), w.size(), channel, reliable);
    }
}

void NetworkManager::relay_to_others(
    PeerId from, const std::uint8_t* data, std::size_t size, std::uint8_t channel, bool reliable) {
    if (!mTransport || mRole != Role::Host) {
        return;
    }
    for (const auto& [id, session] : mPeers) {
        if (id == from || !session.handshakeComplete || session.refused) {
            continue;
        }
        mTransport->send(id, data, size, channel, reliable);
    }
}

void NetworkManager::send_presence() {
    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::Presence));
    if (!presence::build_if_changed(w)) {
        return;
    }
    // Reliable: sent only on change, so a lost one would leave the far end wrong until the next.
    send_to_session(w, kChannelControl, true);
    if (mRole == Role::Host) {
        mLastPresence[kHostPlayerId] = w.data();
    }
}

void NetworkManager::send_cached_presence(PeerId peer, std::uint32_t newcomer) {
    for (const auto& [playerId, bytes] : mLastPresence) {
        if (playerId == newcomer || bytes.empty()) {
            continue;
        }
        mTransport->send(peer, bytes.data(), bytes.size(), kChannelControl, true);
    }
}

void NetworkManager::report_interpolation() {
    if (mSimTick % kInterpReportIntervalTicks != 0) {
        return;
    }
    const ReplicationManager& replication = replication_manager();
    if (replication.empty()) {
        return;
    }
    const ReplicationManager::Diagnostics diag = replication.diagnostics();
    Log.info("Interp: {} remote player(s), buffer {:.1f} ticks, {} starvation(s), {} snap(s); "
             "pose ({:.0f}, {:.0f}, {:.0f}) angleY {} gait rate {:.3f}",
        replication.players().size(), diag.worstDelayTicks, diag.totalStarvations, diag.totalSnaps,
        diag.samplePose.posX, diag.samplePose.posY, diag.samplePose.posZ, diag.samplePose.angleY,
        diag.samplePose.moveRate);
}

void NetworkManager::send_heartbeats() {
    if (mPeers.empty() || mSimTick % kHeartbeatIntervalTicks != 0) {
        return;
    }

    const std::uint32_t seq = ++mHeartbeatSeq;
    mPendingHeartbeats[seq] = std::chrono::steady_clock::now();

    Writer w;
    w.write_u8(static_cast<std::uint8_t>(PacketId::Heartbeat));
    w.write_u32(seq);
    w.write_u64(mSimTick);
    mTransport->broadcast(w.data().data(), w.size(), kChannelControl, true);

    // Don't let unanswered heartbeats accumulate if a peer goes quiet.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = mPendingHeartbeats.begin(); it != mPendingHeartbeats.end();) {
        it = now - it->second > kHeartbeatExpiry ? mPendingHeartbeats.erase(it) : std::next(it);
    }
}

void NetworkManager::report_tick_rate() {
    if (mSimTick - mRateWindowStartTick < kRateReportIntervalTicks) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto seconds = std::chrono::duration<double>(now - mRateWindowStart).count();
    if (seconds > 0.0) {
        const double hz = static_cast<double>(mSimTick - mRateWindowStartTick) / seconds;
        Log.info("Sim tick rate: {:.1f} Hz over {} ticks ({} peers)", hz,
            mSimTick - mRateWindowStartTick, mPeers.size());
    }
    mRateWindowStart = now;
    mRateWindowStartTick = mSimTick;
}

}  // namespace dusk::mp
