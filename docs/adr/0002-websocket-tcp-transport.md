# ADR-0002: WebSocket over plain TCP as the audio transport

- **Status:** Accepted
- **Date:** 2026-08-27

## Context

The audio has to get from a plugin inside a DAW to a page in a phone browser, over the
local network, with no software installed on the phone. That last constraint eliminates
most of the options immediately: whatever we choose has to be something a browser can
speak, from a page served over plain `http://192.168.x.x`.

The browser's real options are:

- **WebSocket** (`ws://`) — TCP, framed, works from an insecure origin.
- **WebRTC data channels** — SCTP over DTLS over UDP, which is what a low-latency
  streaming engineer would reach for first.
- **WebTransport** — QUIC, unreliable datagrams, exactly the right transport for this
  problem.
- **HTTP chunked streaming / SSE** — text-oriented, no framing we want, no back channel.

Two facts collapse this list. First, the page is not a secure context: only
`127.0.0.0/8`, `::1` and `localhost` are trustworthy origins without TLS, and a phone
reaching a laptop at `192.168.1.50` is none of them (see
[ADR-0003](0003-scriptprocessornode-and-plain-http.md)). WebTransport requires a secure
context. WebRTC is not gated in the same way, but a data channel is DTLS-encrypted by
construction and needs a full ICE/SDP negotiation, which means either a signalling server
or a hand-rolled offer/answer exchange, plus a DTLS stack inside the plugin.

Second, the plugin runs **inside someone else's process**. Every dependency is a
dependency the DAW loads. Adding OpenSSL or libwebrtc to an audio plugin means shipping and
symbol-isolating a large native library inside Ableton or Logic, and any of its bugs become
crashes in the user's session. The whole project is built on `juce::StreamingSocket` plus
about 200 lines of protocol code precisely to avoid that.

WebSocket needs no TLS on the LAN, no extra library — the server side is an HTTP parser, a
SHA-1, a Base64 and a frame codec, all of which fit in `src/core/` and are unit-tested —
and it works in every phone browser released in the last decade, with no negotiation
beyond a single HTTP upgrade.

## Decision

Carry audio as **binary WebSocket frames over plain TCP (`ws://`)**, one packet per frame,
using a server implemented in-tree on `juce::StreamingSocket`. No TLS, no UDP, no WebRTC,
no third-party networking library.

## Consequences

What becomes easy:

- One TCP port serves the page *and* the audio, same origin, no CORS, no mixed content, no
  certificate anywhere.
- The dependency list stays at JUCE plus a vendored QR generator. `JUCE_USE_CURL=0`, no
  OpenSSL, no npm.
- Framing is free: a WebSocket frame boundary is a packet boundary, so a receiver never
  has to re-derive message boundaries from a byte stream.
- A receiver is trivially writable in any language — see `tools/listen.js`, which is a
  complete client in ~80 lines of `net` and `crypto`.
- Connection setup is one HTTP request. A QR scan to audible sound is two taps.

What becomes hard, and what we accept:

- **Head-of-line blocking.** This is the real cost. TCP will not deliver packet *n+1* until
  packet *n* has arrived, so a single lost frame on a congested access point stalls
  playback while it is retransmitted, and then the receiver has a burst to catch up on. UDP
  would have simply skipped it and concealed 10 ms. The mitigations are all
  latency-for-robustness trades: a jitter buffer on the phone (40–500 ms, 120 default), and
  a per-client outgoing queue in the plugin that **drops the oldest audio** when a client
  stops draining, so a backlog becomes a gap rather than unbounded delay.
- **A stalled peer must be detected by hand.** `StreamingSocket::write()` blocks once the
  kernel send buffer fills, which is exactly what happens when TCP starts retransmitting,
  so every write waits for writability first and abandons the frame on a 5-second deadline.
- **No encryption.** Anyone on the same Wi-Fi who can capture traffic can read the pairing
  token off the handshake and reconstruct the audio. The token protects against a casual
  listener finding the port, not against an eavesdropper. This is stated plainly in the
  README, and it is the strongest argument on the "HTTPS would pay for itself" ledger.
- **We maintain protocol code.** An HTTP parser and a WebSocket frame parser now live
  inside the DAW's process, reachable by anyone on the LAN. The surface is kept small on
  purpose — two compiled-in assets, no request bodies, no keep-alive, a 16 KB request-head
  cap, a 1 MiB frame cap — and both parsers are unit-tested against the RFC's own vectors.

## Alternatives considered

- **WebRTC data channels (unreliable, unordered).** The technically correct transport for
  real-time audio: UDP semantics, no head-of-line blocking, and browsers allow it from an
  insecure origin. Rejected because it requires ICE/SDP signalling and a DTLS stack inside
  an audio plugin — either libwebrtc, which is enormous, or a hand-rolled DTLS, which is a
  security liability. The complexity is not repayable in v1 for a transport that only has
  to cross one Wi-Fi hop.
- **WebTransport over QUIC.** Unreliable datagrams, modern API, exactly what this wants.
  Rejected outright: it is secure-context-only, so it is unreachable from an
  RFC 1918 origin, permanently.
- **Raw UDP with a native receiver app.** Would fix head-of-line blocking and let us drop
  late packets properly. Rejected because it means an app on the phone, which breaks the
  core promise, and because it re-enters macOS's Local Network permission regime
  ([ADR-0004](0004-qr-code-discovery-no-mdns.md)) that inbound TCP is exempt from.
- **`wss://` with a real certificate.** Would unlock AudioWorklet, wake lock and
  WebTransport all at once and is the obvious v2 direction. Rejected for v1 because it needs
  either a TLS stack in the plugin (mbedTLS via `FetchContent` is the cheapest option) plus
  a publicly resolvable DNS name pointing at a private IP, or a self-signed certificate the
  user must install — a five-step ordeal on iOS. Priced, deferred, not forgotten.
- **HTTP chunked transfer or Server-Sent Events.** No back channel, no binary framing, and
  proxies and browsers buffer them unpredictably. Rejected as strictly worse than WebSocket
  with no compensating advantage.
