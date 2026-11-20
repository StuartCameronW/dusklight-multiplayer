// Dusklight multiplayer rendezvous -- join codes for ENet hole punching.
// Cloudflare Worker + SQLite-backed Durable Objects, Workers FREE plan only (see README.md "Cost guard").
//
// One Room DO per join code (idFromName(code)); one RateLimiter DO per client (idFromName("rl:" + key), where the
// key is the IPv4 address or the IPv6 /64); one CreateCap DO for the whole service (the global daily create cap).
// Never console.log IPs, candidates, tokens or secrets: nothing here logs at all.

import { DurableObject } from "cloudflare:workers";

const CODE_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford base32: no I L O U
const CODE_RE = /^[0-9A-HJKMNP-TV-Z]{6}$/;
const SECRET_RE = /^[0-9a-f]{32}$/;
const JOIN_ID_RE = /^[0-9a-f]{16}$/;
const IPV4_RE = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/;

const TTL_S = 7200;              // room lifetime after the host's last sign of life
const TTL_PERSIST_SLACK_MS = 600_000; // only rewrite the stored expiry when it drifts by >10 min (saves rows written)
const WAIT_HOLD_MS = 25_000;     // host long-poll hold
const MAX_BODY = 8192;           // bytes
const MAX_CANDIDATES = 8;        // client-supplied; the server adds one CF-Connecting-IP hint on top
const MAX_NAME = 32;             // code points
const MIN_PLAYERS = 2;
const MAX_PLAYERS = 8;
const CREATE_ATTEMPTS = 5;
const RATE_LIMITS = { create: 5, join: 10 }; // per client (IPv4 address / IPv6 /64) per rolling 60 s
const PRE_LIMITS = { create: 5, join: 10, ws: 12 }; // the same, per isolate, checked before any DO is called
const PRE_LIMIT_MAX_KEYS = 10_000;  // the per-isolate table is dropped wholesale past this (memory bound)
const RATE_WINDOW_MS = 60_000;
const WS_UPGRADE_MIN_MS = 5000;     // per room: at most one host-socket upgrade per 5 s
const MAX_UNSETTLED_JOINS = 2;      // per room: joins issued in the last PENDING_WINDOW_MS and not released
const PENDING_WINDOW_MS = 90_000;   // the host's own deadline: 60 s for the Hello + 30 s for Allow / Deny
const DEFAULT_CREATE_CAP = 3000;    // rooms per UTC day for the whole service; wrangler.toml CREATE_CAP_PER_DAY

// lan=false candidates must be global unicast: the host UDP-punches them before anyone has been let in.
const NON_GLOBAL_V4 = [
    ["0.0.0.0", 8], ["10.0.0.0", 8], ["100.64.0.0", 10], ["127.0.0.0", 8], ["169.254.0.0", 16],
    ["172.16.0.0", 12], ["192.0.0.0", 24], ["192.168.0.0", 16], ["198.18.0.0", 15], ["224.0.0.0", 4],
    ["240.0.0.0", 4],
].map(([base, bits]) => [v4Number(base), bits]);

// ---------------------------------------------------------------------------------------------------------------
// helpers

function json(status, obj) {
    return new Response(JSON.stringify(obj), {
        status,
        headers: { "content-type": "application/json", "cache-control": "no-store" },
    });
}

function empty(status) {
    return new Response(null, { status, headers: { "cache-control": "no-store" } });
}

const noRoom = () => json(404, { error: "no_room" });
const badRequest = () => json(400, { error: "bad_request" });

function randomHex(bytes) {
    const b = new Uint8Array(bytes);
    crypto.getRandomValues(b);
    let s = "";
    for (const x of b) s += x.toString(16).padStart(2, "0");
    return s;
}

function randomCode() {
    const b = new Uint8Array(6);
    crypto.getRandomValues(b);
    let s = "";
    for (const x of b) s += CODE_ALPHABET[x & 31]; // 256 % 32 == 0, so unbiased
    return s;
}

