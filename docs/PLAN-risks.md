# PhonePostMix — Risk & Verification Report

**Role of this document:** adversarial review of the proposed v1 stack. Not a design doc, not a cheerleading doc.
**Date:** 2026-08-27. **Reviewer stance:** assume the plan is wrong until a source or an experiment says otherwise.

**Plan under review (as stated):**

> JUCE 9 (CMake FetchContent) → VST3 + AU + Standalone; plugin-embedded HTTP server serving one self-contained HTML page; WebSocket server on `juce::StreamingSocket` carrying binary PCM over **plain `ws://`** on the LAN; browser plays via **AudioWorklet where available and ScriptProcessorNode otherwise**; audio thread → `juce::AbstractFifo` → streaming thread. No OpenSSL, no libwebrtc, no npm.

---

## 0. Headline verdict

**One clause of the plan is factually wrong and it is load-bearing.**

> "the browser page plays via AudioWorklet where available and ScriptProcessorNode otherwise"

On a page served from `http://192.168.x.x:PORT`, **AudioWorklet is never available**. Not on iOS Safari, not on Android Chrome, not on any browser, not in any version, now or later. It is not a support-matrix question — it is gated on secure context by specification, and `BaseAudioContext.audioWorklet` is simply `undefined` on an insecure origin. RFC 1918 addresses are explicitly *not* trustworthy origins; only `127.0.0.0/8`, `::1`, `localhost` and `*.localhost` are exempt, and the phone is not the loopback device.

So the "where available / otherwise" branch is a fiction. In the deployed product the `otherwise` branch is taken **100% of the time, on every device, permanently.** You would be shipping a ScriptProcessorNode product while believing you had shipped an AudioWorklet product with a safety net.

The good news, verified: **ScriptProcessorNode is not secure-context gated and still works.** caniuse's current table shows ✅ on **iOS Safari 26.6 and 27-TP** and **Chrome for Android 151**. It is deprecated with no announced removal date in any browser. So the product *can* ship on the stated stack — but as an SPN-only product, with SPN's latency floor and SPN's main-thread fragility as permanent properties, not as temporary fallbacks.

The second casualty of plain HTTP is worse than the first: **`navigator.wakeLock` is secure-context gated too.** On iOS, Web Audio is suspended the moment the screen locks. Without Wake Lock, the phone auto-locks after 30 s–2 min and the stream dies, every time, for every user, unless they have manually set Auto-Lock to Never. That is a product-defining defect, not a papercut.

**Also verified, and this one is good news that de-risks the biggest OS unknown:** on macOS, **listening for and accepting incoming TCP connections does *not* require Local Network permission.** Apple's own Local Network Privacy FAQ lists "Listening for and accepting incoming TCP connections — **no**" and "Receiving an incoming UDP unicast — **no**", against "Making an outgoing TCP connection — yes", "Sending a UDP unicast/multicast/broadcast — yes", "Resolving `.local` names — yes", "All Bonjour operations — yes". A pure inbound TCP server inside a plugin is therefore the *one* network shape that dodges macOS 15+/26 Local Network privacy entirely. The plan accidentally picked the right shape. Do not add Bonjour/mDNS discovery or a UDP broadcast beacon to v1 — those are exactly what re-arms the permission prompt you just avoided.

**Recommendation in one line:** ship v1 on ScriptProcessorNode over `http://` + `ws://` and be honest about it, but treat **HTTPS as the highest-value item in the backlog immediately after v1** — because it buys AudioWorklet, Wake Lock, WebCrypto and `SharedArrayBuffer` in a single move, and the "no OpenSSL" constraint is the only thing standing between you and all four. Revisit that constraint; vendoring mbedTLS through `FetchContent` is neither npm nor an OpenSSL build.

---

## 1. Ranked risk register

Ranked by *expected damage × probability*, not by how interesting they are.

---

### R1 — 🔴 CRITICAL — AudioWorklet is unavailable on the LAN origin, permanently

| | |
|---|---|
| **Symptom** | `ctx.audioWorklet` is `undefined`. `await ctx.audioWorklet.addModule(...)` throws `TypeError: Cannot read properties of undefined`. On every phone, always. Your "primary" playback path never executes once in production. |
| **Why** | AudioWorklet is `[SecureContext]` in the Web Audio spec. MDN: *"Secure context: This feature is available only in secure contexts (HTTPS)."* Per W3C Secure Contexts §3.1 the only non-HTTPS trustworthy origins are `127.0.0.0/8`, `::1/128`, `localhost`, `*.localhost`, `file:`, and UA schemes. `192.168.0.0/16`, `10.0.0.0/8`, `172.16.0.0/12` are conspicuously absent. |
| **Detect early** | 30 seconds, before any C++ is written. Serve one HTML file over plain HTTP from your Mac, open the LAN URL on the phone, print `window.isSecureContext` and `'audioWorklet' in AudioContext.prototype`. See E0. |
| **Mitigation** | Three real options, in order of honesty: **(a)** Accept SPN-only for v1, delete every mention of AudioWorklet from the plan and the code so nobody maintains a dead branch, and write the latency floor into the README. **(b)** Solve HTTPS — a public DNS name that resolves to the private IP plus a DNS-01-issued wildcard cert (the Plex / `tlsmy.net` pattern), which needs TLS in the plugin and therefore breaks the "no OpenSSL" rule (mbedTLS via `FetchContent` is the cheapest way to keep the spirit of it). **(c)** Self-signed cert + user-accepts-warning: works on Android Chrome, is a five-step Certificate Trust Settings ordeal on iOS. Do not build on (c). |
| **Do not** | Do not write the dual-path code "just in case". A branch that never executes on any real device is not a fallback, it is dead code that will rot and lie to you in code review. |

---

### R2 — 🔴 CRITICAL — no Wake Lock on an insecure origin ⇒ the phone screen locks ⇒ iOS kills the audio

| | |
|---|---|
| **Symptom** | User taps Listen, puts the phone on the desk, gets ~60 s of audio, then silence. Returning to the tab may or may not resume. Reads as "your plugin is unstable". |
| **Why** | Two facts compounding. (1) `navigator.wakeLock` is secure-context gated — same gate as R1 — so you cannot keep the screen on. (2) iOS Safari suspends `AudioContext` and WebRTC when the screen locks or Safari backgrounds; there is no page-side workaround and no background-audio entitlement for web content. `<audio>`/`<video>` element playback survives; Web Audio does not. |
| **Detect early** | E2: leave the page playing, don't touch the phone, watch the clock. Also `'wakeLock' in navigator` on the LAN origin. |
| **Mitigation** | v1: a prominent in-page instruction — *"Settings → Display & Brightness → Auto-Lock → Never while you're monitoring"* — plus a visible "audio suspended" state in the page (listen for `ctx.onstatechange`) so the user sees *why* it stopped instead of blaming the mix. Test whether a looping silent `<audio>` element keeps the session alive across a lock (E2b) — it sometimes has; do not assume it. Real fix is HTTPS (R1 mitigation b). |
| **Product framing** | "Phone in your hand or on the desk, screen on" is the honest use case. Say it in the README. Do not let a user discover it in a mix session. |

---

### R3 — 🟠 HIGH — the iOS ringer/silent switch mutes Web Audio

