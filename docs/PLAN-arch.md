# PhonePostMix — v1 Implementation Plan

**Date:** 2026-08-27
**Target:** a single developer, one day, on macOS 26 / Apple Silicon / Xcode 26 / CMake 4.4, with Ableton Live 12 as the test host.
**Contract for strangers:** `git clone && cmake -B build && cmake --build build` works on macOS, Windows and Linux with no submodules, no npm, no Projucer, no package manager.

This plan is the build order and the design. It is opinionated. Where the research documents in `docs/research/` recommend something different, the divergence is called out and justified — the research targets the eventual product; this plan targets the thing that ships today.

---

## 0. Decisions taken as given, and what they imply

These were decided before this plan and are not relitigated:

| Decision | Consequence baked into this plan |
|---|---|
| JUCE 9 via `FetchContent` | Pin `GIT_TAG 9.0.1` (verified latest tag, 2026-08-10). No JUCE submodule, no sibling checkout. |
| VST3 + AU + Standalone | AU is macOS-only; Windows/Linux get VST3 + Standalone. Standalone is the primary *demo* artefact — it lets a stranger evaluate the whole idea with no DAW. |
| AGPL-3.0 | Already in `LICENSE`. `JUCE_DISPLAY_SPLASH_SCREEN=0` is legal under it. README must say so loudly. |
| Zero heavyweight third-party deps | No OpenSSL → **no TLS** → `ws://` and `http://` only. No libopus → **PCM only**. No libdatachannel → **no WebRTC**. These are not oversights; they are the whole point of v1. |
| WebSocket over `juce::StreamingSocket` | We write ~400 lines of RFC 6455. Verified below: JUCE has `Base64` but **no SHA-1**, so a ~100-line public-domain SHA-1 gets vendored. |
| One self-contained HTML page, no build step | Three files (`index.html`, `app.js`, `worklet.js`) embedded via `juce_add_binary_data`. No bundler, no npm, no `node_modules` in the repo. |
| Must work on plain `http://<lan-ip>` | This is the constraint that shapes the receiver. See §0.1 — the research is right and it is worse than it looks. |

### 0.1 Verification of the AudioWorklet / secure-context reasoning — **the reasoning is correct**

The premise stated in the brief — *"it must work on plain `http://` on a LAN IP, therefore feature-detect AudioWorklet and fall back to ScriptProcessorNode"* — is right. Confirmed against the research:

- `docs/research/02-transport-and-codecs.md` §5.3 is unambiguous and cites the W3C Secure Contexts spec §3.1: potentially-trustworthy origins are `https`/`wss`, `127.0.0.0/8`, `::1/128`, `localhost`/`*.localhost`, `file:`, UA schemes, and manually-configured origins. **RFC 1918 space (`192.168/16`, `10/8`, `172.16/12`) is conspicuously absent.** The phone is a different device, so the loopback exemption cannot apply.
- MDN documents `AudioWorklet` (and `BaseAudioContext.audioWorklet`) as **Secure context: available only in secure contexts**. On `http://192.168.1.50:8377`, `ctx.audioWorklet` is `undefined` in Chrome and Safari.
- `ScriptProcessorNode` is deprecated but is **not** secure-context-gated, and no browser has removed it. `AudioContext`, `AudioBufferSourceNode` and `WebSocket ws://` are likewise ungated.
- `docs/research/03-receiver-and-toolchain.md` §1.2 hedges on this (mitigation "(d) just don't need a secure context… `AudioContext` works on insecure origins"). That hedge is only half right: the *context* works, the *worklet* does not. Doc 02 is the accurate one. **Believe doc 02.**

So the audio path must be:

```
AudioWorklet          — used when window.isSecureContext && ctx.audioWorklet   (localhost testing, future https)
ScriptProcessorNode   — the guaranteed v1 path on http://<lan-ip>              (this is what actually runs today)
```

**The rest of the http:// casualty list**, which the receiver must be designed around and the diagnostics panel must display:

| API | Gone on plain-http LAN IP | v1 consequence |
|---|---|---|
| `AudioWorklet` | yes | ScriptProcessorNode fallback. 128-frame quantum becomes a 2048–8192-frame SPN buffer. Accept it. |
| `SharedArrayBuffer` | yes (also needs COOP/COEP) | Ring buffer is plain `Float32Array` in the page. Single-threaded. Fine — SPN callbacks are on the main thread anyway. |
| `WebCodecs` / `AudioDecoder` | yes | Irrelevant: v1 is PCM, no codec. |
| `navigator.wakeLock` | yes | **Cannot keep the screen on.** Must be a documented, in-page warning: "set your phone's auto-lock to Never". |
| `crypto.subtle` | yes | Pairing token is compared as a plain string over the LAN. v1 has no cryptographic pairing; the token is a *mistake-preventer*, not a security boundary. Say so in the README. |
| Service Worker | yes | No offline PWA. Don't want one. |
| `navigator.audioSession` | Safari-only, not documented as secure-context-gated | Feature-detect and set `'playback'` anyway; keep the looping-silent-`<audio>` trick as the real fix for the iOS ringer switch. |

**Security posture, stated plainly, once:** v1 sends an unencrypted, unauthenticated PCM stream of the user's unreleased mix over the local network, and anyone on that Wi-Fi who guesses the port can listen. That is an acceptable trade for a LAN mix-check tool on your own studio network, and it is unacceptable on a café hotspot. This must be a banner in the README and a line in the plugin GUI, not a footnote.

### 0.2 Divergences from the research, and why

| Research says | v1 does | Why |
|---|---|---|
| WebRTC is the recommended transport (doc 03) | WebSocket over TCP | libdatachannel is banned by the constraints, and WebRTC without it is not a one-day job. Doc 02 §1.7 explicitly blesses WebSocket as "the correct v1 for a LAN-only MVP". |
| Opus at 256–510 kbps (doc 02) | 16-bit PCM, no codec | libopus is a dependency. PCM stereo 48k/16 is 1.5 Mbit/s — a rounding error on any AP (doc 02 §2.1). And PCM is *better* for mix judging. |
| Solve HTTPS first; it is the highest-risk item (doc 02 §5.4 Phase 0) | Ship http:// and degrade the audio path | HTTPS-on-a-LAN-IP needs either a public DNS zone + DNS-01 wildcard, or a cert-trust flow that loses users. Neither fits in a day, and neither can be done without OpenSSL. The cost is Wake Lock and AudioWorklet; both are survivable. **This is deliberately deferred to v2, and v2 is where it belongs.** |
| Bonjour/mDNS discovery | QR + plain text URL only | Doc 02 §3.2 proves `.local` names silently fail on Android Chrome. Doc 02 §3.3: "QR code pairing — this is the answer." |
| 128-frame packets to fit a UDP MTU | 512-frame packets | We are on TCP. MTU is the kernel's problem. Larger frames mean fewer syscalls and fewer JS wakeups on the phone. |
| Send at 48 kHz, resample on the sender | Send at the host rate, resample only on the receiver | The receiver needs a drift-correcting fractional resampler *anyway* (doc 02 §4.3). Making it also do the fixed 44.1→48 conversion is the same code with a different nominal ratio. One mechanism, zero sender-side DSP, zero `LagrangeInterpolator` state to reset. |

---

## 1. Repository layout

Every directory and every file, with its purpose. Files marked **(exists)** are already in the repo.

```
phonepostmix/
├── .github/
│   └── workflows/
│       └── build.yml                   CI: macOS/Windows/Linux matrix, ctest, pluginval, auval.
├── .gitignore                          (exists) build/, .DS_Store, IDE dirs.
├── CMakeLists.txt                      Root build. The only build file a user must know about. Written out in §5.
├── CMakePresets.json                   `cmake --preset default` on all three OSes; a `dev` preset with COPY_PLUGIN_AFTER_BUILD=ON.
├── CONTRIBUTING.md                     (exists) How to propose changes.
├── LICENSE                             (exists) AGPL-3.0.
├── README.md                           (exists, to rewrite) Contract with the reader: what it is, build, install, use, latency, security warning.
├── cmake/
│   └── PPMWarnings.cmake               One place for the warning flags; keeps the root file readable.
├── docs/
│   ├── PLAN-arch.md                    This file.
│   ├── architecture.md                 Post-v1 distillation of this plan for readers who won't read the plan.
│   ├── protocol.md                     The wire protocol (§4) extracted so receiver authors don't read C++.
│   ├── troubleshooting.md              macOS Local Network prompt, firewall, AP client isolation, iOS silent switch, plugin rescans.
│   ├── adr/
│   │   ├── README.md                   (exists) ADR index.
│   │   ├── _template.md                (exists)
│   │   ├── 0001-juce9-cmake-fetchcontent.md    Why JUCE 9 + FetchContent, and the AGPL consequence.
│   │   ├── 0002-websocket-pcm-transport.md     Why TCP WebSocket + raw PCM over WebRTC/Opus for v1.
│   │   ├── 0003-plain-http-and-scriptprocessor.md  The secure-context finding from §0.1, recorded so it is never relitigated.
│   │   └── 0004-embedded-web-assets.md         Why binary data, not files on disk.
│   └── research/
│       ├── 01-plugin-and-prior-art.md  (exists)
│       ├── 02-transport-and-codecs.md  (exists)
│       └── 03-receiver-and-toolchain.md (exists)
├── scripts/
│   ├── install-deps-linux.sh           The apt line from doc 03 §3.4, minus webkit2gtk and curl.
│   └── dev-serve.py                    Serves web/ on http://localhost:8377 with a fake WS PCM source, so the receiver can be developed with no C++ build. Stdlib only.
├── source/
│   ├── PluginProcessor.h/.cpp          juce::AudioProcessor. Pass-through tap. Owns everything. §3.1.
│   ├── PluginEditor.h/.cpp             The GUI. §3.10.
│   ├── PpmConfig.h                     Compile-time constants: default port, magic, protocol version, frame sizes.
│   ├── audio/
│   │   ├── AudioRingBuffer.h/.cpp      SPSC lock-free float ring over juce::AbstractFifo. §3.2.
│   │   ├── LevelMeterSource.h          Atomic peak/RMS written by the audio thread, read by the GUI timer. §3.11.
│   │   └── RealtimeGuard.h             `isNonRealtime()` + wall-clock rate limiter — the offline-bounce defence. §3.3.
│   ├── net/
│   │   ├── ServerThread.h/.cpp         Listener + accept loop. Owns ClientConnections. §3.5.
│   │   ├── ClientConnection.h/.cpp     One connected socket: HTTP or WebSocket. Per-client thread. §3.6.
│   │   ├── HttpRequest.h/.cpp          Minimal request-line + header parser. No allocations after the header.
│   │   ├── HttpResponse.h/.cpp         Static-asset responses out of BinaryData; `/api/session` JSON.
│   │   ├── WebSocket.h/.cpp            RFC 6455: upgrade handshake, frame parse, frame serialise, unmasking. §6.
│   │   ├── Sha1.h/.cpp                 Vendored public-domain SHA-1. JUCE has none. §6.1.
│   │   └── NetworkInterfaces.h/.cpp    juce::IPAddress enumeration, ranked; picks the LAN IP for the URL. §3.9.
│   ├── stream/
│   │   ├── StreamFormat.h              sampleRate/channels/format/framesPerPacket + configEpoch. Value type, comparable. §4.4.
│   │   ├── PacketWriter.h/.cpp         Serialises the 32-byte header + PCM payload into a reusable buffer. §4.2.
│   │   ├── StreamerThread.h/.cpp       Drains the ring, packetises, fans out to clients. §3.4.
│   │   └── ClientRegistry.h/.cpp       Thread-safe list of live clients + their reported stats. §3.7.
│   ├── gui/
│   │   ├── QrCodeComponent.h/.cpp      Draws a QR of the URL via vendored qrcodegen.
│   │   ├── ClientListComponent.h/.cpp  One row per phone: IP, buffer ms, underruns, drift ppm.
│   │   └── LevelMeter.h/.cpp           Stereo peak meter, 30 Hz timer.
│   └── vendor/
│       ├── sha1/                       (see net/Sha1 — vendored source lives here if kept verbatim)
│       ├── qrcodegen.h/.c              nayuki/QR-Code-generator, MIT, single .c/.h pair, zero deps.
│       └── README.md                   Provenance + licence of every vendored file. Non-negotiable for an AGPL repo.
├── tests/
│   ├── CMakeLists.txt                  Catch2 v3 via FetchContent; catch_discover_tests.
│   ├── Main.cpp                        ScopedJuceInitialiser_GUI + Catch2 session.
│   ├── RingBufferTests.cpp             SPSC correctness, wraparound, overflow reporting, 2-thread hammer.
│   ├── WebSocketTests.cpp              Accept-key vectors, frame parse/serialise round-trips, masking, fragmentation.
│   ├── Sha1Tests.cpp                   RFC 3174 test vectors.
│   ├── PacketWriterTests.cpp           Byte-exact header layout — offsets asserted numerically.
│   ├── HttpTests.cpp                   Request parsing, header extraction, malformed input.
│   └── ProcessBlockTests.cpp           Pass-through bit-exactness, finiteness, sample-rate/block-size churn, state round-trip.
└── web/
    ├── index.html                      The entire receiver UI. Inline CSS. No external requests, ever.
    ├── app.js                          Transport, jitter buffer, drift control, SPN/Worklet selection, UI, diagnostics. §7.
    └── worklet.js                      AudioWorkletProcessor — the secure-context-only fast path. §7.3.
```

