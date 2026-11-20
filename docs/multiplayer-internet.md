# Playing co-op over the internet

On the same home network, the guest joins with the host's local IP address. Over the internet the host's
router blocks incoming connections by default. Until join codes land, there are two ways around that.

## Option 1: Tailscale (no router changes, works almost everywhere)

[Tailscale](https://tailscale.com) puts your computers on a private network. The free Personal plan allows
up to 6 users. When a direct connection is impossible (for example behind carrier-grade NAT on a mobile
hotspot), Tailscale relays the traffic, so it connects in nearly every case.

1. **Both players** install Tailscale and sign in.
2. **The host** shares their machine with the guest: in the Tailscale admin console, open the host machine's
   menu and choose *Share*, then send the invite to the guest. (Or invite the guest to the host's tailnet.)
3. **The host** reads their Tailscale address, which starts with `100.`. It's in the Tailscale tray menu,
   or run `tailscale ip -4`.
4. **The host** starts Dusklight, opens Multiplayer, and picks *Host*.
5. **The guest** opens Multiplayer, picks *Join*, and enters the host's `100.x.y.z` address. The port stays
   the same.

A good result: the guest's Link appears in the host's world within a few seconds, and both players see each
other move. If it times out, check that both machines show as connected in Tailscale, and that the host's
firewall allows Dusklight (Windows asks the first time you host; choose *Allow*).

[ZeroTier](https://www.zerotier.com) works the same way if you prefer it.

## Option 2: forward the port

In the host's router settings, forward the multiplayer port (UDP, 7777 unless you changed it) to the host's
computer. The guest then joins with the host's public IP address. This doesn't work if the host's internet
provider uses carrier-grade NAT, which is common on mobile and some fibre connections. Use Tailscale then.

## Coming next

Join codes: the host gets a short code like `K7Q-4MZ` to send their friend, and the game connects the two
directly, with no setup on either side.