function secretMatches(given, actual) {
    if (typeof given !== "string" || typeof actual !== "string" || given.length !== actual.length) return false;
    const enc = new TextEncoder();
    return crypto.subtle.timingSafeEqual(enc.encode(given), enc.encode(actual));
}

function clientIp(request) {
    return request.headers.get("CF-Connecting-IP") || "unknown";
}

function v4Number(ip) {
    return ip.split(".").reduce((n, o) => n * 256 + Number(o), 0);
}

/** True for a dotted IPv4 address outside every NON_GLOBAL_V4 range. */
function isGlobalV4(ip) {
    if (!IPV4_RE.test(ip)) return false;
    const n = v4Number(ip);
    for (const [base, bits] of NON_GLOBAL_V4) {
        const size = 2 ** (32 - bits);
        if (Math.floor(n / size) === Math.floor(base / size)) return false;
    }
    return true;
}

/**
 * The rate-limit key for a client address: IPv4 as is, IPv6 by its /64. One subscriber gets a whole /64 (often
 * more), so keying on the full IPv6 address would let a client rotate past every per-address limit.
 */
function clientKey(ip) {
    if (!ip.includes(":")) return ip;
    let s = ip.toLowerCase().split("%")[0];
    const v4tail = s.match(/^(.*:)(\d+\.\d+\.\d+\.\d+)$/);
    if (v4tail && IPV4_RE.test(v4tail[2])) {
        const o = v4tail[2].split(".").map(Number);
        s = v4tail[1] + ((o[0] << 8) | o[1]).toString(16) + ":" + ((o[2] << 8) | o[3]).toString(16);
    }
    const halves = s.split("::");
    if (halves.length > 2) return "v6:" + s;
    const head = halves[0] ? halves[0].split(":") : [];
    const tail = halves.length === 2 && halves[1] ? halves[1].split(":") : [];
    const fill = halves.length === 2 ? 8 - head.length - tail.length : 0;
    const full = [...head, ...Array(Math.max(fill, 0)).fill("0"), ...tail];
    if (full.length !== 8 || !full.every((h) => /^[0-9a-f]{1,4}$/.test(h))) return "v6:" + s;
    const hextets = full.map((h) => parseInt(h, 16));
    if (hextets.slice(0, 5).every((h) => h === 0) && hextets[5] === 0xffff) {
        return [hextets[6] >> 8, hextets[6] & 255, hextets[7] >> 8, hextets[7] & 255].join("."); // v4-mapped
    }
    return "v6:" + hextets.slice(0, 4).map((h) => h.toString(16)).join(":") + "::/64";
}

// Per-isolate pre-limiter. It cannot stop a client spread over many isolates (the RateLimiter DO does that), but
// it turns away the bulk of a flood without spending a DO request on it.
const preHits = new Map(); // kind + "|" + key -> timestamps within the window

function preLimited(key, kind) {
    const now = Date.now();
    const k = kind + "|" + key;
    const recent = (preHits.get(k) || []).filter((t) => now - t < RATE_WINDOW_MS);
    if (recent.length >= PRE_LIMITS[kind]) {
        preHits.set(k, recent);
        return true;
    }
    recent.push(now);
    if (!preHits.has(k) && preHits.size >= PRE_LIMIT_MAX_KEYS) preHits.clear();
    preHits.set(k, recent);
    return false;
}