**Layout rule that pays off:** `source/audio/`, `source/net/`, `source/stream/` and `source/vendor/` must not include any `juce_gui_*` header. The test target then links fast and the protocol code stays portable.

---

## 2. The one-page architecture

```
┌──────────────────────── DAW process (Ableton Live 12) ─────────────────────────┐
│                                                                                │
│  master bus ──▶ PhonePostMixProcessor::processBlock()        [AUDIO THREAD]    │
│                   • ScopedNoDenormals                                          │
│                   • levelMeter.push()          (atomics only)                  │
│                   • realtimeGuard.tick()       (atomics only)                  │
│                   • ring.write(buffer)         (lock-free, no alloc)           │
│                   • audio passes through BIT-EXACT UNCHANGED                   │
│                        │                                                       │
│                   AudioRingBuffer  (~500 ms, preallocated)                     │
│                        │                                                       │
│                        ▼                                     [STREAMER THREAD] │
│                 StreamerThread::run()  Priority::high                          │
│                   • poll ring every 2 ms                                       │
│                   • pull exactly framesPerPacket                               │
│                   • if starved and streaming: emit a SILENCE packet            │
│                   • PacketWriter → 32-byte header + interleaved PCM            │
│                   • ClientRegistry::broadcast(packet)                          │
│                        │                                                       │
│         ┌──────────────┴───────────────┐                                       │
│         ▼                              ▼                    [PER-CLIENT THREAD]│
│   ClientConnection A            ClientConnection B                             │
│   (SPSC packet queue,           • blocking write with 200 ms budget            │
│    drop-oldest on backlog)      • drop-oldest when the phone stalls            │
│         │                              │                                       │
│  ServerThread  :8377  ─ accept loop ─ waitUntilReady(200ms) + waitForNextConn  │
│    GET /            → BinaryData index_html      (embedded, no file I/O)       │
│    GET /app.js      → BinaryData app_js                                        │
│    GET /worklet.js  → BinaryData worklet_js                                    │
│    GET /api/session → JSON: format, rate, channels, name                       │
│    GET /ws          → RFC 6455 upgrade (SHA-1 + Base64 accept key)             │
│                                                                                │
│  PhonePostMixEditor            [MESSAGE THREAD, 30 Hz timer, atomics only]     │
│    URL + QR │ client list + per-client stats │ format & buffer controls        │
│    stereo level meter │ state line │ macOS Local Network hint                  │
└────────────────────────────────────────────────────────────────────────────────┘
                                  │  ws://192.168.1.50:8377/ws   (plain TCP)
                                  ▼
┌───────────────────── Phone browser — http://192.168.1.50:8377 ─────────────────┐
│  NOT a secure context ⇒ no AudioWorklet, no wakeLock, no SAB, no crypto.subtle │
│                                                                                │
│  WsClient ─ binary frames ─▶ JitterBuffer (reorder + target fill)              │
│                                    │                                           │
│                             DriftController (PI, ±0.2 % clamp)                 │
│                                    │  ratio = srcRate/ctxRate · (1+correction) │
│                             Resampler (linear-interp, fractional, stateful)    │
│                                    │                                           │
│                   ┌────────────────┴─────────────────┐                         │
│           AudioWorkletNode                  ScriptProcessorNode                │
│           (only if isSecureContext)         (the v1 path — 4096 frames)        │
│                   └────────────────┬─────────────────┘                         │
│                              ctx.destination                                   │
│                                                                                │
│  UI: [LISTEN] button (gesture-gated) · volume · buffer slider · level meter    │
│  Diagnostics: caps matrix, ctx.sampleRate, buffer ms, underruns, drift ppm,    │
│               packets lost/reordered, bitrate, "keep this page in front"       │
└────────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. C++ module design — class by class

Threading vocabulary used below:
- **AT** = the host's audio callback thread. May not allocate, lock, log, or touch a socket. Ever.
- **ST** = the streamer thread (`juce::Thread`, `Priority::high`). Owns packetisation.
- **NT-accept** = the listener/accept thread.
- **NT-client(n)** = one thread per connected client.
- **MT** = the JUCE message thread. GUI only.

### 3.1 `PhonePostMixProcessor` — `source/PluginProcessor.{h,cpp}`

**Responsibility.** Be a transparent tap. Own the lifetime of the ring, the streamer, the server, and the parameter state. Nothing else.

```cpp
class PhonePostMixProcessor final : public juce::AudioProcessor
{
public:
    void prepareToPlay (double sr, int maxBlock) override;   // MT (host); NOT the audio thread
    void releaseResources() override;                        // MT
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;  // AT
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void getStateInformation (juce::MemoryBlock&) override;  // MT
    void setStateInformation (const void*, int) override;    // MT

private:
    juce::AudioProcessorValueTreeState apvts;
    AudioRingBuffer      ring;
    LevelMeterSource     meter;
    RealtimeGuard        rtGuard;
    ClientRegistry       clients;
    StreamerThread       streamer { ring, clients, meter };
    ServerThread         server   { clients, streamer, *this };
    std::atomic<bool>    streamingEnabled { false };
    std::atomic<int>     overflowCount { 0 };
};
```

**Threading rules.**
- `processBlock` (AT) touches exactly three things, all lock-free: `meter.push()`, `rtGuard.tick()`, `ring.write()`. It never signals a `WaitableEvent` (doc 01 §3.3 flags this as debated; the streamer polls instead, which removes the whole class of priority-inversion risk for the cost of a 2 ms sleep).
- The buffer is **not modified**. Not cleared, not gained, not bypassed. `setLatencySamples(0)` — the stream's latency lives on the phone, and reporting it would make the host delay-compensate the entire session for nothing (doc 01 §3.8).
- `prepareToPlay` runs on the host's message/prepare thread. Anything it writes that `processBlock` reads is `std::atomic` or written before `streamingEnabled` is set true. It: resizes the ring, publishes the new `StreamFormat` with an incremented `configEpoch`, and calls `rtGuard.reset()`. It does **not** create sockets or threads.
- Declared: `acceptsMidi() == false`, `producesMidi() == false`, `getTailLengthSeconds() == 0.0`.
- `isBusesLayoutSupported`: accept mono/mono and stereo/stereo with `in == out`; reject everything else. `auval` probes many configurations and "supports everything" is a fast way to fail (doc 01 §5.4).

**The validator trap, handled explicitly.** `pluginval` and `auval` instantiate and destroy the plugin hundreds of times in rapid succession (doc 01 §5.3). Therefore:
- **The server and streamer threads are created lazily**, only when the user turns streaming on. A freshly constructed, never-enabled plugin opens zero sockets and starts zero threads.
- Teardown is deterministic and bounded: `streamer.stopThread(2000)`, then `server.stopThread(2000)`, then close all client sockets. Nothing in the destructor may wait on a network timeout — `auval` marks a slow destructor as a hang and fails.
- If the port is busy (a second plugin instance), the server probes upward from 8377 to 8397 and reports the port it got. Never hardcode a single port.

### 3.2 `AudioRingBuffer` — `source/audio/AudioRingBuffer.{h,cpp}`

**Responsibility.** Carry float audio from AT to ST without locking. This is the single most safety-critical class in the project.

**Design.** `juce::AbstractFifo` (a **single-reader, single-writer** index manager holding no data) plus a `juce::AudioBuffer<float>` that we own. Two producers corrupts it silently (doc 01 §3.2) — so exactly one writer (AT) and exactly one reader (ST), enforced by making `write()` and `read()` private-by-convention with a comment naming the owning thread on every method.

```cpp
class AudioRingBuffer
{
public:
    // MT only, before streaming starts.
    void setSize (int numChannels, int numFrames);   // allocates; clears; never called again while live

    // AT only. Returns frames DROPPED (>0 means the streamer fell behind).
    int  write (const juce::AudioBuffer<float>& src) noexcept;

    // ST only. Returns frames actually read (may be < wanted).
    int  read  (juce::AudioBuffer<float>& dst, int wanted) noexcept;