| | |
|---|---|
| **Symptom** | iPhone user taps Listen, meters move, everything looks connected, no sound. Classic false "it's broken" support ticket. |
| **Why** | Documented WebKit behaviour (bug 237322): the hardware silent switch mutes Web Audio output but not `<audio>`/`<video>` elements. Safari's default audio session type behaves as `ambient`. |
| **Detect early** | E2. Flip the switch. Two minutes. |
| **Mitigation** | Set `navigator.audioSession.type = 'playback'` inside the Listen handler — Safari-only, feature-detect. MDN does **not** flag AudioSession as secure-context gated, so it should survive plain HTTP, **but this is unverified for insecure origins and must be tested in E0**; if it turns out to be gated, R1 gets more expensive. Belt-and-braces: `feross/unmute-ios-audio` (looping silent `<audio>`). Also render a "🔇 your phone may be on silent" hint next to the level meter when the meter is moving. |

---

### R4 — 🟠 HIGH — ScriptProcessorNode is now your only playback path, with all of SPN's properties

| | |
|---|---|
| **Symptom** | Clicks and dropouts correlated with page activity — scrolling, the level meter repainting, GC. Latency you cannot reduce below the buffer size. Console spam: *"The ScriptProcessorNode is deprecated. Use AudioWorkletNode instead."* |
| **Why** | `onaudioprocess` fires on the **main thread**, in competition with layout, paint, GC and your own JS. AudioWorklet exists precisely because this design cannot be made reliable. Legal buffer sizes are 256/512/1024/2048/4096/8192/16384 frames; on mobile, anything below 2048 will glitch under load. 2048 @ 48 kHz = **42.7 ms** of output buffer; 4096 = **85 ms**. That is your floor, on top of Wi-Fi, jitter buffer, and the phone's own output latency. |
| **Detect early** | E1. Play a stream through SPN at 1024 / 2048 / 4096 on a real mid-range Android while scrolling the page. Count dropouts per minute. |
| **Mitigation** | Pick the buffer size by measurement, default to 4096 on first run and offer a "lower latency (may glitch)" setting. Keep the page's main thread almost empty: no canvas meter at 60 fps, no per-packet DOM writes, throttle all UI to ~10 Hz, do the WebSocket `onmessage` → ring write and nothing else. Deep-copy incoming `ArrayBuffer`s into a preallocated `Float32Array` ring — do not allocate per packet, GC pauses land directly in your audio callback. |
| **Removal risk** | Real but not imminent: deprecated for a decade, still ✅ in iOS Safari 26.6/27-TP and Chrome Android 151, no vendor has posted an intent-to-remove with a date. Treat as a 2–4 year runway, not a 2026 emergency. It is still a reason not to build a business on it. |

---

### R5 — 🟢 LOW (verified — good news) — macOS Local Network privacy does *not* block a listening socket

| | |
|---|---|
| **Feared symptom** | "Ableton Live would like to find devices on your local network" prompt attributed to the DAW; user denies it once; the plugin is silently dead forever with no error. |
| **Reality** | Apple's Local Network Privacy FAQ (developer forums thread 663874, and TN3179) enumerates it explicitly. **Requires permission:** outgoing TCP connect, UDP unicast/multicast/broadcast *send*, connecting a UDP socket, receiving incoming UDP multicast/broadcast, resolving `.local` names, **all Bonjour operations**. **Does not require permission:** *"Listening for and accepting incoming TCP connections"* and *"Receiving an incoming UDP unicast."* A pure inbound TCP server is in the clear. |
| **What still bites** | (a) The **macOS Application Firewall** is a separate mechanism and may prompt to allow incoming connections — attributed to the **host app** (Ableton/Logic), not to your plugin, so the dialog will confuse users. (b) The instant you add Bonjour/mDNS discovery or a UDP broadcast beacon, you re-enter the permission regime and inherit all of its known pathologies: it is enforced by a Network Extension packet filter, **not TCC**, so `tccutil reset` cannot clear it for testing; the prompt names the DAW, not you; there are reports of grants not surviving reboot. |
| **Mitigation** | **Keep v1 inbound-TCP-only.** Discovery is a QR code rendered by the plugin editor containing the URL — no mDNS, no broadcast, no entitlement, no prompt. If a user reports "phone can't reach it", the first diagnostic is the Application Firewall and AP client isolation, not Local Network privacy. |
| **Do not cut** | This is the single best architectural decision in the plan. Protect it. |

---

### R6 — 🟠 HIGH — `StreamingSocket` blocking `accept()` deadlocks plugin teardown

This is the one that hangs the DAW, and the mechanism is specific and verifiable in JUCE's source (`modules/juce_core/network/juce_Socket.cpp`).

| | |
|---|---|
| **Symptom** | Closing a project, removing the plugin, or a validator's destroy step hangs for seconds or forever. `auval` reports the plugin as hung and fails it. Ableton beachballs on project close. |
| **Why** | `StreamingSocket::waitForNextConnection()` is a bare blocking `accept()`. There is **no timeout parameter**. JUCE's `close()` "solves" this in `SocketHelpers::closeSocket` with a hack: if the socket is a listener, it constructs a temporary `StreamingSocket` and does `temp.connect(IPAddress::local().toString(), portNumber, 1000)` — i.e. it connects to **127.0.0.1** on your port to shake `accept()` loose. **If you bound the listener to a specific LAN interface address rather than `INADDR_ANY`, that loopback connect cannot reach your listener.** It times out after 1000 ms, `accept()` is never interrupted, your thread stays blocked forever, `stopThread(n)` times out, and the destructor either hangs or kills the thread mid-flight. |
| **Second defect in the same path** | The self-connect *is* accepted. `waitForNextConnection()` does `if (newSocket >= 0 && connected) return new StreamingSocket(...)`, but `connected` was already set `false` before the self-connect, so it returns `nullptr` **and never closes `newSocket`**. That is a leaked file descriptor on every listener shutdown. Under `pluginval`/`auval`, which instantiate and destroy hundreds of times, that is hundreds of leaked fds and eventual `EMFILE`. |
| **Detect early** | E5: a loop that creates and destroys the processor 500 times, with `lsof -p $(pgrep -f pluginval) | wc -l` sampled throughout. Also just run `auval` — if it says "hung", this is why. |
| **Mitigation — do this, it is not optional** | **(1)** Always `createListener(port, {})` / bind `0.0.0.0`, never a specific interface IP, so JUCE's loopback self-connect can reach it. **(2)** Better: never block in `accept()` at all. Poll: `while (!threadShouldExit()) { if (listener.waitUntilReady(true, 200) == 1) { std::unique_ptr<StreamingSocket> c(listener.waitForNextConnection()); ... } }`. `waitUntilReady` is `select`-based and returns `1` when a connection is pending, `0` on timeout, `-1` on error; it works on a listening socket because `createListener` sets `connected = true`. This makes shutdown a 200 ms worst case with no reliance on the self-connect hack. **(3)** Teardown order: `signalThreadShouldExit()` → `listener.close()` → `stopThread(2000)` → assert it returned true. Never call `stopThread` while the thread can still enter a blocking call. **(4)** Own every accepted socket in a `std::unique_ptr` immediately — `waitForNextConnection` returns a raw owning pointer. |

---

### R7 — 🟠 HIGH — port binding vs. multiple instances, plugin scanning, and DAW rescans