/** Reads a JSON object body of at most MAX_BODY bytes. Returns { value } or { error: Response }. */
async function readJsonBody(request) {
    const len = request.headers.get("content-length");
    if (len !== null && !(Number(len) <= MAX_BODY)) return { error: json(413, { error: "too_large" }) };
    if (!request.body) return { error: badRequest() };
    const reader = request.body.getReader();
    const chunks = [];
    let total = 0;
    for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        total += value.byteLength;
        if (total > MAX_BODY) {
            await reader.cancel().catch(() => {});
            return { error: json(413, { error: "too_large" }) };
        }
        chunks.push(value);
    }
    const buf = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) {
        buf.set(c, off);
        off += c.byteLength;
    }
    let value;
    try {
        value = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(buf));
    } catch {
        return { error: badRequest() };
    }
    if (value === null || typeof value !== "object" || Array.isArray(value)) return { error: badRequest() };
    return { value };
}

/** Returns a cleaned candidate list, or null if anything is invalid. Unknown fields are dropped. */
function validateCandidates(arr) {
    if (!Array.isArray(arr) || arr.length > MAX_CANDIDATES) return null;
    const out = [];
    for (const c of arr) {
        if (c === null || typeof c !== "object" || Array.isArray(c)) return null;
        if (typeof c.ip !== "string" || !IPV4_RE.test(c.ip)) return null;
        if (!Number.isInteger(c.port) || c.port < 0 || c.port > 65535) return null;
        if (c.lan !== undefined && typeof c.lan !== "boolean") return null;
        const lan = c.lan === true;
        if (!lan && !isGlobalV4(c.ip)) return null; // a "public" candidate must be global unicast
        out.push({ ip: c.ip, port: c.port, lan });
    }
    return out;
}

function validateName(name) {
    if (typeof name !== "string") return false;
    const n = [...name].length;
    if (n < 1 || n > MAX_NAME) return false;
    return !/\p{C}/u.test(name); // no control / format / private-use / unassigned / lone-surrogate characters
}

/** The requester's address as seen by Cloudflare, as a diagnostics-only candidate (HTTP can't tell us its UDP port). */
function withHint(candidates, ip) {
    if (isGlobalV4(ip)) candidates.push({ ip, port: 0, lan: false, seen: true });
    return candidates;
}

/** Per-isolate check first, then the client's RateLimiter DO. */
async function rateLimited(env, key, kind) {
    if (preLimited(key, kind)) return true;
    const stub = env.LIMITER.get(env.LIMITER.idFromName("rl:" + key));
    const r = await stub.fetch("https://limiter/hit?kind=" + kind, { method: "POST" });
    return r.status === 429;
}

/** Spends one of the service's rooms for today; false once the global daily cap is used up. */
async function createAllowed(env) {
    const stub = env.CREATE_CAP.get(env.CREATE_CAP.idFromName("global"));
    const r = await stub.fetch("https://cap/hit", { method: "POST" });
    return r.status !== 429;
}

function createCap(env) {
    const n = Number.parseInt(env.CREATE_CAP_PER_DAY ?? "", 10);
    return Number.isInteger(n) && n >= 0 ? n : DEFAULT_CREATE_CAP;
}

/**
 * The host secret of a host-only POST: from the JSON body {secret} if there is one, else from ?secret=. Returns
 * { secret, body } or { error: Response }. @p needBody: the route needs other fields from the body.
 */
async function hostCredentials(request, url, needBody) {
    const query = url.searchParams.get("secret");
    const hasBody = request.body !== null && request.headers.get("content-length") !== "0";
    let body = {};
    if (needBody || (query === null && hasBody)) {
        const read = await readJsonBody(request);
        if (read.error) return read;
        body = read.value;
    }
    const secret = typeof body.secret === "string" ? body.secret : (query ?? "");
    return { secret, body };
}

function roomStub(env, code) {
    return env.ROOMS.get(env.ROOMS.idFromName(code));
}

function postJson(stub, path, obj) {
    return stub.fetch("https://room" + path, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(obj),
    });
}

// ---------------------------------------------------------------------------------------------------------------
// Worker entry: routing + validation. The DOs trust what the Worker hands them.

// Every handler validates the request completely BEFORE it calls a Durable Object, so a malformed request costs one
// Worker request and nothing else.