    int  getNumReady() const noexcept;   // either thread; approximate by design
    void reset() noexcept;               // ST only, or MT while stopped
};
```

**Sizing.** `max (maxBlockSize * 4, (int) (sampleRate * 0.5)) + framesPerPacket`. Half a second at 48 kHz stereo float is ~192 kB — cheap insurance against a Wi-Fi retransmit storm stalling the streamer (doc 01 §3.7).

**Threading rules.** No allocation in `write`/`read`. `setSize` allocates and must be called only from `prepareToPlay` while `streamingEnabled` is false. Channel-count mismatch between `src` and the ring is handled by clamping the source channel index (mono host feeding a stereo ring duplicates; the validator does probe this).

### 3.3 `RealtimeGuard` — `source/audio/RealtimeGuard.h`

**Responsibility.** Stop the plugin from firehosing an hour of audio into the network in four seconds during an offline bounce.

**Why it is not just `isNonRealtime()`.** Doc 01 §3.9: `isNonRealtime()` is only reliable after `prepareToPlay`, can change on every call, Studio One has been reported to leave it false during offline render, and Ableton can bounce at a different sample rate than the project's playback rate.

**Design — belt and braces.**
1. `prepareToPlay` records `isNonRealtime()`. If true, streaming is suspended and the GUI shows "offline render — stream paused".
2. Independently, `tick(numFrames)` on the AT accumulates frames and compares against a wall clock sampled with `juce::Time::getMillisecondCounterHiRes()` **on the streamer thread**, not the audio thread. If more than `1.5 × realtime` worth of frames arrived in a wall-second, the streamer declares an offline render regardless of what the host claims, drops the backlog, and suspends.
3. Recovery is automatic: two consecutive clean wall-seconds resume streaming.

All state is `std::atomic<uint64_t>` / `std::atomic<bool>`. The AT does one relaxed `fetch_add`.

### 3.4 `StreamerThread` — `source/stream/StreamerThread.{h,cpp}`

**Responsibility.** The only place that turns audio into bytes.

```cpp
class StreamerThread final : public juce::Thread
{
public:
    StreamerThread (AudioRingBuffer&, ClientRegistry&, LevelMeterSource&);
    void run() override;                                   // ST
    void setFormat (const StreamFormat&);                  // MT — publishes atomically, bumps configEpoch
    StreamFormat getFormat() const noexcept;               // any thread
    uint64_t getPacketsSent() const noexcept;              // MT (GUI)
};
```

**`run()` loop, exactly:**

```
scratch  = AudioBuffer<float> (2, MAX_FRAMES_PER_PACKET)   // allocated ONCE, here
packet   = std::vector<uint8_t> (HEADER + MAX_PAYLOAD)     // allocated ONCE, here

while (! threadShouldExit())
{
    fmt = format.load();                                   // may have changed under us
    if (fmt != lastFmt) { ring.reset(); sampleClock = 0; lastFmt = fmt; }

    if (! enabled || rtGuard.isOffline()) { wait (10); continue; }

    if (ring.getNumReady() >= fmt.framesPerPacket)
    {
        ring.read (scratch, fmt.framesPerPacket);
        starvedTicks = 0;
    }
    else if (++starvedTicks * 2 >= SILENCE_AFTER_MS)       // host stopped calling processBlock
    {
        scratch.clear();                                   // emit silence, keep the receiver's clock alive
        flags |= FLAG_SILENCE;
    }
    else { wait (2); continue; }

    PacketWriter::write (packet, fmt, seq++, sampleClock, flags, scratch);
    sampleClock += fmt.framesPerPacket;
    clients.broadcast (packet.data(), packetSize);         // non-blocking: enqueues per client
    flags = 0;
}
```

**The silence-fill rule is load-bearing.** Logic does not call `processBlock` when the transport is stopped or the region is silent (doc 01 §5.1); Ableton stops on transport stop too. Without silence packets the receiver's jitter buffer drains, it underruns, and it looks like a disconnect. With them, the phone hears silence and the session stays healthy. PCM silence is not free like Opus silence — 2 kB per packet — but on a LAN that is 1.5 Mbit/s of zeroes and nobody notices. `SILENCE_AFTER_MS = 40`.

**Threading rules.** Everything allocated in `run()` before the loop. Never touches JUCE GUI. `Priority::high`, **never real-time** — competing with the audio thread for a core is a self-inflicted wound (doc 01 §3.4). `stopThread(2000)` is safe because the loop's longest blocking call is `wait(10)`.

### 3.5 `ServerThread` — `source/net/ServerThread.{h,cpp}`

**Responsibility.** Own the listening socket, accept connections, spawn `ClientConnection` threads, reap dead ones.

```cpp
class ServerThread final : public juce::Thread
{
public:
    bool start (int preferredPort);   // MT — probes 8377..8397, returns false if all busy
    void run() override;              // NT-accept
    int  getPort() const noexcept;
    juce::String getUrl() const;      // "http://192.168.1.50:8377"
};
```

**The accept loop, and the JUCE gotcha it works around.** Verified in JUCE 9.0.1 `juce_Socket.cpp:636`: `StreamingSocket::waitForNextConnection()` calls `accept()` **directly, with no timeout and no select**. Calling it naively means the accept thread blocks forever and `stopThread()` times out. The fix is to poll first:

```cpp
void ServerThread::run()
{
    while (! threadShouldExit())
    {
        // waitUntilReady on a listener socket = "is a connection pending?"  (select(2))
        if (listener.waitUntilReady (true, 200) != 1)
            { reapFinishedClients(); continue; }

        if (auto* s = listener.waitForNextConnection())
            clients.add (std::make_unique<ClientConnection> (std::unique_ptr<StreamingSocket> (s), *this));
    }
    clients.closeAll();               // unblocks every per-client thread
}
```

`stopThread(2000)` then always completes within ~200 ms. On shutdown, `listener.close()` is called first so a pending `accept()` returns immediately.

**Binding.** `createListener (port, "0.0.0.0")` — bind on all interfaces. The *displayed* IP is chosen by `NetworkInterfaces` (§3.9), which is a separate question from what we bind.

### 3.6 `ClientConnection` — `source/net/ClientConnection.{h,cpp}`

**Responsibility.** One TCP connection, from raw HTTP through to a live audio stream. One thread each.

**State machine.**

```
ReadingRequest ──▶ (GET /, /app.js, /worklet.js, /api/session) ──▶ ServeStatic ──▶ Close
       │
       └────────▶ (GET /ws with Upgrade: websocket) ──▶ Handshake ──▶ WsOpen ──▶ Streaming
                                                            │
                                                            └─ bad key / wrong version ──▶ 400 ──▶ Close
```

**Reading (NT-client).** Read into a 8 kB buffer with `socket.read (buf, n, false)` after `waitUntilReady (true, 100)`, until `\r\n\r\n` or 8 kB (then 431 and close). Hard 5-second budget on the request line — a half-open connection must not pin a thread.

**Writing (NT-client).** `StreamingSocket::write()` blocks. Before each write, `waitUntilReady (false, 200)`. If it times out, the phone is stalled:
- increment `droppedPackets`,
- discard the **oldest** queued packets down to a 200 ms high-water mark,
- keep the connection.

Drop-oldest, never drop-newest: the listener wants to hear *now*, not to catch up on what they missed. This is the TCP backpressure story that doc 02 §5.2 flags as the reason uWebSockets exposes `getBufferedAmount()`; we get the same behaviour from a bounded queue.

**Per-client queue.** `juce::AbstractFifo` of packet indices over a preallocated slab of N × maxPacketSize (N = 64). Written by ST, read by NT-client(n). SPSC again. If the FIFO is full, ST does not block — `broadcast()` drops for that client only and marks it. **One slow phone must never stall the streamer or the other phones.**

**WebSocket duties.** Respond to `PING` with `PONG`; send a `PING` every 5 s of silence; handle `CLOSE` by echoing and closing; handle client text frames (`ready`, `stat`, `bye`) by forwarding the parsed JSON to `ClientRegistry`. Client→server frames are always masked (RFC 6455 requires it); a server that receives an unmasked frame must fail the connection. Server→client frames must never be masked.

### 3.7 `ClientRegistry` — `source/stream/ClientRegistry.{h,cpp}`

**Responsibility.** The one shared, mutable list of clients. The single place a lock is allowed.

```cpp
class ClientRegistry
{
public:
    void add (std::unique_ptr<ClientConnection>);          // NT-accept
    void broadcast (const uint8_t*, size_t) noexcept;      // ST — takes a SpinLock, enqueues, never blocks on I/O
    void closeAll();                                       // NT-accept / MT
    std::vector<ClientInfo> snapshot() const;              // MT (GUI) — copies under the lock
    void updateStats (ClientId, const ClientStats&);       // NT-client(n)
};
```

**Threading rules.** A `juce::SpinLock` guards the vector, held only for the duration of an enqueue (a memcpy into a preallocated slab). It is **never** taken on the audio thread — the AT does not know this class exists. That is the whole reason a lock is tolerable here. Client removal is deferred: a finished connection sets `finished = true` and NT-accept reaps it on its next 200 ms tick, so no thread ever joins itself.

`ClientStats` is what the phone reports back every 2 s: `bufferMs`, `underruns`, `driftPpm`, `packetsLost`, `ctxSampleRate`, `path` ("worklet"/"spn"). This is what makes the plugin GUI genuinely useful for debugging, and it costs one small JSON frame per client per two seconds.

### 3.8 `WebSocket` / `HttpRequest` / `HttpResponse` — `source/net/`

Covered in full, with code, in §6.

### 3.9 `NetworkInterfaces` — `source/net/NetworkInterfaces.{h,cpp}`

**Responsibility.** Answer "which IP do I print on screen?" correctly, because getting it wrong is the #1 cause of "the QR points at the wrong address" (doc 02 §3.3).

**Algorithm.** `juce::IPAddress::getAllAddresses (false)` (IPv4 only — v1 does not do IPv6; JUCE's own `waitForNextConnection` uses `inet_ntoa` and is IPv4-only anyway). Then rank:

1. Drop loopback (`127/8`), link-local (`169.254/16`), and null addresses.
2. Prefer RFC 1918 addresses: `192.168/16` > `10/8` > `172.16/12`.
3. **De-prioritise known-bad ranges:** `10.211.55.*` / `10.37.129.*` (Parallels), `192.168.64.*` (UTM/Docker Desktop), `172.17-31.*` (Docker bridge), and anything on a `utun*`/`tap*` interface. VPN and virtualisation adapters are the classic wrong answer.
4. If more than one survives, the GUI shows a small combo box so the user can pick, and the choice is persisted in the plugin state.

Re-evaluated on a 5-second timer; if the selected address disappears (DHCP change, Wi-Fi switch), the URL and QR update and the GUI says so.

### 3.10 `PhonePostMixEditor` — `source/PluginEditor.{h,cpp}`

**Responsibility.** Show state, take four decisions from the user. Nothing more. 640 × 420, fixed size in v1 (a resizable editor is a v2 nicety and a source of validator noise).

Layout, top to bottom:

| Region | Contents |
|---|---|
| **Header** | Big **STREAM** toggle (the only thing that opens a socket). State line to its right: `Stopped` / `Listening — no clients` / `Streaming to 2 devices` / `Host not playing — sending silence` / `Offline render — paused` / `Port 8377 busy, using 8378`. |
| **Connect** | The URL in a large monospace font, selectable and copyable (`http://192.168.1.50:8377`). A QR of the same URL, ~160 px, to its left. An interface combo box if more than one candidate IP exists. A "Copy URL" button. |
| **Clients** | One row per connected phone: IP · path (`worklet`/`spn`) · buffer ms · underruns · drift ppm · dropped packets. Empty state reads "No devices yet — scan the code or type the URL into your phone's browser." |
| **Format** | Format combo (`PCM 16-bit` default / `PCM 24-bit` / `Float 32-bit`); packet size combo (`256` / `512` default / `1024` frames) with the resulting ms and kbit/s shown live; a target-latency slider (40–500 ms, default 120) whose value is *sent to the receiver* — the jitter buffer lives on the phone. |
| **Meter** | Stereo peak meter with a 1.5 s peak hold, driven by `LevelMeterSource`. Proves audio is reaching the plugin before the user starts blaming the network. |
| **Footer** | Two persistent one-liners: the security warning ("unencrypted — use only on a network you trust"), and, on macOS only, the Local Network hint. |