| | |
|---|---|
| **Symptom** | Second instance of the plugin silently doesn't work. Or: after a DAW crash and restart, the port is unavailable for ~60 s. Or: a plugin scan opens 200 listeners at once. Or (Windows) the firewall prompt fires during scanning. |
| **Why** | `StreamingSocket::createListener` calls `SocketHelpers::makeReusable` → `setsockopt(SO_REUSEADDR, 1)` — **but only on non-Windows**, with an explicit source comment that on Windows this "produces behaviour different to posix". On macOS/Linux, `SO_REUSEADDR` for TCP lets you rebind a port stuck in `TIME_WAIT`; it does **not** let two live listeners share a port (that needs `SO_REUSEPORT`, which JUCE only sets for `DatagramSocket`). So instance #2 binding the same port fails. On Windows there is no `SO_REUSEADDR` at all, so post-crash `TIME_WAIT` rebind fails too. Separately, hosts instantiate the plugin many times during scanning and validation. |
| **Detect early** | E4: load two instances in AudioPluginHost, both with streaming on. Then kill the host mid-stream and restart within 30 s. |
| **Mitigation** | **(a) Lazily create the listener** — only when the user presses Start, never in the constructor or `prepareToPlay`. This alone fixes scanning, validation, and most multi-instance pain. **(b) Try a small port range** (e.g. 17520–17540) and bind the first that succeeds; put the **actual** bound port in the UI and in the QR code, never a hardcoded constant in the docs. **(c)** Consider a process-wide shared server: one static, refcounted `StreamServer` per host process, with instances registering as sources — this is the correct design for a DAW with a plugin on every bus, but it is a v1.1 refactor, not a v1 requirement. **(d)** Handle bind failure as a first-class UI state ("port in use — retrying / choose another"), not a silent no-op. |

---

### R8 — 🟠 HIGH — blocking `write()` stalls the streaming thread and overflows the ring

| | |
|---|---|
| **Symptom** | The phone's Wi-Fi degrades; a second later the plugin's overflow counter climbs; audio on the phone stutters and then latency grows without bound. |
| **Why** | `StreamingSocket::write()` blocks. JUCE's own docs say it "will block unless you have checked the socket is ready for writing before calling it." Once the kernel send buffer fills — which is exactly what happens when TCP is retransmitting on a bad Wi-Fi link — your streaming thread parks inside `write()`, stops draining the FIFO, and `processBlock` starts dropping. TCP head-of-line blocking then means a single loss stalls the whole stream, with retransmit spikes of 100+ ms. |
| **Detect early** | Instrument it: count FIFO overflows and log the max time spent inside `write()`. Then walk out of Wi-Fi range with the phone. |
| **Mitigation** | Always `if (sock.waitUntilReady(false, 20) != 1) { dropOldestFrames(); continue; }` before `write()`. **When you fall behind, drop the oldest audio, never the newest** — this is a live monitor, not a file transfer; a listener wants "now", not "everything". Cap the send-side queue in *milliseconds of audio*, not bytes. Expose a "packets dropped" number in the plugin UI so the user can see the network is the problem. `TCP_NODELAY` is already set for you: `resetSocketOptions` sets it on every non-datagram socket. |

---

### R9 — 🟡 MEDIUM — browser-side WebSocket backpressure (`bufferedAmount`) and unbounded latency growth

| | |
|---|---|
| **Symptom** | The phone falls minutes behind the DAW and never catches up. Memory climbs. |
| **Why** | A WebSocket receiver has no flow control you can see; the JS ring buffer just fills. `bufferedAmount` only tells you about *outgoing* data — useful for your control channel, useless for the audio direction. TCP will happily buffer for you and hand you a growing backlog. |
| **Mitigation** | The receive ring must be a **fixed-size** preallocated ring with an explicit overflow policy: when fill exceeds `2 × target`, **discard down to target** and log a resync. Display the current buffer fill (in ms) in the page — it is the single most useful diagnostic you can give a user and it costs nothing. Send a monotonic `sampleTime` in every packet header so the receiver can detect a gap versus a stall versus a reorder. |

---

### R10 — 🟡 MEDIUM — real-time safety traps on the JUCE side

Point by point against what was asked:

- **Does `AbstractFifo` allocate?** No. Verified: *"It doesn't actually hold any data itself, but your FIFO class can use one of these to manage its position and status."* It manages indices only. **But** `setTotalSize()` is explicitly documented as **not thread-safe** — *"don't call it if there's any danger that it might overlap with a call to any other method"* — and the `AudioBuffer` you pair it with **does** allocate in `setSize()`. So: size everything in `prepareToPlay`, never in `processBlock`, and never resize while the streaming thread is running (stop the thread, resize, restart).
- **SRSW only.** One writer, one reader, full stop. Two `processBlock` callers (multi-bus, or a host calling from two threads) silently corrupts it — the known symptom is `EXC_BAD_ACCESS` in the field. If you ever need more, use one FIFO per producer.
- **`prepareToPlay` vs `processBlock` ordering.** `prepareToPlay` is not called on the audio thread. Anything written there and read in `processBlock` is a data race unless it is atomic or you trust the host's ordering — and hosts have been observed calling `prepareToPlay` repeatedly and inconsistently (Ableton in particular). Make every field the audio thread reads `std::atomic`, and guard `processBlock` with a `prepared` flag so a call that lands before/during preparation is a no-op rather than a crash.
- **`releaseResources` / suspension.** Do not tear the network server down in `releaseResources` — hosts call it far more often than you expect and the socket teardown is expensive and deadlock-prone (R6). Decouple: the server's lifetime is tied to the user's Start/Stop, not to the host's prepare/release cycle.
- **Plugin suspension / transport stopped.** **Logic does not call `processBlock` when the transport is stopped or the region is silent**, and most hosts stop calling `processBlock` entirely under host-bypass rather than calling `processBlockBypassed`. If your keepalive lives in `processBlock`, the connection dies whenever the user stops the transport. **Send keepalives and silence frames from a timer/network thread, never from the audio thread**, and distinguish "streaming" from "connected, no audio from host" in the UI.
- **Denormals.** `juce::ScopedNoDenormals` at the top of `processBlock` regardless — it is free and a pure tap can still be handed denormals by an upstream plugin.
- **Bounce / offline rendering.** See R11.
- **Sample-rate / block-size changes.** See R12.

**Detect early:** `pluginval --rtcheck enabled` (verified present in `Source/CommandLine.cpp`; **macOS and Linux only**). Also `[[clang::nonblocking]]` on `processBlock` with RealtimeSanitizer under Clang 20+, and a TSan job hammering the FIFO from two threads.

---

### R11 — 🟠 HIGH — offline bounce floods the stream

| | |
|---|---|
| **Symptom** | User bounces a 4-minute track; the plugin tries to transmit 4 minutes of audio in 6 seconds; the FIFO overflows instantly, the phone's buffer explodes or the connection drops, and in the worst case the bounce itself is slowed by your blocking `write()`. |
| **Why** | In offline render `processBlock` is called as fast as the CPU allows. |
| **Detect early** | Bounce a track in Reaper's trial and in Ableton with the plugin streaming. |
| **Mitigation** | `isNonRealtime()` is necessary but **not sufficient** — it is documented as unreliable until after `prepareToPlay`, it can change on each `prepareToPlay`, Studio One has been reported to leave it `false` during offline render, and Ableton can bounce at a different sample rate than the project. **Add a wall-clock rate limiter**: if more than ~1.5× realtime worth of samples arrive in a wall-second, you are rendering regardless of what the host claims — stop transmitting and show "offline render — stream paused". |

---

### R12 — 🟡 MEDIUM — sample-rate and block-size changes mid-stream

