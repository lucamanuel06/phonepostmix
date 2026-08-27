# Development guide

How to work on PhonePostMix: where things live, how to build and test, which thread owns
what, and the traps that have already cost somebody a day.

Read [`CONTRIBUTING.md`](../CONTRIBUTING.md) first for the workflow and commit conventions.
Read [the ADRs](adr/) if you are about to argue with a design decision — the four that
shaped this code are written down with their reasons.

## Repository layout

| Path | What is in it |
| --- | --- |
| `CMakeLists.txt` | The whole build. JUCE is fetched at configure time; the plugin, the binary-data target and the test app are all defined here. |
| `src/core/` | Everything that is not JUCE-plugin-specific and has no GUI: the ring buffer, the wire format, the HTTP and WebSocket protocol code, the server and the streaming engine. Compiled into both the plugin and the test app. |
| `src/plugin/` | The JUCE `AudioProcessor`, its editor, and the QR code component. Depends on `src/core`; nothing in `src/core` depends on it. |
| `web/` | The receiver page: `index.html` and `app.js`. No build step, no modules, no external requests. Compiled into the binary by `juce_add_binary_data`. |
| `tests/` | The C++ test app: a tiny registry-and-macros harness (`TestSupport.h`) plus one file per unit under test. No test framework dependency. |
| `tests/web/` | The receiver's tests, run under Node: a stubbed-browser harness plus the test file. |
| `tools/` | `listen.js`, a dependency-free headless receiver used to test the sender without a phone. |
| `third_party/qrcodegen/` | Nayuki's QR generator, MIT, vendored unmodified. Warnings are disabled for it in CMake; do not edit it. |
| `docs/` | This guide, the protocol reference, the ADRs, and the original plan and risk review. |
| `docs/adr/` | Architecture decision records, numbered and immutable. New decision → new ADR, never an edit to an old one. |
| `docs/research/` | The three research reports the plan was built on. Background only. |
| `build/` | Untracked build output. |

Two rules about the layout: `src/core` must stay free of `juce_audio_processors` and of
anything GUI, so the tests can link it without a host; and the receiver page must stay a
single HTML file plus a single script, because both are served from memory by a server
that has no concept of directories.

## Building