**The macOS Local Network hint is not optional.** Doc 01 §4.3: since macOS 15 the permission attaches to the *host process*, the prompt says "Ableton Live would like to find devices on your local network", the user has no idea it is your plugin, and if they have already denied it you are silently dead with no error. Detection: streaming is enabled, ≥10 s elapsed, zero clients have ever connected. Then show, in orange:

> No device has connected yet. If your phone can't reach this address, open **System Settings → Privacy & Security → Local Network** and enable **Live** *(the host app's name, resolved at runtime via `juce::File::getSpecialLocation (currentApplicationFile).getFileNameWithoutExtension()`)*.

**Threading rules.** MT only. A single `juce::Timer` at 30 Hz reads `std::atomic` counters and calls `ClientRegistry::snapshot()`. The editor never shares a mutex with `processBlock` and never blocks. The editor may be created and destroyed many times over the processor's life — it holds no state the processor needs.

### 3.11 `LevelMeterSource` — `source/audio/LevelMeterSource.h`

Two `std::atomic<float>` per channel (peak, and a slow-decayed peak-hold). `push()` on the AT does a `std::max` over the block and one relaxed store per channel — no loops over samples beyond the max, no allocation. `read()` on MT does relaxed loads and applies decay in *display* time, so the decay rate is independent of block size. This is the standard pattern from doc 01 §5.5: atomics plus a 30 Hz timer, never a shared mutex.

---

## 4. The wire protocol

Also written to `docs/protocol.md` so a receiver author never has to read C++.

### 4.1 Endpoints

| Method | Path | Response |
|---|---|---|
| `GET` | `/` | `index.html` from `BinaryData`, `Content-Type: text/html; charset=utf-8`, `Cache-Control: no-store` |
| `GET` | `/app.js` | `application/javascript` |
| `GET` | `/worklet.js` | `application/javascript` |
| `GET` | `/api/session` | `application/json` — the same object as the `hello` frame, for debugging with `curl` |
| `GET` | `/ws` (with `Upgrade: websocket`) | `101 Switching Protocols` |
| anything else | | `404` with a one-line text body |

`Connection: close` on every static response — v1 does not do keep-alive. Three assets, three connections, and no pipelining bugs.

### 4.2 Binary audio frame — the packet header, field by field

Sent as a WebSocket **binary** frame (opcode `0x2`), unmasked, unfragmented. One audio packet per WebSocket frame. All multi-byte integers are **little-endian** (every target is LE; the receiver uses `DataView` with `littleEndian = true` explicitly).

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 4 | `char[4]` | `magic` | `'P','P','M','X'` = `0x50 0x50 0x4D 0x58`. A receiver MUST drop any frame that does not start with this. |
| 4 | 1 | `u8` | `version` | Protocol version. `1` for v1. A receiver MUST refuse a version it does not know and say so in the UI. |
| 5 | 1 | `u8` | `format` | `0` = PCM signed 16-bit LE · `1` = PCM signed 24-bit LE (3 bytes/sample) · `2` = IEEE float32 LE. |
| 6 | 1 | `u8` | `channels` | `1` or `2`. Interleaved. |
| 7 | 1 | `u8` | `flags` | bit0 `DISCONTINUITY` — sample clock reset, receiver must flush · bit1 `SILENCE` — synthesised silence, host is not playing · bit2 `CONFIG_CHANGED` — first packet of a new `configEpoch` · bits 3–7 reserved, MUST be 0. |
| 8 | 4 | `u32` | `seq` | Increments by 1 per packet, wraps at 2³². Gaps mean loss; decreases mean reordering. |
| 12 | 4 | `u32` | `sampleRate` | The **host's** sample rate, e.g. `44100`, `48000`, `96000`. Present in **every** packet, not just the handshake — packets get lost, receivers join late, and Ableton can change the rate under you (doc 01 §5.2). |
| 16 | 2 | `u16` | `frames` | Frames per channel in this packet. `256`, `512` or `1024`. |
| 18 | 2 | `u16` | `configEpoch` | Increments on any change to `{sampleRate, channels, format, frames}`. The receiver compares this, not the individual fields, to decide whether to rebuild its pipeline. |
| 20 | 8 | `u64` | `sampleClock` | Absolute frame index since the stream started, of the **first frame in this packet**. Resets to 0 on `DISCONTINUITY`. This is what makes drift correction tractable — never derive timing from arrival time (doc 02 §2.2). |
| 28 | 4 | `u32` | `senderTimeMs` | Low 32 bits of the sender's millisecond counter at send time. Diagnostics only (one-way delay variation); the receiver must not use it for playout timing, since the clocks are unrelated. |
| **32** | | | **payload** | `frames × channels × bytesPerSample` bytes, interleaved L,R,L,R… |

Header is **32 bytes**. The offset is deliberately a multiple of 4 so the receiver can do `new Float32Array(buf, 32, n)` for the float32 format without copying (a `Float32Array` view requires a 4-byte-aligned `byteOffset`).

```cpp
// source/stream/PacketWriter.h  — asserted byte-for-byte in tests/PacketWriterTests.cpp
namespace ppm::wire
{
    inline constexpr uint32_t kMagic       = 0x584D5050u;  // 'PPMX' when written LE byte-by-byte
    inline constexpr int      kHeaderBytes = 32;
    inline constexpr uint8_t  kVersion     = 1;

    enum Format : uint8_t { pcm16 = 0, pcm24 = 1, f32 = 2 };
    enum Flags  : uint8_t { discontinuity = 1u << 0, silence = 1u << 1, configChanged = 1u << 2 };
}
```

**Payload sizes** (stereo, per packet):

| frames | @48 kHz | pcm16 | pcm24 | f32 | pcm16 bitrate |
|---:|---:|---:|---:|---:|---:|
| 256 | 5.33 ms | 1024 B | 1536 B | 2048 B | 1.54 Mbit/s |
| **512** | **10.67 ms** | **2048 B** | 3072 B | 4096 B | **1.54 Mbit/s** |
| 1024 | 21.33 ms | 4096 B | 6144 B | 8192 B | 1.54 Mbit/s |

Bitrate is a function of rate/channels/format only; packet size trades wakeup frequency against granularity. **512 is the default**: 94 packets/s, comfortable for a phone's JS event loop, 10.7 ms of packetisation delay.

### 4.3 Handshake — JSON over WebSocket text frames

Immediately after the `101`, the **server sends** one text frame:

```json
{
  "type": "hello",
  "protocol": 1,
  "session": "9a1f0c2e-4b6d-4e8a-9f21-77c3a0b5d412",
  "sender": "PhonePostMix 0.1.0",
  "host": "Live",
  "headerBytes": 32,
  "format": "pcm16",
  "sampleRate": 48000,
  "channels": 2,
  "framesPerPacket": 512,
  "configEpoch": 3,
  "suggestedTargetLatencyMs": 120,
  "serverTimeMs": 1724781234567,
  "warnings": ["insecure-transport"]
}
```

The **client replies** with one text frame. The server will start sending audio whether or not this arrives (a receiver that never speaks still gets audio), but the reply is what populates the plugin's client list:

```json
{
  "type": "ready",
  "protocol": 1,
  "ctxSampleRate": 44100,
  "path": "spn",
  "caps": {
    "secureContext": false,
    "audioWorklet": false,
    "sharedArrayBuffer": false,
    "audioDecoder": false,
    "wakeLock": false,
    "audioSession": true
  },
  "targetLatencyMs": 120,
  "ua": "Mozilla/5.0 (iPhone; ...)"
}
```

Then, every 2 seconds, the client sends a stats frame. This is what the plugin GUI displays per row:

```json
{
  "type": "stat",
  "bufferMs": 118,
  "targetMs": 120,
  "driftPpm": -37,
  "underruns": 0,
  "overruns": 0,
  "packetsReceived": 940,
  "packetsLost": 0,
  "packetsReordered": 0,
  "playing": true
}
```

Server→client control frames, all text, all with a `type`:

| `type` | Direction | Meaning |
|---|---|---|
| `hello` | S→C | Once, on open. |
| `config` | S→C | Format changed. Same fields as `hello` plus the new `configEpoch`. The next binary packet carries `CONFIG_CHANGED`. |
| `state` | S→C | `{"type":"state","streaming":true,"hostPlaying":false}` — lets the phone say "DAW is stopped" instead of showing an unexplained silence. |
| `ping` / `pong` | both | JSON-level heartbeat, in addition to RFC 6455 control frames. Belt and braces; some intermediaries eat control frames. |
| `bye` | S→C | Server shutting down cleanly. Receiver shows "sender closed the stream" rather than "connection lost". |
| `ready` | C→S | Once, after `hello`. |
| `stat` | C→S | Every 2 s. |
| `setLatency` | C→S | `{"type":"setLatency","ms":80}` — echoed to the plugin GUI so the engineer can see what the listener chose. |

**Unknown `type` values and unknown JSON keys MUST be ignored by both ends.** That is the entire forward-compatibility story and it costs nothing to honour.

### 4.4 Signalling a sample rate / channel count / format change

Three mechanisms, deliberately redundant, because each one alone fails in a real scenario:

1. **In every packet header.** `sampleRate`, `channels`, `format`, `frames` are in all 32 bytes of every packet. A receiver that joins mid-stream, or loses the `config` text frame, still knows exactly what it is decoding.
2. **`configEpoch`, a `u16` bumped on any change.** The receiver latches the epoch. When it changes, it: flushes the jitter buffer, resets the resampler phase, re-derives `ratio = sampleRate / ctx.sampleRate`, and resets the drift controller's integrator. It does *not* tear down the `AudioContext` — one context per page, for the life of the page (doc 03 §2.5); re-creating it would lose the user-gesture unlock and require another tap.
3. **The `config` text frame plus the `CONFIG_CHANGED` flag on the first packet of the new epoch.** The text frame arrives ahead of the audio, so the receiver can rebuild before the first mismatched sample lands; the flag is the authoritative marker in case the frames race.

**On the sender side**, a change is only ever published from `prepareToPlay` or from a GUI control, both on the message thread:

```
newFormat.configEpoch = oldFormat.configEpoch + 1;
streamer.setFormat (newFormat);          // atomic store
// StreamerThread notices fmt != lastFmt on its next tick:
//   ring.reset();  sampleClock = 0;  flags |= DISCONTINUITY | CONFIG_CHANGED
// ClientRegistry sends the `config` text frame to every client.
```