| | |
|---|---|
| **Symptom** | Pitch-shifted or chipmunked audio on the phone after the user changes the interface buffer or the project rate. Or a hard crash if the ring was sized for 512 and the host hands you 8192. |
| **Why** | `prepareToPlay(sr, maxBlock)` gives a **maximum**, not a guarantee, and can be called at any time with new values. Validators deliberately call `processBlock` at 1 sample and at 8192. |
| **Mitigation** | Put `sampleRate`, `channels` and `frames` **in every packet header**, not just in a handshake — packets get lost and late-joining receivers exist. On any change: flush the FIFO, reset any interpolator, and send an explicit "format change" packet so the receiver resets its ring and its `AudioContext` mapping rather than reinterpreting bytes at the old rate. Size the ring for `maxBlock + networkFrame` at minimum and realistically 200–500 ms (≈192 kB at 48 k stereo float — free). Never assume `buffer.getNumSamples()` equals what you were told. |

---

### R13 — 🟡 MEDIUM — clock drift between the DAW and the phone

| | |
|---|---|
| **Symptom** | Works beautifully for two minutes, then a click every few minutes, then either growing latency or repeated underruns. Reported as "it's unstable". |
| **Why** | Two free-running crystals at ±50 ppm each. 100 ppm ≈ 4.8 samples/second at 48 kHz ≈ 0.36 s accumulated per hour. The receive buffer monotonically fills or drains. Compounded by the fact that **`AudioContext.sampleRate` is whatever the device gives you** — often 44100 on iPhones, 48000 on Android — so you are resampling anyway. |
| **Mitigation for v1** | Do **not** build a DLL + ASRC in v1. Measure `fillLevel = writePtr - readPtr`, low-pass it hard (10–60 s time constant), and when it crosses a threshold, **drop or duplicate one frame at a zero crossing**. It is audible occasionally; it is ten lines of code; it converts "breaks after two minutes" into "works for a session". Real ASRC with a PI-controlled resampler clamped to ±0.2 % is v1.1. |
| **Detect early** | E8: a 20-minute soak with the buffer-fill number logged once a second. If the plot has a slope, you have measured your drift in ppm; if it has steps, you have found a different bug. |

---

### R14 — 🟡 MEDIUM — `auval` and `pluginval` versus a plugin that opens sockets and spawns threads

| | |
|---|---|
| **Symptom** | `auval` fails or reports the plugin hung. `pluginval` crashes at strictness 8+. Logic then refuses to load the AU at all — `auval` is a hard gate for AU. |
| **Why** | Validators instantiate and destroy the plugin **hundreds of times in rapid succession**, off the main thread and in parallel; they call `processBlock` at absurd block sizes; and they do repeated state save/restore. A plugin that opens a socket and starts a thread per instance exhausts file descriptors (aggravated by the fd leak in R6) or hangs in the destructor. |
| **Mitigation** | **(a)** Lazy: no socket, no thread until the user presses Start. A freshly constructed processor must own zero OS resources. **(b)** Deterministic destruction: `signalThreadShouldExit` → `close()` → `stopThread(2000)`, and `jassert` the join succeeded. **(c)** Persist "streaming enabled" as a setting but **require an explicit user action to reconnect on project load** — otherwise a saved project with streaming on means every validator instantiation opens a socket. **(d)** Reject unsupported layouts crisply in `isBusesLayoutSupported` (in == out, mono or stereo) rather than half-supporting them; AU's negotiation is stricter than VST3's and `auval` probes widely. **(e)** `setLatencySamples(0)` and `getTailLengthSeconds() == 0` — the stream's latency is on the phone, not in the DAW's signal path; reporting nonzero latency would make the host PDC-delay everything else for nothing. |
| **CI note — correct this before you copy it** | `--rtcheck [disabled\|enabled\|relaxed]` is confirmed present, **macOS/Linux only**. But `--validate-in-process`, which appears in the research docs' proposed CI YAML, is **not in the recognised-option table in current pluginval `develop` (`Source/CommandLine.cpp`)**, while their own CI doc still shows it — and pluginval's user guide separately warns that in-process validation means *"a failed validation will bring down the app"*. Pin a pluginval release tag and run `--help` against that exact binary before trusting any flag list, including this document's. |

---

### R15 — 🟠 HIGH — an unauthenticated audio server on `0.0.0.0`

| | |
|---|---|
| **Symptom** | Anyone on the same Wi-Fi — coworking space, studio guest network, hotel, a compromised IoT device — can open `http://<your-ip>:PORT` and listen to an unreleased master in real time. Worse: you have put an HTTP parser you wrote inside the DAW's process, reachable by anyone on the LAN. A parser bug is now arbitrary code execution inside Logic. |
| **Why** | The plan has no auth. `0.0.0.0` means every interface, including any VPN or bridge adapter. |
| **Mitigation — v1, non-negotiable** | **(a)** A per-session random token (≥128 bits) generated on Start, embedded in the QR URL fragment, and required by the WebSocket handshake before a single audio byte is sent. Reject and close unauthenticated connections immediately. **(b)** Bound the HTTP surface brutally: serve exactly one route and one embedded asset blob, hard-cap request line and header sizes, hard-cap total header bytes, drop anything malformed without parsing further, cap concurrent connections (e.g. 8). Do not implement ranges, keep-alive pipelining, chunked encoding, or path traversal-capable file serving — the page is compiled in via `juce_add_binary_data`, so there is no filesystem path to traverse. Keep it that way. **(c)** Fuzz it: `echo` a few thousand random bytes at the port and confirm the plugin survives. |
| **Honest limitation** | Without TLS the audio is in the clear on the wire. Say so in the README. This is another entry on the "HTTPS pays for itself" ledger. |

---

### R16 — 🟡 MEDIUM — browser Local Network Access gating on the phone side

| | |
|---|---|
| **Symptom** | Phone loads the page but the WebSocket never connects; or the page itself refuses to load; no useful error. |
| **Why / current state** | **Chrome:** the Local Network Access permission prompt launched in Chrome 142. It gates requests *from* the public network *to* local/loopback destinations, **including top-level navigations to LAN IPs from public pages**. Requests from a page already served from a LAN IP to local addresses are **not currently gated** — and **WebSockets, WebTransport and WebRTC are explicitly not yet gated**, with Chrome stating it plans to ship LNA for them "soon". So today the QR → LAN page → `ws://` same-origin flow is clear, **but two of the three legs are on a published roadmap to be gated.** **Safari/iOS:** murkier. Apple's Local Network permission is app-scoped (Settings → Privacy → Local Network); a WKWebView inside an app *is* the app for this purpose, but Safari itself is inconsistently reported. There are live developer-forum reports of "local network request blocked in Safari but working in Chrome" that Apple's DTS answered by pointing at TN3179 rather than by denying the behaviour. |
| **Detect early** | E0 covers it for free — if the phone can't even load the page, you've found it in the first two minutes. Re-test on every Chrome and iOS release; this is a moving target and it moves against you. |
| **Mitigation** | Serve the page **from** the LAN device and keep everything same-origin. Never build the "cloud-hosted HTTPS page reaches into your LAN" design — it is being actively deprecated across browsers and will also hit mixed-content blocking. Detect a failed WebSocket connect within 3 s and show a concrete diagnostic panel naming the three real causes: browser local-network permission, AP client isolation, wrong subnet. |

---

### R17 — 🟡 MEDIUM — network reality: AP client isolation, VPNs, and the wrong IP in the QR

| | |
|---|---|
| **Symptom** | "The QR code opens a page that never loads." Most common real-world failure after permissions. |
| **Why** | (a) Guest/公 Wi-Fi and many consumer mesh systems enable **client isolation**, which blocks phone→laptop traffic entirely. (b) Phone on 5 GHz SSID, laptop on a different VLAN or on Ethernet in another subnet. (c) The machine has four interfaces — Ethernet, Wi-Fi, a VPN adapter, a Docker bridge — and you picked the wrong one. VPN adapters are the number one cause of "the QR points at the wrong IP". |
| **Mitigation** | Enumerate all interfaces, **default to the one on the same subnet as the default route**, and let the user pick from a dropdown showing every candidate IP. Show the plain URL under the QR for manual entry. Regenerate the QR when the IP changes (DHCP lease, network switch). Ship a `docs/troubleshooting.md` covering exactly these three causes before you ship the plugin. |

