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
  runtime answers that without waking the object, so it is free. **Any other frame closes the socket with code
  1008**; to get what is pending again, reconnect. While the socket is open the room does not expire; the 7200 s TTL
  counts from the disconnect. A new socket replaces the old one (close code 4000), but a room takes at most one
  upgrade per 5 s (`429 {error:"rate_limited"}` otherwise). When the room is deleted or expires, the socket closes
  with code 1000.
- `POST /v1/rooms/{code}/join` `{v:1, name, candidates:[…]}` → `200 {joinId, token, host:{candidates:[…]}}`.
  token = 32 hex, the SAME token is delivered to the host in its wait. Unknown and expired codes → identical `404
  {error:"no_room"}`. Full room → `409 {error:"full"}`. Two joins already waiting on the host (issued in the last
  90 s and not released) → `429 {error:"busy"}`. Rate limits (per IPv4 address or IPv6 /64): join 10/min, create
  5/min → `429 {error:"rate_limited"}`. Past the service-wide daily create cap → `503 {error:"daily_cap"}`.
- `DELETE /v1/rooms/{code}?secret=…` → `204` (wrong secret / no room → `404 {error:"no_room"}`).
- `POST /v1/rooms/{code}/close` `{secret}` → `204`, the same as DELETE (the game uses this: its HTTP layer has no
  DELETE). `?secret=` in the query with no body also works; the body keeps the secret out of URLs.
- `POST /v1/rooms/{code}/release` `{secret, joinId}` → `204`: the host gives back the slot of a guest it denied, or
  whose Hello never came. An unknown `joinId` is a no-op `204`; wrong secret / no room → `404`; a `joinId` that isn't
  16 hex → `400`. `?secret=` is accepted here too.