`DISCONTINUITY` also fires without a format change — on stream start, on resume after an offline render, and after a >1 s gap. It means exactly one thing to the receiver: **throw away what you have, the sample clock restarted.**

---

## 5. Root `CMakeLists.txt` — ready to use

```cmake
# ============================================================================
#  PhonePostMix — root build
#  Builds VST3 + AU (macOS) + Standalone. JUCE is fetched automatically.
#  Usage:  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# ============================================================================
cmake_minimum_required(VERSION 3.22...4.0)

project(PhonePostMix
        VERSION      0.1.0
        DESCRIPTION  "Stream your DAW master bus to a phone browser over Wi-Fi"
        LANGUAGES    C CXX)

# ---------------------------------------------------------------------------
#  Options
# ---------------------------------------------------------------------------
option(PPM_BUILD_TESTS       "Build the Catch2 unit tests"                        ON)
option(PPM_COPY_AFTER_BUILD  "Install the plugin into the user plug-in folders"  OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)          # clangd / VS Code / CLion
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Universal binary + deployment target must be set BEFORE any target exists.
if (APPLE)
    set(CMAKE_OSX_ARCHITECTURES  "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0"      CACHE STRING "" FORCE)
endif()

# Single-config generators default to Debug, which nobody wants by accident.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
#  JUCE 9 — fetched, not vendored, not a submodule.
#  Pinned to a tag: a repo that breaks next month is worse than one a version
#  behind. 9.0.1 was released 2026-08-10.
# ---------------------------------------------------------------------------
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.1
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
#  The plugin
# ---------------------------------------------------------------------------
juce_add_plugin(PhonePostMix
    PRODUCT_NAME                "PhonePostMix"
    COMPANY_NAME                "PhonePostMix"
    COMPANY_WEBSITE             "https://github.com/lucamanuel06/phonepostmix"
    BUNDLE_ID                   "dev.phonepostmix.plugin"
    PLUGIN_MANUFACTURER_CODE    Ppmx        # 4 chars, at least one uppercase
    PLUGIN_CODE                 Ppm1        # 4 chars, exactly one uppercase
    FORMATS                     AU VST3 Standalone
    VST3_CATEGORIES             "Analyzer" "Tools"
    AU_MAIN_TYPE                kAudioUnitType_Effect
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    NEEDS_WEB_BROWSER           FALSE
    NEEDS_CURL                  FALSE
    COPY_PLUGIN_AFTER_BUILD     ${PPM_COPY_AFTER_BUILD}
    # The Standalone target opens listening sockets, so it must NOT be
    # sandboxed, and it needs a Local Network usage string on macOS 15+.
    HARDENED_RUNTIME_ENABLED    TRUE
    APP_SANDBOX_ENABLED         FALSE
    PLIST_TO_MERGE              "<plist version=\"1.0\"><dict>
        <key>NSLocalNetworkUsageDescription</key>
        <string>PhonePostMix streams audio to phones and tablets on your local network.</string>
        <key>NSBonjourServices</key><array><string>_phonepostmix._tcp</string></array>
        </dict></plist>")

# ---------------------------------------------------------------------------
#  The receiver page, embedded in the binary — no runtime file dependency.
#  Produces BinaryData::index_html, BinaryData::app_js, BinaryData::worklet_js.
# ---------------------------------------------------------------------------
juce_add_binary_data(PhonePostMixWebAssets
    HEADER_NAME "WebAssets.h"
    NAMESPACE   ppmweb
    SOURCES
        web/index.html
        web/app.js
        web/worklet.js)

# Binary data is linked into a shared/module target on some platforms; PIC is
# required or the VST3 link fails on Linux.
set_target_properties(PhonePostMixWebAssets PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

# ---------------------------------------------------------------------------
#  Sources
# ---------------------------------------------------------------------------
target_sources(PhonePostMix PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp

    source/audio/AudioRingBuffer.cpp

    source/net/ServerThread.cpp
    source/net/ClientConnection.cpp
    source/net/HttpRequest.cpp
    source/net/HttpResponse.cpp
    source/net/WebSocket.cpp
    source/net/Sha1.cpp
    source/net/NetworkInterfaces.cpp

    source/stream/PacketWriter.cpp
    source/stream/StreamerThread.cpp
    source/stream/ClientRegistry.cpp

    source/gui/QrCodeComponent.cpp
    source/gui/ClientListComponent.cpp
    source/gui/LevelMeter.cpp

    source/vendor/qrcodegen.c)

target_include_directories(PhonePostMix PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/source)

target_compile_definitions(PhonePostMix PUBLIC
    JUCE_WEB_BROWSER=0              # drops the libwebkit2gtk dep on Linux
    JUCE_USE_CURL=0                 # drops the libcurl dep on Linux
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0    # legal under AGPL-3.0; see LICENSE
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(PhonePostMix
    PRIVATE
        PhonePostMixWebAssets
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# The vendored C is not ours to warn about.
set_source_files_properties(source/vendor/qrcodegen.c PROPERTIES COMPILE_OPTIONS "")

# ---------------------------------------------------------------------------
#  Tests
# ---------------------------------------------------------------------------
if (PPM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
#  Friendly summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "PhonePostMix ${PROJECT_VERSION}")
message(STATUS "  build type ....... ${CMAKE_BUILD_TYPE}")
message(STATUS "  formats .......... AU VST3 Standalone")
message(STATUS "  tests ............ ${PPM_BUILD_TESTS}")
message(STATUS "  copy after build . ${PPM_COPY_AFTER_BUILD}")
message(STATUS "  artefacts ........ ${CMAKE_BINARY_DIR}/PhonePostMix_artefacts")
message(STATUS "")
```

**Notes on choices above.**
- `3.22...4.0` is the version-range form: minimum 3.22, policies set as for 4.0. This is what keeps CMake 4.4 happy without `CMAKE_POLICY_VERSION_MINIMUM` hacks. If a future JUCE dependency trips CMake 4's removal of `<3.5` compatibility, the escape hatch is `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`; document it in troubleshooting rather than in the build file.
- `COPY_PLUGIN_AFTER_BUILD` **must** be OFF in CI — the runner has no plugin folder and the copy step fails or pollutes (doc 03 §3.2). Hence the option, defaulting OFF, with a `dev` preset turning it on.
- `JUCE_WEB_BROWSER=0` + `JUCE_USE_CURL=0` removes `libwebkit2gtk-4.1-dev` and `libcurl4-openssl-dev` from the Linux dependency list, which is the version-churny part of doc 03 §3.4.
- **No `kAudioComponentFlag_SandboxSafe`.** JUCE does not set it by default and we must not add it. Declaring sandbox-safety on a networking AU is declaring a falsehood, and hosts that trust the flag will load us into a sandbox where the sockets silently fail (doc 01 §4.1).

`CMakePresets.json` (small, ships alongside):

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "default", "generator": "Ninja", "binaryDir": "${sourceDir}/build",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "PPM_BUILD_TESTS": "ON" } },
    { "name": "dev", "inherits": "default", "binaryDir": "${sourceDir}/build-dev",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "PPM_COPY_AFTER_BUILD": "ON" } }
  ],
  "buildPresets": [ { "name": "default", "configurePreset": "default" },
                    { "name": "dev", "configurePreset": "dev" } ],
  "testPresets":  [ { "name": "default", "configurePreset": "default",
                      "output": { "outputOnFailure": true } } ]
}
```

---

## 6. WebSocket server — a minimal but real implementation sketch

### 6.1 What JUCE gives us, verified against the 9.0.1 tree

I checked the actual JUCE 9.0.1 source tree rather than relying on memory:

| Need | In JUCE? | Verdict |
|---|---|---|
| Base64 | ✅ `modules/juce_core/text/juce_Base64.h` — `juce::Base64::toBase64 (const void*, size_t)` returns a `String`. | Use it. Exactly the API we need. |
| SHA-256 | ✅ `modules/juce_cryptography/hashing/juce_SHA256.{h,cpp}` | Present but **useless here** — RFC 6455 mandates SHA-1. |
| **SHA-1** | ❌ **Not present.** `modules/juce_cryptography/` contains only `BlowFish`, `Primes`, `RSAKey`, `MD5`, `SHA256`, `Whirlpool`. | **Vendor a ~100-line public-domain SHA-1.** Confirmed by listing the module tree at tag 9.0.1. |
| TCP sockets | ✅ `juce::StreamingSocket` — `createListener`, `waitForNextConnection`, `waitUntilReady`, `read`, `write`, `close`. | Use it, with the accept-loop workaround from §3.5. |
| IP enumeration | ✅ `juce::IPAddress::getAllAddresses (bool includeIPv6)` | Use it. |
| JSON | ✅ `juce::JSON` / `juce::var` | Use it for the handshake frames. Small and slow, but this is 2 messages per client per 2 seconds. |

We also do **not** link `juce_cryptography` at all — nothing in it is useful to us, so it stays out of the build.

### 6.2 Vendored SHA-1 — `source/net/Sha1.{h,cpp}`

Steve Reid's public-domain implementation (the one in the RFC 3174 lineage, used by dozens of projects) is ~100 lines and has no dependencies. Interface we expose:

```cpp
// source/net/Sha1.h
namespace ppm
{
    struct Sha1
    {
        static void hash (const void* data, size_t len, uint8_t out[20]) noexcept;
    };
}
```

`source/vendor/README.md` records: file, upstream URL, licence (public domain), the commit/version taken, and the date. Non-negotiable in an AGPL repo.

Tested against the RFC 3174 vectors:
- `""` → `da39a3ee5e6b4b0d3255bfef95601890afd80709`
- `"abc"` → `a9993e364706816aba3e25717850c26c9cd0d89d`
- `"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"` → `84983e441c3bd26ebaae4aa1f95129e5e54670f1`
- 1 000 000 × `'a'` → `34aa973cd4c4daa4f61eeb2bdbad27316534016f`

### 6.3 The upgrade handshake

```cpp
// source/net/WebSocket.cpp
namespace ppm::ws
{
    static constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    juce::String makeAcceptKey (const juce::String& clientKey)
    {
        const auto concatenated = (clientKey + kGuid).toStdString();
        uint8_t digest[20] {};
        Sha1::hash (concatenated.data(), concatenated.size(), digest);
        return juce::Base64::toBase64 (digest, sizeof (digest));
    }

    // Returns the raw 101 response, or an empty string if the request is not a
    // valid upgrade (caller then replies 400).
    juce::String makeHandshakeResponse (const HttpRequest& req)
    {
        if (! req.headerContainsToken ("Upgrade", "websocket"))         return {};
        if (! req.headerContainsToken ("Connection", "upgrade"))        return {};
        if (req.getHeader ("Sec-WebSocket-Version") != "13")            return {};

        const auto key = req.getHeader ("Sec-WebSocket-Key").trim();
        if (key.isEmpty())                                              return {};

        return "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " + makeAcceptKey (key) + "\r\n\r\n";
    }
}
```

Header matching is **case-insensitive** for names, and `Connection:` may be a comma-separated list containing `keep-alive, Upgrade` — hence `headerContainsToken` rather than an equality test. This is the single most common bug in hand-rolled WebSocket servers and it manifests as "works in Chrome, fails in Safari".

**The canonical test vector from RFC 6455 §1.3**, asserted in `tests/WebSocketTests.cpp`:

```
Sec-WebSocket-Key:    dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
```

### 6.4 Frame parsing (client → server; always masked)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| op    |M| Payload len |    Extended payload length     |
|I|S|S|S| code  |A|     (7)     |            (16/64)             |
|N|V|V|V|       |S|             |                                |
| |1|2|3|       |K|             |                                |
+-+-+-+-+-------+-+-------------+-------------------------------+
|          Masking-key (4 bytes, present iff MASK == 1)          |
+----------------------------------------------------------------+
|                        Payload Data                            |
```