---

### R18 — 🟡 MEDIUM — Android tab backgrounding and throttling

| | |
|---|---|
| **Symptom** | Android user switches apps; audio continues for a while then degrades or stops; timers stop firing. |
| **Why** | Chrome throttles background tabs aggressively (timers, rAF). Audio output is more resilient than on iOS but a ScriptProcessorNode driven from the main thread is exactly the thing throttling hurts. |
| **Mitigation** | Same story as iOS: Wake Lock (needs HTTPS) and an explicit "keep this page in the foreground" instruction. Handle `visibilitychange` — on return, resync the ring rather than trying to play out a stale backlog. |

---

### R19 — 🟡 MEDIUM — Bluetooth latency dwarfs everything you optimise

| | |
|---|---|
| **Symptom** | "There's a huge delay." User is on AirPods. |
| **Why** | AirPods Pro 2/3: 80–160 ms. AirPods 3: 150–220 ms. Android A2DP: 200–280 ms. Your entire transport budget is 60–120 ms. A2DP is designed for playback buffering, not monitoring; nothing you write can fix it. |
| **Mitigation** | Say it in the plugin UI, in the page, and in the README: *"Use wired headphones. Bluetooth adds 100–250 ms."* This is expectation management, and it is cheaper than any engineering. Also: this is the strongest argument that a native app is not worth building — the framework delta is ~30 ms against a 200 ms Bluetooth term. |

---

### R20 — 🟡 MEDIUM — Logic / GarageBand AU sandbox

| | |
|---|---|
| **Symptom** | Works in Live and Reaper, silently fails in Logic. |
| **Why** | An AU loaded into a sandboxed host inherits the host's sandbox — TN2312 defines "Sandbox Safe" as functioning under the most restrictive settings, which includes **no network**. Logic runs AUs out of process in `AUHostingService`. |
| **Mitigation** | **Do not set `kAudioComponentFlag_SandboxSafe`.** JUCE does not set it by default — *verify this in your generated `Info.plist`* rather than assuming. Declaring it would be a falsehood, and hosts that trust the flag will load you where your sockets fail. Test in Logic **and** GarageBand in the first week (E3), not the last. This is a go/no-go for a large share of your users and finding out late is the most expensive possible ordering. |

---

### R21 — 🟢 LOW–MEDIUM — licensing: JUCE AGPLv3 §13

| | |
|---|---|
| **Issue** | Under the free tier, JUCE is AGPLv3, so PhonePostMix must be AGPLv3. AGPLv3 §13 requires that users **interacting with the software remotely over a network** be offered the corresponding source. Your plugin's entire purpose is to serve a page to a remote user over a network. |
| **Mitigation** | Trivially satisfiable and worth doing correctly: put a "Source" link on the served page pointing at the repo tag that built this binary. Cost: one `<a>`. State the licence consequence loudly in the README — it is the single most common JUCE surprise for newcomers and people will ask. |

---

### R22 — 🟢 LOW — Windows Defender Firewall prompt

Symptom: on first bind, Windows prompts, **attributed to the DAW executable**, not to your plugin. Users deny it or never see it (it can appear behind the DAW window). Mitigation: document it; detect "listener bound but zero connections ever" and surface a firewall hint in the UI. Not a v1 blocker.

---

## 2. Prove it in 10 minutes — smallest experiments, in order

Do these **before writing the real code**. E0 alone can invalidate the architecture, and it costs two minutes.

---

### E0 — Capability probe on the real LAN origin (2 min) — **the one that decides everything**

```bash
mkdir -p /tmp/ppm-probe && cd /tmp/ppm-probe
cat > index.html <<'HTML'
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<style>body{font:16px/1.6 -apple-system,system-ui;padding:1rem}b{color:#c00}</style>
<h2>PhonePostMix capability probe</h2><pre id=o></pre>
<button id=go style="font-size:24px;padding:12px 24px">Listen (tap me)</button>
<script>
const AC = window.AudioContext || window.webkitAudioContext;
const caps = {
  origin:            location.origin,
  isSecureContext:   window.isSecureContext,
  audioWorklet:      'audioWorklet' in (AC?.prototype ?? {}),
  scriptProcessor:   'createScriptProcessor' in (AC?.prototype ?? {}),
  audioDecoder:      'AudioDecoder' in window,
  wakeLock:          'wakeLock' in navigator,
  audioSession:      'audioSession' in navigator,
  sharedArrayBuffer: typeof SharedArrayBuffer !== 'undefined',
  crossOriginIsolated: self.crossOriginIsolated,
  subtleCrypto:      !!(window.crypto && window.crypto.subtle),
  webSocket:         'WebSocket' in window,
};
o.textContent = JSON.stringify(caps, null, 2);
go.onclick = async () => {
  const ctx = new AC({latencyHint:'interactive'});
  if ('audioSession' in navigator) { try { navigator.audioSession.type='playback'; } catch(e){} }
  await ctx.resume();
  const sp = ctx.createScriptProcessor(4096, 0, 2);   // the path you will actually ship
  let p = 0;
  sp.onaudioprocess = e => { for (const ch of [0,1]) {
    const b = e.outputBuffer.getChannelData(ch);
    for (let i=0;i<b.length;i++) b[i] = 0.15*Math.sin(2*Math.PI*440*(p+i)/ctx.sampleRate);
  } p += e.outputBuffer.length; };
  sp.connect(ctx.destination);
  let wl = null; try { wl = await navigator.wakeLock.request('screen'); } catch(e){}
  o.textContent += `\n\nctx.sampleRate = ${ctx.sampleRate}\nctx.state = ${ctx.state}` +
    `\nbaseLatency = ${ctx.baseLatency}\noutputLatency = ${ctx.outputLatency}` +
    `\nwakeLock = ${wl ? 'ACQUIRED' : 'FAILED'}`;
};
</script>
HTML
ipconfig getifaddr en0            # your Wi-Fi IP on macOS
python3 -m http.server 8080 --bind 0.0.0.0
```

Open `http://<that-ip>:8080` on **an iPhone and an Android phone**. Record every field.

**Pass/fail gates:**
- `isSecureContext: false` and `audioWorklet: false` → **expected**, and it confirms R1. If you see `true`, re-read the URL bar; you are on localhost.
- `scriptProcessor: true` and you hear a 440 Hz tone → the ship path works. **If this fails, stop and rethink the entire receiver.**
- `wakeLock: FAILED` → confirms R2. Now decide about HTTPS with real data instead of an argument.
- `audioSession: true/false` → answers the open question in R3.
- `ctx.sampleRate` → note it. It will be 44100 on many iPhones and 48000 on most Android. You are resampling either way.

---

### E0b — The same page over HTTPS, to price the alternative (5 min, optional but do it)

```bash
brew install mkcert && mkcert -install
mkcert 192.168.1.50    # your actual IP
python3 - <<'PY'
import http.server, ssl, functools
c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain('192.168.1.50.pem','192.168.1.50-key.pem')
h = functools.partial(http.server.SimpleHTTPRequestHandler, directory='/tmp/ppm-probe')
s = http.server.HTTPServer(('0.0.0.0', 8443), h); s.socket = c.wrap_socket(s.socket, server_side=True)
s.serve_forever()
PY
```