- Server also records the requester's `CF-Connecting-IP` as an extra `{ip, port:0, lan:false, seen:true}` hint
  candidate (port unknown over HTTP, so the client still uses STUN's port; useful for diagnostics only). It is added
  only when that address is global-unicast IPv4.
- No logging of candidates/IPs beyond the room's lifetime. No accounts. CORS not needed.

Validation: a body larger than 8 KiB gets `413 {error:"too_large"}`. A malformed body or field gets
`400 {error:"bad_request"}`: `v` must be `1`; candidates are ≤ 8 objects, each with `ip` (dotted IPv4), `port`
(integer 0-65535) and an optional boolean `lan`; a candidate with `lan` false or absent must be global unicast (not
in 0/8, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.0.0/24, 192.168/16, 198.18/15, 224/4 or 240/4);
`name` is 1-32 characters with no control or format characters;
`maxPlayers` is an integer 2-8 and includes the host. Codes in the path are matched case-insensitively.
Other errors: `405 method_not_allowed`, `426 upgrade_required` (`/ws` without an Upgrade header),
`410 longpoll_disabled`, `503 busy` (5 code collisions in a row), `503 daily_cap` and `503 disabled`.

Each issued token uses up a guest slot until the host releases it. The game releases a slot when the host presses
Deny or the guest's Hello doesn't arrive in time, so a guest who gives up doesn't hold a slot for good.

## Deploy

```sh
cd tools/mp-rendezvous
npx wrangler login     # opens a browser to authorise wrangler for the Cloudflare account
npx wrangler deploy    # prints mp.kong.bike (custom domain)
```

It is served on the custom domain `mp.kong.bike` (a Free zone on the same account), which is what every build
uses by default (`kDefaultRendezvousUrl` in `src/dusk/settings.h`). The zone's one free WAF rate-limiting rule
sits in front of it: `http.host eq "mp.kong.bike"`, 40 requests per 10 s per IP, then Block for 10 s. That rule
lives in the dashboard (Security → Security rules), not in this repo. `workers_dev` and `preview_urls` are off so the WAF cannot be
bypassed. Observability/Workers Logs
are deliberately not enabled in `wrangler.toml`, because host URLs carry `?secret=`. Leave them off.

## Test with curl

```sh
BASE=https://mp.kong.bike

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
npx wscat -c "wss://mp.kong.bike/v1/rooms/7KQ2MX/ws?secret=<hostSecret>"
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
- create: 1 Worker request + 1 rate-limit DO request + 1 create-cap DO request + 1-5 room DO requests + 3 rows written;
- join: 1 Worker request + 2 DO requests + 2 rows written;
- release: 1 Worker request + 1 DO request + 1 row written;
- a rejected request (malformed, bad code, pre-limited): 1 Worker request and no DO request;
- expiry: 1 alarm plus the deletes.

To keep rows written low, room state sits in one storage key, and the stored expiry is rewritten only when it has
moved by more than 10 minutes.

## Rate limiting

Each client gets its own `RateLimiter` Durable Object (`idFromName("rl:" + key)`), which keeps a sliding
60-second window of timestamps **in memory only**. The key is the IPv4 address, or the **/64** of an IPv6 address
(one subscriber usually holds a whole /64, so per-address keys could be rotated past). It writes no storage, so it
costs one DO request per create/join and no rows. A per-isolate copy of the same limits runs in the Worker first:
on its own it can't stop a client spread over many isolates and locations (the DO does that), but it turns away
most of a flood without spending a DO request. The runtime only evicts a limiter after it has been idle, i.e.
after the caller stopped, so losing the window then does no harm. A join to a code outside the public alphabet
answers `404` at once: such a code can never name a room, so it tells a prober nothing. Wrong secrets need no
limit: they are 128-bit random values, and an unknown room, an expired room and a wrong secret all give the same 404.

## Security

What stops each abuse (2026-11-20 review). None of it can make a bill; it keeps one abuser from using up the free
daily quota, which would be an outage for everyone until 00:00 UTC.

| Abuse | Protection |
|---|---|
| Flooding the host socket with frames (each wakes the DO) | any frame but the auto-answered `ping` closes the socket with 1008 and does no work |
| A `/ws` reconnect loop (each costs a close + a touch) | at most one upgrade per room per 5 s (in memory, checked after the secret); disconnects don't force a storage write |
| Requests that will be rejected anyway costing DO requests | every body, code and secret is validated before any DO is called; a per-isolate pre-limiter runs before the limiter DO |
| Rotating IPv6 addresses past per-address limits | limits key on the IPv6 /64 |
| Many clients creating rooms to spend rows written | a global cap on creates per UTC day (`CREATE_CAP_PER_DAY`, default 3000) in the singleton `CreateCap` DO |
| Someone with the code filling a room for good | at most 2 unsettled joins per room (`429 busy`); the host releases denied / never-connected joins |
| A guest making the host UDP-punch arbitrary addresses (its LAN, loopback, multicast, broadcast) | a `lan:false` candidate must be global unicast (`400` otherwise); the game also drops such candidates and punches at most 4 per guest |
| Secrets in URLs | `/close` and `/release` take the secret in the JSON body |
| Guessing secrets / codes | 128-bit secrets compared with `timingSafeEqual`; 2^30 codes; identical 404 for unknown, expired and wrong-secret |
| Oversized or hostile bodies | 8 KiB cap, strict JSON object, typed and bounded fields, names without control characters |
| Leaking addresses or secrets | nothing is logged; rooms delete everything when they expire |

Accepted, by design: `409 full` tells a code-holder the room exists; clients with no `CF-Connecting-IP` share one
bucket; a guest sees the host's address before the host approves it (the guest needs it to connect).

Done on the account, not in code: every request that reaches the Worker, even a `404`/`429`, counts toward the
Worker's 100,000/day. So it runs on a custom domain (`mp.kong.bike`, Free zone) whose free WAF rate-limiting rule
blocks a flooding IP before the Worker runs, and `workers_dev = false` closes the bypass.

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
*Settings → Domains & Routes* to remove the `mp.kong.bike` custom domain, or run `npx wrangler delete`.

Abuse can't create a bill. The worst it can do is use up the free daily quota, after which the service stops
answering until 00:00 UTC.