```cpp
struct Frame
{
    bool     fin        = false;
    uint8_t  opcode     = 0;     // 0x0 cont, 0x1 text, 0x2 binary, 0x8 close, 0x9 ping, 0xA pong
    std::vector<uint8_t> payload;
};

enum class ParseResult { needMoreData, ok, protocolError };

// `in` is the accumulated read buffer. On `ok`, `consumed` bytes are removed by the caller.
ParseResult parseFrame (const uint8_t* in, size_t available, Frame& out, size_t& consumed)
{
    if (available < 2) return ParseResult::needMoreData;

    out.fin           = (in[0] & 0x80) != 0;
    const auto rsv    =  in[0] & 0x70;
    out.opcode        =  in[0] & 0x0F;
    const bool masked = (in[1] & 0x80) != 0;
    uint64_t len      =  in[1] & 0x7F;
    size_t   pos      = 2;

    if (rsv != 0)   return ParseResult::protocolError;   // no extensions negotiated
    if (! masked)   return ParseResult::protocolError;   // RFC 6455 §5.1: client frames MUST be masked

    if (len == 126)
    {
        if (available < pos + 2) return ParseResult::needMoreData;
        len = (uint64_t) in[pos] << 8 | in[pos + 1];     // network byte order — big-endian
        pos += 2;
        if (len < 126) return ParseResult::protocolError;             // non-minimal length
    }
    else if (len == 127)
    {
        if (available < pos + 8) return ParseResult::needMoreData;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | in[pos + i];
        pos += 8;
        if (len < 65536 || (len >> 63) != 0) return ParseResult::protocolError;
    }

    // Control frames: <=125 bytes, never fragmented (RFC 6455 §5.5).
    if (out.opcode >= 0x8 && (len > 125 || ! out.fin)) return ParseResult::protocolError;

    // A client that sends us a >1 MB frame is not our receiver. Refuse rather than allocate.
    if (len > kMaxIncomingFrame) return ParseResult::protocolError;

    if (available < pos + 4 + len) return ParseResult::needMoreData;

    const uint8_t* mask = in + pos;  pos += 4;

    out.payload.resize ((size_t) len);
    for (uint64_t i = 0; i < len; ++i)
        out.payload[(size_t) i] = in[pos + i] ^ mask[i & 3];   // §5.3 unmask

    consumed = pos + (size_t) len;
    return ParseResult::ok;
}
```

Points worth being explicit about, because each is a real bug people ship:
- **Extended lengths are big-endian**, unlike our audio payload which is little-endian. Two byte orders in one file; comment it.
- **Non-minimal length encodings are a protocol error.** A conformant client never sends `126` for a 100-byte payload.
- **Fragmentation:** the receiver we ship never fragments, but a browser may. Handle `opcode 0x0` continuation by appending into a per-connection assembly buffer with a total-size cap; a control frame may be interleaved *between* fragments and must be handled immediately, not queued.
- **Cap incoming frames** (`kMaxIncomingFrame = 64 * 1024`). We only ever expect small JSON. Refusing early means no attacker-controlled allocation.

### 6.5 Frame serialisation (server → client; never masked)

```cpp
// Appends a complete unmasked frame to `out`. Server frames MUST NOT be masked (§5.1).
void writeFrame (std::vector<uint8_t>& out, uint8_t opcode,
                 const void* payload, size_t len)
{
    out.clear();
    out.push_back ((uint8_t) (0x80 | opcode));                 // FIN=1, RSV=0

    if (len < 126)
    {
        out.push_back ((uint8_t) len);                         // MASK=0
    }
    else if (len <= 0xFFFF)
    {
        out.push_back (126);
        out.push_back ((uint8_t) (len >> 8));                  // big-endian
        out.push_back ((uint8_t) (len & 0xFF));
    }
    else
    {
        out.push_back (127);
        for (int i = 7; i >= 0; --i)
            out.push_back ((uint8_t) ((len >> (i * 8)) & 0xFF));
    }

    const auto* p = static_cast<const uint8_t*> (payload);
    out.insert (out.end(), p, p + len);
}
```

For audio, `writeFrame` is called with `opcode = 0x2` and the 32-byte header + PCM payload. At 512 frames / pcm16 / stereo that is 2080 bytes of payload → a 4-byte frame header (`126` + 2 length bytes… actually 2080 > 125 so the 16-bit form). One `write()` syscall per packet: the frame header and the payload are assembled into **one** contiguous preallocated buffer, because two `write()` calls on a TCP socket is two syscalls and an invitation to interleaving bugs when the same socket is written from a future second code path.

**Close handshake.** On `opcode 0x8`, echo a close frame with the same status code (or `1000`) and then close the socket. On protocol error, send `1002`. On oversized frame, `1009`. Never just drop the socket — a clean close is what makes the phone say "sender stopped" instead of "connection lost".

### 6.6 What this deliberately does not implement

No `permessage-deflate` (PCM does not compress and it would cost CPU on the audio path). No subprotocol negotiation. No TLS. No keep-alive HTTP. No IPv6. Each of these is a v2 line item, and each is listed in `docs/architecture.md` under "known limitations" rather than discovered by a contributor.

---

## 7. Receiver page design — `web/`

Three files, no build step, no npm, no external requests. `index.html` carries all the CSS inline; `app.js` is a single ES module-free script (plain `<script>`, no `type="module"` — modules are fetched with CORS semantics and there is no reason to invite that on a hand-rolled server); `worklet.js` is separate because `addModule()` requires a URL.

### 7.1 Modules inside `app.js`

Written as plain closures/classes in one file, in this order:

| Module | Responsibility |
|---|---|
| `Caps` | Runs the feature-detection matrix once at load (doc 03 §2.7): `isSecureContext`, `crossOriginIsolated`, `audioWorklet`, `SharedArrayBuffer`, `AudioDecoder`, `WebTransport`, `RTCPeerConnection`, `wakeLock`, `audioSession`. Drives both the audio-path choice and the diagnostics panel. |
| `WsClient` | Opens `ws://<location.host>/ws`. Routes text frames to `Control` and binary frames to `PacketParser`. Reconnects with backoff (0.5 s, 1 s, 2 s, 4 s, capped 5 s) and reports state to the UI. `binaryType = 'arraybuffer'`. |
| `PacketParser` | Validates magic + version, reads the 32-byte header with a `DataView`, converts the payload to a `Float32Array` per channel. |
| `JitterBuffer` | Sequence-ordered ring. Reorder window, loss detection, target fill level. §7.4. |
| `Resampler` | Fractional, stateful, per-channel linear interpolation with a `double` phase accumulator. Handles both the fixed rate conversion and the drift correction — one mechanism. |
| `DriftController` | PI controller over buffer fill level → resample ratio correction. §7.5. |
| `Player` | Owns the `AudioContext` and either an `AudioWorkletNode` or a `ScriptProcessorNode`. Presents one interface: `pull(outL, outR, n)`. §7.3. |
| `Meter` | Peak-follows the played output; drives the on-page level bars. |
| `Ui` | Listen button, volume, buffer slider, status line, diagnostics panel. |
| `Stats` | Aggregates counters and posts the `stat` frame every 2 s. |

### 7.2 The ring buffer in the page

Plain `Float32Array`, one per channel, power-of-two length (`1 << 18` = 262 144 frames ≈ 5.5 s at 48 kHz — far more than needed, but it costs 2 MB and removes any chance of an accidental overflow). Read and write indices are plain `Number`s; `SharedArrayBuffer` is unavailable on `http://` and unnecessary, because with `ScriptProcessorNode` both the network callback and the audio callback run on the **main thread** — there is no cross-thread hazard at all.

In the AudioWorklet path (secure contexts only) the ring is transferred to the worklet via `processorOptions` at construction — **never** via `port.postMessage`, per the WebKit bugs catalogued in doc 03 §2.2 (237144 fixed, 220038 separately broken). In v1 that path is used only for `http://localhost` desktop testing, so the risk is contained either way.

### 7.3 The playback path — feature-detected, as the brief requires

```js
async function createPlayer(ctx, pull) {
  const canWorklet = window.isSecureContext && ctx.audioWorklet;
  if (canWorklet) {
    try {
      await ctx.audioWorklet.addModule('/worklet.js');
      const node = new AudioWorkletNode(ctx, 'ppm-player', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { ring: ringDescriptor },
      });
      return { node, path: 'worklet', quantum: 128 };
    } catch (e) { /* fall through — never let this be fatal */ }
  }
  // The guaranteed path on http://<lan-ip>.
  const size = 4096;                        // 85 ms @48k. 2048 if the phone keeps up.
  const node = ctx.createScriptProcessor(size, 0, 2);
  node.onaudioprocess = (e) => {
    pull(e.outputBuffer.getChannelData(0), e.outputBuffer.getChannelData(1), size);
  };
  return { node, path: 'spn', quantum: size };
}
```

**Why `ScriptProcessorNode` is acceptable despite being deprecated:**
- It is not secure-context-gated and no browser has removed it. Removal would break a large fraction of the web-audio web; when it does happen it will be announced years ahead.
- Its callback runs on the main thread, so a busy page *can* glitch it. Mitigation: the page does almost nothing else — the UI redraws at 10 Hz via one `setInterval`, not `requestAnimationFrame`, and the meter is CSS `width` on two divs, not a canvas.
- Buffer size is chosen from `{2048, 4096, 8192}`. Default **4096**. This is ~85 ms at 48 kHz and it is the dominant term in the receiver's latency — which is honest, and which the diagnostics panel displays as a separate line so nobody blames the network for it.

`ctx.sampleRate` is **read, never requested**. Doc 02 §4.1: iOS often gives 44.1 kHz and forcing 48 kHz has long-standing WebKit bugs. Everything downstream uses `ctx.sampleRate` as ground truth.

The `AudioContext` is created **once**, inside the click handler of the LISTEN button, and never recreated (doc 03 §2.5). `ctx.resume()` is called from inside that handler; on iOS the gesture completes on `touchend`, so the listener is bound to `click`, which fires after it.

### 7.4 Jitter buffer

```
ws.onmessage ──▶ [reorder window: 4 packets] ──▶ [ring: target 120 ms] ──▶ [resampler] ──▶ SPN/worklet
```