The phone will show a cert warning (mkcert's root isn't on the phone). Accept it on Android; on iOS observe exactly how many steps it takes. **You are measuring two things:** how much capability HTTPS buys (compare the probe output), and how bad the trust flow is. That is the whole HTTPS decision, quantified, in five minutes.

---

### E1 — ScriptProcessorNode under load, over `ws://` (10 min)

Extend the probe: a tiny Python WebSocket server pushing 48 kHz stereo Float32 frames, and an SPN that drains a preallocated ring. Then, **on a mid-range Android and an iPhone**: scroll the page continuously, open and close other tabs, and count underruns per minute at `bufferSize` 1024 / 2048 / 4096.

**Deliverable:** the smallest buffer size with zero underruns in 3 minutes of abuse, per platform. That number **is** your latency floor and belongs in the README. If 4096 (85 ms) still glitches on a real phone, the SPN-only plan is in trouble and R1's mitigation (b) becomes mandatory rather than desirable.

---

### E2 — iOS behavioural traps (3 min, no code beyond E0)

1. Flip the silent switch while the tone plays. Does it mute? Does `audioSession.type='playback'` prevent it?
2. Let the screen auto-lock. Time how long until audio stops.
3. **E2b:** add a looping silent `<audio>` element alongside the context, repeat step 2. Does the audio survive a lock? (Historically it sometimes has — this is worth 20 lines if it works.)
4. Switch to another app for 30 s and back. Does it resume, or is the ring hopelessly stale?

---

### E3 — Listening socket inside a real DAW (15 min) — the OS go/no-go

Smallest possible JUCE plugin: no audio path, no FIFO. In the editor constructor, on a button press:

```cpp
listener = std::make_unique<juce::StreamingSocket>();
const bool ok = listener->createListener (17520, {});   // {} => INADDR_ANY. Never a specific IP. See R6.
statusLabel.setText (ok ? "listening on 17520" : "bind FAILED", juce::dontSendNotification);
```

