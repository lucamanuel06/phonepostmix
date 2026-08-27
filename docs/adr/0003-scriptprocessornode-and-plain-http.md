# ADR-0003: Plain HTTP, and therefore ScriptProcessorNode with no AudioWorklet path

- **Status:** Accepted
- **Date:** 2026-08-27

## Context

The original plan said the receiver page would "play via AudioWorklet where available and
ScriptProcessorNode otherwise". The adversarial review of that plan
([`../PLAN-risks.md`](../PLAN-risks.md) R1) found that one clause is factually wrong, and
load-bearing.

`BaseAudioContext.audioWorklet` is annotated `[SecureContext]` in the Web Audio
specification. W3C Secure Contexts §3.1 enumerates the only origins that are "potentially
trustworthy" without TLS: `127.0.0.0/8`, `::1/128`, `localhost`, `*.localhost`, `file:` and
UA-specific schemes. `192.168.0.0/16`, `10.0.0.0/8` and `172.16.0.0/12` are conspicuously
absent. A phone loading `http://192.168.1.50:17520` is therefore on an insecure origin, and
`ctx.audioWorklet` is simply `undefined` — on iOS Safari, on Android Chrome, on every
browser, in every version, now and later. This is not a support-matrix question that time
will fix.

So the "where available" branch never executes. In the deployed product the "otherwise"
branch is taken 100% of the time, on every device, permanently. The same gate removes
`navigator.wakeLock`, `SharedArrayBuffer`, WebCodecs, service workers and `crypto.subtle`
along with it.

The alternative is to make the origin trustworthy, which means TLS, which means one of:
a TLS stack inside the plugin plus a publicly resolvable DNS name that answers with a
private IP and a DNS-01-issued certificate (the pattern Plex uses); or a self-signed
certificate the user installs by hand, which is a multi-step Certificate Trust Settings
ordeal on iOS and cannot be a v1 requirement. Both break the "no OpenSSL inside the DAW"
constraint from [ADR-0002](0002-websocket-tcp-transport.md).

The remaining playback path is `ScriptProcessorNode`. It is deprecated, it runs its
callback on the main thread, and its smallest phone-safe buffer is 4096 frames — 85 ms at
48 kHz, which is the largest single term in the latency budget. It is also not gated on
secure context, has been deprecated without removal for a decade, and works today on every
phone we care about.

## Decision

Serve the receiver over **plain HTTP** and play through **`ScriptProcessorNode` only**, at
4096 frames. **Do not write an AudioWorklet code path at all.** Continue to *detect*
AudioWorklet, wake lock, `SharedArrayBuffer`, WebCodecs and WebTransport, and display the
results in the page's diagnostics panel.

## Consequences

What becomes easy:

- No certificates, no DNS, no TLS library, no installation step on the phone. Scan a QR
  code, tap once, hear audio.
- With ScriptProcessorNode, the socket callback and the audio callback both run on the main
  thread, so the receiver's ring buffer is a plain `Float32Array` with no cross-thread
  hazard and no `SharedArrayBuffer` (which is unavailable anyway).
- One code path means one code path to test, and the number the diagnostics panel reports
  is the number that ran.

What becomes hard, and what we accept:

- **A fixed ~85 ms of output latency** we cannot remove. Smaller ScriptProcessorNode
  buffers glitch on phones under any real load. It is the dominant controllable term in the
  budget, and the README says so rather than blaming the network.
- **The audio callback shares a thread with the page.** Layout, painting and garbage
  collection all compete with filling the audio buffer. The page runs one 10 Hz timer
  instead of `requestAnimationFrame` for exactly this reason, and the diagnostics table is
  only rebuilt while the panel is open.
- **No wake lock, so the screen must stay on.** iOS suspends web audio when the screen
  locks, and the page cannot prevent it. All the page can do is say so, request a wake lock
  in case it is ever granted, and keep a looping silent `<audio>` element alive to hold the
  audio session up on older iOS.
- **Deprecation risk.** If a browser finally removes `ScriptProcessorNode`, the receiver
  stops working and the only fix is HTTPS. Detecting AudioWorklet on every real device is
  what will justify that work when the time comes: the capability numbers are already being
  collected.
- **Nothing is encrypted**, which is a consequence of the same decision — see
  [ADR-0002](0002-websocket-tcp-transport.md) and the README's security section.

## Alternatives considered

- **Write the dual path anyway, "just in case".** Rejected, emphatically. A branch that
  cannot execute on any real device is not a fallback; it is dead code that rots, that
  nobody can test, and that lies to you in code review by making the product look like an
  AudioWorklet product with a safety net. The comment at the top of `web/app.js` exists so
  this decision is not quietly reversed.
- **HTTPS with a DNS-01 wildcard certificate resolving to a private IP.** The genuinely
  correct fix: it unlocks AudioWorklet, wake lock, WebTransport and encryption in one move.
  Rejected for v1 because it needs a TLS stack in the plugin, a domain, and a certificate
  renewal story, for a tool whose first job is to work on a laptop with no internet
  connection. This is the top item on the v2 list.
- **Self-signed certificate the user trusts manually.** Works on Android Chrome; on iOS it
  is a five-step trip through Certificate Trust Settings. Rejected as a shipping
  requirement — no monitoring tool is worth that.
- **Serving from `localhost` on the phone** (a local proxy or app). Would give a
  trustworthy origin, and requires installing something on the phone, which is the one
  thing this project promises not to do.
- **Smaller ScriptProcessorNode buffers (2048, 1024).** Would cut 40–60 ms. Rejected as the
  default: they underrun audibly on phones once anything else touches the main thread. The
  jitter-buffer slider is the honest knob for people who want to trade stability for
  latency.