async function handleCreate(request, env, ip, key) {
    const body = await readJsonBody(request);
    if (body.error) return body.error;
    const { v, candidates, maxPlayers } = body.value;
    if (v !== 1) return badRequest();
    const cands = validateCandidates(candidates);
    if (!cands) return badRequest();
    if (!Number.isInteger(maxPlayers) || maxPlayers < MIN_PLAYERS || maxPlayers > MAX_PLAYERS) return badRequest();
    if (await rateLimited(env, key, "create")) return json(429, { error: "rate_limited" });
    if (!(await createAllowed(env))) return json(503, { error: "daily_cap" });
    withHint(cands, ip);
    for (let i = 0; i < CREATE_ATTEMPTS; ++i) {
        const code = randomCode();
        const r = await postJson(roomStub(env, code), "/create", { code, candidates: cands, maxPlayers });
        if (r.status === 409) continue; // code taken by a live room, try another
        return r;
    }
    return json(503, { error: "busy" });
}

async function handleJoin(request, env, ip, key, code) {
    const body = await readJsonBody(request);
    if (body.error) return body.error;
    const { v, name, candidates } = body.value;
    if (v !== 1 || !validateName(name)) return badRequest();
    const cands = validateCandidates(candidates);
    if (!cands) return badRequest();
    if (await rateLimited(env, key, "join")) return json(429, { error: "rate_limited" });
    withHint(cands, ip);
    return postJson(roomStub(env, code), "/join", { name, candidates: cands });
}

async function route(request, env) {
    if (env.DISABLED === "1") return json(503, { error: "disabled" });

    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean);
    if (parts[0] !== "v1" || parts[1] !== "rooms" || parts.length > 4) return json(404, { error: "not_found" });
    const ip = clientIp(request);
    const key = clientKey(ip);
    const method = request.method;

    if (parts.length === 2) {
        if (method !== "POST") return json(405, { error: "method_not_allowed" });
        return handleCreate(request, env, ip, key);
    }

    const code = parts[2].toUpperCase();
    const action = parts[3] ?? "";

    if (action === "join") {
        if (method !== "POST") return json(405, { error: "method_not_allowed" });
        // A code outside the public alphabet can never name a room, so answering at once leaks nothing.
        if (!CODE_RE.test(code)) return noRoom();
        return handleJoin(request, env, ip, key, code);
    }

    // Host-only routes: authenticated by hostSecret. Bad code, bad secret, unknown and expired rooms all -> 404.
    const querySecret = url.searchParams.get("secret") ?? "";
    const authOk = CODE_RE.test(code) && SECRET_RE.test(querySecret);

    if (action === "") {
        if (method !== "DELETE") return json(405, { error: "method_not_allowed" });
        if (!authOk) return noRoom();
        return postJson(roomStub(env, code), "/delete", { secret: querySecret });
    }
    // Same as DELETE, for clients whose HTTP layer has no DELETE (the game's borealis::http: GET/POST/HEAD).
    // The secret may come in the JSON body {secret} instead of the query, which keeps it out of URLs.
    if (action === "close" || action === "release") {
        if (method !== "POST") return json(405, { error: "method_not_allowed" });
        if (!CODE_RE.test(code)) return noRoom();
        const cred = await hostCredentials(request, url, action === "release");
        if (cred.error) return cred.error;
        if (!SECRET_RE.test(cred.secret)) return noRoom();
        if (action === "close") return postJson(roomStub(env, code), "/delete", { secret: cred.secret });
        const joinId = cred.body.joinId;
        if (typeof joinId !== "string" || !JOIN_ID_RE.test(joinId)) return badRequest();
        return postJson(roomStub(env, code), "/release", { secret: cred.secret, joinId });
    }
    if (action === "wait") {
        if (method !== "GET") return json(405, { error: "method_not_allowed" });
        if (env.LONGPOLL === "0") return json(410, { error: "longpoll_disabled" });
        if (!authOk) return noRoom();
        return postJson(roomStub(env, code), "/wait", { secret: querySecret });
    }
    if (action === "ws") {
        if (method !== "GET") return json(405, { error: "method_not_allowed" });
        if ((request.headers.get("Upgrade") || "").toLowerCase() !== "websocket") {
            return json(426, { error: "upgrade_required" });
        }
        if (!authOk) return noRoom();
        if (preLimited(key, "ws")) return json(429, { error: "rate_limited" });
        const headers = new Headers(request.headers);
        headers.set("x-dusk-secret", querySecret);
        return roomStub(env, code).fetch("https://room/ws", { headers });
    }
    return json(404, { error: "not_found" });
}

