# dusk-mp-rendezvous

A small Cloudflare Worker that turns a 6-character **join code** into the two peers' network candidates, so the
host and guest can STUN + UDP hole-punch an ENet connection without port forwarding. It relays no gameplay
traffic: it only passes addresses and a per-guest session token. Rooms live in a SQLite-backed Durable Object
(one per code) and are deleted, with everything in them, when they expire.

It runs on the **Workers Free plan only**. See [Cost guard](#cost-guard).

## API v1 (JSON, HTTPS only)

- `POST /v1/rooms` `{v:1, candidates:[{ip,port,lan}], maxPlayers}` → `201 {code, hostSecret, ttl}`.
  code = 6 chars from `0123456789ABCDEFGHJKMNPQRSTVWXYZ` (Crockford, no I L O U); hostSecret = 32 hex; ttl 7200 s.
- `GET /v1/rooms/{code}/wait?secret=…` host long-poll, held ≤ 25 s → `200 {guests:[{joinId,name,candidates,token}]}`
  as soon as ≥1 join is pending, else `204` at timeout. Each wait refreshes the TTL. Wrong secret → 404.
  **Fallback only**: it costs far more than the WebSocket below. See [Long-poll vs WebSocket](#long-poll-vs-websocket).
- `GET /v1/rooms/{code}/ws?secret=…` (WebSocket upgrade) is the **recommended host channel**. The server sends
  `{"type":"guests","guests":[{joinId,name,candidates,token}]}` text frames whenever joins arrive, including any that
  were pending when the socket connected. The host sends the text `ping` every 30 s and gets `pong` back. The
  runtime answers that without waking the object, so it is free. Any other text (for example `sync`) re-sends what
  is pending. While the socket is open the room does not expire; the 7200 s TTL counts from the disconnect. A new
  socket replaces the old one (close code 4000). When the room is deleted or expires, the socket closes with code 1000.
- `POST /v1/rooms/{code}/join` `{v:1, name, candidates:[…]}` → `200 {joinId, token, host:{candidates:[…]}}`.
  token = 32 hex, the SAME token is delivered to the host in its wait. Unknown and expired codes → identical `404
  {error:"no_room"}`. Full room → `409 {error:"full"}`. Rate limits (per client IP, in the DO): join 10/min,
  create 5/min → `429 {error:"rate_limited"}`.
- `DELETE /v1/rooms/{code}?secret=…` → `204` (wrong secret / no room → `404 {error:"no_room"}`).
- `POST /v1/rooms/{code}/close?secret=…` → `204`, the same as DELETE (the game uses this: its HTTP layer has no DELETE).
- Server also records the requester's `CF-Connecting-IP` as an extra `{ip, port:0, lan:false, seen:true}` hint
  candidate (port unknown over HTTP, so the client still uses STUN's port; useful for diagnostics only). It is added
  only when that address is IPv4.
- No logging of candidates/IPs beyond the room's lifetime. No accounts. CORS not needed.

Validation: a body larger than 8 KiB gets `413 {error:"too_large"}`. A malformed body or field gets
`400 {error:"bad_request"}`: `v` must be `1`; candidates are ≤ 8 objects, each with `ip` (dotted IPv4), `port`
(integer 0-65535) and an optional boolean `lan`; `name` is 1-32 characters with no control or format characters;
`maxPlayers` is an integer 2-8 and includes the host. Codes in the path are matched case-insensitively.
Other errors: `405 method_not_allowed`, `426 upgrade_required` (`/ws` without an Upgrade header),
`410 longpoll_disabled`, `503 busy` (5 code collisions in a row) and `503 disabled`.

v1 limitation: each issued token uses up a guest slot, and a slot is not given back if that guest never connects.
A host that wants fresh slots closes the room and creates a new one.

## Deploy

```sh
cd tools/mp-rendezvous
npx wrangler login     # opens a browser to authorise wrangler for the Cloudflare account
npx wrangler deploy    # prints https://dusk-mp-rendezvous.<subdomain>.workers.dev
```

No routes and no custom domain are configured, so it is served only on `workers.dev`. Observability/Workers Logs
are deliberately not enabled in `wrangler.toml`, because host URLs carry `?secret=`. Leave them off.

## Test with curl

```sh
BASE=https://dusk-mp-rendezvous.<subdomain>.workers.dev

# host: create a room
curl -s -X POST $BASE/v1/rooms -H 'content-type: application/json' \
  -d '{"v":1,"candidates":[{"ip":"192.168.1.20","port":51000,"lan":true}],"maxPlayers":2}'
# -> {"code":"7KQ2MX","hostSecret":"<32 hex>","ttl":7200}

# host: long-poll (returns 204 after 25 s if nobody joined)
curl -si "$BASE/v1/rooms/7KQ2MX/wait?secret=<hostSecret>"

# guest (another terminal, while the wait is held): join
curl -s -X POST $BASE/v1/rooms/7KQ2MX/join -H 'content-type: application/json' \
  -d '{"v":1,"name":"Alex","candidates":[{"ip":"10.0.0.3","port":52000,"lan":true}]}'
# -> {"joinId":"…","token":"<32 hex>","host":{"candidates":[…]}}   and the held wait returns 200 with the same token

# host over WebSocket instead (any WS client, e.g. `npx wscat`):
npx wscat -c "wss://dusk-mp-rendezvous.<subdomain>.workers.dev/v1/rooms/7KQ2MX/ws?secret=<hostSecret>"
#   type: ping   -> pong

# host: close the room
curl -si -X DELETE "$BASE/v1/rooms/7KQ2MX?secret=<hostSecret>"   # -> 204; the code now answers 404 no_room
```

## Long-poll vs WebSocket

A Durable Object is billed for **wall-clock duration at 128 MB** while it is active, and a held request keeps it
active. A WebSocket accepted through the Hibernation API costs nothing while idle.

| 20 hosts online for 24 h | Duration (GB-s/day) | Requests/day | Free allowance |
|---|---|---|---|
| Long-poll `/wait` (25 s holds) | 0.125 GB × 86,400 s × 20 = **216,000** | ~69,000 Worker + ~69,000 DO | 13,000 GB-s, 100,000 req |
| Hibernating WebSocket `/ws` | a few ms per join ≈ **0** | ~20 upgrades + reconnects (pings are auto-answered) | same |

One long-polling host left on for a day uses 10,800 GB-s, which is 83 % of the whole daily allowance. Twenty hosts
would be about 16.6× over it. On the Free plan that would not cost money. Duration-billed operations would start
failing for the rest of the day (UTC), which is an outage for everyone. So the WebSocket is the host channel to ship.
`/wait` stays for development and for clients that cannot do WebSocket yet. Set `LONGPOLL = "0"` to turn it off.

Other costs per action:
- create: 1 Worker request + 1 rate-limit DO request + 1-5 room DO requests + 2 rows written;
- join: 1 Worker request + 2 DO requests + 2 rows written;
- expiry: 1 alarm plus the deletes.

To keep rows written low, room state sits in one storage key, and the stored expiry is rewritten only when it has
moved by more than 10 minutes.

## Rate limiting

Each client IP gets its own `RateLimiter` Durable Object (`idFromName("rl:" + ip)`), which keeps a sliding
60-second window of timestamps **in memory only**. It writes no storage, so it costs one DO request per create/join
and no rows. Per-isolate counters in the Worker were rejected: one IP's requests land on many isolates and
locations, so an attacker could spread its requests to get past them. The runtime only evicts a limiter after it
has been idle, i.e. after the caller stopped, so losing the window then does no harm. A malformed code in a join
path still costs a join attempt, so it can't be used to probe without limits. Wrong secrets need no limit: they
are 128-bit random values, and an unknown room, an expired room and a wrong secret all give the same 404.

## Cost guard

**The hard rule: this must never be able to bill anyone.** The Free plan meets that by design:

- The Workers Free plan has no overage billing. Past the daily request limit, Workers return error 1027 instead of
  being charged ([Workers limits](https://developers.cloudflare.com/workers/platform/limits/)).
- Durable Objects on the Free plan: "If you exceed any one of the free tier limits, further operations of that type
  will fail with an error" ([DO pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)).
- The Free plan only offers SQLite-backed Durable Objects, which is what the `new_sqlite_classes` migration declares.
  The KV-backed storage backend needs Workers Paid
  ([DO pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)).
- This Worker uses no KV, R2, D1, Queues, TURN or other add-on. It makes no outbound subrequests except calls to
  its own Durable Objects.

Free-plan limits as of 2026-11-20:

| Resource | Free limit | Source |
|---|---|---|
| Worker requests | 100,000 / day (resets 00:00 UTC) | [Workers limits](https://developers.cloudflare.com/workers/platform/limits/) |
| Worker CPU time | 10 ms / request (waiting on I/O doesn't count) | same |
| DO requests | 100,000 / day (incoming WebSocket messages count 20:1; auto-responses aren't charged) | [DO pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/) |
| DO duration | 13,000 GB-s / day, billed at 128 MB per active object; idle hibernating objects aren't billed | same |
| SQLite rows read / written | 5,000,000 / 100,000 per day (each `put`, `delete` and `setAlarm` is a row written) | same |
| SQLite stored data | 5 GB total | same |

**What Stuart must check in the dashboard, so it stays unbillable:**
1. *Workers & Pages → Plans*: the plan says **Free**. Never upgrade to **Workers Paid**. That plan bills for usage
   past its included amounts, and that billing is what the free limits otherwise rule out.
2. No payment method is needed for Workers Free. Don't add one for this.
3. Don't enable paid add-ons (Workers Logs beyond the free tier, Logpush, Cloudflare Calls/TURN, Argo, and so on)
   for this account.

**Manual off-switch:** set `DISABLED = "1"` (dashboard: Worker → *Settings → Variables*, or edit `wrangler.toml`
and run `npx wrangler deploy`). Every route then answers `503 {"error":"disabled"}` without touching a Durable
Object. Worker requests still count toward the free daily limit. To stop even those, use the Worker's
*Settings → Domains & Routes* to disable the `workers.dev` route, or run `npx wrangler delete`.

Abuse can't create a bill. The worst it can do is use up the free daily quota, after which the service stops
answering until 00:00 UTC.