Requirements: CMake ≥ 3.22, a C++17 compiler, and (on Linux) the JUCE development
packages listed in the [README](../README.md#linux). JUCE 9.0.1 is downloaded at configure
time.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Options:

| Option | Default | Why you would change it |
| --- | --- | --- |
| `PPM_JUCE_PATH` | empty | Point at an existing JUCE checkout to skip the download. Worth doing once you have a second clone: `-DPPM_JUCE_PATH=$HOME/dev/JUCE`. |
| `PPM_UNIVERSAL_BINARY` | `OFF` | macOS arm64 + x86_64. Doubles build time; on for releases and CI, off day to day. Must be set on the *first* configure of a build directory — the deployment target and architectures are baked in before the first target exists. |
| `PPM_BUILD_TESTS` | `ON` | Turn off for a plugin-only build. |
| `PPM_COPY_PLUGIN_AFTER_BUILD` | `ON` | Installs the AU and VST3 into your user plug-in folders after every build. CI sets it `OFF`: a build agent has no business writing outside its workspace. |

`PPM_COPY_PLUGIN_AFTER_BUILD` is on by default, so every build installs the AU and VST3
into your user plug-in folders. That is convenient and occasionally surprising: your DAW picks up the new
binary the next time it scans, including a debug build with assertions live.

Useful artefacts after a build:

```
build/PhonePostMix_artefacts/<Config>/VST3/PhonePostMix.vst3
build/PhonePostMix_artefacts/<Config>/AU/PhonePostMix.component        (macOS)
build/PhonePostMix_artefacts/<Config>/Standalone/PhonePostMix.app      (macOS)
build/PhonePostMixTests_artefacts/<Config>/PhonePostMixTests
```

## The test suite

```sh
ctest --test-dir build --output-on-failure
```

`ctest` runs two suites.

`PhonePostMixTests` is a console app that runs all 43 C++ tests in-process and prints a
pass/fail line each, then every failure with file and line. It needs no audio device, no
plugin host and no network beyond loopback; a full run takes a few seconds.

`PhonePostMixReceiverTests` runs `tests/web/receiver.test.js` under Node. It loads
`web/app.js` into a stubbed browser (`tests/web/harness.js` — a `vm` context and about
forty lines of DOM stubs, not jsdom) and drives the packet decoder, the ring buffer, the
resampler, the concealment and the drift controller directly. It is registered only when
CMake finds `node`, so a contributor without Node still gets a green C++ build.

You can also run the binary directly — useful for a quick loop, and the only way to get the
output without ctest's buffering:

```sh
./build/PhonePostMixTests_artefacts/Debug/PhonePostMixTests
```

What is covered, and therefore what you will break if you are careless:

| File | Guarantees |
| --- | --- |
| `AudioRingBufferTests.cpp` | Interleaved round-trip, wrap-around, whole-block discard on overrun, mono→stereo upmix, and a concurrent producer/consumer soak. |
| `WirePacketTests.cpp` | The header layout **byte for byte** — this is the test that stops an accidental protocol break. Payload alignment, packet sizes, and each format's conversion including full-scale behaviour. |
| `HttpMessageTests.cpp` | Request-line and header parsing, case-insensitive lookup, incomplete heads, the three signals that make an upgrade, MIME types, response head construction. |
| `WebSocketProtocolTests.cpp` | SHA-1 against the FIPS 180 vectors, the RFC 6455 accept-key example, shortest-length frame encoding, unmasking, reassembly across arbitrary byte splits, control frames during reassembly, and rejection of unmasked and oversized client frames. |
| `StreamServerTests.cpp` | Serving and 404 over a real loopback socket, upgrade and broadcast, client text delivery, ping/pong, disconnect detection, port fallback, clean shutdown with a client attached, drop-oldest backpressure, and 60 start/stop cycles with a file-descriptor count (POSIX only). |
| `StreamEngineTests.cpp` | The page is served from `BinaryData`, upgrades without the token are refused with 403, packets match the declared format and carry `discontinuity` on the first one, the URL carries the token in the fragment, and `pushAudio` is safe while stopped. |
| `tests/web/receiver.test.js` | The receiver: all three formats decode to the same audio, mono is duplicated, loss is distinguished from reordering, discontinuity and config-epoch changes flush, the ring discards down to target instead of growing, resampling consumes the right number of input frames, underruns fade instead of clicking, and the drift correction stays inside its clamp. |

### Adding a test

Tests are functions registered by a macro. There is no framework and no discovery step:

```cpp
#include "TestSupport.h"
#include "core/WirePacket.h"

PPM_TEST (packetSizeIncludesTheHeader)
{
    ppm::wire::PacketInfo info;
    info.frames = 256;
    info.channels = 2;
    info.format = ppm::wire::Format::pcm16;

    PPM_CHECK_EQ (ppm::wire::packetSize (info), 32 + 256 * 2 * 2);
    PPM_CHECK (ppm::wire::packetSize (info) > ppm::wire::headerBytes);
}
```

- `PPM_CHECK(expr)` records a failure and keeps going. Nothing aborts, so one run tells you
  everything that is broken.
- `PPM_CHECK_EQ(a, b)` prints both values, so prefer it when comparing numbers.
- The test name becomes a C identifier — no spaces, and it must be unique across the whole
  suite.

A new `tests/*.cpp` file must be added to `target_sources(PhonePostMixTests …)` in
`CMakeLists.txt`. If it compares floats copied rather than computed, add it to the
`-Wno-float-equal` list next to the two files already there; exact comparison is the
correct assertion for a byte-preservation test.

For anything involving the server, copy the `waitFor` / `eventually` helper pattern from
the existing tests rather than sleeping a fixed amount — it keeps the suite fast when
things work and gives it a real timeout when they do not.

## Threading

Four kinds of thread touch this code. Getting them confused is how you ship a plugin that
crackles on someone else's machine and never on yours.

| Thread | Owns | May not |
| --- | --- | --- |
| **Audio thread** (the host's `processBlock`) | Writing into `AudioRingBuffer`, storing peak levels, setting `hostPlaying`. | Allocate, lock, log, do any I/O, or make any system call. Ever. |
| **Stream thread** (`StreamEngine`, `juce::Thread`, `Priority::high`) | Draining the ring, building packets, calling `broadcastBinary`. Owns `scratch`, `packet`, `sequence`, `sampleClock`. | Touch the editor or any JUCE GUI object. |
| **Acceptor thread** (`StreamServer::Acceptor`) | Polling the listening socket and spawning connections. | Block indefinitely — it polls with a 100 ms timeout so shutdown is bounded. |
| **Connection threads** (one per client) | That client's socket, its outgoing queue, its frame parser. Calls the `StreamServer::Listener` callbacks. | Block for long: `Listener` handlers run here, so keep them short. |
| **Message thread** (JUCE) | The editor, `start`/`stop`, settings, state save/restore. | Assume the engine is idle — it is not. |

The communication rules that follow from that:

- **The audio thread talks to the stream thread only through the ring buffer and atomics.**
  Nothing else. It deliberately does not signal the stream thread when data is ready:
  waking a `WaitableEvent` is a system call, and system calls are not allowed in
  `processBlock`. The stream thread polls every 2 ms instead.
- **The ring buffer is strictly single-producer/single-consumer.** One audio thread writes,
  one stream thread reads. Do not add a second reader.
- **On overrun the producer discards the whole block** rather than blocking or writing a
  partial block, and counts it. A listener wants the newest audio; the receiver sees the
  gap in the sequence numbers.
- **`broadcastBinary`/`broadcastText` take a lock**, so they may be called from any thread
  *except* the audio thread.
- `StreamEngine::stateMutex` protects the client list, the address list and the settings.
  It is held only for short copies, and never while doing I/O.
- Shutdown order matters and is the opposite of the obvious one. `StreamEngine::stop`
  joins the stream thread *before* stopping the server, because the stream thread calls
  into the server. `StreamServer::stop` signals and joins the acceptor *before* closing the
  listening socket, because closing the listener while the acceptor is alive triggers
  JUCE's loopback self-connect and leaks the descriptor it produces.

### The rule that matters most

**Never block the audio thread.** No locks, no allocations, no `new`, no `std::string`, no
logging, no file or socket I/O, no `juce::Thread::notify`, inside `processBlock` or
anything it calls. Everything that can block happens on a worker thread and communicates
through the lock-free FIFO.

This is not style. A single mutex in `processBlock` produces a dropout you cannot
reproduce, on a machine you do not own, in a session someone is being paid for. A pull
request that violates it will not be merged, however convenient it looks.

## Running the standalone headlessly

The plugin deliberately does **not** start the server in its constructor: hosts instantiate
plugins many times during a scan, and validators instantiate them hundreds of times.
Binding a port there would mean a port conflict storm and a firewall prompt during a plugin
scan.

For scripted testing there is one opt-in escape hatch, `PPM_AUTOSTART=1`, which starts the
server in the constructor and prints the listen URL — token included — to stdout:

```sh
PPM_AUTOSTART=1 \
  build/PhonePostMix_artefacts/Debug/Standalone/PhonePostMix.app/Contents/MacOS/PhonePostMix
# PPM_LISTEN_URL=http://192.168.1.50:17520/#t=1f3c9ab2…
```

On Linux/Windows the standalone binary is at
`build/PhonePostMix_artefacts/Debug/Standalone/PhonePostMix{,.exe}`.

The standalone opens JUCE's audio device, so you can route anything into it — or leave it
on the default input and stream that. The plugin build reads the same environment variable,
so `PPM_AUTOSTART=1` on the DAW's process works too; use that sparingly, since every
instance the host creates will then bind a port.

## Testing with `tools/listen.js`

`tools/listen.js` is a real receiver: it performs the WebSocket handshake, checks the
accept key, decodes packets and reports what it sees. Node 16+, no `npm install`, no
`package.json`.

```sh
# Quote the URL — an unquoted '#' is a comment in every shell.
node tools/listen.js 'http://192.168.1.50:17520/#t=1f3c9ab2…'
```

```
connected to 192.168.1.50:17520
sender: PhonePostMix 0.1.0
stream: 48000 Hz · 2 ch · pcm16 · 512 frames/packet
418 packets · 1547 kbit/s · lost 0 · peak -14.2 dBFS
```

The live line updates twice a second; Ctrl-C prints a summary. Flags:

| Flag | Effect |
| --- | --- |
| `--seconds N` | Disconnect and exit after N seconds. Exit code 0. |
| `--wav out.wav` | Write everything received to a WAV file, using the stream's own format (16/24-bit PCM or 32-bit float). |

```sh
node tools/listen.js 'http://127.0.0.1:17520/#t=…' --wav /tmp/capture.wav --seconds 10
```

Exit codes: `0` if any audio arrived, `1` if the connection failed or no packets ever came,
`2` for a usage error. That makes it usable as a smoke test in a script. A `403` during the
handshake is reported explicitly as "wrong or missing token" — that is almost always a
stale URL from a previous session.

Things it is good for: confirming the sender still produces well-formed packets after a
change; measuring actual throughput; capturing audio to compare bit-for-bit against a DAW
bounce; and reproducing a bug without a phone in your hand.

## Validators

Run both before calling a change done. They instantiate the plugin far more brutally than
any DAW, which is exactly why they catch socket and thread lifetime bugs.

### `auval` (macOS, AU)

```sh
auval -a                                  # is PhonePostMix registered at all?
auval -v aufx Ppm1 Ppmx                   # the full validation pass
```

`aufx` is the effect type, `Ppm1` is `PLUGIN_CODE` and `Ppmx` is `PLUGIN_MANUFACTURER_CODE`,
both from `CMakeLists.txt`. `auval` creates and destroys the processor many times; a leaked
socket or an unjoined thread shows up here as a hang or a resource failure, not as a nice
error message. It must pass before an AU build is shipped.

Note that the AU is **not** declared sandbox-safe, deliberately — it opens sockets, and
declaring the flag would be a falsehood that makes hosts load it where its sockets fail.

### `pluginval`

[pluginval](https://github.com/Tracktion/pluginval) at strictness 10:

```sh
pluginval --strictness-level 10 --verbose \
  --validate build/PhonePostMix_artefacts/Debug/VST3/PhonePostMix.vst3
```

Add `--skip-gui-tests` on a headless CI machine. Strictness 10 includes repeated
open/close of the editor and randomised `prepareToPlay` parameters, which is where
sample-rate and block-size churn bugs surface.

## Things that will bite you

Drawn from [`PLAN-risks.md`](PLAN-risks.md), which is worth reading in full before making
architectural changes. These are the ones that have already shaped the code.

- **AudioWorklet does not exist on the phone, and never will over plain HTTP.** It is
  `[SecureContext]`, and `192.168.x.x` is not a trustworthy origin. Do not add a
  "worklet if available" branch: it cannot execute on any device, so it is dead code that
  will rot and lie to you in review. Same for `navigator.wakeLock`, `SharedArrayBuffer`,
  WebCodecs and service workers. See [ADR-0003](adr/0003-scriptprocessornode-and-plain-http.md).
- **`juce::StreamingSocket::waitForNextConnection()` is a bare `accept()` with no
  timeout**, and JUCE unblocks it from `close()` by connecting to `127.0.0.1` on the
  listening port. That self-connected socket is accepted and then leaked, so a host that
  validates the plugin hundreds of times leaks a descriptor per teardown. The acceptor
  polls `waitUntilReady` instead, and `StreamServerTests` counts descriptors across 60
  cycles to keep it that way.
- **`StreamingSocket::write()` blocks once the kernel send buffer fills**, which is exactly
  what happens when TCP starts retransmitting on weak Wi-Fi. Blocking there stops a
  connection draining its queue and turns into unbounded latency. Every write waits for
  writability first and gives up on a 5-second deadline; the drop-oldest queue policy is
  what bounds latency from there.
- **Do not bind a port in the processor's constructor.** Plugin scans, `auval` and
  `pluginval` would produce a port conflict storm and a firewall prompt during a scan.
  `PPM_AUTOSTART` is the deliberate, opt-in exception.
- **Offline bounce runs faster than real time.** `processBlock` checks `isNonRealtime()`
  and skips streaming entirely; without it a render fires thousands of packets a second at
  a listener who cannot use them.
- **Sample rate and block size change underneath a running plugin.** That is why every
  packet carries its own description and why `configEpoch` exists. Test at 44.1/48/88.2/96
  kHz and across block sizes; `prepareToPlay` may be called at any time.
- **macOS Local Network privacy does not cover inbound TCP** — "listening for and accepting
  incoming TCP connections" is explicitly exempt — but it covers *every* discovery
  mechanism: Bonjour, mDNS, `.local` resolution, UDP multicast and broadcast. Adding any of
  those puts the whole plugin into a permission regime that is enforced by a packet filter
  rather than TCC, so `tccutil reset` cannot clear it for testing, and the prompt names the
  DAW rather than you. See [ADR-0004](adr/0004-qr-code-discovery-no-mdns.md).
- **The macOS Application Firewall is a separate mechanism** and may still prompt for
  incoming connections, attributed to the host app. So will Windows Defender Firewall, with
  the dialog often hidden behind the DAW window.
- **Clock drift is real.** The DAW's clock and the phone's audio output clock differ by tens
  of ppm; over a long session that is seconds of accumulated error. The receiver runs a PI
  controller on ring fill, clamped to ±0.2 % (about 3.5 cents), and hard-resyncs if it stays
  pinned at the clamp for five seconds.
- **Bluetooth adds 80–280 ms** and nothing in this codebase can fix it. Do not spend a day
  shaving 5 ms off the packet path for a user on AirPods.
- **Logic and GarageBand run AUs out of process**, and a sandboxed host means no network.
  This has not been verified yet; if you are testing there, do it early, not last.
- **JUCE is AGPLv3 under the free tier**, and §13 covers users interacting with the
  software over a network — which is literally what this plugin does. See
  [ADR-0001](adr/0001-juce-and-agpl.md).