export default {
    async fetch(request, env) {
        try {
            return await route(request, env);
        } catch {
            return json(500, { error: "server_error" }); // deliberately not logged: errors may carry request data
        }
    },
};

// ---------------------------------------------------------------------------------------------------------------
// Room: one per join code. State lives in ONE storage key ("room") so each change costs one row written.
//
// room = { code, hostSecret, maxPlayers, hostCandidates, pending: [guest], admitted, slots: [slot], expiresAt,
//          persistedExpiry }
// guest = { joinId, name, candidates, token };  slot = { joinId, at }: one per issued token until the host releases it
// (rooms stored before slots existed have none; their admitted count simply can't be released).

export class Room extends DurableObject {
    constructor(ctx, env) {
        super(ctx, env);
        this.room = null;
        this.waiter = null; // resolve() of the one held long-poll, if any
        this.lastUpgradeAt = 0; // memory only: losing it means the object sat idle, i.e. nobody was reconnecting
        // Host keepalive: "ping" -> "pong" is answered by the runtime without waking the object (no duration billed).
        ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
        ctx.blockConcurrencyWhile(async () => {
            this.room = (await ctx.storage.get("room")) ?? null;
        });
    }

    hostSockets() {
        return this.ctx.getWebSockets("host").filter((ws) => ws.readyState === 1);
    }

    /** The room if it exists and has not expired; expires it (deleting everything) otherwise. */
    async live() {
        if (!this.room) return null;
        if (Date.now() >= this.room.expiresAt && this.hostSockets().length === 0) {
            await this.expire();
            return null;
        }
        return this.room;
    }

    async save() {
        await this.ctx.storage.put("room", this.room);
    }

    /** Pushes the expiry to now + TTL; persists (put + setAlarm) only when it drifted far from the stored one. */
    async touch(force = false) {
        const r = this.room;
        r.expiresAt = Date.now() + TTL_S * 1000;
        if (force || r.expiresAt - r.persistedExpiry > TTL_PERSIST_SLACK_MS) {
            r.persistedExpiry = r.expiresAt;
            await this.save();
            await this.ctx.storage.setAlarm(r.expiresAt);
        }
    }

    async expire() {
        for (const ws of this.ctx.getWebSockets()) {
            try {
                ws.close(1000, "room closed");
            } catch {}
        }
        this.room = null;
        this.wake(false);
        await this.ctx.storage.deleteAlarm();
        await this.ctx.storage.deleteAll();
    }

    wake(value) {
        const w = this.waiter;
        this.waiter = null;
        if (w) w(value);
    }

    async takePending() {
        const g = this.room.pending;
        this.room.pending = [];
        await this.save();
        return g;
    }

    /** Hands pending joins to the host: its WebSocket if connected, else a held long-poll, else they wait. */
    async deliver() {
        const r = this.room;
        if (!r || r.pending.length === 0) return;
        const sockets = this.hostSockets();
        if (sockets.length > 0) {
            try {
                sockets[sockets.length - 1].send(JSON.stringify({ type: "guests", guests: r.pending }));
            } catch {
                return; // keep them pending; the host's next connect gets them
            }
            await this.takePending();
            return;
        }
        this.wake(true);
    }