- **Reorder window: 4 packets.** Late arrivals within the window are inserted at their correct `seq` position; a packet more than 4 behind the write head is discarded and counted as `packetsLost`. On TCP reordering is theoretically impossible — but the window costs nothing and makes the receiver correct if the transport is ever swapped for a UDP-based one.
- **Target fill: user-settable 40–500 ms, default 120 ms.** This is the knob doc 02 §4.4 insists on exposing, and doc 01 §3.7 confirms Wi-Fi needs more headroom than Ethernet. Slider labelled "Lowest latency ⟷ Most stable" with the resulting ms shown.
- **Adaptive:** grow **fast**, shrink **slowly**. On an underrun, immediately add 20 ms to the target (capped at 500). After 20 consecutive clean seconds, reduce by 5 ms toward the user's setting. Growing is done by *inserting* silence once; shrinking is done purely by the resampler ratio, never by dropping samples.
- **Loss concealment.** Raw PCM has no PLC, and a hard zero-fill click is unacceptable in a mix-referencing tool — the user will think it is *their mix* (doc 02 §4.4). Minimum viable, and what v1 ships: on an underrun, cross-fade the last 5 ms of output into the repeat of the previous ~20 ms period, then fade to silence over 20 ms if the drought continues. Cheap, and it turns a click into a soft artefact the user correctly reads as a network problem.
- On `DISCONTINUITY` or a `configEpoch` change: flush, reset the resampler phase, zero the drift integrator, and refill to target before resuming output.

### 7.5 Drift handling

The DAW's interface crystal and the phone's DAC crystal differ by tens of ppm; at 100 ppm that is 4.8 samples/second at 48 kHz, ~0.36 s per hour (doc 02 §4.1). Left alone the buffer monotonically drains or overflows.

**Measure the fill level, never the arrival times.** Fill level is the integral of the rate difference and is therefore a clean, low-noise estimate; arrival times are dominated by Wi-Fi jitter.

```js
// Called once per output block. Deliberately sluggish.
update(fillFrames, targetFrames, blockFrames, ctxRate) {
  const err = (fillFrames - targetFrames) / targetFrames;
  this.integral += err * (blockFrames / ctxRate);
  const kp = 2e-5, ki = 1e-6;
  const correction = kp * err + ki * this.integral;
  this.correction = Math.max(-0.002, Math.min(0.002, correction));   // ±0.2 % ≈ ±3.5 cents
  this.driftPpm = this.correction * 1e6;
}
// The resampler consumes:
//   ratio = (packetSampleRate / ctx.sampleRate) * (1 + correction)
```

**Clamped hard at ±0.2 %.** That is ~3.5 cents — inaudible on program material and an order of magnitude more than the ~0.01 % crystal drift actually requires. The integrator is clamped too (anti-windup), or a long stall winds it up and the recovery pitch-bends audibly.

**If the error stays outside the clamp for more than 5 seconds**, something structural is wrong (a stall, a device change, a sample-rate change we missed). Do **not** pitch-bend back over minutes: resynchronise discontinuously — flush to target, count it as a `resync`, and show it in diagnostics.

### 7.6 UI

Deliberately one screen, no scrolling above the fold, large touch targets, dark by default (studios are dark; also it is 3 a.m.).

```
┌──────────────────────────────────────────┐
│  PhonePostMix                      ● live│   status dot: grey/amber/green
│  192.168.1.50 · 48 kHz · PCM16 · stereo  │
│                                          │
│        ┌──────────────────────┐          │
│        │       LISTEN         │          │   giant; becomes STOP
│        └──────────────────────┘          │
│                                          │
│  ▁▃▅▇▅▃▁  L  ────────────────────────    │   level meter, CSS widths
│  ▁▃▅▇▅▃▁  R  ────────────────────────    │
│                                          │
│  Volume   ●───────────────────           │
│  Buffer   Lowest ──●──────── Stable      │   40 … 500 ms, shows "120 ms"
│                                          │
│  ⚠ Screen lock will stop the audio.      │   only when wakeLock is unavailable
│    Set Auto-Lock to Never.               │
│  ⚠ Use wired headphones — Bluetooth adds │
│    80–220 ms.                            │
│                                          │
│  ▸ Diagnostics                           │   collapsed by default
└──────────────────────────────────────────┘
```

**The two warnings are permanent fixtures, not dismissible toasts.** Doc 03 §1.0 is blunt: AirPods add 80–220 ms, Android BT 200–280 ms — the headphone choice moves the total by more than every engineering decision in this document combined. And on `http://` there is no `wakeLock`, so the screen *will* sleep and iOS *will* suspend the audio (doc 03 §1.2). Telling the user before it happens is the whole difference between "known limitation" and "bug report".

The iOS silent-switch fix ships unconditionally, both halves: `navigator.audioSession.type = 'playback'` behind a feature check, and a looping 1-second silent `<audio>` element started in the same gesture handler (doc 03 §2.6). Web Audio is muted by the hardware ringer switch; `<audio>` element playback is not.

### 7.7 Diagnostics panel

Collapsed by default; one tap opens it. This panel is the support burden reduction and it is worth building properly on day one.

| Line | Source |
|---|---|
| Capability matrix | `Caps` — secure context, worklet, SAB, AudioDecoder, WebTransport, wakeLock, audioSession. Green tick / grey cross each. |
| Audio path | `worklet (128)` or `scriptprocessor (4096)`. |
| Context | `ctx.sampleRate`, `ctx.baseLatency`, `ctx.outputLatency` (both often `undefined` — display "n/a", never compute with them). |
| Stream | source rate, channels, format, packets/s, kbit/s, `configEpoch`. |
| Buffer | current ms / target ms, min & max over the last 10 s. |
| Health | underruns, overruns, resyncs, packets lost, packets reordered, drift ppm. |
| Connection | WebSocket state, reconnect count, seconds connected. |
| Estimated latency | `packetMs + bufferMs + outputBufferMs` with a clear "does not include your headphones" caveat. |
| Copy button | Dumps the whole panel as text to the clipboard for pasting into a GitHub issue. |

---

## 8. Build order — small, testable, committable steps

Fourteen steps. Each is one branch, one merge to `main`, and has a stated verification. Steps 1–9 get you audible sound on the phone; 10–14 make it good. If the day runs out, stopping after step 9 leaves a working, honest v0.

Global convention: branch off `main`, open no PR (single dev), `git merge --no-ff` back into `main`. Conventional Commits.

| # | Branch | Commit message | What it adds | How to verify |
|---:|---|---|---|---|
| 1 | `build/cmake-juce-skeleton` | `build: add CMake skeleton with JUCE 9.0.1 via FetchContent` | Root `CMakeLists.txt`, `CMakePresets.json`, empty `PluginProcessor`/`PluginEditor` that pass audio through, `.gitignore` for `build*/`. | `cmake --preset default && cmake --build --preset default`. Standalone launches and passes audio. Load the VST3 in Ableton Live 12 → it appears and is transparent. First build ~10 min (JUCE clone + compile). |
| 2 | `test/catch2-harness` | `test: add Catch2 v3 harness and processBlock smoke tests` | `tests/CMakeLists.txt`, `tests/Main.cpp` with `ScopedJuceInitialiser_GUI`, `ProcessBlockTests.cpp` (pass-through bit-exactness, finiteness, sample-rate/block-size churn incl. block size 1). | `ctest --test-dir build --output-on-failure` — all green. |
| 3 | `feat/audio-ring-buffer` | `feat(audio): add lock-free SPSC ring buffer between audio and network threads` | `AudioRingBuffer`, wired into `processBlock`, `RingBufferTests.cpp` including a two-thread hammer test. | `ctest`. Then `pluginval --strictness-level 10 --rtcheck enabled --validate …vst3` → **exit 0**. This is the run that proves no allocation or lock crept into `processBlock`. |
| 4 | `feat/sha1-and-base64` | `feat(net): vendor public-domain SHA-1 and add WebSocket accept-key derivation` | `source/net/Sha1.{h,cpp}`, `source/vendor/README.md`, `makeAcceptKey`, `Sha1Tests.cpp` with the RFC 3174 vectors, and the RFC 6455 §1.3 accept-key vector. | `ctest`. The two published test vectors must match exactly. |
| 5 | `feat/http-server` | `feat(net): serve embedded web assets over HTTP from a listener thread` | `ServerThread`, `ClientConnection` (static path only), `HttpRequest`/`HttpResponse`, `NetworkInterfaces`, `juce_add_binary_data` with a placeholder `index.html`. | Enable streaming in the Standalone app. `curl -v http://<lan-ip>:8377/` returns the page. Open it on the phone. Kill the plugin → `stopThread` returns in <300 ms (add a temporary `DBG` of the teardown duration). |
| 6 | `feat/websocket-upgrade` | `feat(net): implement RFC 6455 upgrade, frame parse and frame serialise` | `WebSocket.{h,cpp}` complete: handshake, parse, serialise, masking, ping/pong, close. `WebSocketTests.cpp`. | `ctest`. Then from the browser console on the served page: `new WebSocket('ws://<ip>:8377/ws').onopen = () => console.log('up')` → logs `up`. Test in **both** Safari and Chrome — the `Connection:` header token bug only shows in one. |
| 7 | `feat/wire-protocol` | `feat(stream): add 32-byte packet header and PCM packet writer` | `StreamFormat`, `PacketWriter`, `PacketWriterTests.cpp` asserting each field's byte offset numerically, `docs/protocol.md`. | `ctest`. Hexdump one packet and eyeball it against the table in §4.2. |
| 8 | `feat/streamer-thread` | `feat(stream): add streamer thread with client registry and fan-out` | `StreamerThread`, `ClientRegistry`, per-client bounded queue with drop-oldest, silence-fill on host starvation. | Browser console: log `event.data.byteLength` on binary messages → a steady 2080 bytes at ~94/s. Stop the DAW transport → packets keep arriving with the `SILENCE` flag set. |
| 9 | `feat/receiver-page` | `feat(web): add self-contained receiver page with ScriptProcessor playback` | `web/index.html`, `web/app.js` (Caps, WsClient, PacketParser, simple fixed-size ring, Resampler, Player with SPN path), the LISTEN button, volume. **No jitter adaptation, no drift correction yet.** | **Sound comes out of the phone.** Verify on iPhone Safari and Android Chrome, both over `http://<lan-ip>`. Confirm `Caps.audioWorklet === false` and `path === 'spn'` — this is the §0.1 prediction being tested for real. |
| 10 | `feat/jitter-and-drift` | `feat(web): add jitter buffer, PI drift control and loss concealment` | `JitterBuffer`, `DriftController`, cross-fade concealment, buffer slider, adaptive target. | Play for **20 minutes** without a click and without the buffer creeping. Watch `driftPpm` settle to a small constant. Then deliberately microwave-jam the 2.4 GHz band (or `sudo pfctl` throttle) and confirm it recovers with soft artefacts, not clicks. |
| 11 | `feat/diagnostics-panel` | `feat(web): add diagnostics panel and client stat reporting` | The panel from §7.7, the `stat` text frame, `ready` handshake reply, copy-to-clipboard. | Open the panel on the phone; every field populates. The plugin GUI (step 12) will consume the same data. |
| 12 | `feat/editor-gui` | `feat(gui): add editor with URL, client list, format controls and level meter` | `PhonePostMixEditor`, `ClientListComponent`, `LevelMeter`, `LevelMeterSource`, format/packet-size/latency controls, interface picker, the macOS Local Network hint, the security warning. | Connect two phones; both appear with live stats. Change the format combo → `configEpoch` bumps, both phones rebuild and keep playing. Meter tracks the mix. |
| 13 | `feat/qr-code` | `feat(gui): render a QR code of the receiver URL` | Vendored `qrcodegen.{h,c}`, `QrCodeComponent`, provenance in `source/vendor/README.md`. | Point an iPhone camera at the plugin window → Safari opens the receiver. Same with an Android camera. |
| 14 | `ci/github-actions` + `docs/readme-and-troubleshooting` | `ci: add build/test/pluginval/auval matrix for macOS, Windows and Linux` · `docs: rewrite README and add troubleshooting guide` | `.github/workflows/build.yml` (matrix, sccache, `_deps` cache, `ctest`, `pluginval --strictness-level 10 --rtcheck enabled`, `auval -v aufx Ppm1 Ppmx` on macOS, artefact upload with an OS suffix); README rewrite; `docs/troubleshooting.md`; the four ADRs. | Green CI on all three OSes. A colleague clones the repo on a machine that has never seen it and gets a working plugin in under 10 minutes with no questions. |

