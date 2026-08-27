# ADR-0004: Discovery is a QR code — no mDNS, no Bonjour, no UDP beacon

- **Status:** Accepted
- **Date:** 2026-08-27

## Context

The phone has to learn one string: `http://<address>:<port>/#t=<token>`. The obvious
engineering answer is service discovery — publish a `_phonepostmix._tcp` Bonjour service
and let the phone find it. That answer is wrong here for two independent reasons.

**The browser cannot use it.** There is no mDNS API on the web platform. A page in Safari
or Chrome cannot browse for Bonjour services, cannot resolve a `.local` name of its own
accord, and cannot be handed one by anything short of a native app. Since the receiver is
deliberately a web page ([ADR-0003](0003-scriptprocessornode-and-plain-http.md)), any
discovery protocol we implement in the plugin has no counterpart on the other end.

**It would cost us the best property this design has.** Apple's Local Network Privacy rules
(TN3179, and the developer-forum FAQ in thread 663874) enumerate exactly which operations
require the user's permission. Requiring permission: outgoing TCP connects, UDP unicast /
multicast / broadcast *send*, connecting a UDP socket, receiving incoming UDP multicast or
broadcast, resolving `.local` names, and **all Bonjour operations**. Not requiring
permission: **"listening for and accepting incoming TCP connections"** and receiving an
incoming UDP unicast.

A pure inbound-TCP server is therefore entirely outside the permission regime. The moment
we add mDNS or a UDP broadcast beacon, we are inside it — and inside it the pathologies are
severe: the permission is enforced by a Network Extension packet filter rather than TCC, so
`tccutil reset` cannot clear a cached denial during development; the prompt names the host
DAW ("Ableton Live would like to find devices on your local network"), not PhonePostMix, so
a user who denies it once has silently killed a plugin they will never connect to the
dialog; and there are credible reports of grants not surviving a reboot. See
[`../PLAN-risks.md`](../PLAN-risks.md) R5, which calls keeping v1 inbound-TCP-only the
single best architectural decision in the plan.

Meanwhile a camera pointed at a monitor resolves an address in about a second, needs no
protocol, no permission and no network round trip, and works identically on iOS and
Android. It also solves a problem discovery does not: the token has to reach the phone
somehow, and a QR code carries it in the same gesture.

## Decision

Discovery is a **QR code rendered in the plugin editor**, containing the complete listen
URL including the pairing token in the URL fragment, with the plain URL shown underneath
for manual entry and a **Copy link** button beside it. No mDNS, no Bonjour, no UDP beacon,
no outbound connections of any kind.

Because the machine usually has several addresses, the editor enumerates every IPv4
interface, discards loopback (`127.x`) and link-local (`169.254.x`) addresses, sorts the
rest so the one a phone is most likely to reach comes first (`192.168.` before `10.`
before `172.` before anything else), and lets the user pick. The QR code regenerates
whenever the chosen address or the port changes.

## Consequences

What becomes easy:

- **The plugin never triggers a Local Network permission prompt on macOS**, and never
  depends on a grant that cannot be reset for testing.
- No discovery protocol to implement, specify, test or maintain, and no multicast traffic
  on the user's network.
- The token travels with the address in one gesture, so pairing and discovery are the same
  action. Because it is in the fragment, it never reaches a server log or a `Referer`
  header.
- It works where discovery would not: across subnets, on networks with multicast filtered,
  and on any browser at all.
- The interface picker turns the single most common real-world failure — a VPN or Docker
  bridge address in the URL — into a dropdown the user can fix themselves.

What becomes hard, and what we give up:

- **The user must be able to see the screen.** Scanning from another room means using
  **Copy link** and sending the URL by some other means; there is no "find my DAW" button.
- **Nothing auto-reconnects to a new address.** If DHCP hands the laptop a different IP
  mid-session, the phone's page stops working and the user has to rescan. The editor
  regenerates the code, but nobody tells the phone.
- **We still own the "wrong IP" problem**, just in a visible form. The sort order is a
  heuristic: on a network that genuinely uses `10.x` it puts a VPN address first if one is
  present. Hence the dropdown and the troubleshooting table.
- **The macOS Application Firewall is untouched by this.** It is a separate mechanism from
  Local Network privacy and may still prompt to allow incoming connections, attributed to
  the DAW. So may Windows Defender Firewall. Documented, not solved.
- Rendering QR codes costs a vendored dependency (nayuki's `qrcodegen`, MIT, ~1500 lines).
  That is a cheap trade for deleting a permission regime.

## Alternatives considered

- **Bonjour / mDNS (`_phonepostmix._tcp`).** The textbook answer. Rejected twice over: the
  browser has no mDNS API, so there is nothing on the phone to consume it; and publishing
  it would put the whole plugin into Apple's Local Network permission regime for zero
  benefit. This is the alternative the decision exists to refuse.
- **A UDP broadcast beacon with a small native helper on the phone.** Would work, and
  requires an app on the phone — which is the one thing this project promises not to need.
  It also re-enters the permission regime.
- **A cloud rendezvous service: the plugin registers, the phone looks it up.** Rejected. It
  requires an outbound connection (permission-gated), an internet dependency for a tool
  that must work on an offline studio network, a service to run, and it would leak session
  metadata off the machine.
- **Typing the URL by hand only.** Zero dependencies, and it is already the fallback under
  the QR code. Rejected as the primary path: `http://192.168.1.50:17520/#t=1f3c9ab27d5e…`
  is thirty-odd characters of hex that nobody should be asked to type on a phone keyboard.
- **A short numeric pairing code redeemed against the server.** Nicer to type, but it needs
  the phone to already know *where* the server is, which is the problem we were trying to
  solve.