    async alarm() {
        if (!this.room) {
            await this.ctx.storage.deleteAll();
            return;
        }
        if (this.hostSockets().length > 0) {
            await this.touch(true); // a connected host keeps its room alive
            return;
        }
        if (Date.now() < this.room.expiresAt) {
            // In-memory expiry was refreshed past the stored one; re-arm for it.
            this.room.persistedExpiry = this.room.expiresAt;
            await this.save();
            await this.ctx.storage.setAlarm(this.room.expiresAt);
            return;
        }
        await this.expire();
    }

    async fetch(request) {
        const path = new URL(request.url).pathname;
        if (path === "/ws") return this.onWebSocket(request);
        const body = await request.json();
        switch (path) {
            case "/create":
                return this.onCreate(body);
            case "/join":
                return this.onJoin(body);
            case "/wait":
                return this.onWait(body);
            case "/delete":
                return this.onDelete(body);
            case "/release":
                return this.onRelease(body);
        }
        return json(404, { error: "not_found" });
    }

    async onCreate({ code, candidates, maxPlayers }) {
        if (await this.live()) return json(409, { error: "taken" });
        const expires = Date.now() + TTL_S * 1000;
        this.room = {
            code,
            hostSecret: randomHex(16),
            maxPlayers,
            hostCandidates: candidates,
            pending: [],
            admitted: 0,
            slots: [],
            expiresAt: expires,
            persistedExpiry: expires,
        };
        await this.save();
        await this.ctx.storage.setAlarm(expires);
        return json(201, { code, hostSecret: this.room.hostSecret, ttl: TTL_S });
    }

    async onJoin({ name, candidates }) {
        const r = await this.live();
        if (!r) return noRoom();
        // A slot is spent per issued token, until the host gives it back with /release (Deny, or no Hello in time).
        if (r.admitted >= r.maxPlayers - 1) return json(409, { error: "full" });
        // Someone with the code can't fill the room for good: at most MAX_UNSETTLED_JOINS joins the host hasn't had
        // the time to settle yet. Past PENDING_WINDOW_MS the host has decided, and a refused join is released.
        const now = Date.now();
        r.slots ??= [];
        if (r.slots.filter((s) => now - s.at < PENDING_WINDOW_MS).length >= MAX_UNSETTLED_JOINS) {
            return json(429, { error: "busy" });
        }
        const guest = { joinId: randomHex(8), name, candidates, token: randomHex(16) };
        r.admitted += 1;
        r.slots.push({ joinId: guest.joinId, at: now });
        r.pending.push(guest);
        await this.save();
        await this.deliver();
        return json(200, { joinId: guest.joinId, token: guest.token, host: { candidates: r.hostCandidates } });
    }

    async onWait({ secret }) {
        const r = await this.live();
        if (!r || !secretMatches(secret, r.hostSecret)) return noRoom();
        await this.touch();
        if (r.pending.length > 0) return json(200, { guests: await this.takePending() });

        this.wake(false); // a newer wait supersedes an older one (it answers 204)
        const got = await new Promise((resolve) => {
            const timer = setTimeout(() => {
                if (this.waiter === done) this.waiter = null;
                resolve(false);
            }, WAIT_HOLD_MS);
            const done = (v) => {
                clearTimeout(timer);
                resolve(v);
            };
            this.waiter = done;
        });
        if (got && this.room && this.room.pending.length > 0) {
            return json(200, { guests: await this.takePending() });
        }
        return empty(204);
    }

    async onDelete({ secret }) {
        const r = await this.live();
        if (!r || !secretMatches(secret, r.hostSecret)) return noRoom();
        await this.expire();
        return empty(204);
    }