**Ordering rationale.** Steps 1–4 are pure C++ with unit tests and no network — fast, safe, and they front-load the one genuinely uncertain dependency question (SHA-1). Step 5 is the first thing that opens a socket and therefore the first place macOS's Local Network prompt can bite; hitting it early is deliberate. Step 9 is the go/no-go: if the phone does not make a sound at step 9, nothing after it matters. Steps 10–13 are quality. Step 14 is what makes it a repo rather than a folder.

**A note on step 5 and macOS 26.** The first time the Standalone app binds a listener, macOS will prompt (or silently deny) local network access, attributed to *PhonePostMix* for the standalone and to *Live* for the plugin. There is no `tccutil reset` for this — it is a Network Extension packet filter, not TCC (doc 01 §4.3). If a denial is cached during development the reliable reset is System Settings → Privacy & Security → Local Network, toggle the app off and on, and relaunch the host. Budget 15 minutes for discovering this the first time.

---

## 9. Tests

### 9.1 Unit tests — `tests/`, Catch2 v3 via `FetchContent`, `catch_discover_tests`

Run with `ctest --test-dir build --output-on-failure`.

| File | Asserts |
|---|---|
| `Sha1Tests.cpp` | The four RFC 3174 vectors, plus the RFC 6455 §1.3 accept-key vector (`dGhlIHNhbXBsZSBub25jZQ==` → `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`). Empty input, exactly-55-byte and exactly-56-byte inputs (the padding boundary — this is where hand-rolled SHA-1 breaks). |
| `WebSocketTests.cpp` | Handshake accepted with a comma-separated `Connection: keep-alive, Upgrade`; rejected with version ≠ 13, missing key, missing `Upgrade`. Frame round-trip for every length class (0, 1, 125, 126, 127, 65535, 65536). Masked-frame unmasking against a hand-computed vector. Unmasked client frame → protocol error. Non-minimal length → protocol error. Control frame >125 bytes → protocol error. Fragmented text frame reassembly. Partial-buffer feeding byte by byte → `needMoreData` until the last byte, then `ok`. |
| `PacketWriterTests.cpp` | Every field at its documented byte offset, read back with explicit `memcpy` + little-endian reassembly (never a struct cast — that is what the test exists to catch). Payload size for all three formats × three packet sizes × mono/stereo. `f32` payload starts at a 4-byte-aligned offset. pcm16 conversion clips at ±1.0 rather than wrapping. pcm24 byte order. |
| `RingBufferTests.cpp` | Write-then-read round-trip is bit-exact. Wraparound across the buffer end. Overflow returns the correct dropped count and does not corrupt. Read of more than available returns a short count. **Two-thread hammer:** a writer pushing random block sizes and a reader pulling fixed sizes for 5 seconds, asserting the output is the input sequence with only whole blocks missing. Run this one under TSan. |
| `HttpTests.cpp` | Request-line parse for `GET / HTTP/1.1`, query strings, unknown methods. Case-insensitive header lookup. `headerContainsToken` with lists and odd whitespace. Header block split across two reads. Oversized header → 431. Missing `\r\n\r\n` → keep waiting, never over-read. |
| `ProcessBlockTests.cpp` | **Pass-through is bit-exact** (the single most important assertion in the project — feed noise, assert `memcmp` equality of in and out). No NaN/Inf. Sample-rate × block-size churn: `{44100, 48000, 88200, 96000, 192000} × {1, 16, 64, 128, 480, 512, 1024, 2048}`, `prepareToPlay`/`processBlock`/`releaseResources` each time. `getLatencySamples() == 0`. `getTailLengthSeconds() == 0`. State round-trips through `getStateInformation`/`setStateInformation`. Constructing and destroying 200 processors in a loop opens no sockets and leaks no threads. |
| `NetworkInterfaceTests.cpp` | The ranking function, fed synthetic address lists: prefers `192.168.x` over `10.211.55.x` (Parallels), skips loopback and link-local, returns empty gracefully when there is no LAN. |

### 9.2 Real-time safety

Three independent nets, because each catches things the others miss:

1. **`pluginval --strictness-level 10 --rtcheck enabled`** — the primary gate, in CI. `--rtcheck` flags allocations and locks inside `processBlock`, and at strictness >5 pluginval adds `-stress 20`: 20 simulated seconds of multi-threaded audio I/O, which is specifically the test that finds FIFO races (doc 01 §5.3).
2. **`[[clang::nonblocking]]` on `processBlock`** plus RealtimeSanitizer (`-fsanitize=realtime`, Clang 20+). Compiler-enforced, and a genuinely good fit for a plugin that also runs a network stack (doc 03 §4.3). Gate it behind `PPM_RTSAN=ON` so it does not break contributors on older toolchains.
3. **TSan and ASan jobs on Linux** over the test binary, especially the ring-buffer hammer test.

### 9.3 Running the plugin headlessly

| Tool | Command | What it is for |
|---|---|---|
| **Catch2 tests** | `ctest --test-dir build --output-on-failure` | Everything above. No host, no audio device. |
| **pluginval** (VST3) | `pluginval --strictness-level 10 --validate-in-process --skip-gui-tests --rtcheck enabled --timeout-ms 60000 --output-dir logs --validate build/PhonePostMix_artefacts/Release/VST3/PhonePostMix.vst3` | Exit code 0 = pass. The CI gate. `--sample-rates 44100,48000,96000 --block-sizes 1,64,512,4096` for the churn. |
| **pluginval** (AU) | same, against `.../AU/PhonePostMix.component` | AU has stricter channel-layout negotiation than VST3. |
| **auval** | `cp -R …/PhonePostMix.component ~/Library/Audio/Plug-Ins/Components/ && killall -9 AudioComponentRegistrar; auval -v aufx Ppm1 Ppmx` | Mandatory for AU: Logic refuses a plugin that fails. `auval` will not look in the build directory, and the registrar caches — both are why the `cp` and `killall` are part of the command, not optional preamble. |
| **JUCE AudioPluginHost** | Built from the fetched JUCE at `build/_deps/juce-src/extras/AudioPluginHost` | The inner dev loop. Launches instantly, breakpoints in `processBlock` work. |
| **Standalone target** | `build/PhonePostMix_artefacts/Release/Standalone/PhonePostMix.app` | Demo and manual testing with no DAW. Feed it system audio or a file player. |
| **`scripts/dev-serve.py`** | `python3 scripts/dev-serve.py` | Serves `web/` on `http://localhost:8377` and streams a synthesised 440 Hz + pink-noise PCM stream over `/ws` using the real §4 protocol. **Lets the entire receiver be developed and debugged with no C++ build at all** — and, because `localhost` *is* a secure context, it is also how the AudioWorklet path gets exercised. Python stdlib only (`http.server` + a hand-rolled WS frame writer, ~120 lines). |

### 9.4 Manual test matrix before calling v1 done

| Check | Pass condition |
|---|---|
| Ableton Live 12, VST3, master bus | Audio unaffected; phone hears the mix. |
| Ableton Live 12, AU | Same. |
| Ableton transport stopped | Phone hears silence, stays connected, plugin says "Host not playing". |
| Ableton offline bounce (Export Audio) | Streaming pauses; GUI says so; no packet flood. Test at a *different* export sample rate too. |
| Two phones at once | Both play. Killing one does not disturb the other. |
| Phone leaves Wi-Fi and returns | Reconnects automatically within ~5 s. |
| 20-minute continuous listen | No clicks; buffer stable; `driftPpm` constant. |
| Plugin removed while streaming | All sockets closed, threads joined, no crash, no hang. |
| 200× instantiate/destroy (pluginval) | Exit 0. No FD leak (`lsof -p` before and after). |
| iPhone Safari + Android Chrome | Both work on `http://<lan-ip>`; both report `path: "spn"`. |
| iPhone with the ringer switch on silent | Audio still plays (the `<audio>` + `audioSession` fix works). |

---

## 10. Explicitly out of scope for v1

Recorded here so it is a decision, not an omission. Each belongs in `docs/architecture.md` under "known limitations" too.

- **TLS / `wss://` / HTTPS.** Therefore no AudioWorklet on a LAN IP, no Wake Lock, no `crypto.subtle`. This is *the* v2 headline: a DNS name resolving to the private IP plus a DNS-01 wildcard certificate (the Plex pattern, doc 02 §5.3). It needs an OpenSSL-class dependency and a DNS zone; neither fits today.
- **Opus, FLAC, any codec.** PCM only. Adding libopus is a v2 dependency decision.
- **WebRTC / WebTransport / UDP.** TCP WebSocket only. Head-of-line blocking on a bad Wi-Fi link is a real, accepted cost, mitigated by the jitter buffer and honest diagnostics.
- **mDNS/Bonjour discovery.** QR + typed URL. Doc 02 §3.2 shows `.local` silently fails on Android Chrome, so discovery built on it would be a 50 % solution that generates support tickets from exactly half of users.
- **Authentication and encryption.** The pairing token is a mistake-preventer, not a security boundary. Stated loudly in the README and in the GUI.
- **Internet / relay mode.** LAN only.
- **Multichannel above stereo.** `isBusesLayoutSupported` rejects it cleanly rather than half-supporting it.
- **AAX, CLAP, LV2.** AAX needs Avid + PACE. CLAP needs `clap-juce-extensions` (JUCE 9.0.1 does not ship native CLAP authoring despite the roadmap — doc 03 §3.1). LV2 is a one-line `FORMATS` addition whenever a Linux user asks.
- **Code signing and notarisation.** CI signs only on tags, gated on the secrets existing, so forks and PRs still build. Dev builds are unsigned; the README documents `xattr -dr com.apple.quarantine`.
- **A native phone app.** The only thing it would buy is background/screen-off audio, which is genuinely impossible from a web page on iOS. Document it as a limitation; do not build an app for it.
