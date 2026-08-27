# PhonePostMix

[![build](https://github.com/lucamanuel06/phonepostmix/actions/workflows/build.yml/badge.svg)](https://github.com/lucamanuel06/phonepostmix/actions/workflows/build.yml)

Listen to your DAW's master bus on your phone, in real time, over your own Wi-Fi — no app
to install on the phone, no cloud service, no cable.

The problem it solves is the oldest one in mixing: your monitors and your room lie to you.
Checking a mix on a phone speaker or on earbuds normally means bouncing, transferring the
file, and listening to a snapshot of a mix you have already moved on from. PhonePostMix
puts the *live* master bus on the phone: you move a fader, and you hear it there.

## Status

**v1, working on macOS.** The plugin builds as AU, VST3 and Standalone, streams uncompressed
PCM to a phone browser over the LAN, and the test suite passes.

What works:

- Bit-exact pass-through: the plugin never alters the audio it monitors.
- AU / VST3 / Standalone from one CMake build.
- Embedded HTTP + WebSocket server; the receiver page is compiled into the binary, so
  there is nothing to install or host.
- QR code and copyable URL in the editor; a per-session token gates the audio stream.
- PCM 16-bit, 24-bit and float32; 256 / 512 / 1024 frames per packet.
- Mono or stereo master bus (mono is duplicated to a stereo stream).
- Receiver page with jitter buffer, drift correction, level meters and a diagnostics panel.
- A dependency-free headless receiver (`tools/listen.js`) for testing and bug reports.

What does not work, or is not there yet:

- **Windows and Linux build and pass the test suite in CI, but nobody has loaded the
  plugin into a DAW there.** The whole test suite runs green on windows-2022 and
  ubuntu-22.04, so the code compiles and the protocol works on those platforms. Whether a
  host loads it, whether the firewall prompt is survivable, and whether the editor looks
  right are all unknown. Treat them as unproven in a way that "it builds" does not fix.
- **No compression.** Uncompressed PCM only, 1.5–3.1 Mbit/s.
- **No encryption.** Plain HTTP and plain `ws://` (see [Security](#security)).
- The phone's buffer slider is applied by the receiver, not negotiated with the sender.
  The plugin records what each listener chose and shows it, but has no control over it.
- No latency slider in the plugin editor; the target latency is only in saved state.
- No measured end-to-end latency figure yet. The budget below is computed from the code,
  not from a microphone.

## How it works

Audio never leaves the audio thread by any path that could block it. The audio thread
writes into a lock-free ring buffer, a separate thread drains it and packetises, and each
connected phone has its own thread that owns its socket.

```
 ┌─ DAW process ──────────────────────────────────────────────────────────────┐
 │                                                                            │
 │  master bus                                                                │
 │      │                                                                     │
 │      ▼   processBlock()                                     [AUDIO THREAD] │
 │  PhonePostMixProcessor  ── audio passes through untouched ──▶ to the host   │
 │      │  peak meters (atomics) + ring.write()  — no locks, no allocations    │
 │      ▼                                                                     │
 │  AudioRingBuffer   interleaved float, ~0.5 s, drop-whole-block on overrun   │
 │      │                                                                     │
 │      ▼   polls every 2 ms, pulls exactly framesPerPacket   [STREAM THREAD]  │
 │  StreamEngine::run()                                                       │
 │      │  32-byte PPMX header + interleaved pcm16 / pcm24 / float32          │
 │      ▼                                                                     │
 │  StreamServer::broadcastBinary()  — one WebSocket frame, shared by clients  │
 │      │                                                                     │
 │      ├──▶ Connection A queue ──┐                        [ONE THREAD EACH]   │
 │      └──▶ Connection B queue ──┤  drop OLDEST audio when a phone stalls     │
 │                                │  control frames are never dropped          │
 │  Acceptor thread  :17520  ─────┘                                            │
 │      GET /        → index.html   (compiled in, no file I/O)                 │
 │      GET /app.js  → app.js       (compiled in)                              │
 │      Upgrade: websocket + ?t=<token> → RFC 6455 upgrade, else 403           │
 │                                                                            │
 │  Editor  [MESSAGE THREAD, 15 Hz]  QR code · URL · meters · client stats     │
 └────────────────────────────────────────────────────────────────────────────┘
                        │  ws://192.168.x.x:17520/ws   (plain TCP, no TLS)
                        ▼
 ┌─ Phone browser — http://192.168.x.x:17520 ─────────────────────────────────┐
 │  NOT a secure context: no AudioWorklet, no wake lock, no SharedArrayBuffer  │
 │                                                                            │
 │  WebSocket ─▶ packet parse ─▶ ring (Float32Array, ~5.5 s)                   │
 │                                    │                                        │
 │                          PI drift controller (±0.2 % clamp)                 │
 │                                    │  ratio = srcRate/ctxRate · (1+corr)    │
 │                          linear-interpolating resampler                     │
 │                                    │                                        │
 │                          ScriptProcessorNode (4096 frames) ─▶ gain ─▶ out   │
 └────────────────────────────────────────────────────────────────────────────┘
```

The whole discovery story is the QR code. There is no mDNS, no Bonjour, no UDP beacon —
that is what keeps the plugin out of macOS's Local Network permission regime entirely
(see [ADR-0004](docs/adr/0004-qr-code-discovery-no-mdns.md)).

## Quick start (users)

1. Build the plugin (see below) or install a release build.
2. The build copies the plugin into your user plug-in folders:

   | Platform | AU | VST3 | Standalone |
   | --- | --- | --- | --- |
   | macOS | `~/Library/Audio/Plug-Ins/Components/PhonePostMix.component` | `~/Library/Audio/Plug-Ins/VST3/PhonePostMix.vst3` | `build/PhonePostMix_artefacts/<Config>/Standalone/PhonePostMix.app` |
   | Windows | — | `%COMMONPROGRAMFILES%\VST3\PhonePostMix.vst3` | `build\PhonePostMix_artefacts\<Config>\Standalone\PhonePostMix.exe` |
   | Linux | — | `~/.vst3/PhonePostMix.vst3` | `build/PhonePostMix_artefacts/<Config>/Standalone/PhonePostMix` |

3. Insert **PhonePostMix** on the master bus, last in the chain. It is a monitoring tool,
   not an effect: it passes audio through bit-for-bit and reports zero latency, so it is
   safe to leave on the master while you work.
4. Open the plugin window and press **START STREAMING**. Nothing binds a port until you
   do — instantiating the plugin, or a host scanning your plugin folder, opens no sockets.
5. Make sure the phone is on the same Wi-Fi as the computer, and scan the QR code with the
   phone's camera. If the machine has several network interfaces (Ethernet, Wi-Fi, a VPN,
   a Docker bridge), pick the right address in the **Network** dropdown; the QR code and
   URL update immediately. **Copy link** puts the URL — token included — on the clipboard
   if you would rather send it to yourself.
6. On the phone, tap **LISTEN**. A tap is required: browsers do not let a page start audio
   without a user gesture.

Two things to tell the person holding the phone:

- **Use wired headphones.** Bluetooth adds more delay than everything else in this chain
  put together.
- **The screen must stay on** and the page must stay in the foreground. The page cannot
  hold a wake lock (see [Known limitations](#known-limitations)), so set Auto-Lock to
  Never for the session.

## Quick start (builders)

The only hard requirements are CMake ≥ 3.22 and a C++17 compiler. JUCE 9.0.1 is fetched at
configure time, so a fresh clone needs nothing else.

Useful options for every platform:

- `-DPPM_JUCE_PATH=/path/to/JUCE` — use an existing JUCE checkout instead of downloading
  one. Saves several minutes and a few hundred MB on a second clone.
- `-DPPM_UNIVERSAL_BINARY=ON` — macOS only; builds arm64 + x86_64. Off by default because
  it doubles build time; turn it on for releases.
- `-DPPM_BUILD_TESTS=OFF` — skip the test target.
- `-DPPM_COPY_PLUGIN_AFTER_BUILD=OFF` — do not install into the user plug-in folders after
  each build. On by default, and what CI uses.

### macOS

Xcode command line tools, and nothing else.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

For a release build that runs on both Apple silicon and Intel:

```sh
cmake -B build-universal -G Ninja -DCMAKE_BUILD_TYPE=Release -DPPM_UNIVERSAL_BINARY=ON
cmake --build build-universal
```

The deployment target is pinned to macOS 11.0.

### Windows

Visual Studio 2022 (the Desktop C++ workload).

```pwsh
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

AU is macOS-only; on Windows the build produces VST3 and Standalone. CI builds this and
runs the full test suite on `windows-2022`, but **nobody has loaded the plugin into a DAW
on Windows** — if you do, the result either way is a genuinely useful bug report. Expect a
Windows Defender Firewall prompt the first time you press start; it will name your DAW, not
PhonePostMix.

### Linux

JUCE needs its development packages even for a plugin with a small GUI. `JUCE_USE_CURL=0`
and `JUCE_WEB_BROWSER=0` are set in `CMakeLists.txt`, so libcurl and WebKitGTK are *not*
needed.

```sh
sudo apt install build-essential cmake ninja-build \
    libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
    libharfbuzz-dev \
    libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxi-dev \
    libxinerama-dev libxrandr-dev libxrender-dev \
    libglu1-mesa-dev mesa-common-dev

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

VST3 and Standalone only. CI builds this and runs the full test suite on `ubuntu-22.04`,
but **nobody has loaded the plugin into a DAW on Linux**, same caveat as Windows.

## Testing

The unit tests need no audio device, no host and no network beyond loopback:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

57 tests cover the ring buffer (including a concurrent producer/consumer soak), the packet
header layout byte-for-byte, the sample conversions, the HTTP parser, the WebSocket frame
parser and accept-key derivation, and the server and engine end-to-end over a real
loopback socket — asset serving, token rejection, ping/pong, port fallback, drop-oldest
backpressure, and 60 start/stop cycles with a file-descriptor count to catch leaks (POSIX
only). A second suite runs the *receiver* under Node — `tests/web/receiver.test.js` loads
`web/app.js` into a stubbed browser and drives the packet decoder, the ring buffer, the
resampler and the concealment directly, because a bug in any of those sounds like a bad
mix rather than like a bug.

`tools/listen.js` is a headless receiver: a real client, no dependencies, Node 16+. It
decodes the stream and reports what it actually sees, which is what a useful bug report
contains.

```sh
# Start the standalone with the server already running, and print the URL:
PPM_AUTOSTART=1 build/PhonePostMix_artefacts/Debug/Standalone/PhonePostMix.app/Contents/MacOS/PhonePostMix
# → PPM_LISTEN_URL=http://192.168.1.50:17520/#t=1f3c9ab2...

# In another terminal — quote the URL, the shell will eat the '#' otherwise:
node tools/listen.js 'http://192.168.1.50:17520/#t=1f3c9ab2...'
```

```
connected to 192.168.1.50:17520
sender: PhonePostMix 0.1.0
stream: 48000 Hz · 2 ch · pcm16 · 512 frames/packet
418 packets · 1547 kbit/s · lost 0 · peak -14.2 dBFS
```

Capture ten seconds to a WAV file and exit:

```sh
node tools/listen.js 'http://192.168.1.50:17520/#t=…' --wav /tmp/capture.wav --seconds 10
```

## Latency

Nothing here has been measured with a microphone yet; every figure is computed from the
code and from published device numbers. Treat it as a budget, not a measurement.

| Stage | Default | Range | Where it comes from |
| --- | --- | --- | --- |
| DAW output buffer | ~5–11 ms | 1–43 ms | Your host's buffer size. PhonePostMix adds nothing here and reports zero latency. |
| Ring buffer + poll | ~1–2 ms | — | The stream thread polls the ring every 2 ms (`pollIntervalMs`). |
| Packetisation | **10.7 ms** | 5.3 / 10.7 / 21.3 ms | One packet must be full before it is sent: 256 / 512 / 1024 frames at 48 kHz. |
| Wi-Fi + TCP | ~2–10 ms | 2 ms – seconds | A quiet LAN is a few ms. A congested access point is not, and TCP retransmits rather than dropping (see [ADR-0002](docs/adr/0002-websocket-tcp-transport.md)). |
| Receiver jitter buffer | **120 ms** | 40–500 ms | The phone's buffer slider. This is the knob that trades latency for robustness. |
| ScriptProcessorNode | **85 ms** | fixed | 4096 frames at 48 kHz. Fixed in the receiver; smaller sizes glitch on phones. This is the single largest term we control, and it is the price of no AudioWorklet ([ADR-0003](docs/adr/0003-scriptprocessornode-and-plain-http.md)). |
| **Subtotal, defaults** | **≈ 225 ms** | ≈ 135 ms best case | 512-frame packets, 120 ms target, wired output. |
| Wired headphones | ~1–10 ms | — | Phone DAC and analogue output. |
| **Bluetooth** | **+80–280 ms** | — | AirPods Pro 2/3 80–160 ms; AirPods 3 150–220 ms; Android A2DP 200–280 ms. |

> ⚠️ **Bluetooth roughly doubles the total delay and nothing in this project can fix it.**
> A2DP is designed for buffered playback, not monitoring. Use wired headphones or a wired
> speaker. If you must use Bluetooth, expect around half a second and treat the tool as a
> tonal-balance check rather than a timing reference.

To pull the total down: set the packet size to 256 frames, drag the phone's buffer slider
towards "lowest latency" until you hear dropouts, then back off. The diagnostics panel on
the phone shows its own estimate (packet + buffer + output), explicitly excluding the
headphones.

## Known limitations

These are consequences of one root cause: **the page is served over plain HTTP from an
RFC 1918 address, which is not a secure context.** Only `127.0.0.0/8`, `::1` and
`localhost` are trustworthy origins without TLS; `192.168.x.x` is not. Everything below
follows from that.

- **No AudioWorklet.** It is `[SecureContext]`, so `audioContext.audioWorklet` is simply
  `undefined` on the phone. There is deliberately no worklet code path in the receiver: a
  branch that can never execute on any device is dead code, not a fallback.
- **ScriptProcessorNode is deprecated.** It is the only playback path available, it runs
  on the main thread, and it costs a fixed 85 ms. Browsers have kept it working for a
  decade; if one removes it, this receiver stops working and the fix is HTTPS.
- **No wake lock, so the phone's screen must stay on.** `navigator.wakeLock` is also
  secure-context-only. iOS suspends web audio when the screen locks. The page says so, and
  requests a wake lock anyway in case it is ever granted, but you must set Auto-Lock to
  Never.
- **Backgrounding the tab breaks playback**, especially on iOS. Keep the page in front.
- **TCP, not UDP.** A late packet blocks the ones behind it (head-of-line blocking), so a
  bad Wi-Fi moment becomes a gap rather than a glitch. The alternatives all require WebRTC
  or WebTransport, which need TLS. See [ADR-0002](docs/adr/0002-websocket-tcp-transport.md).
- **LAN only.** There is no relay, no NAT traversal, no internet path. The phone and the
  computer must be on the same network, and that network must allow client-to-client
  traffic.
- **iOS silent switch mutes web audio.** The page sets `navigator.audioSession.type` where
  Safari supports it, which usually fixes it; if you see meters but hear nothing, check
  the ringer switch.
- **Windows and Linux are unverified.** Nobody has run a build there yet.
- **The AU is not declared sandbox-safe**, deliberately — it opens sockets. Logic and
  GarageBand behaviour has not been verified.

## Security

When you press start, the plugin generates a fresh random token (up to 128 bits from
JUCE's `Random`, hex-encoded) and binds a listening socket on `0.0.0.0` — every interface.
The token goes in the **fragment** of the listen URL (`http://host:port/#t=…`), so it never
appears in a request line, a server log or a `Referer` header. The page reads it out of
the fragment and puts it on the WebSocket query string; the server refuses any upgrade
whose `t=` does not match, with a 403.

What the token protects: **the audio.** Someone else on the same Wi-Fi who guesses the
port cannot listen to your unreleased mix without the token from the QR code.

What it does *not* protect:

- **The page itself is unauthenticated.** `GET /` and `GET /app.js` are served to anyone
  who asks. That is intentional — they are two static assets with no secrets in them — but
  it does mean the port is trivially identifiable as PhonePostMix.
- **Nothing is encrypted.** No TLS anywhere. Anyone who can capture your Wi-Fi traffic can
  read the token off the WebSocket handshake and reconstruct the audio from the packets.
  On a shared or public network, assume the mix is in the clear.
- **The token is not a session key.** It lives as long as the plugin instance and is not
  rotated. Anyone you send the link to keeps access until you stop streaming.
- **There is an HTTP parser inside your DAW's process**, reachable by anyone on the LAN.
  The attack surface is kept deliberately tiny — two compiled-in assets, no filesystem
  path to traverse, no request bodies, no keep-alive, a 16 KB cap on the request head, and
  a 1 MiB cap on WebSocket payloads — but it is not zero.

Press stop when you are done. Practical advice: this is a tool for your own studio network,
not for a coworking space or a hotel.

## Troubleshooting

| Symptom | Likely cause | What to do |
| --- | --- | --- |
| QR code scans, page never loads | **AP client isolation.** Guest networks and many consumer mesh systems block phone-to-laptop traffic entirely. | Use your own SSID, not the guest one. Disable "client isolation" / "AP isolation" / "guest mode" on the router. |
| Page never loads, and the address looks unfamiliar | **A VPN or virtual adapter** is first in the list. This is the most common cause of a wrong IP in the QR code. | Pick the right address in the **Network** dropdown (typically `192.168.…`), or disconnect the VPN. `10.…` and `172.…` are often VPN, Docker or VM bridges. |
| Page never loads, address looks right | **Wrong interface or subnet** — phone on a 5 GHz guest SSID, laptop on Ethernet in another VLAN. | Put both on the same SSID. Compare the first three octets of the phone's IP with the plugin's. |
| Page never loads, macOS | **The macOS Application Firewall** blocks incoming connections. The prompt names your DAW, not PhonePostMix, so it is easy to miss or deny. | System Settings → Network → Firewall → Options → allow incoming connections for the DAW (or the PhonePostMix standalone). |
| Page never loads, Windows | **Windows Defender Firewall** prompts on first bind, attributed to the DAW, and the dialog often appears behind the DAW window. | Allow the DAW on private networks. If the prompt was dismissed, remove the deny rule from Windows Defender Firewall settings. |
| Page loads, LISTEN does nothing | Browser blocked audio without a gesture, or the WebSocket was refused. | Tap the button directly, then open **Diagnostics**. Status "connecting" that never becomes "connected" means the upgrade was refused — usually a stale token from an old QR code. Rescan. |
| "Could not bind a port" in the editor | Ports 17520–17539 are all taken, usually by other plugin instances. | Close other PhonePostMix instances; only one needs to be streaming. |
| Meters move on the phone, no sound | iOS ringer/silent switch, or volume at zero. | Flip the silent switch; check the page's own volume slider. |
| Audio drops out every few seconds | Wi-Fi congestion; the jitter buffer is too small. | Drag the buffer slider towards "most stable". Check the plugin's status line: rising "frames dropped" means the phone cannot keep up. |
| Huge delay | Bluetooth. | Use wired headphones. See [Latency](#latency). |

The phone's **Diagnostics** panel has a *Copy diagnostics* button — paste that into any bug
report, along with the output of `node tools/listen.js`.

## Licence

**AGPL-3.0-or-later.** See [LICENSE](LICENSE).

This is not a preference, it is a consequence. PhonePostMix is built on JUCE 9, whose
modules are dual-licensed: AGPLv3, or a commercial licence from the copyright holder. The
free tier is AGPLv3, so anything that links JUCE and is distributed must be AGPLv3 too, and
that licence propagates to this whole project. See
[ADR-0001](docs/adr/0001-juce-and-agpl.md).

One practical consequence worth knowing: AGPLv3 §13 requires that users who interact with
the software **remotely over a network** be offered the corresponding source. Serving a
page to a phone is exactly that. Adding a source link to the served page is the obvious
way to satisfy it, and the page does not have one yet.

### Credits

- [JUCE](https://juce.com) 9.0.1 — audio plugin framework, AGPLv3 or commercial.
- [qrcodegen](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki, MIT —
  vendored unmodified in `third_party/qrcodegen/`, used to draw the listen URL.

## Documentation

- [`docs/protocol.md`](docs/protocol.md) — the complete wire protocol: packet header,
  control messages, HTTP endpoints and token rules. Enough to write a receiver in another
  language.
- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) — repo layout, threading rules, how to add a
  test, how to run the validators.
- [`docs/adr/`](docs/adr/) — the four decisions that shaped this code, and why.
- [`docs/PLAN-arch.md`](docs/PLAN-arch.md), [`docs/PLAN-risks.md`](docs/PLAN-risks.md) —
  the original design and the adversarial review of it. Background reading; where they
  disagree with the code, the code is right.
- [`docs/research/`](docs/research/) — the prior-art, transport and receiver research the
  plan was built on.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — workflow, and the one rule that matters most.