    /** The host gives a guest's slot back (it denied them, or their Hello never came). Unknown joinIds: 204, no-op. */
    async onRelease({ secret, joinId }) {
        const r = await this.live();
        if (!r || !secretMatches(secret, r.hostSecret)) return noRoom();
        const i = (r.slots ?? []).findIndex((s) => s.joinId === joinId);
        if (i < 0) return empty(204);
        r.slots.splice(i, 1);
        r.admitted = Math.max(0, r.admitted - 1);
        r.pending = r.pending.filter((g) => g.joinId !== joinId);
        await this.save();
        return empty(204);
    }

    async onWebSocket(request) {
        const r = await this.live();
        if (!r || !secretMatches(request.headers.get("x-dusk-secret"), r.hostSecret)) return noRoom();
        // A reconnect loop would otherwise cost a socket close plus a touch per round. Checked after the secret, so
        // someone who only knows the code can't lock the host out.
        const now = Date.now();
        if (now - this.lastUpgradeAt < WS_UPGRADE_MIN_MS) return json(429, { error: "rate_limited" });
        this.lastUpgradeAt = now;
        for (const old of this.ctx.getWebSockets("host")) {
            try {
                old.close(4000, "replaced");
            } catch {}
        }
        const [client, server] = Object.values(new WebSocketPair());
        this.ctx.acceptWebSocket(server, ["host"]); // Hibernation API: idle sockets cost no duration
        await this.touch();
        await this.deliver();
        return new Response(null, { status: 101, webSocket: client });
    }

    // The host only ever sends "ping", which the runtime answers without waking us. Anything else reaching here is a
    // flood (each frame would wake the object): hang up at once and do nothing else. A host that wants what is
    // pending reconnects, and onWebSocket delivers it.
    async webSocketMessage(ws, message) {
        try {
            ws.close(1008, "unexpected message");
        } catch {}
    }

    // Not forced: a forced touch is a put + setAlarm (2 rows) per disconnect. Unforced, the stored expiry may lag the
    // real one by up to TTL_PERSIST_SLACK_MS, so after an eviction the room can expire up to 10 min early.
    async webSocketClose(ws, code, reason, wasClean) {
        try {
            ws.close(1000, "bye");
        } catch {}
        if (await this.live()) await this.touch(); // TTL counts from the host's disconnect
    }

    async webSocketError(ws, error) {
        if (await this.live()) await this.touch();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// CreateCap: one for the whole service. Caps the rooms created per UTC day (CREATE_CAP_PER_DAY), so that no number
// of clients or addresses can spend the daily rows-written allowance on creates. One row written per create.

export class CreateCap extends DurableObject {
    constructor(ctx, env) {
        super(ctx, env);
        this.day = "";
        this.count = 0;
        ctx.blockConcurrencyWhile(async () => {
            const saved = await ctx.storage.get("cap");
            if (saved) {
                this.day = saved.day;
                this.count = saved.count;
            }
        });
    }

    async fetch(request) {
        const day = new Date().toISOString().slice(0, 10);
        if (day !== this.day) {
            this.day = day;
            this.count = 0;
        }
        if (this.count >= createCap(this.env)) return empty(429);
        this.count += 1;
        await this.ctx.storage.put("cap", { day: this.day, count: this.count });
        return empty(204);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// RateLimiter: one per client IP. Memory only -- no storage, so it costs requests but never rows written.
// If the runtime evicts it, that only happens after it sat idle, i.e. the caller had already stopped.

export class RateLimiter extends DurableObject {
    constructor(ctx, env) {
        super(ctx, env);
        this.hits = new Map(); // kind -> timestamps within the window
    }

    async fetch(request) {
        const kind = new URL(request.url).searchParams.get("kind");
        const limit = RATE_LIMITS[kind];
        if (!limit) return empty(400);
        const now = Date.now();
        const recent = (this.hits.get(kind) || []).filter((t) => now - t < RATE_WINDOW_MS);
        if (recent.length >= limit) {
            this.hits.set(kind, recent);
            return empty(429);
        }
        recent.push(now);
        this.hits.set(kind, recent);
        return empty(204);
    }
}