Load it in **Logic**, **GarageBand**, and **Ableton Live** on macOS 15+/26. Then from the phone: `http://<mac-ip>:17520` (connection refused is fine — you're testing whether the TCP SYN arrives at all; `lsof -iTCP:17520 -sTCP:LISTEN` and a `tcpdump` on the Mac confirm it).

**What you are proving:** (a) no Local Network prompt appears (predicted by R5 — confirm it), (b) Logic's AU sandbox does not block the bind, (c) whether the macOS Application Firewall interposes, and against which process name. **If Logic fails here, you have found the project's biggest structural risk in week one instead of week ten.**

---

### E4 — Multi-instance and port conflict (5 min)

Two instances in `AudioPluginHost`, both pressing Start. Expect instance #2 to fail to bind — confirm your UI says so rather than silently doing nothing. Then `kill -9` the host mid-listen and restart within 30 s; confirm the rebind succeeds on macOS (`SO_REUSEADDR` is set) and note the behaviour on Windows (it is not set there).

---

### E5 — Teardown, fd leaks, and the accept() deadlock (5 min)

```bash
# 500 instantiate/destroy cycles with the listener enabled
pluginval --strictness-level 10 --repeat 20 --randomise --timeout-ms 60000 \
          --output-dir ./pv-logs --verbose \
          --validate build/PhonePostMix_artefacts/Release/VST3/PhonePostMix.vst3 &
PV=$!
while kill -0 $PV 2>/dev/null; do lsof -p $PV 2>/dev/null | wc -l; sleep 2; done
```

A monotonically climbing fd count is R6's leak. A hang is R6's deadlock. Then the AU gate:

```bash
cp -R build/PhonePostMix_artefacts/Release/AU/PhonePostMix.component ~/Library/Audio/Plug-Ins/Components/
killall -9 AudioComponentRegistrar || true
auval -v aufx Ppm1 Ppmx        # must print PASS, and must not hang
```

---

### E6 — Real-time safety (2 min, once there is a `processBlock`)

```bash
pluginval --strictness-level 10 --rtcheck enabled \
          --sample-rates 44100,48000,96000 --block-sizes 1,64,128,512,1024,8192 \
          --skip-gui-tests --timeout-ms 60000 --output-dir ./pv-logs \
          --validate <path-to-plugin>
echo "exit=$?"    # 0 = pass; wire straight into CI
```

`--rtcheck` is macOS/Linux only. **Run `pluginval --help` against your pinned binary first** — flag sets differ between releases (see R14).

---

### E7 — End-to-end latency, measured not guessed (10 min)

Put a click track on the DAW master. Record the room with a laptop mic while the phone plays through **wired** earbuds held near the mic. Open the recording, measure the offset between the DAW click and the phone click. Repeat with AirPods. **Publish both numbers in the README.** They will do more to prevent bad issues than any amount of optimisation.

---

### E8 — Drift soak (20 min, unattended)

Stream for 20 minutes; log receive-ring fill in ms once per second; plot it. A straight slope = your drift in ppm (R13), and the slope tells you how long a session survives before a correction is needed. Steps or cliffs = a different bug (R8/R9), and you want to know which one you have before writing a drift corrector for a problem you don't have.

---

## 3. Cut from v1 / must not be cut

### Cut — ruthlessly

| Cut | Why |
|---|---|
| **The AudioWorklet code path** | It cannot execute on the deployment origin (R1). Ship SPN only. Add AudioWorklet in the same commit that adds HTTPS, not before. |
| **mDNS / Bonjour / DNS-SD discovery** | Re-arms macOS Local Network privacy (R5), needs an iOS entitlement, and doesn't work from a browser anyway — a web page has no UDP or multicast API, and `.local` resolution silently fails on Android Chrome. QR code beats it on every axis. |
| **UDP broadcast beacons** | Same permission trap, worse. |
| **Opus / FLAC / any codec** | LAN has gigabits of headroom; 24-bit stereo PCM at 48 k is 2.3 Mbit/s. A codec adds a dependency, encoder latency, and an entire class of format-negotiation bug for zero benefit on the only network v1 supports. |
| **WebRTC / libdatachannel / WebTransport** | Each is a large dependency solving a problem you don't have on a LAN. Revisit only when you want internet listeners. |
| **`SharedArrayBuffer` + COOP/COEP** | Needs a secure context anyway (R1) and is pointless with SPN. `postMessage` transfer of preallocated buffers is fine at these rates. |
| **Multichannel / surround / >2 channels** | Reject cleanly in `isBusesLayoutSupported`. AU negotiation is strict and "supports everything" is a fast way to fail `auval`. |
| **AAX** | The only format with a genuine paid gatekeeper (Avid program + PACE signing). Pro Tools users are not the phone-listening demographic. |
| **CLAP** | JUCE 9.0.1 does not appear to ship native CLAP authoring despite the roadmap; `clap-juce-extensions` is a dependency for a format no target host in this product's world requires. |
| **Full ASRC / DLL drift correction** | Frame drop/duplicate at a threshold is 10 lines and buys you a working session (R13). Do the real thing in v1.1 with data from E8. |
| **PWA manifest, service worker, offline** | Service workers need a secure context. Cosmetic. |
| **Multiple simultaneous listeners** | Support exactly one connected phone in v1; reject the second with a clear message. Fan-out is a whole buffering and backpressure design (R8/R9). |
| **Adaptive/auto jitter buffer** | One fixed, user-visible buffer slider with three presets. Adaptive is v1.1, and E8 tells you where to set the presets. |
| **Windows and Linux polish** | Build them in CI, do not promise them. macOS is where the Local Network, sandbox and `auval` risks concentrate; prove there first. |

### Must NOT be cut — these are what make it work at all

| Keep | Why |
|---|---|
| **Inbound-TCP-only network shape** | The single reason macOS Local Network privacy doesn't kill you (R5). Every "convenience" feature that would add discovery destroys this property. |
| **Lock-free SPSC `AbstractFifo`, zero allocation in `processBlock`** | Non-negotiable. `--rtcheck enabled` in CI from day one, not at the end. |
| **Lazy socket/thread creation — nothing on construction** | Fixes plugin scanning, `auval`, `pluginval`, and half of the multi-instance pain in one decision (R7, R14). |
| **Non-blocking accept via `waitUntilReady` poll + deterministic teardown** | The difference between a plugin and a DAW that hangs on project close (R6). |
| **Backpressure: drop oldest, never block the streaming thread** | Without it, one bad Wi-Fi moment turns into unbounded latency growth (R8, R9). |
| **A session token in the QR fragment, enforced before any audio byte** | You are publishing someone's unreleased master to an open port on the LAN (R15). |
| **Offline-render guard: `isNonRealtime()` **plus** a wall-clock rate limiter** | `isNonRealtime()` alone is documented-unreliable and has host-specific failures (R11). |
| **Keepalive / silence frames from a timer thread, not `processBlock`** | Logic stops calling `processBlock` when the transport stops (R10). |
| **Sample rate + channels + frame count in every packet header** | Packets are lost, receivers join late, and Ableton can change the rate under you (R12). |
| **A visible diagnostics panel on the page** | `isSecureContext`, `ctx.sampleRate`, buffer fill in ms, packets dropped, connection state. This is your entire support channel for free. |
| **The Standalone target** | Someone can evaluate the whole idea with no DAW. It is also your fastest debugging harness. |
| **Honest latency and Bluetooth messaging in the UI and README** | Cheaper than any engineering and prevents the most common false bug report (R19). |
| **A `docs/troubleshooting.md` written before release** | AP client isolation, firewall, wrong subnet, silent switch, screen lock. This is most of your issue tracker, pre-answered. |

---

## 4. Definition of done for v1 — acceptance checklist

Someone other than the author must be able to run this end to end. Every line is pass/fail; no line says "should".

### A. Build and repository

- [ ] `git clone && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build` succeeds on a clean macOS machine with no manual JUCE checkout and no Projucer.
- [ ] The same two commands succeed on clean Windows and Linux runners in CI.
- [ ] JUCE is pinned to an exact tag (`GIT_TAG 9.0.1`), not a branch.
- [ ] `COPY_PLUGIN_AFTER_BUILD` is OFF in CI and gated behind a CMake option.
- [ ] `LICENSE` is AGPL-3.0, the README explains what that means for a fork, and the served page carries a source link (R21).
- [ ] `docs/troubleshooting.md` exists and covers: plugin not appearing in the DAW, Gatekeeper quarantine, AP client isolation, firewall, wrong subnet/VPN IP, iOS silent switch, screen lock.

### B. Validation gates (all automated, all in CI, all exit-code-enforced)

- [ ] `pluginval --strictness-level 10 --rtcheck enabled --sample-rates 44100,48000,96000 --block-sizes 1,64,128,512,1024,8192 --repeat 20 --randomise` exits 0 for VST3 and AU, **with streaming enabled and a phone connected during at least one manual run**.
- [ ] `auval -v aufx <code> <mfr>` prints PASS and does **not** hang.
- [ ] `ctest` green, including: silence-in/silence-out, no NaN/Inf, sample-rate × block-size churn (1 to 8192), state round-trip, and a TSan test hammering the FIFO from two threads.
- [ ] E5's fd-count watch is **flat** across 500 instantiate/destroy cycles.
- [ ] Constructing and destroying the processor without ever pressing Start opens **zero** sockets and starts **zero** threads (assert it in a unit test).

### C. Host behaviour — manually verified, on macOS 15+/26

- [ ] Loads and streams in **Logic Pro** (the AU-sandbox gate, R20).
- [ ] Loads and streams in **GarageBand**.
- [ ] Loads and streams in **Ableton Live**.
- [ ] Loads and streams in **Reaper** (trial) and **JUCE AudioPluginHost**.
- [ ] **No macOS Local Network permission prompt appears** in any of them (R5). If one does, the network shape has regressed — find what added an outbound connect, a `.local` lookup, or a Bonjour call.
- [ ] Audio passes through **bit-identically** with streaming on and off (assert on a null test, not by ear).
- [ ] Transport stopped → the phone stays connected and shows "no audio from host", not a disconnect (R10).
- [ ] Host-bypass engaged → connection survives.
- [ ] Offline bounce → the stream pauses and the UI says so; the bounce completes at normal speed; no FIFO overflow (R11).
- [ ] Changing the project sample rate mid-session → the phone resyncs at the new rate within 2 s, no chipmunk (R12).
- [ ] Changing the interface buffer size mid-stream → no dropout on the DAW side.
- [ ] Two instances loaded: the second reports "port in use" clearly and the first is unaffected (R7).
- [ ] Closing the project with streaming active returns in **< 1 s** with no hang and no crash (R6).
- [ ] A project saved with streaming on reopens **not connected**, requiring an explicit Start (R14).

### D. Receiver — manually verified on real hardware

- [ ] iPhone (current iOS) and a mid-range Android phone, both on the same Wi-Fi, both work.
- [ ] QR scan → page loads → tap Listen → audio, in **under 15 seconds**, with no typing.
- [ ] The page's diagnostics panel shows `isSecureContext`, `ctx.sampleRate`, buffer fill (ms), packets dropped, and connection state.
- [ ] iPhone with the silent switch **on**: either audio plays, or the page says "your phone is on silent" (R3).
- [ ] Screen lock behaviour is **documented and matches reality** — whatever E2 found, the page and README say it (R2).
- [ ] Walking out of Wi-Fi range and back: audio recovers within 5 s without a page reload, and latency does **not** grow permanently (R8, R9).
- [ ] **20-minute continuous soak with zero user-audible dropouts** and buffer fill within ±30 ms of target at the end (R13). This is the "works for a session, not for two minutes" gate and it is the one most likely to fail.
- [ ] Connecting a second phone is refused with a clear message, and the first phone is unaffected.
- [ ] Connecting **without** the session token is refused before any audio byte is sent (R15).
- [ ] Throwing 10 kB of random bytes at the HTTP port does not crash or hang the DAW (R15).

### E. Numbers that must be written down before shipping

- [ ] Measured end-to-end latency, wired earbuds, from E7 — in the README.
- [ ] The same number on AirPods — in the README, next to "use wired headphones".
- [ ] The chosen ScriptProcessorNode buffer size per platform, and the measured floor from E1.
- [ ] Measured clock drift in ppm from E8, and the resulting time-to-first-correction.
- [ ] A one-line honest statement of what v1 does **not** do: no background audio on iOS, no internet listeners, no encryption on the wire, one listener at a time.

---

## 5. Summary of what this review changes about the plan

| Plan element | Verdict |
|---|---|
| JUCE 9 via CMake `FetchContent`, VST3 + AU + Standalone | **Keep.** No issues found. |
| Plugin-embedded HTTP server, one self-contained page | **Keep**, but bound the parser hard and add a session token (R15). |
| WebSocket on `juce::StreamingSocket` | **Keep the choice, fix the usage.** Bind `0.0.0.0`, poll with `waitUntilReady` instead of blocking `accept()`, check writability before `write()`, own accepted sockets, lazy creation. (R6, R7, R8) |
| Binary PCM packets | **Keep.** Add rate/channels/frames/sampleTime to every header. (R12) |
| Plain `ws://` on the LAN | **Keep for v1** — it works, and it keeps you inside the macOS "inbound TCP needs no permission" exemption. But it costs you AudioWorklet, Wake Lock, WebCrypto and SAB, and two of the three Chrome LNA legs are on a roadmap to be gated. (R1, R2, R16) |
| **"AudioWorklet where available, ScriptProcessorNode otherwise"** | **WRONG. Delete the AudioWorklet branch.** It can never execute on an `http://` LAN origin. You are shipping an SPN product; design for SPN's main-thread fragility and 42–85 ms buffer floor deliberately. (R1, R4) |
| `AbstractFifo`, audio thread writes, streaming thread drains | **Keep.** It does not allocate. Size it in `prepareToPlay`, never resize it live, and keep it strictly single-reader/single-writer. (R10) |
| No OpenSSL, no libwebrtc, no npm | **"No libwebrtc" and "no npm" are correct and should be defended. "No OpenSSL" is the constraint that costs you the most** — it is the sole reason R1 and R2 exist. Revisit it right after v1 ships; mbedTLS via `FetchContent` is neither npm nor an OpenSSL build. |

---

## Sources

**Secure contexts, AudioWorklet, ScriptProcessorNode**
- [MDN: AudioWorklet](https://developer.mozilla.org/en-US/docs/Web/API/AudioWorklet) — *"available only in secure contexts (HTTPS)"*
- [MDN: BaseAudioContext.audioWorklet](https://developer.mozilla.org/en-US/docs/Web/API/BaseAudioContext/audioWorklet) — property is not exposed outside secure contexts
- [W3C Secure Contexts](https://www.w3.org/TR/secure-contexts/) · [MDN: Secure Contexts](https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts)
- [WebAudio/web-audio-api issue #1436 — enforcing SecureContext on `audioWorklet.addModule()`](https://github.com/WebAudio/web-audio-api/issues/1436)
- [caniuse: ScriptProcessorNode](https://caniuse.com/mdn-api_scriptprocessornode) — ✅ iOS Safari 26.6 / 27-TP, ✅ Chrome Android 151, as of 2026-08
- [MDN: BaseAudioContext.createScriptProcessor()](https://developer.mozilla.org/en-US/docs/Web/API/BaseAudioContext/createScriptProcessor) — deprecated, no secure-context requirement, buffer sizes 256–16384
- [MDN: ScriptProcessorNode](https://developer.mozilla.org/en-US/docs/Web/API/ScriptProcessorNode)

**macOS / iOS local network privacy and sandboxing**
- [Apple Developer Forums — Local Network Privacy FAQ](https://developer.apple.com/forums/thread/663874) — the authoritative "requires / does not require" table; listening + accepting TCP does **not** require permission
- [Apple TN3179 — Understanding Local Network Privacy](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)
- [Apple Developer Forums 811690 — local network request blocked in Safari but working in Chrome](https://developer.apple.com/forums/thread/811690)
- [Apple TN2312 — Audio Unit Host Sandboxing Guide](https://developer.apple.com/library/archive/technotes/tn2312/_index.html)
- [The Eclectic Light Company — How local network privacy could affect you](https://eclecticlight.co/2026/01/14/how-local-network-privacy-could-affect-you/)
- [Native Instruments — granting audio apps local network access on macOS 15](https://support.native-instruments.com/hc/en-us/articles/27553634808861)

**Browser local network access**
- [Chrome for Developers — New permission prompt for Local Network Access](https://developer.chrome.com/blog/local-network-access) — launching in Chrome 142; WebSockets/WebTransport/WebRTC *not yet* gated
- [WICG Local Network Access explainer](https://github.com/WICG/local-network-access/blob/main/explainer.md)

**JUCE internals (read against `master`)**
- [`modules/juce_core/network/juce_Socket.cpp`](https://github.com/juce-framework/JUCE/blob/master/modules/juce_core/network/juce_Socket.cpp) — `closeSocket` self-connect-to-loopback hack for listeners; `makeReusable` → `SO_REUSEADDR` on non-Windows only; `TCP_NODELAY` set in `resetSocketOptions`; `waitForNextConnection` = bare blocking `accept()` with no timeout and an unclosed socket on the shutdown path
- [JUCE docs: StreamingSocket](https://docs.juce.com/master/classStreamingSocket.html) — `write()` "will block unless you have checked the socket is ready for writing"
- [JUCE docs: AbstractFifo](https://docs.juce.com/master/classAbstractFifo.html) — holds no data, lock-free, single-reader/single-writer, `setTotalSize` not thread-safe
- [JUCE docs: AudioProcessor](https://docs.juce.com/master/classAudioProcessor.html) — `isNonRealtime()` unreliable until after `prepareToPlay`
- [JUCE forum — killing a StreamingSocket listener](https://forum.juce.com/t/killing-a-streamingsocket-listener/4216)

**Validation**
- [pluginval `Source/CommandLine.cpp`](https://github.com/Tracktion/pluginval/blob/develop/Source/CommandLine.cpp) — confirms `--rtcheck [disabled|enabled|relaxed]` (macOS/Linux only), `--strictness-level 1-10`, `--repeat`, `--randomise`, `--sample-rates`, `--block-sizes`, `--timeout-ms`, `--skip-gui-tests`; **`--validate-in-process` is absent from the recognised-option table**
- [pluginval README](https://github.com/Tracktion/pluginval/blob/develop/README.md) · [Adding pluginval to CI](https://github.com/Tracktion/pluginval/blob/develop/docs/Adding%20pluginval%20to%20CI.md) · [Testing plugins with pluginval](https://github.com/Tracktion/pluginval/blob/develop/docs/Testing%20plugins%20with%20pluginval.md)
- [Apple TN2204 — Audio Unit validation using auval](https://developer.apple.com/library/archive/technotes/tn2204/_index.html)

**iOS audio behaviour**
- [WebKit bug 237322 — Web Audio muted when the iOS ringer is muted](https://bugs.webkit.org/show_bug.cgi?id=237322)
- [MDN: AudioSession](https://developer.mozilla.org/en-US/docs/Web/API/AudioSession) · [W3C Audio Session](https://www.w3.org/TR/audio-session/) · [WebKit PR #7190 — experimental AudioSession Web API](https://github.com/WebKit/WebKit/pull/7190)
- [feross/unmute-ios-audio](https://github.com/feross/unmute-ios-audio) · [swevans/unmute](https://github.com/swevans/unmute)
- [Apple Developer Forums 774239 — Web Audio/WebRTC suspended on background/lock](https://developer.apple.com/forums/thread/774239)
- [MDN: Autoplay guide](https://developer.mozilla.org/en-US/docs/Web/Media/Guides/Autoplay) · [Matt Montag — Unlock Web Audio in Safari](https://www.mattmontag.com/web/unlock-web-audio-in-safari-for-ios-and-macos)
- [web.dev — Screen Wake Lock supported in all browsers](https://web.dev/blog/screen-wake-lock-supported-in-all-browsers) (secure context required)

**Latency reference**
- [HPBN — Wi-Fi](https://hpbn.co/wifi/) (p99 tail) · [Android audio latency](https://source.android.com/docs/core/audio/latency/app) · [AirPods Pro 2 latency](https://onlineaudiotest.com/devices/airpods-pro-2/)

**Prior art (context for the cut list)**
- [SonoBus](https://github.com/sonosaurus/sonobus) · [SonoBus user guide](https://sonobus.net/sonobus_userguide.html) · [Audiomovers LISTENTO](https://audiomovers.com/listento)
