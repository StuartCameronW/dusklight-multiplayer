// Dusklight multiplayer rendezvous -- join codes for ENet hole punching.
// Cloudflare Worker + SQLite-backed Durable Objects, Workers FREE plan only (see README.md "Cost guard").
//
// One Room DO per join code (idFromName(code)); one RateLimiter DO per client IP (idFromName("rl:" + ip)).
// Never console.log IPs, candidates, tokens or secrets: nothing here logs at all.

import { DurableObject } from "cloudflare:workers";

const CODE_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford base32: no I L O U
const CODE_RE = /^[0-9A-HJKMNP-TV-Z]{6}$/;
const SECRET_RE = /^[0-9a-f]{32}$/;
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
const RATE_LIMITS = { create: 5, join: 10 }; // per client IP per rolling 60 s
const RATE_WINDOW_MS = 60_000;

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
        out.push({ ip: c.ip, port: c.port, lan: c.lan === true });
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
    if (IPV4_RE.test(ip)) candidates.push({ ip, port: 0, lan: false, seen: true });
    return candidates;
}

async function rateLimited(env, ip, kind) {
    const stub = env.LIMITER.get(env.LIMITER.idFromName("rl:" + ip));
    const r = await stub.fetch("https://limiter/hit?kind=" + kind, { method: "POST" });
    return r.status === 429;
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

async function handleCreate(request, env, ip) {
    if (await rateLimited(env, ip, "create")) return json(429, { error: "rate_limited" });
    const body = await readJsonBody(request);
    if (body.error) return body.error;
    const { v, candidates, maxPlayers } = body.value;
    if (v !== 1) return badRequest();
    const cands = validateCandidates(candidates);
    if (!cands) return badRequest();
    if (!Number.isInteger(maxPlayers) || maxPlayers < MIN_PLAYERS || maxPlayers > MAX_PLAYERS) return badRequest();
    withHint(cands, ip);
    for (let i = 0; i < CREATE_ATTEMPTS; ++i) {
        const code = randomCode();
        const r = await postJson(roomStub(env, code), "/create", { code, candidates: cands, maxPlayers });
        if (r.status === 409) continue; // code taken by a live room, try another
        return r;
    }
    return json(503, { error: "busy" });
}

async function handleJoin(request, env, ip, code) {
    if (await rateLimited(env, ip, "join")) return json(429, { error: "rate_limited" });
    const body = await readJsonBody(request);
    if (body.error) return body.error;
    const { v, name, candidates } = body.value;
    if (v !== 1 || !validateName(name)) return badRequest();
    const cands = validateCandidates(candidates);
    if (!cands) return badRequest();
    withHint(cands, ip);
    return postJson(roomStub(env, code), "/join", { name, candidates: cands });
}

async function route(request, env) {
    if (env.DISABLED === "1") return json(503, { error: "disabled" });

    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean);
    if (parts[0] !== "v1" || parts[1] !== "rooms" || parts.length > 4) return json(404, { error: "not_found" });
    const ip = clientIp(request);
    const method = request.method;

    if (parts.length === 2) {
        if (method !== "POST") return json(405, { error: "method_not_allowed" });
        return handleCreate(request, env, ip);
    }

    const code = parts[2].toUpperCase();
    const action = parts[3] ?? "";

    if (action === "join") {
        if (method !== "POST") return json(405, { error: "method_not_allowed" });
        if (!CODE_RE.test(code)) {
            // Same rate-limit charge and same answer as a real-but-missing code: no enumeration oracle.
            if (await rateLimited(env, ip, "join")) return json(429, { error: "rate_limited" });
            return noRoom();
        }
        return handleJoin(request, env, ip, code);
    }

    // Host-only routes: authenticated by hostSecret. Bad code, bad secret, unknown and expired rooms all -> 404.
    const secret = url.searchParams.get("secret") ?? "";
    const authOk = CODE_RE.test(code) && SECRET_RE.test(secret);

    if (action === "") {
        if (method !== "DELETE") return json(405, { error: "method_not_allowed" });
        if (!authOk) return noRoom();
        return postJson(roomStub(env, code), "/delete", { secret });
    }
    if (action === "wait") {
        if (method !== "GET") return json(405, { error: "method_not_allowed" });
        if (env.LONGPOLL === "0") return json(410, { error: "longpoll_disabled" });
        if (!authOk) return noRoom();
        return postJson(roomStub(env, code), "/wait", { secret });
    }
    if (action === "ws") {
        if (method !== "GET") return json(405, { error: "method_not_allowed" });
        if ((request.headers.get("Upgrade") || "").toLowerCase() !== "websocket") {
            return json(426, { error: "upgrade_required" });
        }
        if (!authOk) return noRoom();
        const headers = new Headers(request.headers);
        headers.set("x-dusk-secret", secret);
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
// room = { code, hostSecret, maxPlayers, hostCandidates, pending: [guest], admitted, expiresAt, persistedExpiry }
// guest = { joinId, name, candidates, token }

export class Room extends DurableObject {
    constructor(ctx, env) {
        super(ctx, env);
        this.room = null;
        this.waiter = null; // resolve() of the one held long-poll, if any
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
                return; // keep them pending; the host's next connect or "sync" gets them
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
        // v1: a slot is spent per issued token; guests that never connect are not given back.
        if (r.admitted >= r.maxPlayers - 1) return json(409, { error: "full" });
        const guest = { joinId: randomHex(8), name, candidates, token: randomHex(16) };
        r.admitted += 1;
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

    async onWebSocket(request) {
        const r = await this.live();
        if (!r || !secretMatches(request.headers.get("x-dusk-secret"), r.hostSecret)) return noRoom();
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

    // Any text other than the auto-answered "ping" (e.g. "sync") re-sends whatever is pending.
    async webSocketMessage(ws, message) {
        if (!(await this.live())) {
            try {
                ws.close(1000, "room closed");
            } catch {}
            return;
        }
        await this.deliver();
    }

    async webSocketClose(ws, code, reason, wasClean) {
        try {
            ws.close(1000, "bye");
        } catch {}
        if (await this.live()) await this.touch(true); // TTL counts from the host's disconnect
    }

    async webSocketError(ws, error) {
        if (await this.live()) await this.touch(true);
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
